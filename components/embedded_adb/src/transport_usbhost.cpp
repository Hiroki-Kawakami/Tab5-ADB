// Device USB transport: esp-idf usb_host. NOT YET IMPLEMENTED — placeholder so
// the device build links and the per-target split is in place. The real backend
// (USB host install, ADB interface enumeration, bulk IN/OUT transfer) lands in
// P6 and mirrors transport_libusb.cpp behind the same Transport interface.
#include "esp_log.h"

#include "adb_transport.hpp"

namespace adb {

std::unique_ptr<Transport> open_usb_transport() {
    ESP_LOGW("adb/usbhost", "usb_host transport not implemented yet (P6)");
    return nullptr;
}

}  // namespace adb
