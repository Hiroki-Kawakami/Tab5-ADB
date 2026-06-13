// Simulator USB transport: libusb. Talks to a real Android device plugged into
// the host PC, so the protocol/auth/stream layers can be developed on the
// desktop. Run `adb kill-server` first so the host's adb-server doesn't hold the
// interface.
#include <libusb-1.0/libusb.h>

#include <cstdio>
#include <cstring>

#include "adb_transport.hpp"

namespace adb {

namespace {

constexpr unsigned kReadTimeoutMs = 1000;
constexpr unsigned kWriteTimeoutMs = 5000;

void log(const char* msg) { std::fprintf(stderr, "[adb/libusb] %s\n", msg); }

class LibusbTransport : public Transport {
public:
    LibusbTransport(libusb_context* ctx, libusb_device_handle* h, int iface,
                    uint8_t ep_in, uint8_t ep_out)
        : ctx_(ctx), handle_(h), iface_(iface), ep_in_(ep_in), ep_out_(ep_out) {}

    ~LibusbTransport() override { close(); }

    bool write_packet(const Packet& p) override {
        if (!bulk_out(reinterpret_cast<const uint8_t*>(&p.header), sizeof(p.header))) {
            return false;
        }
        if (!p.payload.empty()) {
            return bulk_out(p.payload.data(), p.payload.size());
        }
        return true;
    }

    IoResult read_packet(Packet& p) override {
        // Header first.
        int got = 0;
        IoResult r = bulk_in(reinterpret_cast<uint8_t*>(&p.header), sizeof(p.header),
                             &got, kReadTimeoutMs);
        if (r != IoResult::Ok) return r;
        if (got != sizeof(p.header)) return IoResult::Error;

        // Then the payload, if any.
        uint32_t len = p.header.data_length;
        p.payload.resize(len);
        if (len > 0) {
            int pgot = 0;
            // Use a generous timeout: the header promised this payload is coming.
            r = bulk_in(p.payload.data(), len, &pgot, kWriteTimeoutMs);
            if (r != IoResult::Ok) return r == IoResult::Timeout ? IoResult::Error : r;
            if (static_cast<uint32_t>(pgot) != len) return IoResult::Error;
        }
        return IoResult::Ok;
    }

    void close() override {
        if (handle_) {
            libusb_release_interface(handle_, iface_);
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (ctx_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
    }

private:
    bool bulk_out(const uint8_t* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            int transferred = 0;
            int rc = libusb_bulk_transfer(handle_, ep_out_,
                                          const_cast<uint8_t*>(data) + off,
                                          static_cast<int>(len - off), &transferred,
                                          kWriteTimeoutMs);
            if (rc != 0) {
                log(libusb_error_name(rc));
                return false;
            }
            off += transferred;
        }
        return true;
    }

    IoResult bulk_in(uint8_t* data, size_t len, int* out_total, unsigned timeout_ms) {
        size_t off = 0;
        while (off < len) {
            int transferred = 0;
            int rc = libusb_bulk_transfer(handle_, ep_in_, data + off,
                                          static_cast<int>(len - off), &transferred,
                                          timeout_ms);
            if (rc == LIBUSB_ERROR_TIMEOUT) {
                // A timeout on the very first chunk means an idle bus; mid-packet
                // it means the device stalled — treat both as Timeout and let the
                // caller decide (read_packet upgrades mid-payload to Error).
                *out_total = static_cast<int>(off + transferred);
                return off == 0 ? IoResult::Timeout : IoResult::Error;
            }
            if (rc != 0) {
                log(libusb_error_name(rc));
                return IoResult::Error;
            }
            off += transferred;
        }
        *out_total = static_cast<int>(off);
        return IoResult::Ok;
    }

    libusb_context* ctx_;
    libusb_device_handle* handle_;
    int iface_;
    uint8_t ep_in_;
    uint8_t ep_out_;
};

// Scan a device's active config for an ADB interface; on match, fill iface/eps.
bool find_adb_interface(libusb_device* dev, int* out_iface, uint8_t* out_ep_in,
                        uint8_t* out_ep_out) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return false;

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const libusb_interface& intf = cfg->interface[i];
        for (int a = 0; a < intf.num_altsetting && !found; ++a) {
            const libusb_interface_descriptor& d = intf.altsetting[a];
            if (d.bInterfaceClass != kAdbClass ||
                d.bInterfaceSubClass != kAdbSubclass ||
                d.bInterfaceProtocol != kAdbProtocol) {
                continue;
            }
            uint8_t ep_in = 0, ep_out = 0;
            for (uint8_t e = 0; e < d.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = d.endpoint[e];
                if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    ep_in = ep.bEndpointAddress;
                } else {
                    ep_out = ep.bEndpointAddress;
                }
            }
            if (ep_in && ep_out) {
                *out_iface = d.bInterfaceNumber;
                *out_ep_in = ep_in;
                *out_ep_out = ep_out;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

}  // namespace

std::unique_ptr<Transport> open_usb_transport() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        log("libusb_init failed");
        return nullptr;
    }

    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    std::unique_ptr<Transport> result;

    for (ssize_t i = 0; i < n; ++i) {
        int iface = 0;
        uint8_t ep_in = 0, ep_out = 0;
        if (!find_adb_interface(list[i], &iface, &ep_in, &ep_out)) continue;

        libusb_device_handle* h = nullptr;
        if (libusb_open(list[i], &h) != 0) {
            log("found ADB interface but libusb_open failed (permissions? adb-server holding it?)");
            continue;
        }
        libusb_set_auto_detach_kernel_driver(h, 1);
        if (libusb_claim_interface(h, iface) != 0) {
            log("claim_interface failed (run `adb kill-server`)");
            libusb_close(h);
            continue;
        }
        log("ADB device opened");
        result = std::make_unique<LibusbTransport>(ctx, h, iface, ep_in, ep_out);
        break;
    }

    if (list) libusb_free_device_list(list, 1);
    if (!result) {
        log("no ADB device found");
        libusb_exit(ctx);
    }
    return result;
}

// No host port to reset on the simulator (libusb talks to a device the host OS
// already enumerated); the hook is a device-only concern.
void set_usb_host_reset_hook(UsbHostResetHook) {}

}  // namespace adb
