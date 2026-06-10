#include "screencap_preview.hpp"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "adb_app.hpp"
#include "adb_client.hpp"
#include "esp_log.h"

static const char* TAG = "screencap_preview";

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Reconstruct one filtered scanline (`in`, `stride` bytes, filter type `ft`) into
// `out`, using the previous reconstructed row `prev`. bpp = bytes/pixel.
bool unfilter(uint8_t ft, const uint8_t* in, const uint8_t* prev, uint8_t* out,
              int stride, int bpp) {
    for (int i = 0; i < stride; i++) {
        int a = i >= bpp ? out[i - bpp] : 0;   // left
        int b = prev[i];                        // up
        int c = i >= bpp ? prev[i - bpp] : 0;   // up-left
        int x = in[i];
        int v;
        switch (ft) {
            case 0: v = x; break;
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + ((a + b) >> 1); break;
            case 4: v = x + paeth(a, b, c); break;
            default: return false;
        }
        out[i] = uint8_t(v);
    }
    return true;
}

// Decode a PNG and downscale it on the fly into a dst_w*dst_h RGB565 buffer
// (`out`, dst_w*dst_h uint16). The full-resolution image is never materialized:
// PNG row filters only reference the *previous* reconstructed row, so we inflate
// row by row keeping just two scanlines and emit the kept (nearest-neighbour)
// rows straight into `out`. Constraints (all met by Android `screencap -p`):
// 8-bit, colour type RGB(2)/RGBA(6), non-interlaced. Returns false otherwise.
bool decode_png_downscale_rgb565(const uint8_t* png, size_t len,
                                 uint16_t* out, int dst_w, int dst_h) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (len < 8 + 25 || memcmp(png, SIG, 8) != 0) return false;

    int W = 0, H = 0, bpp = 0;
    std::vector<uint8_t, PsramAllocator<uint8_t>> idat;  // concatenated IDAT (compressed)

    size_t pos = 8;
    bool seen_ihdr = false;
    while (pos + 8 <= len) {
        uint32_t clen = be32(png + pos);
        const uint8_t* type = png + pos + 4;
        const uint8_t* data = png + pos + 8;
        if (pos + 12 + (size_t)clen > len) break;  // truncated

        if (memcmp(type, "IHDR", 4) == 0) {
            W = int(be32(data));
            H = int(be32(data + 4));
            int bit_depth = data[8], color_type = data[9], interlace = data[12];
            if (W <= 0 || H <= 0 || W > 10000 || H > 10000) return false;
            if (bit_depth != 8 || interlace != 0) return false;
            if (color_type == 2) bpp = 3;       // RGB
            else if (color_type == 6) bpp = 4;  // RGBA
            else return false;
            seen_ihdr = true;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + clen);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;  // length + type + data + CRC
    }
    if (!seen_ihdr || idat.empty()) return false;

    const int stride = W * bpp;
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = idat.data();
    zs.avail_in = uInt(idat.size());

    std::vector<uint8_t> rowbuf(stride + 1);          // filter byte + pixels
    std::vector<uint8_t> prev(stride, 0), cur(stride, 0);

    bool ok = true;
    int dy = 0;  // next dst row to fill (monotonic with src row)
    for (int y = 0; y < H && ok; y++) {
        // Inflate exactly stride+1 bytes (the next filtered scanline).
        zs.next_out = rowbuf.data();
        zs.avail_out = uInt(stride + 1);
        while (zs.avail_out > 0) {
            int r = inflate(&zs, Z_NO_FLUSH);
            if (r == Z_STREAM_END) break;
            if (r != Z_OK) { ok = false; break; }
            if (zs.avail_in == 0 && zs.avail_out > 0) { ok = false; break; }
        }
        if (!ok || zs.avail_out != 0) { ok = false; break; }

        if (!unfilter(rowbuf[0], rowbuf.data() + 1, prev.data(), cur.data(), stride, bpp)) {
            ok = false;
            break;
        }

        // Emit every dst row whose nearest source row is this y (centre sampling).
        while (dy < dst_h && ((2 * dy + 1) * (long long)H) / (2 * dst_h) == y) {
            uint16_t* dst = out + (size_t)dy * dst_w;
            for (int dx = 0; dx < dst_w; dx++) {
                int sx = int(((2 * dx + 1) * (long long)W) / (2 * dst_w));
                const uint8_t* p = cur.data() + (size_t)sx * bpp;
                dst[dx] = uint16_t(((p[0] & 0xf8) << 8) | ((p[1] & 0xfc) << 3) | (p[2] >> 3));
            }
            dy++;
        }
        std::swap(prev, cur);
    }
    inflateEnd(&zs);
    return ok && dy == dst_h;
}

}  // namespace

std::shared_ptr<ScreencapPreview> ScreencapPreview::create(lv_obj_t* image, int dst_w, int dst_h) {
    return std::shared_ptr<ScreencapPreview>(new ScreencapPreview(image, dst_w, dst_h));
}

ScreencapPreview::ScreencapPreview(lv_obj_t* image, int dst_w, int dst_h)
    : image_(image), dst_w_(dst_w), dst_h_(dst_h) {
    buf_size_ = (size_t)dst_w_ * dst_h_ * 2;  // RGB565
    img_buf_ = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    decode_buf_ = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    if (!img_buf_ || !decode_buf_) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B x2)", buf_size_);
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.header.w = dst_w_;
    dsc_.header.h = dst_h_;
    dsc_.data = img_buf_;
    dsc_.data_size = buf_size_;
}

ScreencapPreview::~ScreencapPreview() {
    stop();
    heap_caps_free(img_buf_);
    heap_caps_free(decode_buf_);
}

void ScreencapPreview::start() {
    stopped_.store(false);
    capture_once();
}

void ScreencapPreview::stop() {
    stopped_.store(true);
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
}

void ScreencapPreview::capture_once() {
    if (stopped_.load() || !img_buf_ || !decode_buf_) return;
    capturing_ = true;
    auto* client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online) {
        capturing_ = false;
        schedule_next();  // retry after the interval
        return;
    }
    png_.clear();
    // exec: (not shell:) — binary-safe, no PTY CR/LF translation of the PNG bytes.
    stream_ = client->open_stream(
        "exec:screencap -p",
        std::weak_ptr<adb::StreamListener>(shared_from_this()));
    if (!stream_) {
        capturing_ = false;
        schedule_next();
    }
}

void ScreencapPreview::schedule_next() {
    if (stopped_.load() || timer_) return;
    timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ScreencapPreview*>(lv_timer_get_user_data(t));
            lv_timer_delete(t);
            self->timer_ = nullptr;
            self->capture_once();
        },
        interval_ms_, this);
}

void ScreencapPreview::present() {
    if (stopped_.load() || !have_frame_) return;
    memcpy(img_buf_, decode_buf_, buf_size_);
    have_frame_ = false;
    // RGB565 in-memory images are zero-copy: LVGL's cached decode references
    // dsc_.data directly, so overwriting the buffer + invalidate shows the fresh
    // pixels (no cache drop needed, unlike compressed/indexed formats).
    lv_image_set_src(image_, &dsc_);
    lv_obj_invalidate(image_);
}

void ScreencapPreview::on_stream_data(adb::Stream*, const uint8_t* data, size_t len) {
    png_.insert(png_.end(), data, data + len);
}

void ScreencapPreview::on_stream_close(adb::Stream*, adb::Error) {
    // Reader thread: decode + downscale here (off the LVGL thread), then marshal
    // the cheap present()/schedule to LVGL.
    bool ok = decode_png_downscale_rgb565(
        png_.data(), png_.size(),
        reinterpret_cast<uint16_t*>(decode_buf_), dst_w_, dst_h_);
    png_.clear();
    have_frame_ = ok;
    if (!ok) ESP_LOGW(TAG, "decode failed");

    lv_async_call([self = shared_from_this()]() {
        self->capturing_ = false;
        self->stream_.reset();
        self->present();
        self->schedule_next();
    });
}
