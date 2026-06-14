#include "png_decode.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

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

}  // namespace

namespace app {

void aspect_fit(int src_w, int src_h, int max_w, int max_h, int* fit_w, int* fit_h) {
    double s = std::min((double)max_w / src_w, (double)max_h / src_h);
    *fit_w = std::min(std::max(1, (int)std::lround(src_w * s)), max_w);
    *fit_h = std::min(std::max(1, (int)std::lround(src_h * s)), max_h);
}

bool decode_png_downscale_rgb565(const uint8_t* png, size_t len,
                                 uint16_t* out, int max_w, int max_h,
                                 int* fit_w, int* fit_h,
                                 int* src_w, int* src_h) {
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

    int dst_w, dst_h;
    aspect_fit(W, H, max_w, max_h, &dst_w, &dst_h);
    *fit_w = dst_w;
    *fit_h = dst_h;

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

}  // namespace app
