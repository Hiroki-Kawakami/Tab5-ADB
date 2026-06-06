// Host (JSON-file) implementation of the ESP-IDF NVS C API (nvs.h / nvs_flash.h).
// Mirrors the semantics shared code relies on: namespaced key/value storage,
// fixed-width integers matched by size, and length-out str/blob reads. The
// on-disk format is a JSON object { namespace: { key: "<hex bytes>" } }.
//
// Fidelity notes (acceptable for a host simulator): values are matched by byte
// length rather than NVS type, so e.g. set_i8 followed by get_u8 succeeds where
// real NVS would return ESP_ERR_NVS_TYPE_MISMATCH. Writes are persisted eagerly
// (every set), so nvs_commit() is effectively a flush.

#include "nvs_flash.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

// ---------------------------------------------------------------------------
// In-memory store: a flat array of namespaces, each a flat array of entries.
// ---------------------------------------------------------------------------
typedef struct {
    char *key;
    uint8_t *data;
    size_t len;
} entry_t;

typedef struct {
    char *name;
    entry_t *entries;
    size_t count;
    size_t cap;
} namespace_t;

typedef struct {
    nvs_handle_t handle;
    char *ns;
    bool readonly;
    bool in_use;
} open_handle_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_path[1024] = "nvs_data.json";
static bool s_loaded = false;

static namespace_t *s_namespaces;
static size_t s_ns_count, s_ns_cap;

static open_handle_t *s_handles;
static size_t s_handle_count, s_handle_cap;
static nvs_handle_t s_next_handle = 1;

#define LOCK()   pthread_mutex_lock(&s_mutex)
#define UNLOCK() pthread_mutex_unlock(&s_mutex)

// ---------------------------------------------------------------------------
// Store helpers (all assume the lock is held)
// ---------------------------------------------------------------------------
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static namespace_t *find_ns(const char *name) {
    for (size_t i = 0; i < s_ns_count; i++)
        if (strcmp(s_namespaces[i].name, name) == 0) return &s_namespaces[i];
    return NULL;
}

static namespace_t *get_or_create_ns(const char *name) {
    namespace_t *ns = find_ns(name);
    if (ns) return ns;
    if (s_ns_count == s_ns_cap) {
        size_t ncap = s_ns_cap ? s_ns_cap * 2 : 4;
        namespace_t *t = realloc(s_namespaces, ncap * sizeof(*t));
        if (!t) return NULL;
        s_namespaces = t;
        s_ns_cap = ncap;
    }
    ns = &s_namespaces[s_ns_count++];
    ns->name = strdup(name);
    ns->entries = NULL;
    ns->count = 0;
    ns->cap = 0;
    return ns;
}

static entry_t *find_entry(namespace_t *ns, const char *key) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->entries[i].key, key) == 0) return &ns->entries[i];
    return NULL;
}

static void entry_set(namespace_t *ns, const char *key, const void *data, size_t len) {
    entry_t *e = find_entry(ns, key);
    if (!e) {
        if (ns->count == ns->cap) {
            size_t ncap = ns->cap ? ns->cap * 2 : 4;
            entry_t *t = realloc(ns->entries, ncap * sizeof(*t));
            if (!t) return;
            ns->entries = t;
            ns->cap = ncap;
        }
        e = &ns->entries[ns->count++];
        e->key = strdup(key);
        e->data = NULL;
        e->len = 0;
    }
    free(e->data);
    e->data = NULL;
    e->len = len;
    if (len > 0) {
        e->data = malloc(len);
        memcpy(e->data, data, len);
    }
}

static void entry_remove(namespace_t *ns, entry_t *e) {
    free(e->key);
    free(e->data);
    *e = ns->entries[--ns->count];  // swap with last
}

static void ns_clear(namespace_t *ns) {
    for (size_t i = 0; i < ns->count; i++) {
        free(ns->entries[i].key);
        free(ns->entries[i].data);
    }
    free(ns->entries);
    ns->entries = NULL;
    ns->count = 0;
    ns->cap = 0;
}

static void store_free(void) {
    for (size_t i = 0; i < s_ns_count; i++) {
        ns_clear(&s_namespaces[i]);
        free(s_namespaces[i].name);
    }
    free(s_namespaces);
    s_namespaces = NULL;
    s_ns_count = 0;
    s_ns_cap = 0;
}

static open_handle_t *find_handle(nvs_handle_t handle) {
    for (size_t i = 0; i < s_handle_count; i++)
        if (s_handles[i].in_use && s_handles[i].handle == handle) return &s_handles[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Persistence (assume the lock is held)
// ---------------------------------------------------------------------------
static void ensure_loaded(void) {
    if (s_loaded) return;
    s_loaded = true;

    FILE *f = fopen(s_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return;
    }
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *ns_item = NULL;
    cJSON_ArrayForEach(ns_item, root) {
        namespace_t *ns = get_or_create_ns(ns_item->string);
        cJSON *kv = NULL;
        cJSON_ArrayForEach(kv, ns_item) {
            const char *hex = cJSON_GetStringValue(kv);
            if (!hex) continue;
            size_t n = strlen(hex) / 2;
            uint8_t *bytes = n ? malloc(n) : NULL;
            for (size_t i = 0; i < n; i++)
                bytes[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
            entry_set(ns, kv->string, bytes, n);
            free(bytes);
        }
    }
    cJSON_Delete(root);
}

static void save_unlocked(void) {
    cJSON *root = cJSON_CreateObject();

    for (size_t i = 0; i < s_ns_count; i++) {
        namespace_t *ns = &s_namespaces[i];
        cJSON *ns_obj = cJSON_CreateObject();
        for (size_t j = 0; j < ns->count; j++) {
            entry_t *e = &ns->entries[j];
            char *hex = malloc(e->len * 2 + 1);
            for (size_t k = 0; k < e->len; k++)
                sprintf(hex + k * 2, "%02x", e->data[k]);
            hex[e->len * 2] = '\0';
            cJSON_AddStringToObject(ns_obj, e->key, hex);
            free(hex);
        }
        cJSON_AddItemToObject(root, ns->name, ns_obj);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return;

    FILE *f = fopen(s_path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    cJSON_free(text);
}

// ---------------------------------------------------------------------------
// Typed access helpers (acquire the lock)
// ---------------------------------------------------------------------------
static esp_err_t set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    if (oh->readonly) { ret = ESP_ERR_NVS_READ_ONLY; goto done; }
    ns = get_or_create_ns(oh->ns);
    entry_set(ns, key, data, length);
    save_unlocked();
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

static esp_err_t get_fixed(nvs_handle_t handle, const char *key, void *out, size_t expected) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    entry_t *e;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    ns = find_ns(oh->ns);
    if (!ns) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    e = find_entry(ns, key);
    if (!e) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    if (e->len != expected) { ret = ESP_ERR_NVS_INVALID_LENGTH; goto done; }
    memcpy(out, e->data, expected);
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

// out == NULL → report required size only (matches nvs_get_str/blob).
static esp_err_t get_var(nvs_handle_t handle, const char *key, void *out, size_t *length) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    entry_t *e;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    ns = find_ns(oh->ns);
    if (!ns) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    e = find_entry(ns, key);
    if (!e) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    if (out == NULL) {
        *length = e->len;
        ret = ESP_OK;
        goto done;
    }
    if (*length < e->len) { ret = ESP_ERR_NVS_INVALID_LENGTH; goto done; }
    memcpy(out, e->data, e->len);
    *length = e->len;
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void nvs_flash_sim_set_path(const char *path) {
    LOCK();
    snprintf(s_path, sizeof(s_path), "%s", path);
    store_free();
    s_loaded = false;
    UNLOCK();
}

esp_err_t nvs_flash_init(void) {
    LOCK();
    ensure_loaded();
    UNLOCK();
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    LOCK();
    store_free();
    s_loaded = true;
    save_unlocked();
    UNLOCK();
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    esp_err_t ret;
    open_handle_t *slot = NULL;
    LOCK();
    ensure_loaded();
    if (open_mode == NVS_READONLY && !find_ns(name)) {
        ret = ESP_ERR_NVS_NOT_FOUND;
        goto done;
    }
    for (size_t i = 0; i < s_handle_count; i++)
        if (!s_handles[i].in_use) { slot = &s_handles[i]; break; }
    if (!slot) {
        if (s_handle_count == s_handle_cap) {
            size_t ncap = s_handle_cap ? s_handle_cap * 2 : 4;
            open_handle_t *t = realloc(s_handles, ncap * sizeof(*t));
            if (!t) { ret = ESP_ERR_NO_MEM; goto done; }
            s_handles = t;
            s_handle_cap = ncap;
        }
        slot = &s_handles[s_handle_count++];
    }
    slot->handle = s_next_handle++;
    slot->ns = strdup(name);
    slot->readonly = (open_mode == NVS_READONLY);
    slot->in_use = true;
    *out_handle = slot->handle;
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

void nvs_close(nvs_handle_t handle) {
    LOCK();
    open_handle_t *oh = find_handle(handle);
    if (oh) {
        free(oh->ns);
        oh->ns = NULL;
        oh->in_use = false;
    }
    UNLOCK();
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    esp_err_t ret;
    LOCK();
    if (!find_handle(handle)) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    save_unlocked();
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    entry_t *e;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    if (oh->readonly) { ret = ESP_ERR_NVS_READ_ONLY; goto done; }
    ns = find_ns(oh->ns);
    if (!ns) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    e = find_entry(ns, key);
    if (!e) { ret = ESP_ERR_NVS_NOT_FOUND; goto done; }
    entry_remove(ns, e);
    save_unlocked();
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    if (oh->readonly) { ret = ESP_ERR_NVS_READ_ONLY; goto done; }
    ns = find_ns(oh->ns);
    if (ns) ns_clear(ns);
    save_unlocked();
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

esp_err_t nvs_get_used_entry_count(nvs_handle_t handle, size_t *used_entries) {
    esp_err_t ret;
    open_handle_t *oh;
    namespace_t *ns;
    LOCK();
    ensure_loaded();
    oh = find_handle(handle);
    if (!oh) { ret = ESP_ERR_NVS_INVALID_HANDLE; goto done; }
    ns = find_ns(oh->ns);
    *used_entries = ns ? ns->count : 0;
    ret = ESP_OK;
done:
    UNLOCK();
    return ret;
}

esp_err_t nvs_set_i8(nvs_handle_t h, const char *k, int8_t v)    { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)   { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_i16(nvs_handle_t h, const char *k, int16_t v)  { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)  { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *k, int64_t v)  { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v) { return set_blob(h, k, &v, sizeof(v)); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return set_blob(h, k, v, strlen(v) + 1); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return set_blob(h, k, v, len); }

esp_err_t nvs_get_i8(nvs_handle_t h, const char *k, int8_t *v)    { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)   { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_i16(nvs_handle_t h, const char *k, int16_t *v)  { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)  { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v)  { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *v) { return get_fixed(h, k, v, sizeof(*v)); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len)  { return get_var(h, k, v, len); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) { return get_var(h, k, v, len); }
