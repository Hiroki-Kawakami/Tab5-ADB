#include "adb_protocol.hpp"

namespace adb {

uint32_t payload_checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

Packet Packet::make(uint32_t command, uint32_t arg0, uint32_t arg1,
                    std::vector<uint8_t> data) {
    Packet p;
    p.header.command = command;
    p.header.arg0 = arg0;
    p.header.arg1 = arg1;
    p.payload = std::move(data);
    p.finalize();
    return p;
}

void Packet::finalize() {
    header.data_length = static_cast<uint32_t>(payload.size());
    header.data_check = payload_checksum(payload.data(), payload.size());
    header.magic = header.command ^ 0xffffffffu;
}

bool Packet::valid() const {
    if (header.magic != (header.command ^ 0xffffffffu)) {
        return false;
    }
    if (header.data_length != payload.size()) {
        return false;
    }
    // data_check of 0 means the sender skipped the checksum (A_VERSION). Only
    // verify when non-zero so we stay compatible with skip-checksum peers.
    if (header.data_check != 0 &&
        header.data_check != payload_checksum(payload.data(), payload.size())) {
        return false;
    }
    return true;
}

}  // namespace adb
