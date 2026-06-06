/*
 * Copyright (c) 2024 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cjson/cJSON.h>

class NVS {
public:
    enum class Error {
        OK = 0,
        NotFound,
        Failed,
    };

    enum class OpenMode {
        ReadWrite = 0,
        ReadOnly  = 1,
    };

    static void setStoragePath(const std::string &path) {
        std::lock_guard<std::mutex> lock(mutex());
        storagePath() = path;
        loaded() = false;
    }

    static Error init() { return Error::OK; }

    NVS(const char *name, OpenMode open_mode = OpenMode::ReadWrite) : name_(name), open_mode_(open_mode) {}
    ~NVS() = default;

    Error erase(std::string key) {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        auto &ns = store()[name_];
        auto it = ns.find(key);
        if (it == ns.end()) return Error::NotFound;
        ns.erase(it);
        saveUnlocked();
        return Error::OK;
    }

    Error eraseAll() {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        store().erase(name_);
        saveUnlocked();
        return Error::OK;
    }

    Error commit() {
        std::lock_guard<std::mutex> lock(mutex());
        saveUnlocked();
        return Error::OK;
    }

    Error getUsedEntryCount(size_t *used_entries) {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        auto it = store().find(name_);
        *used_entries = (it != store().end()) ? it->second.size() : 0;
        return Error::OK;
    }

    // set functions
    Error set(std::string key, int8_t value)   { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, uint8_t value)  { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, int16_t value)  { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, uint16_t value) { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, int32_t value)  { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, uint32_t value) { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, int64_t value)  { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, uint64_t value) { return setBlob(key, &value, sizeof(value)); }
    Error set(std::string key, const char *value) { return setBlob(key, value, strlen(value) + 1); }
    Error set(std::string key, const void *value, size_t length) { return setBlob(key, value, length); }

    // get functions
    Error get(std::string key, int8_t *out_value)   { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, uint8_t *out_value)  { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, int16_t *out_value)  { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, uint16_t *out_value) { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, int32_t *out_value)  { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, uint32_t *out_value) { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, int64_t *out_value)  { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, uint64_t *out_value) { return getFixed(key, out_value, sizeof(*out_value)); }
    Error get(std::string key, char *out_value, size_t *length) { return getVar(key, out_value, length); }
    Error get(std::string key, void *out_value, size_t *length) { return getVar(key, out_value, length); }

private:
    const char *name_;
    OpenMode open_mode_;

    using Namespace = std::unordered_map<std::string, std::vector<uint8_t>>;

    static std::mutex &mutex() {
        static std::mutex m;
        return m;
    }
    static std::string &storagePath() {
        static std::string path = "nvs_data.json";
        return path;
    }
    static bool &loaded() {
        static bool v = false;
        return v;
    }
    static std::unordered_map<std::string, Namespace> &store() {
        static std::unordered_map<std::string, Namespace> s;
        return s;
    }

    static void ensureLoaded() {
        if (loaded()) return;
        loaded() = true;

        std::ifstream f(storagePath());
        if (!f.is_open()) return;
        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        cJSON *root = cJSON_Parse(json.c_str());
        if (!root) return;

        cJSON *ns_item = nullptr;
        cJSON_ArrayForEach(ns_item, root) {
            auto &ns = store()[ns_item->string];
            cJSON *kv = nullptr;
            cJSON_ArrayForEach(kv, ns_item) {
                const char *hex = cJSON_GetStringValue(kv);
                if (!hex) continue;
                std::vector<uint8_t> bytes;
                for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
                    auto hexByte = [](char c) -> uint8_t {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return c - 'A' + 10;
                    };
                    bytes.push_back((hexByte(hex[i]) << 4) | hexByte(hex[i + 1]));
                }
                ns[kv->string] = std::move(bytes);
            }
        }
        cJSON_Delete(root);
    }

    static void saveUnlocked() {
        cJSON *root = cJSON_CreateObject();

        for (auto &[ns_name, ns] : store()) {
            cJSON *ns_obj = cJSON_CreateObject();
            for (auto &[key, bytes] : ns) {
                std::ostringstream hex;
                for (uint8_t b : bytes)
                    hex << std::hex << std::setw(2) << std::setfill('0') << (int)b;
                cJSON_AddStringToObject(ns_obj, key.c_str(), hex.str().c_str());
            }
            cJSON_AddItemToObject(root, ns_name.c_str(), ns_obj);
        }

        char *text = cJSON_Print(root);
        cJSON_Delete(root);
        if (!text) return;

        std::ofstream f(storagePath());
        if (f.is_open()) f << text;
        cJSON_free(text);
    }

    Error setBlob(const std::string &key, const void *data, size_t length) {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        auto *bytes = static_cast<const uint8_t *>(data);
        store()[name_][key].assign(bytes, bytes + length);
        saveUnlocked();
        return Error::OK;
    }

    Error getFixed(const std::string &key, void *out, size_t expected) {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        auto it1 = store().find(name_);
        if (it1 == store().end()) return Error::NotFound;
        auto it2 = it1->second.find(key);
        if (it2 == it1->second.end()) return Error::NotFound;
        if (it2->second.size() != expected) return Error::Failed;
        memcpy(out, it2->second.data(), expected);
        return Error::OK;
    }

    // out == nullptr → report required size only (matches nvs_get_str / nvs_get_blob)
    Error getVar(const std::string &key, void *out, size_t *length) {
        std::lock_guard<std::mutex> lock(mutex());
        ensureLoaded();
        auto it1 = store().find(name_);
        if (it1 == store().end()) return Error::NotFound;
        auto it2 = it1->second.find(key);
        if (it2 == it1->second.end()) return Error::NotFound;
        size_t stored = it2->second.size();
        if (out == nullptr) {
            *length = stored;
            return Error::OK;
        }
        if (*length < stored) return Error::Failed;
        memcpy(out, it2->second.data(), stored);
        *length = stored;
        return Error::OK;
    }
};
