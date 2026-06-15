#pragma once
// Parser for `dumpsys media_session` — a pure function on the command's text
// (no I/O, no LVGL, no adb types) so it can run on the adb reader thread and be
// host-unit-tested (app/test/run.sh). Like device_info, malformed/absent input
// yields an "empty" result (valid == false) rather than an error, so the caller
// renders an idle media card instead of garbage.

#include <string>
#include <vector>

namespace app::mediainfo {

// PlaybackState.STATE_* (the subset we distinguish; everything else = Other).
enum class State { None, Playing, Paused, Stopped, Buffering, Other };

// The active media session worth surfacing, picked from the Sessions Stack.
struct NowPlaying {
    bool valid = false;          // a session with metadata + a real state was found
    std::string package;         // owning app package (e.g. com.apple.android.music)
    std::string title;           // 1st metadata-description segment
    std::string artist;          // 2nd segment (may be empty)
    std::string album;           // 3rd segment (may be empty)
    State state = State::None;
    long long position_ms = -1;  // last reported position (static; not advanced)

    bool playing() const { return state == State::Playing; }
    // Has audible/visible content the transport controls act on.
    bool active() const {
        return state == State::Playing || state == State::Paused ||
               state == State::Buffering;
    }
};

// Parse `dumpsys media_session` output. Selects the highest-priority session
// that has metadata and a play/pause/buffering state (Playing preferred over
// Buffering over Paused). Returns {valid=false} when nothing is playing.
NowPlaying parse_media_session(const std::string &dumpsys_out);

}  // namespace app::mediainfo
