// ADB packet transport seam for USB and TCP.
//
// The connection layer does not know whether bytes go over USB or a socket. USB
// is the only device/simulator split (esp-idf usb_host vs libusb); TCP uses the
// same BSD-socket contract on both targets and can upgrade in place to TLS.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "adb_crypto.hpp"   // RsaKey (start_tls)
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

    // Upgrade the stream to TLS for the ADB STARTTLS handshake (Android 11+
    // wireless debugging): present a self-signed cert derived from `key` and run a
    // TLS 1.3 client handshake. After success, write_packet/read_packet run over
    // TLS. Returns false if the transport can't do TLS (USB) or the handshake
    // fails. Default: unsupported.
    virtual bool start_tls(const RsaKey& key) { return false; }
};

// Find and open the first connected Android device exposing an ADB interface.
// Returns nullptr if none is found or the open fails. Per-target implementation.
std::unique_ptr<Transport> open_usb_transport();

// Open an ADB-over-TCP connection to host:port (a device already listening via
// `adb tcpip` / wireless debugging). Returns nullptr on resolve/connect failure.
// Same implementation on both targets (BSD sockets / lwip), so it is not part of
// the USB device/simulator split.
std::unique_ptr<Transport> open_tcp_transport(const std::string& host, uint16_t port);

// Optional hook to reset the USB host port, so a device already attached when the
// host transport opens re-attaches with a clean connect edge (some host
// controllers can't detect a device that was already present at power-up). The
// device transport calls it once while opening; the caller supplies the mechanism
// (embedded_adb has no board knowledge). nullptr = no reset.
using UsbHostResetHook = void (*)();
void set_usb_host_reset_hook(UsbHostResetHook hook);

}  // namespace adb
