// embedded_adb — ADB (Android Debug Bridge) *host-side* client library.
//
// Tab5 (ESP32-P4) plays the ADB host (like WebADB): it drives a USB-connected
// Android device. Only the host side of the protocol is implemented.
//
// Layering (see CLAUDE.md for the cross-target rationale):
//   adb_protocol   — pure wire format (amessage/apacket, checksum, codec)
//   adb_crypto     — RSA-2048 keygen / signing / Android pubkey (mbedTLS, shared)
//   adb_keystore   — RSA private key persistence in NVS (C API, shared)
//   transport      — USB bulk transfer; the ONLY device/simulator split
//                    (esp-idf usb_host vs libusb), selected in CMakeLists.txt
//   adb_connection — CNXN handshake + AUTH state machine, packet dispatch
//   adb_stream     — A_OPEN/OKAY/WRTE/CLSE stream multiplexing
//   adb_client     — high-level API (connect, shell, ...)
//
// This umbrella header re-exports the public surface; each phase adds to it.
#pragma once

#include "adb_protocol.hpp"
