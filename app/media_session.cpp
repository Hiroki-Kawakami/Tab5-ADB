#include "media_session.hpp"

#include <cstring>

#include "device_info.hpp"  // app::devinfo::trim (shared string helper)

namespace app::mediainfo {

namespace {

using app::devinfo::trim;

// Split into lines (no terminators), tolerating \r\n.
std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < s.size()) lines.push_back(s.substr(pos));
            break;
        }
        std::string line = s.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    return lines;
}

// Integer right after `needle` (first occurrence), or `fallback`. Skips a sign.
long long int_after(const std::string &s, const char *needle, long long fallback) {
    size_t pos = s.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += std::strlen(needle);
    const char *p = s.c_str() + pos;
    char *end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    return end == p ? fallback : v;
}

State state_from_name(const std::string &name) {
    if (name == "PLAYING") return State::Playing;
    if (name == "PAUSED") return State::Paused;
    if (name == "STOPPED") return State::Stopped;
    if (name == "BUFFERING") return State::Buffering;
    if (name == "NONE") return State::None;
    return State::Other;
}

// Per-session accumulator built while walking the dump.
struct Block {
    std::string package;
    State state = State::None;
    long long position_ms = -1;
    bool has_metadata = false;
    std::string description;  // raw "title, subtitle, description" from metadata
};

// Priority for picking the session to surface: a buffering/playing track beats
// a merely-paused one, which beats everything else.
int rank(const Block &b) {
    if (!b.has_metadata) return 0;
    switch (b.state) {
        case State::Playing: return 4;
        case State::Buffering: return 3;
        case State::Paused: return 2;
        default: return 0;  // None/Stopped/Other without playback = idle
    }
}

}  // namespace

NowPlaying parse_media_session(const std::string &dumpsys_out) {
    std::vector<Block> blocks;
    Block *cur = nullptr;  // valid only while `blocks` doesn't reallocate

    // Each session block carries exactly one `package=` line; the fields we read
    // (active/state/metadata) all follow it within the same block, so a new
    // `package=` starts a fresh accumulator.
    for (const auto &raw : lines_of(dumpsys_out)) {
        std::string line = trim(raw);
        if (line.rfind("package=", 0) == 0) {
            blocks.push_back(Block{});
            cur = &blocks.back();
            cur->package = trim(line.substr(std::strlen("package=")));
        } else if (cur && line.rfind("state=", 0) == 0) {
            // "state=null" or "state=PlaybackState {state=PLAYING(3), position=278, ...}"
            size_t inner = line.find("{state=");
            if (inner != std::string::npos) {
                size_t name_pos = inner + std::strlen("{state=");
                size_t paren = line.find('(', name_pos);
                if (paren != std::string::npos)
                    cur->state = state_from_name(line.substr(name_pos, paren - name_pos));
                cur->position_ms = int_after(line, "position=", -1);
            }
        } else if (cur && line.rfind("metadata:", 0) == 0) {
            // "metadata: null" or "metadata: size=52, description=Title, Artist, Album"
            size_t d = line.find("description=");
            if (d != std::string::npos) {
                std::string desc = trim(line.substr(d + std::strlen("description=")));
                if (!desc.empty() && desc != "null") {
                    cur->has_metadata = true;
                    cur->description = desc;
                }
            }
        }
    }

    // Pick the highest-ranked block; ties keep the first (the Sessions Stack is
    // already in priority order — the media-button session comes first).
    const Block *best = nullptr;
    int best_rank = 0;
    for (const auto &b : blocks) {
        int r = rank(b);
        if (r > best_rank) {
            best_rank = r;
            best = &b;
        }
    }

    NowPlaying np;
    if (!best) return np;  // valid == false: idle controls

    np.valid = true;
    np.package = best->package;
    np.state = best->state;
    np.position_ms = best->position_ms;

    // The metadata description is the MediaDescription's
    // "title, subtitle, description" joined with ", " (track / artist / album for
    // music). A field containing ", " would mis-split — inherent to the dump; the
    // future agent path reads the discrete MediaMetadata keys instead.
    const std::string &desc = best->description;
    size_t a = desc.find(", ");
    if (a == std::string::npos) {
        np.title = desc;
    } else {
        np.title = desc.substr(0, a);
        size_t b2 = desc.find(", ", a + 2);
        if (b2 == std::string::npos) {
            np.artist = desc.substr(a + 2);
        } else {
            np.artist = desc.substr(a + 2, b2 - (a + 2));
            np.album = desc.substr(b2 + 2);
        }
    }
    return np;
}

}  // namespace app::mediainfo
