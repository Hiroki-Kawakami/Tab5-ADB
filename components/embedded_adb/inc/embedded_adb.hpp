// embedded_adb — ADB (Android Debug Bridge) *host-side* client library.
//
// Tab5 (ESP32-P4) plays the ADB host (like WebADB): it drives a USB-connected
// Android device. Only the host side of the protocol is implemented.
//
// Layering (see CLAUDE.md for the cross-target rationale):
//   adb_protocol   — pure wire format (amessage/apacket, checksum, codec)
//   adb_crypto     — RSA-2048 keygen / signing / Android pubkey (mbedTLS, shared)
//   adb_keystore   — RSA private key persistence in NVS (C API, shared)
//   transport      — USB/TCP byte pipes; USB is the device/simulator split
//   adb_pairing    — TLS/SPAKE2 six-digit wireless-debugging pairing
//   adb_connection — CNXN handshake + AUTH state machine, packet dispatch
//   adb_stream     — A_OPEN/OKAY/WRTE/CLSE stream multiplexing
//   adb_client     — high-level API (connect, shell, ...)
//
// This umbrella header re-exports the public surface; each phase adds to it.
#pragma once

#include "adb_protocol.hpp"
#include "adb_crypto.hpp"
#include "adb_keystore.hpp"
#include "adb_pairing.hpp"
#include "adb_transport.hpp"
#include "adb_stream.hpp"
#include "adb_connection.hpp"
