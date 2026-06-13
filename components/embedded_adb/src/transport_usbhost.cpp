// Device USB transport: esp-idf usb_host. The host stack is asynchronous
// (transfers complete via callbacks pumped by usb_host_client_handle_events);
// we wrap it as the synchronous Transport the connection layer expects, using a
// binary semaphore per direction. Mirrors transport_libusb.cpp.
//
// Two background tasks pump the stack: a "lib" task (usb_host_lib_handle_events)
// and a "client" task (usb_host_client_handle_events, which runs our transfer
// callbacks). bulk reads/writes submit a transfer then block on the matching
// semaphore.
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <usb/usb_host.h>

#include <cstring>

#include "esp_log.h"

#include "adb_transport.hpp"

namespace adb {

namespace {

const char* TAG = "adb/usbhost";

size_t round_up(size_t v, size_t a) { return a ? (v + a - 1) / a * a : v; }

UsbHostResetHook g_usb_reset_hook = nullptr;

// Transfer completion callback (runs on the client-events task). The context is
// the semaphore to release; the caller reads transfer->status afterwards.
void xfer_done(usb_transfer_t* t) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
}

class UsbHostTransport : public Transport {
public:
    bool open();   // install host, find + claim the ADB interface
    bool write_packet(const Packet& p) override;
    IoResult read_packet(Packet& p) override;
    void close() override;
    ~UsbHostTransport() override { close(); }

private:
    bool submit_in(usb_transfer_t* t, size_t bytes);
    bool write_one(const uint8_t* data, size_t len);
    bool find_and_claim(uint8_t dev_addr);

    static void lib_task(void* arg);
    static void client_task(void* arg);
    static void client_event(const usb_host_client_event_msg_t* msg, void* arg);

    usb_host_client_handle_t client_ = nullptr;
    usb_device_handle_t dev_ = nullptr;
    int iface_ = -1;
    uint8_t ep_in_ = 0, ep_out_ = 0;
    uint16_t mps_in_ = 64, mps_out_ = 64;

    TaskHandle_t lib_task_ = nullptr;
    TaskHandle_t client_task_ = nullptr;
    volatile bool running_ = false;
    volatile bool device_gone_ = false;

    SemaphoreHandle_t in_sem_ = nullptr;
    SemaphoreHandle_t out_sem_ = nullptr;
    usb_transfer_t* hdr_xfer_ = nullptr;   // persistent header IN transfer
    bool hdr_in_flight_ = false;
    usb_transfer_t* data_xfer_ = nullptr;  // persistent payload IN transfer, grown lazily
    size_t data_cap_ = 0;                  // its allocated capacity (MPS-rounded)
};

void UsbHostTransport::lib_task(void* arg) {
    auto* self = static_cast<UsbHostTransport*>(arg);
    while (self->running_) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
    vTaskDelete(nullptr);
}

void UsbHostTransport::client_task(void* arg) {
    auto* self = static_cast<UsbHostTransport*>(arg);
    while (self->running_) {
        usb_host_client_handle_events(self->client_, portMAX_DELAY);
    }
    vTaskDelete(nullptr);
}

void UsbHostTransport::client_event(const usb_host_client_event_msg_t* msg, void* arg) {
    auto* self = static_cast<UsbHostTransport*>(arg);
    if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "device gone");
        self->device_gone_ = true;
    }
}

bool UsbHostTransport::open() {
    in_sem_ = xSemaphoreCreateBinary();
    out_sem_ = xSemaphoreCreateBinary();
    if (!in_sem_ || !out_sem_) return false;

    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    // The default (BALANCED) FIFO bias gives the non-periodic TX FIFO only
    // dfifo_depth/16 lines → a bulk OUT MPS limit of 256, which rejects an
    // Android device's 512-byte high-speed bulk endpoints (interface_claim
    // returns ESP_ERR_NOT_SUPPORTED). ADB is bulk-only, and for the mirror the
    // hot direction is **bulk IN** (the screen stream), so we bias the FIFO
    // toward RX: rx=512 lines (2 KiB, 4×512-byte packets) so the DWC2 RX FIFO can
    // absorb DMA-drain latency spikes under sustained high-rate IN without
    // overflowing, and nptx=192 lines (OUT MPS limit ~768, ample for the 512-byte
    // bulk OUT — Tab5→phone is light: OKAY acks + control). The periodic TX FIFO
    // is unused. (If usb_host_install fails here it's a FIFO-budget overflow — the
    // P4 DWC2 DFIFO is finite; dial rx back. This bias is the fix for the
    // high-throughput bulk-IN corruption — undersized RX, not transfer size: the
    // usb_host_uvc reference does clean 10 KiB IN transfers on this same chip.)
    host_cfg.fifo_settings_custom.rx_fifo_lines = 512;
    host_cfg.fifo_settings_custom.nptx_fifo_lines = 192;
    host_cfg.fifo_settings_custom.ptx_fifo_lines = 0;
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return false;
    }

    running_ = true;
    xTaskCreate(lib_task, "adb_usb_lib", 4096, this, 5, &lib_task_);

    usb_host_client_config_t client_cfg = {};
    client_cfg.is_synchronous = false;
    client_cfg.max_num_event_msg = 5;
    client_cfg.async.client_event_callback = client_event;
    client_cfg.async.callback_arg = this;
    err = usb_host_client_register(&client_cfg, &client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(err));
        return false;
    }
    xTaskCreate(client_task, "adb_usb_cli", 4096, this, 5, &client_task_);

    // Reset the host port *now*, after the host stack is up. open() runs on
    // demand, so a device may have been attached (D+ pulled up) long before
    // usb_host_install() powered the root port — the DWC2 then sees no
    // idle→connected transition and never enumerates, so the poll below would spin
    // forever (toggling the controller's internal root-port-power does NOT fix
    // this: the device's VBUS doesn't physically cycle, so its line state never
    // changes). The caller-supplied reset makes the device re-attach into the
    // already-ready host = the connect edge the controller needs.
    if (g_usb_reset_hook) g_usb_reset_hook();

    // Poll for a connected device exposing an ADB interface (~20s).
    ESP_LOGI(TAG, "usb_host installed; waiting for a device on the host port...");
    int last_num = -1;
    for (int tries = 0; tries < 200 && dev_ == nullptr; ++tries) {
        uint8_t addrs[8];
        int num = 0;
        if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &num) == ESP_OK) {
            if (num != last_num) {
                ESP_LOGI(TAG, "enumerated devices: %d", num);
                last_num = num;
            }
            for (int i = 0; i < num && dev_ == nullptr; ++i) {
                if (find_and_claim(addrs[i])) break;
            }
        }
        if (dev_ == nullptr) vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (dev_ == nullptr) {
        ESP_LOGE(TAG, "no ADB device found");
        return false;
    }

    // Persistent header IN transfer, sized to one IN max-packet.
    if (usb_host_transfer_alloc(round_up(sizeof(MessageHeader), mps_in_), 0, &hdr_xfer_) != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "ADB interface claimed (in=0x%02x out=0x%02x mps=%u)", ep_in_, ep_out_, mps_in_);
    return true;
}

bool UsbHostTransport::find_and_claim(uint8_t dev_addr) {
    usb_device_handle_t dev = nullptr;
    if (usb_host_device_open(client_, dev_addr, &dev) != ESP_OK) return false;

    const usb_device_desc_t* dd = nullptr;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK) {
        ESP_LOGI(TAG, "device addr=%u vid=%04x pid=%04x class=%u", dev_addr,
                 dd->idVendor, dd->idProduct, dd->bDeviceClass);
    }

    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        usb_host_device_close(client_, dev);
        return false;
    }

    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        int off = 0;
        const usb_intf_desc_t* intf = usb_parse_interface_descriptor(cfg, i, 0, &off);
        if (!intf) continue;
        ESP_LOGI(TAG, "  iface %d: class=%u sub=%u proto=%u eps=%u", i,
                 intf->bInterfaceClass, intf->bInterfaceSubClass,
                 intf->bInterfaceProtocol, intf->bNumEndpoints);
        if (intf->bInterfaceClass != kAdbClass ||
            intf->bInterfaceSubClass != kAdbSubclass ||
            intf->bInterfaceProtocol != kAdbProtocol) {
            continue;
        }
        uint8_t ep_in = 0, ep_out = 0;
        uint16_t mps_in = 64, mps_out = 64;
        for (int e = 0; e < intf->bNumEndpoints; ++e) {
            int epoff = 0;
            const usb_ep_desc_t* ep =
                usb_parse_endpoint_descriptor_by_index(intf, e, cfg->wTotalLength, &epoff);
            if (!ep) continue;
            ESP_LOGI(TAG, "    ep 0x%02x attr=0x%02x mps=%u", ep->bEndpointAddress,
                     ep->bmAttributes, USB_EP_DESC_GET_MPS(ep));
            if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) continue;
            if (USB_EP_DESC_GET_EP_DIR(ep)) {
                ep_in = ep->bEndpointAddress;
                mps_in = USB_EP_DESC_GET_MPS(ep);
            } else {
                ep_out = ep->bEndpointAddress;
                mps_out = USB_EP_DESC_GET_MPS(ep);
            }
        }
        if (!ep_in || !ep_out) {
            ESP_LOGW(TAG, "  ADB iface but eps not found (in=0x%02x out=0x%02x)", ep_in, ep_out);
            continue;
        }
        esp_err_t cerr = usb_host_interface_claim(client_, dev, intf->bInterfaceNumber, 0);
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "  interface_claim failed: %s", esp_err_to_name(cerr));
            continue;
        }
        dev_ = dev;
        iface_ = intf->bInterfaceNumber;
        ep_in_ = ep_in;
        ep_out_ = ep_out;
        mps_in_ = mps_in ? mps_in : 64;
        mps_out_ = mps_out ? mps_out : 64;
        return true;
    }

    usb_host_device_close(client_, dev);
    return false;
}

bool UsbHostTransport::submit_in(usb_transfer_t* t, size_t bytes) {
    t->device_handle = dev_;
    t->bEndpointAddress = ep_in_;
    t->callback = xfer_done;
    t->context = in_sem_;
    t->num_bytes = static_cast<int>(round_up(bytes, mps_in_));  // IN must be MPS-multiple
    return usb_host_transfer_submit(t) == ESP_OK;
}

IoResult UsbHostTransport::read_packet(Packet& p) {
    if (device_gone_) return IoResult::Error;

    // Header — persistent transfer so a 1s wait can yield Timeout without
    // cancelling an in-flight transfer (it stays pending across calls).
    if (!hdr_in_flight_) {
        if (!submit_in(hdr_xfer_, sizeof(MessageHeader))) return IoResult::Error;
        hdr_in_flight_ = true;
    }
    if (xSemaphoreTake(in_sem_, pdMS_TO_TICKS(1000)) == pdFALSE) {
        return IoResult::Timeout;  // still pending; retry next call
    }
    hdr_in_flight_ = false;
    if (hdr_xfer_->status != USB_TRANSFER_STATUS_COMPLETED ||
        hdr_xfer_->actual_num_bytes < static_cast<int>(sizeof(MessageHeader))) {
        return IoResult::Error;
    }
    std::memcpy(&p.header, hdr_xfer_->data_buffer, sizeof(MessageHeader));

    uint32_t len = p.header.data_length;
    p.payload.resize(len);
    if (len == 0) return IoResult::Ok;

    // Read the whole payload in ONE transfer. adbd writes each A_WRTE payload with
    // a single usb_write(), so it arrives as one bulk-IN byte stream (terminated by
    // a short packet when len isn't an MPS multiple) — no need to split it. The
    // earlier ≤4 KiB chunking was a band-aid for "large IN corrupts", but the real
    // cause is an undersized RX FIFO starving under high-rate IN (see open()): the
    // usb_host_uvc reference streams clean 10 KiB IN transfers on this same chip.
    // The payload transfer is persistent and grown lazily (like hdr_xfer_) so the
    // hot path does no per-packet allocation.
    size_t need = round_up(len, mps_in_);
    if (need > data_cap_) {
        if (data_xfer_) { usb_host_transfer_free(data_xfer_); data_xfer_ = nullptr; }
        if (usb_host_transfer_alloc(need, 0, &data_xfer_) != ESP_OK) {
            data_cap_ = 0;
            return IoResult::Error;
        }
        data_cap_ = need;
    }
    bool ok = submit_in(data_xfer_, len) &&
              xSemaphoreTake(in_sem_, portMAX_DELAY) == pdTRUE &&
              data_xfer_->status == USB_TRANSFER_STATUS_COMPLETED &&
              data_xfer_->actual_num_bytes >= static_cast<int>(len);
    if (!ok) return IoResult::Error;
    std::memcpy(p.payload.data(), data_xfer_->data_buffer, len);
    return IoResult::Ok;
}

bool UsbHostTransport::write_one(const uint8_t* data, size_t len) {
    usb_transfer_t* xf = nullptr;
    if (usb_host_transfer_alloc(len ? len : 1, 0, &xf) != ESP_OK) return false;
    std::memcpy(xf->data_buffer, data, len);
    xf->device_handle = dev_;
    xf->bEndpointAddress = ep_out_;
    xf->callback = xfer_done;
    xf->context = out_sem_;
    xf->num_bytes = static_cast<int>(len);
    xf->flags = USB_TRANSFER_FLAG_ZERO_PACK;  // add ZLP if len is an MPS multiple
    bool ok = usb_host_transfer_submit(xf) == ESP_OK &&
              xSemaphoreTake(out_sem_, portMAX_DELAY) == pdTRUE &&
              xf->status == USB_TRANSFER_STATUS_COMPLETED;
    usb_host_transfer_free(xf);
    return ok;
}

bool UsbHostTransport::write_packet(const Packet& p) {
    if (device_gone_) return false;
    if (!write_one(reinterpret_cast<const uint8_t*>(&p.header), sizeof(p.header))) {
        return false;
    }
    if (!p.payload.empty()) return write_one(p.payload.data(), p.payload.size());
    return true;
}

void UsbHostTransport::close() {
    if (!running_) return;
    running_ = false;

    // Make sure no IN transfer (header or payload) is in flight before freeing
    // them. Both use ep_in_, so one endpoint flush covers either; drain whatever
    // completion it produces so the freed buffer can't be written after free.
    if (dev_) {
        usb_host_endpoint_flush(dev_, ep_in_);
        xSemaphoreTake(in_sem_, pdMS_TO_TICKS(500));
        hdr_in_flight_ = false;
    }
    if (hdr_xfer_) {
        usb_host_transfer_free(hdr_xfer_);
        hdr_xfer_ = nullptr;
    }
    if (data_xfer_) {
        usb_host_transfer_free(data_xfer_);
        data_xfer_ = nullptr;
        data_cap_ = 0;
    }
    if (dev_) {
        if (iface_ >= 0) usb_host_interface_release(client_, dev_, iface_);
        usb_host_device_close(client_, dev_);
        dev_ = nullptr;
    }
    // Unblock the event tasks so they can exit.
    if (client_) usb_host_client_unblock(client_);
    usb_host_lib_unblock();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (client_) {
        usb_host_client_deregister(client_);
        client_ = nullptr;
    }
    usb_host_uninstall();
    if (in_sem_) vSemaphoreDelete(in_sem_);
    if (out_sem_) vSemaphoreDelete(out_sem_);
    in_sem_ = out_sem_ = nullptr;
}

}  // namespace

std::unique_ptr<Transport> open_usb_transport() {
    auto t = std::make_unique<UsbHostTransport>();
    if (!t->open()) return nullptr;
    return t;
}

void set_usb_host_reset_hook(UsbHostResetHook hook) { g_usb_reset_hook = hook; }

}  // namespace adb
