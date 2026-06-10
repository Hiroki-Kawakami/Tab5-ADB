#include "screencap_preview.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adb_app.hpp"
#include "adb_client.hpp"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_ppa_pipeline.h"

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
                                 uint16_t* out, int dst_w, int dst_h,
                                 int* src_w = nullptr, int* src_h = nullptr) {
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
    if (src_w) *src_w = W;
    if (src_h) *src_h = H;

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
    img_buf_[0] = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    img_buf_[1] = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    if (!img_buf_[0] || !img_buf_[1]) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B x2)", buf_size_);
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.header.w = dst_w_;
    dsc_.header.h = dst_h_;
    dsc_.data = img_buf_[0];  // front; write_idx_ starts at 1 (the back)
    dsc_.data_size = buf_size_;
}

ScreencapPreview::~ScreencapPreview() {
    stop();  // joins the decode task before the buffers it writes are freed
    if (work_sem_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(work_sem_));
    if (decode_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_));
    heap_caps_free(img_buf_[0]);
    heap_caps_free(img_buf_[1]);
}

void ScreencapPreview::start() {
    stopped_.store(false);
    decode_stop_.store(false);
    if (!decode_task_) {
        work_sem_ = xSemaphoreCreateBinary();
        decode_done_ = xSemaphoreCreateBinary();
        TaskHandle_t t = nullptr;
        // Core 1, priority 3 (below the prio-5 adb reader / LVGL) so the heavy PNG
        // inflate+downscale never preempts them — this is a rough, low-rate preview.
        xTaskCreatePinnedToCore(&ScreencapPreview::decode_trampoline, "scap_decode",
                                8192, this, 3, &t, 1);
        decode_task_ = t;
    }
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
    if (decode_task_) {
        decode_stop_.store(true);
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));        // wake to exit
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;  // sems freed in the dtor (a late on_stream_close may
                                 // still give work_sem_ while the weak listener is locked)
    }
    // The decode task (sole owner of the pipeline) is now joined, so it's safe to
    // release the JPEG HW engine / PPA client / ring buffers here.
    if (jpeg_pipe_) {
        jpeg_ppa_pipeline_del(static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_));
        jpeg_pipe_ = nullptr;
        jpeg_pipe_max_ = 0;
    }
}

void ScreencapPreview::capture_once() {
    if (stopped_.load() || !img_buf_[0] || !img_buf_[1]) return;
    capturing_ = true;
    auto* client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online) {
        capturing_ = false;
        schedule_next();  // retry after the interval
        return;
    }
    png_.clear();
    t_capture_us_ = esp_timer_get_time();
    // exec: (not shell:) — binary-safe, no PTY CR/LF translation of the image bytes.
    // -j (JPEG, much smaller) on capable devices; -p (PNG) otherwise.
    const char* service = use_jpeg_.load() ? "exec:screencap -j" : "exec:screencap -p";
    stream_ = client->open_stream(
        service, std::weak_ptr<adb::StreamListener>(shared_from_this()));
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

void ScreencapPreview::present(int idx, bool ok) {
    if (stopped_.load() || !ok) return;  // failed decode: keep showing the last frame
    // Flip dsc_ to the buffer the decode task just filled (no copy); the next decode
    // then targets the other buffer (the one LVGL is done showing).
    dsc_.data = img_buf_[idx];
    write_idx_.store(idx ^ 1);
    lv_image_set_src(image_, &dsc_);
    lv_obj_invalidate(image_);

    // Per-stage timing (µs->ms) for tuning. xfer = phone screencap + USB transfer;
    // wait = decode task scheduling latency (low prio, Core 1); decode = inflate +
    // downscale; total = capture open -> shown.
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "preview %s %dx%d->%dx%d %ukB | xfer %lldms wait %lldms decode %lldms total %lldms",
             last_fmt_, src_w_, src_h_, dst_w_, dst_h_, (unsigned)(png_bytes_ / 1024),
             (long long)((t_recv_us_ - t_capture_us_) / 1000),
             (long long)((t_dec_start_us_ - t_recv_us_) / 1000),
             (long long)((t_dec_end_us_ - t_dec_start_us_) / 1000),
             (long long)((now - t_capture_us_) / 1000));
}

void ScreencapPreview::on_stream_data(adb::Stream*, const uint8_t* data, size_t len) {
    png_.insert(png_.end(), data, data + len);
}

void ScreencapPreview::on_stream_close(adb::Stream*, adb::Error) {
    // Reader thread (high priority): just hand the captured PNG to the decode task
    // and return — decode/downscale must not run here or it blocks the reader (and
    // thus LVGL / other adb streams). The pipeline is serial (next capture only
    // arms 2 s after present), so png_ stays stable while the task reads it.
    t_recv_us_ = esp_timer_get_time();
    png_bytes_ = png_.size();
    if (work_sem_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));
}

bool ScreencapPreview::decode_jpeg(const uint8_t* data, size_t len, int idx) {
    jpeg_decode_picture_info_t pi;
    if (jpeg_decoder_get_info(data, len, &pi) != ESP_OK) return false;

    // Lazy pipeline, sized to cover both orientations of this device (max(W,H) on
    // both axes, MCU-aligned) so a portrait<->landscape rotation needs no rebuild.
    uint32_t maxdim = (std::max(pi.width, pi.height) + 15u) & ~15u;
    if (!jpeg_pipe_ || jpeg_pipe_max_ < maxdim) {
        if (jpeg_pipe_) jpeg_ppa_pipeline_del(static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_));
        jpeg_pipe_ = nullptr;
        jpeg_ppa_pipeline_cfg_t cfg = {};
        cfg.max_pic_w = maxdim;
        cfg.max_pic_h = maxdim;
        cfg.strip_h_hint = 16;
        cfg.ring_count = 4;
        cfg.strip_color_mode = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // R-high RGB565 for LVGL
        cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        cfg.yuv_full_range = true;                       // JFIF/UltraHDR base is full-range
        cfg.strip_alloc_caps = MALLOC_CAP_SPIRAM;        // PSRAM ring; rate isn't critical
        cfg.worker_core = 1;
        cfg.worker_priority = 3;                         // low, like our decode task
        jpeg_ppa_pipeline_handle_t p = nullptr;
        if (jpeg_ppa_pipeline_new(&cfg, &p) != ESP_OK) {
            ESP_LOGW(TAG, "jpeg pipeline new failed (%ux%u)", (unsigned)pi.width,
                     (unsigned)pi.height);
            return false;
        }
        jpeg_pipe_ = p;
        jpeg_pipe_max_ = maxdim;
    }

    // Aspect-fit; quantize scale down to PPA's 1/16 step so the rendered extent is
    // exact and fits the preview box, with black letterbox margins.
    float s = std::min((float)dst_w_ / pi.width, (float)dst_h_ / pi.height);
    float q = std::floor(s * 16.0f) / 16.0f;
    if (q < 1.0f / 16) q = 1.0f / 16;
    uint32_t ew = (uint32_t)(pi.width * q), eh = (uint32_t)(pi.height * q);

    memset(img_buf_[idx], 0, buf_size_);  // letterbox

    jpeg_ppa_transform_t t = {};
    t.rotation = PPA_SRM_ROTATION_ANGLE_0;
    t.scale_x = t.scale_y = q;
    t.out_offset_x = ew < (uint32_t)dst_w_ ? ((uint32_t)dst_w_ - ew) / 2 : 0;
    t.out_offset_y = eh < (uint32_t)dst_h_ ? ((uint32_t)dst_h_ - eh) / 2 : 0;

    jpeg_ppa_output_t out = {};
    out.buffer = img_buf_[idx];
    out.buffer_size = buf_size_;
    out.pic_w = dst_w_;
    out.pic_h = dst_h_;
    out.color_mode = PPA_SRM_COLOR_MODE_RGB565;

    esp_err_t e = jpeg_ppa_pipeline_process(
        static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_), data, len, &out, &t, nullptr);
    if (e != ESP_OK) { ESP_LOGW(TAG, "jpeg pipeline process: %d", (int)e); return false; }
    src_w_ = pi.width;
    src_h_ = pi.height;
    return true;
}

void ScreencapPreview::decode_trampoline(void* arg) {
    static_cast<ScreencapPreview*>(arg)->decode_loop();
}

void ScreencapPreview::decode_loop() {
    for (;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(work_sem_), portMAX_DELAY);
        if (decode_stop_.load()) break;

        t_dec_start_us_ = esp_timer_get_time();
        int idx = write_idx_.load();  // the back buffer (front = idx ^ 1, shown by LVGL)
        const uint8_t* d = png_.data();
        size_t n = png_.size();
        bool is_jpeg = n >= 2 && d[0] == 0xFF && d[1] == 0xD8;
        bool is_png = n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
        bool ok;
        if (is_jpeg) {
            last_fmt_ = "jpeg";
            ok = decode_jpeg(d, n, idx);
        } else {
            last_fmt_ = "png";
            ok = is_png && decode_png_downscale_rgb565(
                              d, n, reinterpret_cast<uint16_t*>(img_buf_[idx]),
                              dst_w_, dst_h_, &src_w_, &src_h_);
        }
        // We asked for `-j` but got something else (or nothing) → the device lacks
        // it; drop to PNG for the rest of the session.
        if (use_jpeg_.load() && !is_jpeg) {
            use_jpeg_.store(false);
            ESP_LOGW(TAG, "screencap -j unsupported here; falling back to PNG");
        }
        t_dec_end_us_ = esp_timer_get_time();
        png_.clear();
        if (!ok) ESP_LOGW(TAG, "decode failed");

        lv_async_call([self = shared_from_this(), idx, ok]() {
            self->capturing_ = false;
            self->stream_.reset();
            self->present(idx, ok);
            self->schedule_next();
        });
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}
