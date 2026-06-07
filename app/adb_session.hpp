// Provisional app-level ADB session: owns the live connection + its reader task
// and bridges the (worker-thread) connection callbacks to the LVGL thread. Lives
// in app/ rather than the library so the library stays thread-model-agnostic
// (the read loop is run on a FreeRTOS task here; the host unit tests use a
// std::thread). Same FreeRTOS API on both targets.
#pragma once

#include <functional>
#include <string>

#include "embedded_adb.hpp"

namespace app {

// Connect to a device in the background (loads/creates the RSA key, opens the
// USB transport, runs CNXN+AUTH). `on_result` is invoked on the LVGL thread:
// true once the connection reaches Online, false on failure.
void adb_connect_async(std::function<void(bool ok)> on_result);

// The live connection, valid after a successful connect (else nullptr).
adb::AdbConnection* adb_connection();

// The device banner captured at connect ("device::...key=val;..."). Empty if not
// connected.
const std::string& adb_banner();

}  // namespace app
