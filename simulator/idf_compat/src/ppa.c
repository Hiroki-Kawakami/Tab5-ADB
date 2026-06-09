// Host (simulator) implementation of the ESP-IDF PPA (Pixel-Processing
// Accelerator) driver API (driver/ppa.h). The device offloads
// scale/rotate/mirror, blend and fill to the P4 PPA HW; here the same operations
// run in plain C on the CPU so the simulator previews the equivalent output.
//
// Fidelity / limitations (the supported surface covers the RGB display formats):
//   - Color modes: ARGB8888, RGB888, RGB565 for SRM in/out, blend bg/fg/out and
//     fill out; blend foreground additionally supports A8 / A4. YUV420 / YUV444
//     are NOT implemented and return ESP_ERR_NOT_SUPPORTED (the panel and the
//     app's framebuffers are RGB565; add YUV here if a use case needs it).
//   - SRM scaling uses bilinear interpolation (anti-aliased), matching the HW
//     which interpolates rather than point-sampling; rotation is counter-clockwise,
//     matching the PPA_SRM_ROTATION_ANGLE_* convention.
//   - Blend is straight alpha-over compositing honouring the alpha-update modes;
//     color-keying (bg_ck_en / fg_ck_en) is NOT implemented (a warning is logged
//     once and plain alpha blending is performed).
//   - byte_swap is honoured for RGB565 / ARGB8888; rgb_swap swaps R<->B.
//   - All operations run synchronously regardless of PPA_TRANS_MODE_*; the
//     on_trans_done callback (if registered) fires inline before returning.
//   - data_burst_length / max_pending_trans_num / buffer alignment are accepted
//     and ignored (no DMA / cache on the host).

#include "driver/ppa.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ppa(sim)";

struct ppa_client_t {
    ppa_operation_t oper_type;
    ppa_event_callback_t on_trans_done;
};

/* ---- color format helpers ------------------------------------------------ */

typedef enum {
    FMT_UNSUPPORTED = 0,
    FMT_ARGB8888,
    FMT_RGB888,
    FMT_RGB565,
    FMT_A8,
    FMT_A4,
} pix_fmt_t;

// SRM / blend / fill color-mode enums all share the COLOR_TYPE_ID values, so one
// mapping serves every operation.
static pix_fmt_t fmt_of(uint32_t color_mode)
{
    switch (color_mode) {
    case (uint32_t)PPA_SRM_COLOR_MODE_ARGB8888: return FMT_ARGB8888;
    case (uint32_t)PPA_SRM_COLOR_MODE_RGB888:   return FMT_RGB888;
    case (uint32_t)PPA_SRM_COLOR_MODE_RGB565:   return FMT_RGB565;
    case (uint32_t)PPA_BLEND_COLOR_MODE_A8:     return FMT_A8;
    case (uint32_t)PPA_BLEND_COLOR_MODE_A4:     return FMT_A4;
    default:                                    return FMT_UNSUPPORTED;
    }
}

static int bpp_of(pix_fmt_t f)
{
    switch (f) {
    case FMT_ARGB8888: return 4;
    case FMT_RGB888:   return 3;
    case FMT_RGB565:   return 2;
    case FMT_A8:       return 1;
    default:           return 0;   // A4 is sub-byte; handled specially
    }
}

static inline uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline uint8_t A_(uint32_t c) { return (c >> 24) & 0xff; }
static inline uint8_t R_(uint32_t c) { return (c >> 16) & 0xff; }
static inline uint8_t G_(uint32_t c) { return (c >> 8) & 0xff; }
static inline uint8_t B_(uint32_t c) { return c & 0xff; }

static inline uint8_t scale5(uint8_t v5) { return (uint8_t)((v5 * 255 + 15) / 31); }
static inline uint8_t scale6(uint8_t v6) { return (uint8_t)((v6 * 255 + 31) / 63); }
static inline uint8_t to5(uint8_t v) { return (uint8_t)((v * 31 + 127) / 255); }
static inline uint8_t to6(uint8_t v) { return (uint8_t)((v * 63 + 127) / 255); }

// Read one pixel from `buf` (stride = pic_w pixels) at (x,y), returning ARGB8888.
// `fix_rgb` supplies the RGB for the alpha-only A8/A4 modes.
static uint32_t get_pixel(const uint8_t *buf, uint32_t pic_w, pix_fmt_t fmt,
                          uint32_t x, uint32_t y, bool byte_swap, bool rgb_swap,
                          color_pixel_rgb888_data_t fix_rgb)
{
    uint8_t a = 255, r = 0, g = 0, b = 0;

    if (fmt == FMT_A4) {
        size_t bit = (size_t)y * pic_w + x;       // 4 bits per pixel, packed
        uint8_t byte = buf[bit >> 1];
        uint8_t nib = (bit & 1) ? (byte >> 4) : (byte & 0x0f);
        a = (uint8_t)(nib * 255 / 15);
        r = fix_rgb.r; g = fix_rgb.g; b = fix_rgb.b;
        return pack_argb(a, r, g, b);
    }

    int bpp = bpp_of(fmt);
    const uint8_t *p = buf + ((size_t)y * pic_w + x) * bpp;
    uint8_t px[4];
    for (int i = 0; i < bpp; i++) px[i] = p[i];
    if (byte_swap && (fmt == FMT_RGB565 || fmt == FMT_ARGB8888)) {
        for (int i = 0; i < bpp / 2; i++) {
            uint8_t t = px[i]; px[i] = px[bpp - 1 - i]; px[bpp - 1 - i] = t;
        }
    }

    switch (fmt) {
    case FMT_ARGB8888:
        b = px[0]; g = px[1]; r = px[2]; a = px[3];
        break;
    case FMT_RGB888:
        b = px[0]; g = px[1]; r = px[2]; a = 255;
        break;
    case FMT_RGB565: {
        uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
        r = scale5((v >> 11) & 0x1f);
        g = scale6((v >> 5) & 0x3f);
        b = scale5(v & 0x1f);
        a = 255;
        break;
    }
    case FMT_A8:
        a = px[0]; r = fix_rgb.r; g = fix_rgb.g; b = fix_rgb.b;
        break;
    default:
        break;
    }

    if (rgb_swap) { uint8_t t = r; r = b; b = t; }
    return pack_argb(a, r, g, b);
}

static inline uint8_t lerp8(uint8_t a, uint8_t b, float t)
{
    float v = a + (b - a) * t;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5f);
}

static inline uint32_t lerp_argb(uint32_t c0, uint32_t c1, float t)
{
    return pack_argb(lerp8(A_(c0), A_(c1), t), lerp8(R_(c0), R_(c1), t),
                     lerp8(G_(c0), G_(c1), t), lerp8(B_(c0), B_(c1), t));
}

// Bilinear sample the input block at fractional block-space coords (fx, fy),
// where (0,0) is the block's top-left pixel. Neighbours are clamped to the block
// extent [0, bw-1] x [0, bh-1] (edge clamp), so the sampler never reads outside
// the requested block. This is the anti-aliased path the PPA HW uses for scaling.
static uint32_t sample_bilinear(const uint8_t *buf, uint32_t pic_w, pix_fmt_t fmt,
                                uint32_t ox0, uint32_t oy0, uint32_t bw, uint32_t bh,
                                float fx, float fy, bool byte_swap, bool rgb_swap,
                                color_pixel_rgb888_data_t fix)
{
    float fxf = floorf(fx), fyf = floorf(fy);
    float wx = fx - fxf, wy = fy - fyf;
    int xmax = (int)bw - 1, ymax = (int)bh - 1;
    int x0 = (int)fxf, y0 = (int)fyf;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : (x0 > xmax ? xmax : x0);
    x1 = x1 < 0 ? 0 : (x1 > xmax ? xmax : x1);
    y0 = y0 < 0 ? 0 : (y0 > ymax ? ymax : y0);
    y1 = y1 < 0 ? 0 : (y1 > ymax ? ymax : y1);

    uint32_t c00 = get_pixel(buf, pic_w, fmt, ox0 + x0, oy0 + y0, byte_swap, rgb_swap, fix);
    uint32_t c10 = get_pixel(buf, pic_w, fmt, ox0 + x1, oy0 + y0, byte_swap, rgb_swap, fix);
    uint32_t c01 = get_pixel(buf, pic_w, fmt, ox0 + x0, oy0 + y1, byte_swap, rgb_swap, fix);
    uint32_t c11 = get_pixel(buf, pic_w, fmt, ox0 + x1, oy0 + y1, byte_swap, rgb_swap, fix);
    return lerp_argb(lerp_argb(c00, c10, wx), lerp_argb(c01, c11, wx), wy);
}

// Write ARGB8888 `c` to `buf` (stride = pic_w pixels) at (x,y) in format `fmt`.
static void put_pixel(uint8_t *buf, uint32_t pic_w, pix_fmt_t fmt,
                      uint32_t x, uint32_t y, uint32_t c)
{
    int bpp = bpp_of(fmt);
    uint8_t *p = buf + ((size_t)y * pic_w + x) * bpp;
    switch (fmt) {
    case FMT_ARGB8888:
        p[0] = B_(c); p[1] = G_(c); p[2] = R_(c); p[3] = A_(c);
        break;
    case FMT_RGB888:
        p[0] = B_(c); p[1] = G_(c); p[2] = R_(c);
        break;
    case FMT_RGB565: {
        uint16_t v = (uint16_t)((to5(R_(c)) << 11) | (to6(G_(c)) << 5) | to5(B_(c)));
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
        break;
    }
    default:
        break;
    }
}

static uint8_t upd_alpha(uint8_t a, ppa_alpha_update_mode_t m, uint32_t fix, float ratio)
{
    switch (m) {
    case PPA_ALPHA_FIX_VALUE: return (uint8_t)(fix & 0xff);
    case PPA_ALPHA_SCALE: {
        int v = (int)lroundf(ratio * 256.0f);
        int out = (a * v) >> 8;
        return (uint8_t)(out > 255 ? 255 : out);
    }
    case PPA_ALPHA_INVERT: return (uint8_t)(255 - a);
    case PPA_ALPHA_NO_CHANGE:
    default: return a;
    }
}

static bool out_in_bounds(const ppa_out_pic_blk_config_t *o, uint32_t x, uint32_t y)
{
    return x < o->pic_w && y < o->pic_h;
}

static void invoke_done(ppa_client_handle_t c, void *user_data)
{
    if (c->on_trans_done) {
        ppa_event_data_t ev;   // empty struct (mirrors the IDF API)
        c->on_trans_done(c, &ev, user_data);
    }
}

/* ---- client lifecycle ---------------------------------------------------- */

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client)
{
    if (!config || !ret_client || config->oper_type >= PPA_OPERATION_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    struct ppa_client_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->oper_type = config->oper_type;
    *ret_client = c;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t ppa_client)
{
    if (!ppa_client) return ESP_ERR_INVALID_ARG;
    free(ppa_client);
    return ESP_OK;
}

esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t ppa_client,
                                              const ppa_event_callbacks_t *cbs)
{
    if (!ppa_client || !cbs) return ESP_ERR_INVALID_ARG;
    ppa_client->on_trans_done = cbs->on_trans_done;
    return ESP_OK;
}

/* ---- SRM ----------------------------------------------------------------- */

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client,
                                     const ppa_srm_oper_config_t *cfg)
{
    if (!ppa_client || !cfg) return ESP_ERR_INVALID_ARG;
    if (ppa_client->oper_type != PPA_OPERATION_SRM) return ESP_ERR_INVALID_ARG;

    pix_fmt_t infmt = fmt_of(cfg->in.srm_cm);
    pix_fmt_t outfmt = fmt_of(cfg->out.srm_cm);
    if (infmt == FMT_UNSUPPORTED || outfmt == FMT_UNSUPPORTED ||
        infmt == FMT_A8 || infmt == FMT_A4 || outfmt == FMT_A8 || outfmt == FMT_A4) {
        ESP_LOGE(TAG, "SRM color mode not supported by the simulator (in=0x%lx out=0x%lx); "
                      "only ARGB8888/RGB888/RGB565 are implemented",
                 (unsigned long)cfg->in.srm_cm, (unsigned long)cfg->out.srm_cm);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint8_t *in = (const uint8_t *)cfg->in.buffer;
    uint8_t *out = (uint8_t *)cfg->out.buffer;
    if (!in || !out) return ESP_ERR_INVALID_ARG;

    uint32_t bw = cfg->in.block_w, bh = cfg->in.block_h;
    float sx = cfg->scale_x > 0 ? cfg->scale_x : 1.0f;
    float sy = cfg->scale_y > 0 ? cfg->scale_y : 1.0f;
    uint32_t sw = (uint32_t)lroundf(bw * sx);
    uint32_t sh = (uint32_t)lroundf(bh * sy);
    if (sw == 0 || sh == 0 || bw == 0 || bh == 0) { invoke_done(ppa_client, cfg->user_data); return ESP_OK; }

    bool swap_wh = (cfg->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                    cfg->rotation_angle == PPA_SRM_ROTATION_ANGLE_270);
    uint32_t ow = swap_wh ? sh : sw;
    uint32_t oh = swap_wh ? sw : sh;

    color_pixel_rgb888_data_t no_fix = {0};

    for (uint32_t oy = 0; oy < oh; oy++) {
        for (uint32_t ox = 0; ox < ow; ox++) {
            // The HW applies scale -> rotate -> mirror (TRM 37.5.3.1), so the
            // inverse map (output pixel back to source) undoes them in reverse:
            // un-mirror (in output space) -> un-rotate -> un-scale. Doing mirror
            // in output space matters when combined with a 90/270 rotation.
            uint32_t rx = cfg->mirror_x ? (ow - 1 - ox) : ox;
            uint32_t ry = cfg->mirror_y ? (oh - 1 - oy) : oy;

            uint32_t px, py;   // coords in the scaled-space [0,sw)x[0,sh)
            switch (cfg->rotation_angle) {
            case PPA_SRM_ROTATION_ANGLE_90:  px = sw - 1 - ry; py = rx;            break;
            case PPA_SRM_ROTATION_ANGLE_180: px = sw - 1 - rx; py = sh - 1 - ry;   break;
            case PPA_SRM_ROTATION_ANGLE_270: px = ry;          py = sh - 1 - rx;   break;
            case PPA_SRM_ROTATION_ANGLE_0:
            default:                         px = rx;          py = ry;            break;
            }

            // Map back to fractional block-space coords using the pixel-centre
            // convention, then bilinear-sample (anti-aliased, like the HW). At
            // scale 1 these land on integer coords, so rotation stays exact.
            float fx = (px + 0.5f) / sx - 0.5f;
            float fy = (py + 0.5f) / sy - 0.5f;

            uint32_t c = sample_bilinear(in, cfg->in.pic_w, infmt,
                                         cfg->in.block_offset_x, cfg->in.block_offset_y,
                                         bw, bh, fx, fy, cfg->byte_swap, cfg->rgb_swap, no_fix);
            uint8_t a = upd_alpha(A_(c), cfg->alpha_update_mode,
                                  cfg->alpha_fix_val, cfg->alpha_scale_ratio);
            c = pack_argb(a, R_(c), G_(c), B_(c));

            uint32_t dx = cfg->out.block_offset_x + ox;
            uint32_t dy = cfg->out.block_offset_y + oy;
            if (out_in_bounds(&cfg->out, dx, dy)) {
                put_pixel(out, cfg->out.pic_w, outfmt, dx, dy, c);
            }
        }
    }

    invoke_done(ppa_client, cfg->user_data);
    return ESP_OK;
}

/* ---- blend --------------------------------------------------------------- */

esp_err_t ppa_do_blend(ppa_client_handle_t ppa_client, const ppa_blend_oper_config_t *cfg)
{
    if (!ppa_client || !cfg) return ESP_ERR_INVALID_ARG;
    if (ppa_client->oper_type != PPA_OPERATION_BLEND) return ESP_ERR_INVALID_ARG;

    pix_fmt_t bgfmt = fmt_of(cfg->in_bg.blend_cm);
    pix_fmt_t fgfmt = fmt_of(cfg->in_fg.blend_cm);
    pix_fmt_t outfmt = fmt_of(cfg->out.blend_cm);
    bool bg_ok = (bgfmt == FMT_ARGB8888 || bgfmt == FMT_RGB888 || bgfmt == FMT_RGB565);
    bool fg_ok = (fgfmt == FMT_ARGB8888 || fgfmt == FMT_RGB888 || fgfmt == FMT_RGB565 ||
                  fgfmt == FMT_A8 || fgfmt == FMT_A4);
    bool out_ok = (outfmt == FMT_ARGB8888 || outfmt == FMT_RGB888 || outfmt == FMT_RGB565);
    if (!bg_ok || !fg_ok || !out_ok) {
        ESP_LOGE(TAG, "blend color mode not supported by the simulator "
                      "(bg=0x%lx fg=0x%lx out=0x%lx)",
                 (unsigned long)cfg->in_bg.blend_cm, (unsigned long)cfg->in_fg.blend_cm,
                 (unsigned long)cfg->out.blend_cm);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (cfg->bg_ck_en || cfg->fg_ck_en) {
        ESP_LOGW(TAG, "blend color-keying is not implemented in the simulator; "
                      "doing plain alpha blending");
    }

    const uint8_t *bg = (const uint8_t *)cfg->in_bg.buffer;
    const uint8_t *fg = (const uint8_t *)cfg->in_fg.buffer;
    uint8_t *out = (uint8_t *)cfg->out.buffer;
    if (!bg || !fg || !out) return ESP_ERR_INVALID_ARG;

    uint32_t w = cfg->in_fg.block_w, h = cfg->in_fg.block_h;
    color_pixel_rgb888_data_t no_fix = {0};

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t bc = get_pixel(bg, cfg->in_bg.pic_w, bgfmt,
                                    cfg->in_bg.block_offset_x + x, cfg->in_bg.block_offset_y + y,
                                    cfg->bg_byte_swap, cfg->bg_rgb_swap, no_fix);
            uint32_t fc = get_pixel(fg, cfg->in_fg.pic_w, fgfmt,
                                    cfg->in_fg.block_offset_x + x, cfg->in_fg.block_offset_y + y,
                                    cfg->fg_byte_swap, cfg->fg_rgb_swap, cfg->fg_fix_rgb_val);

            uint8_t ba = upd_alpha(A_(bc), cfg->bg_alpha_update_mode,
                                   cfg->bg_alpha_fix_val, cfg->bg_alpha_scale_ratio);
            uint8_t fa = upd_alpha(A_(fc), cfg->fg_alpha_update_mode,
                                   cfg->fg_alpha_fix_val, cfg->fg_alpha_scale_ratio);

            // straight alpha-over: out = fg over bg
            uint32_t ofa = fa, oba = ba * (255 - fa) / 255;
            uint32_t denom = ofa + oba;
            uint8_t r, g, b, a;
            if (denom == 0) {
                r = g = b = 0; a = 0;
            } else {
                r = (uint8_t)((R_(fc) * ofa + R_(bc) * oba) / denom);
                g = (uint8_t)((G_(fc) * ofa + G_(bc) * oba) / denom);
                b = (uint8_t)((B_(fc) * ofa + B_(bc) * oba) / denom);
                a = (uint8_t)(denom > 255 ? 255 : denom);
            }

            uint32_t dx = cfg->out.block_offset_x + x;
            uint32_t dy = cfg->out.block_offset_y + y;
            if (out_in_bounds(&cfg->out, dx, dy)) {
                put_pixel(out, cfg->out.pic_w, outfmt, dx, dy, pack_argb(a, r, g, b));
            }
        }
    }

    invoke_done(ppa_client, cfg->user_data);
    return ESP_OK;
}

/* ---- fill ---------------------------------------------------------------- */

esp_err_t ppa_do_fill(ppa_client_handle_t ppa_client, const ppa_fill_oper_config_t *cfg)
{
    if (!ppa_client || !cfg) return ESP_ERR_INVALID_ARG;
    if (ppa_client->oper_type != PPA_OPERATION_FILL) return ESP_ERR_INVALID_ARG;

    pix_fmt_t outfmt = fmt_of(cfg->out.fill_cm);
    if (outfmt != FMT_ARGB8888 && outfmt != FMT_RGB888 && outfmt != FMT_RGB565) {
        ESP_LOGE(TAG, "fill color mode not supported by the simulator (out=0x%lx)",
                 (unsigned long)cfg->out.fill_cm);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t *out = (uint8_t *)cfg->out.buffer;
    if (!out) return ESP_ERR_INVALID_ARG;

    uint32_t c = pack_argb(cfg->fill_argb_color.a, cfg->fill_argb_color.r,
                           cfg->fill_argb_color.g, cfg->fill_argb_color.b);

    for (uint32_t y = 0; y < cfg->fill_block_h; y++) {
        for (uint32_t x = 0; x < cfg->fill_block_w; x++) {
            uint32_t dx = cfg->out.block_offset_x + x;
            uint32_t dy = cfg->out.block_offset_y + y;
            if (out_in_bounds(&cfg->out, dx, dy)) {
                put_pixel(out, cfg->out.pic_w, outfmt, dx, dy, c);
            }
        }
    }

    invoke_done(ppa_client, cfg->user_data);
    return ESP_OK;
}
