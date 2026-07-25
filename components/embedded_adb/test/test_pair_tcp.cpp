#include "embedded_adb.hpp"

#include <nvs_flash.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    const char* target = std::getenv("TAB5ADB_PAIR_TARGET");
    const char* code = std::getenv("TAB5ADB_PAIR_CODE");
    if (!target || !*target || !code || !*code) {
        std::printf(
            "SKIP: set TAB5ADB_PAIR_TARGET=host:port and "
            "TAB5ADB_PAIR_CODE to run this test\n");
        return 0;
    }

    std::string address = target;
    size_t colon = address.rfind(':');
    if (colon == std::string::npos) {
        std::printf("FAIL: TAB5ADB_PAIR_TARGET must be host:port\n");
        return 1;
    }
    std::string host = address.substr(0, colon);
    long parsed_port = std::strtol(address.c_str() + colon + 1,
                                   nullptr, 10);
    if (host.empty() || parsed_port <= 0 || parsed_port > 65535) {
        std::printf("FAIL: invalid pairing target\n");
        return 1;
    }

    nvs_flash_sim_set_path("/tmp/tab5adb_pair_test_nvs.json");
    auto key = adb::load_or_create_key();
    if (!key) {
        std::printf("FAIL: could not load/create RSA key\n");
        return 1;
    }
    adb::PairingResult result = adb::pair_tcp(
        host, static_cast<uint16_t>(parsed_port), code, *key);
    if (!result) {
        std::printf("FAILED: %s\n", adb::to_string(result.error));
        return 1;
    }
    std::printf("PASSED: paired with device guid=%s\n",
                result.device_guid.c_str());
    return 0;
}
