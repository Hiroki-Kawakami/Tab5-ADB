#include "tcp_history.hpp"

#include <nvs.h>
#include <nvs_flash.h>

namespace app {
namespace tcp_history {
namespace {

constexpr const char* kNamespace = "tab5adb";  // shared with app settings
constexpr const char* kKey = "tcp_history";

// nvs_flash is initialised at boot, but a history access could precede it; ensure
// once here too (idempotent — nvs_flash_init is ESP_OK if already initialised).
void ensure_nvs() {
    static bool done = false;
    if (done) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    done = true;
}

// Read the raw newline-separated blob ("" if unset). nvs_get_str reports the
// required size (incl. the NUL) into len, then fills the buffer.
std::string read_raw() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return {};
    std::string out;
    size_t len = 0;
    if (nvs_get_str(h, kKey, nullptr, &len) == ESP_OK && len > 1) {
        out.resize(len);
        if (nvs_get_str(h, kKey, out.data(), &len) == ESP_OK)
            out.resize(len > 0 ? len - 1 : 0);  // drop the trailing NUL
        else
            out.clear();
    }
    nvs_close(h);
    return out;
}

void write(const std::vector<std::string>& items) {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    if (items.empty()) {
        nvs_erase_key(h, kKey);  // keep nvs_data.json clean when history is empty
    } else {
        std::string joined;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) joined += '\n';
            joined += items[i];
        }
        nvs_set_str(h, kKey, joined.c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

// Trim leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::vector<std::string> load() {
    ensure_nvs();
    std::string raw = read_raw();
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line =
            trim(raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (!line.empty() && out.size() < kCap) out.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

void add(const std::string& target) {
    std::string t = trim(target);
    if (t.empty()) return;
    std::vector<std::string> items = load();
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (*it == t) {
            items.erase(it);  // dedupe: it moves back to the front below
            break;
        }
    }
    items.insert(items.begin(), std::move(t));
    if (items.size() > kCap) items.resize(kCap);
    write(items);
}

void remove(const std::string& target) {
    std::string t = trim(target);
    if (t.empty()) return;
    std::vector<std::string> items = load();
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (*it == t) {
            items.erase(it);
            write(items);
            return;
        }
    }
}

}  // namespace tcp_history
}  // namespace app
