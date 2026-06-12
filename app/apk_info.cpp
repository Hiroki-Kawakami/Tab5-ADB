#include "apk_info.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace app::apkinfo {

namespace {

// ---- little-endian readers with bounds checks ------------------------------

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ---- zip: extract AndroidManifest.xml --------------------------------------

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

struct File {
    FILE* f = nullptr;
    long size = 0;
    ~File() {
        if (f) fclose(f);
    }
    bool read_at(long off, void* dst, size_t n) const {
        if (off < 0 || (long)(off + n) > size) return false;
        if (fseek(f, off, SEEK_SET) != 0) return false;
        return fread(dst, 1, n, f) == n;
    }
};

// Locate AndroidManifest.xml via the central directory and inflate it.
bool read_manifest(const File& zip, std::vector<uint8_t>& out, std::string& err) {
    // EOCD: scan back from the end (the comment can push it up to ~64 KB in).
    long tail_len = zip.size < 66 * 1024 ? zip.size : 66 * 1024;
    std::vector<uint8_t> tail(tail_len);
    if (tail_len < 22 || !zip.read_at(zip.size - tail_len, tail.data(), tail_len)) {
        err = "not a zip (too small)";
        return false;
    }
    long eocd = -1;
    for (long i = tail_len - 22; i >= 0; --i) {
        if (le32(&tail[i]) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        err = "not a zip (no end-of-central-directory)";
        return false;
    }
    uint16_t entry_count = le16(&tail[eocd + 10]);
    uint32_t cd_off = le32(&tail[eocd + 16]);
    if (cd_off == 0xFFFFFFFF) {
        err = "zip64 is not supported";
        return false;
    }

    // Walk the central directory for AndroidManifest.xml.
    long off = cd_off;
    for (uint16_t i = 0; i < entry_count; ++i) {
        uint8_t hdr[46];
        if (!zip.read_at(off, hdr, sizeof(hdr)) || le32(hdr) != kCentralSig) {
            err = "broken central directory";
            return false;
        }
        uint16_t method = le16(hdr + 10);
        uint32_t comp_size = le32(hdr + 20);
        uint32_t uncomp_size = le32(hdr + 24);
        uint16_t name_len = le16(hdr + 28);
        uint16_t extra_len = le16(hdr + 30);
        uint16_t comment_len = le16(hdr + 32);
        uint32_t local_off = le32(hdr + 42);

        char name[64];
        bool match = false;
        if (name_len == 19 && zip.read_at(off + 46, name, name_len)) {
            match = memcmp(name, "AndroidManifest.xml", 19) == 0;
        }
        if (!match) {
            off += 46 + name_len + extra_len + comment_len;
            continue;
        }

        // Local header tells where the data really starts (its own name/extra
        // sizes may differ from the central copy's).
        uint8_t lh[30];
        if (!zip.read_at(local_off, lh, sizeof(lh)) || le32(lh) != kLocalSig) {
            err = "broken local header";
            return false;
        }
        long data_off = local_off + 30 + le16(lh + 26) + le16(lh + 28);

        if (uncomp_size == 0 || uncomp_size > 4 * 1024 * 1024) {
            err = "manifest size out of range";
            return false;
        }
        std::vector<uint8_t> comp(comp_size);
        if (!zip.read_at(data_off, comp.data(), comp_size)) {
            err = "truncated manifest data";
            return false;
        }
        out.resize(uncomp_size);
        if (method == 0) {  // stored
            if (comp_size != uncomp_size) {
                err = "stored size mismatch";
                return false;
            }
            memcpy(out.data(), comp.data(), comp_size);
            return true;
        }
        if (method != 8) {
            err = "unsupported compression method";
            return false;
        }
        z_stream zs = {};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {  // raw deflate
            err = "inflate init failed";
            return false;
        }
        zs.next_in = comp.data();
        zs.avail_in = comp_size;
        zs.next_out = out.data();
        zs.avail_out = uncomp_size;
        int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (r != Z_STREAM_END || zs.total_out != uncomp_size) {
            err = "manifest inflate failed";
            return false;
        }
        return true;
    }
    err = "no AndroidManifest.xml";
    return false;
}

// ---- binary AXML ------------------------------------------------------------

constexpr uint16_t kChunkXml = 0x0003;
constexpr uint16_t kChunkStringPool = 0x0001;
constexpr uint16_t kChunkStartElement = 0x0102;
constexpr uint32_t kUtf8Flag = 1u << 8;
constexpr uint8_t kTypeString = 0x03;
constexpr uint8_t kTypeIntDec = 0x10;
constexpr uint8_t kTypeIntHex = 0x11;

void utf16_to_utf8(const uint8_t* p, size_t units, std::string& out) {
    for (size_t i = 0; i < units; ++i) {
        uint32_t c = le16(p + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {  // surrogate pair
            uint32_t lo = le16(p + 2 * (i + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
}

// Decode the string pool chunk at d[0..size) into `pool`.
bool parse_string_pool(const uint8_t* d, uint32_t size, std::vector<std::string>& pool) {
    if (size < 28) return false;
    uint32_t count = le32(d + 8);
    uint32_t flags = le32(d + 16);
    uint32_t strings_start = le32(d + 20);
    bool utf8 = (flags & kUtf8Flag) != 0;
    if (28 + (uint64_t)count * 4 > size || strings_start >= size) return false;

    pool.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rel = le32(d + 28 + 4 * i);
        uint64_t off = (uint64_t)strings_start + rel;
        std::string s;
        if (utf8) {
            if (off + 2 > size) return false;
            // u8 char count then u8 byte count, each with a high-bit extension.
            uint32_t n = d[off++];
            if (n & 0x80) {
                if (off >= size) return false;
                n = ((n & 0x7F) << 8) | d[off++];
            }
            if (off >= size) return false;
            uint32_t bytes = d[off++];
            if (bytes & 0x80) {
                if (off >= size) return false;
                bytes = ((bytes & 0x7F) << 8) | d[off++];
            }
            if (off + bytes > size) return false;
            s.assign((const char*)d + off, bytes);
        } else {
            if (off + 2 > size) return false;
            uint32_t units = le16(d + off);
            off += 2;
            if (units & 0x8000) {
                if (off + 2 > size) return false;
                units = ((units & 0x7FFF) << 16) | le16(d + off);
                off += 2;
            }
            if (off + (uint64_t)units * 2 > size) return false;
            utf16_to_utf8(d + off, units, s);
        }
        pool.push_back(std::move(s));
    }
    return true;
}

const std::string& pool_str(const std::vector<std::string>& pool, uint32_t idx) {
    static const std::string empty;
    return idx < pool.size() ? pool[idx] : empty;
}

bool parse_axml(const std::vector<uint8_t>& xml, ApkInfo& out, std::string& err) {
    const uint8_t* d = xml.data();
    size_t n = xml.size();
    if (n < 8 || le16(d) != kChunkXml) {
        err = "not a binary XML";
        return false;
    }
    std::vector<std::string> pool;

    size_t off = le16(d + 2);  // first chunk follows the XML header
    while (off + 8 <= n) {
        uint16_t type = le16(d + off);
        uint16_t header_size = le16(d + off + 2);
        uint32_t chunk_size = le32(d + off + 4);
        if (chunk_size < 8 || off + chunk_size > n) break;

        if (type == kChunkStringPool) {
            if (!parse_string_pool(d + off, chunk_size, pool)) {
                err = "broken string pool";
                return false;
            }
        } else if (type == kChunkStartElement) {
            // header (16) + ns(4) name(4) attrStart(2) attrSize(2) attrCount(2)...
            if (chunk_size < 16 + 20) {
                off += chunk_size;
                continue;
            }
            const uint8_t* e = d + off + 16;
            uint32_t name_idx = le32(e + 4);
            uint16_t attr_start = le16(e + 8);
            uint16_t attr_size = le16(e + 10);
            uint16_t attr_count = le16(e + 12);
            const std::string& elem = pool_str(pool, name_idx);

            bool want_manifest = elem == "manifest";
            bool want_sdk = elem == "uses-sdk";
            bool want_app = elem == "application";
            if (want_manifest || want_sdk || want_app) {
                for (uint16_t i = 0; i < attr_count; ++i) {
                    size_t a = off + 16 + attr_start + (size_t)i * attr_size;
                    if (a + 20 > off + chunk_size) break;
                    const uint8_t* ap = d + a;
                    const std::string& attr = pool_str(pool, le32(ap + 4));
                    uint32_t raw_idx = le32(ap + 8);
                    uint8_t data_type = ap[15];
                    uint32_t data = le32(ap + 16);

                    auto str_value = [&]() -> std::string {
                        if (data_type == kTypeString) return pool_str(pool, data);
                        if (raw_idx != 0xFFFFFFFF) return pool_str(pool, raw_idx);
                        return "";
                    };
                    auto int_value = [&]() -> long {
                        if (data_type == kTypeIntDec || data_type == kTypeIntHex)
                            return (long)data;
                        return -1;
                    };

                    if (want_manifest) {
                        if (attr == "package") out.package = str_value();
                        else if (attr == "versionName") out.version_name = str_value();
                        else if (attr == "versionCode" && int_value() >= 0)
                            out.version_code = (uint32_t)data;
                    } else if (want_sdk) {
                        if (attr == "minSdkVersion" && int_value() >= 0)
                            out.min_sdk = (int)data;
                        else if (attr == "targetSdkVersion" && int_value() >= 0)
                            out.target_sdk = (int)data;
                    } else if (want_app) {
                        // Literal labels only; a resource reference (type 0x01)
                        // would need resources.arsc — left empty.
                        if (attr == "label" && data_type == kTypeString)
                            out.label = pool_str(pool, data);
                    }
                }
            }
        }
        off += chunk_size;
    }
    if (out.package.empty()) {
        err = "no manifest package";
        return false;
    }
    return true;
}

}  // namespace

bool parse(const char* path, ApkInfo& out, std::string& err) {
    File zip;
    zip.f = fopen(path, "rb");
    if (!zip.f) {
        err = "cannot open the file";
        return false;
    }
    if (fseek(zip.f, 0, SEEK_END) != 0 || (zip.size = ftell(zip.f)) <= 0) {
        err = "cannot read the file";
        return false;
    }
    std::vector<uint8_t> manifest;
    if (!read_manifest(zip, manifest, err)) return false;
    return parse_axml(manifest, out, err);
}

}  // namespace app::apkinfo
