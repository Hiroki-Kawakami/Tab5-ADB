// adb — app-facing, object-oriented ADB API over embedded_adb.
//
// embedded_adb is the protocol engine (wire format, CNXN/AUTH, raw streams) and
// stays thread-model agnostic; this component owns the connection lifecycle
// (RSA key, USB transport, the reader task) and exposes typed services. See
// README.md for the design (and docs/<surface>.md for per-surface specs).
//
// Umbrella header; each slice adds to it.
#pragma once

#include "adb_client.hpp"
#include "adb_error.hpp"
#include "adb_raw_stream.hpp"
#include "adb_shell.hpp"
#include "adb_sync.hpp"
