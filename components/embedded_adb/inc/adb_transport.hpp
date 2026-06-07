// USB transport seam — the ONLY device/simulator split in embedded_adb.
//
// The connection layer reads/writes whole ADB packets through this interface; it
// does not know whether the bytes go over esp-idf usb_host (device) or libusb
// (simulator). open_usb_transport() finds and opens the first Android device on
// the bus; its implementation is the per-target piece selected by ESP_PLATFORM
// in the component CMakeLists.txt.
#pragma once

#include <memory>

#include "adb_protocol.hpp"

namespace adb {

// The ADB interface is identified on USB by a fixed (class, subclass, protocol).
constexpr uint8_t kAdbClass = 0xFF;
constexpr uint8_t kAdbSubclass = 0x42;
constexpr uint8_t kAdbProtocol = 0x01;

enum class IoResult { Ok, Timeout, Error };

class Transport {
public:
    virtual ~Transport() = default;

    // Blocking write of a full packet (24-byte header, then payload) to the bulk
    // OUT endpoint. Returns false on error.
    virtual bool write_packet(const Packet& p) = 0;

    // Read one full packet from the bulk IN endpoint. Timeout lets the caller's
    // read loop stay responsive (e.g. to a close request) without treating an
    // idle bus as a disconnect.
    virtual IoResult read_packet(Packet& p) = 0;

    // Release the interface / handle. Idempotent.
    virtual void close() = 0;
};

// Find and open the first connected Android device exposing an ADB interface.
// Returns nullptr if none is found or the open fails. Per-target implementation.
std::unique_ptr<Transport> open_usb_transport();

}  // namespace adb
