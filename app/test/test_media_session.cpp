// Host unit test for app/media_session — the main fixture mirrors the shape of
// Pixel 10 (frankel) `dumpsys media_session` output with a music app playing,
// plus synthetic edge cases (paused, idle, single-segment metadata). Track and
// artist names are generic placeholders, not real media; the CJK metadata is
// plain dictionary words kept only to exercise UTF-8 byte passthrough. No phone.

#include <cstdio>
#include <string>

#include "media_session.hpp"

using namespace app::mediainfo;

static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

// Pixel 10 capture shape (a music app PLAYING; two other inactive sessions + the
// global HeadsetMediaButton). The metadata uses generic CJK words ("タイトル,
// アーティスト, アルバム" = title, artist, album) only to check UTF-8 passthrough.
static const char kPixel10[] =
    "MEDIA SESSION SERVICE (dumpsys media_session)\n"
    "\n"
    "8 sessions listeners.\n"
    "Global priority session is com.android.server.telecom/HeadsetMediaButton/1 (userId=0)\n"
    "  HeadsetMediaButton com.android.server.telecom/HeadsetMediaButton/1 (userId=0)\n"
    "    ownerPid=1703, ownerUid=1000, userId=0\n"
    "    package=com.android.server.telecom\n"
    "    active=false\n"
    "    state=null\n"
    "    metadata: null\n"
    "User Records:\n"
    "Record for full_user=0\n"
    "  Media button session is com.example.musicplayer/MediaPlaybackService/6 (userId=0)\n"
    "  Sessions Stack - have 3 sessions:\n"
    "    MediaPlaybackService com.example.musicplayer/MediaPlaybackService/6 (userId=0)\n"
    "      ownerPid=12941, ownerUid=10157, userId=0\n"
    "      package=com.example.musicplayer\n"
    "      active=true\n"
    "      flags=7\n"
    "      controllers: 12\n"
    "      state=PlaybackState {state=PLAYING(3), position=278, buffered position=60836, "
    "speed=1.0, updated=6149628, actions=130483, custom actions=[], active item id=30064771080, "
    "error=null}\n"
    "      metadata: size=52, description=タイトル, アーティスト, アルバム\n"
    "      queueTitle=キュー, size=3\n"
    "    play_movies_media com.google.android.videos/play_movies_media/2 (userId=0)\n"
    "      ownerPid=9076, ownerUid=10201, userId=0\n"
    "      package=com.google.android.videos\n"
    "      active=false\n"
    "      state=null\n"
    "      metadata: null\n"
    "    media-session com.google.android.apps.labs.language.tailwind/media-session/3 (userId=0)\n"
    "      ownerPid=11181, ownerUid=10311, userId=0\n"
    "      package=com.google.android.apps.labs.language.tailwind\n"
    "      active=false\n"
    "      state=PlaybackState {state=NONE(0), position=0, buffered position=0, speed=1.0, "
    "updated=396018, actions=3669711, custom actions=[], active item id=-1, error=null}\n"
    "      metadata: null\n"
    "Audio playback (lastly played comes first)\n"
    "  uid=10157 packages=com.example.musicplayer \n";

// Same music app session but PAUSED, and no other playable session.
static const char kPaused[] =
    "  Sessions Stack - have 1 sessions:\n"
    "    MediaPlaybackService com.example.musicplayer/MediaPlaybackService/6 (userId=0)\n"
    "      package=com.example.musicplayer\n"
    "      active=true\n"
    "      state=PlaybackState {state=PAUSED(2), position=12345, speed=0.0, updated=1, "
    "actions=130483}\n"
    "      metadata: size=52, description=Track One, Performer A, Record B\n";

// Nothing playing: only the idle HeadsetMediaButton / NONE sessions.
static const char kIdle[] =
    "  Sessions Stack - have 1 sessions:\n"
    "    play_movies_media com.google.android.videos/play_movies_media/2 (userId=0)\n"
    "      package=com.google.android.videos\n"
    "      active=false\n"
    "      state=null\n"
    "      metadata: null\n";

// Single-segment metadata (e.g. a podcast/title-only stream).
static const char kTitleOnly[] =
    "    Foo com.example.player/Foo/1 (userId=0)\n"
    "      package=com.example.player\n"
    "      active=true\n"
    "      state=PlaybackState {state=PLAYING(3), position=0}\n"
    "      metadata: size=4, description=Just A Title\n";

int main() {
    // ---- a music app playing (UTF-8 metadata) ----
    NowPlaying p = parse_media_session(kPixel10);
    CHECK(p.valid);
    CHECK(p.playing());
    CHECK(p.state == State::Playing);
    CHECK(p.package == "com.example.musicplayer");
    CHECK(p.title == "タイトル");
    CHECK(p.artist == "アーティスト");
    CHECK(p.album == "アルバム");
    CHECK(p.position_ms == 278);

    // ---- paused ----
    NowPlaying pa = parse_media_session(kPaused);
    CHECK(pa.valid);
    CHECK(!pa.playing());
    CHECK(pa.active());
    CHECK(pa.state == State::Paused);
    CHECK(pa.title == "Track One");
    CHECK(pa.artist == "Performer A");
    CHECK(pa.album == "Record B");
    CHECK(pa.position_ms == 12345);

    // ---- idle: empty result, controls render in their disabled state ----
    NowPlaying id = parse_media_session(kIdle);
    CHECK(!id.valid);
    CHECK(!id.active());
    CHECK(id.title.empty());

    NowPlaying empty = parse_media_session("");
    CHECK(!empty.valid);

    // ---- title-only metadata ----
    NowPlaying t = parse_media_session(kTitleOnly);
    CHECK(t.valid);
    CHECK(t.title == "Just A Title");
    CHECK(t.artist.empty());
    CHECK(t.album.empty());

    std::printf("OK (%d checks)\n", g_checks);
    return 0;
}
