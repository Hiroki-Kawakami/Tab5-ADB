// agent_link — the Tab5-side link to tab5adb-agent.
//
// This component owns the Tab5 end of the single TYPE-multiplexed ADB stream
// defined in android-agent/docs/protocol.md (control + video + future audio). It
// opens `localabstract:tab5adb-agent` over an Online adb::Client, answers the
// agent-initiated HELLO, and (in later slices) receives the JPEG strip stream.
//
// Dependency direction is deliberate: agent_link (App-specific) -> adb (generic
// raw stream) -> embedded_adb. The generic adb::Client never knows about
// agent_link; the entry point lives here (Link::open takes the Client).
//
// Layering of the receive path: the framing / HELLO / (later) strip-placement
// logic is portable C++ with no LVGL or HW dependency, so the headless test and
// the on-device app share it and only swap the decode + framebuffer seam.
//
// Threading: the frame parser runs on adb's reader thread, so listener callbacks
// fire there; marshalling to LVGL is the app's job (the headless test has no LVGL
// and uses them directly).
//
// Multiplexing: the link mirrors the wire's TYPE multiplexing (protocol.md §2.1).
// Rather than one monolithic listener, callbacks are split per logical channel so
// independent consumers can attach to just their slice:
//   - LinkLifecycleListener — link establishment (HELLO) + close. The link OWNER
//     (app::AgentClient) implements it and passes it to open(); it drives the
//     connection state machine.
//   - VideoListener — the JPEG mirror stream. The feature (e.g. the mirror
//     screen) implements it and registers it with set_video_listener(); it comes
//     and goes independently of the link, which the owner keeps alive across
//     features.
//   - AudioListener — the PCM mirror stream (§6), registered with
//     set_audio_listener(); the same independent come-and-go as VideoListener.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "adb_error.hpp"          // adb::Error
#include "adb_raw_stream.hpp"     // adb::Stream, adb::StreamListener
#include "agent_link_protocol.hpp"  // FrameHeader, wire constants

namespace adb {
class Client;
}

namespace agent_link {

// The agent's HELLO (its self-introduction, protocol.md §4.4 request args).
// Link-only: proto/version/capability. Mirror params (source size, codec) arrive
// later in the MIRROR_START response (see MirrorInfo).
struct AgentInfo {
    uint8_t proto_version = 0;
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    uint8_t version_patch = 0;
    uint16_t capabilities = 0;  // agent's offered features (§4.6 Cap bits)
};

// Tab5's HELLO response params (protocol.md §4.4 response result). Defaults: the
// simulator/libusb max_payload (device advertises 16 KiB, so the app overrides
// this) and the features Tab5 can accept. Link-only — no mirror params here.
struct HelloConfig {
    uint32_t max_payload = 256 * 1024;
    uint16_t capabilities = kCapVideo | kCapAppInfo | kCapMedia;  // §4.6
};

// Mirror parameters Tab5 hands the agent in MIRROR_START (protocol.md §4.4 args).
// Defaults match the Tab5 panel + fit mode, video only. target_* is the viewer
// surface (not necessarily the panel — e.g. the DeviceScreen preview box); it
// must be 16-aligned when split_count > 1 (§5.2 strip alignment), even is
// enough for a single-strip stream.
struct MirrorConfig {
    uint16_t target_width = 720;
    uint16_t target_height = 1280;
    uint8_t scale_mode = kScaleFit;
    uint8_t streams = kCapVideo;   // which streams to start (§4.6 bit assignment)
    uint8_t max_fps = 0;           // frame-rate cap; 0 = the agent's own cap only
    uint8_t jpeg_quality = 0;      // 1..100; 0 = the agent's default (80)
    uint8_t split_count = 0;       // strips per frame; 0 = the agent's default (4),
                                   // 1 = whole frame as one JPEG (the preview)
};

// The agent's MIRROR_START response (protocol.md §4.4 result): the source it is
// actually streaming and the output frame size it chose (== target for fit/fill;
// the aspect-derived size for kScaleAspect — size receive buffers from this).
struct MirrorInfo {
    uint16_t source_width = 0;   // physical source width [px] (informational)
    uint16_t source_height = 0;  // physical source height [px] (informational)
    uint8_t video_codec = 0;     // 0x01 = JPEG(YUV420)
    uint16_t out_width = 0;      // streamed frame width [px]
    uint16_t out_height = 0;     // streamed frame height [px]
};

// The audio format the agent chose, from the MIRROR_START response audio tail
// (§6.2). Delivered to the AudioListener as on_audio_started when AUDIO was
// started; open the audio sink (bsp_audio_open) with these. v1 codec is PCM_S16LE.
struct AudioInfo {
    uint32_t sample_rate = 0;  // e.g. 48000
    uint8_t channels = 0;      // e.g. 2 (stereo; the Tab5 BSP downmixes for speaker)
    uint8_t codec = 0;         // agent_link::AudioCodec (kAudioCodecPcmS16le)
};

// The source device's logical orientation (§4.4 ORIENTATION event). The Tab5
// keeps mirroring the device's natural-orientation framebuffer either way (§5.1);
// this only tells the feature how to lay out its overlay UI (portrait vs
// landscape), since the user physically turns the Tab5 to view a landscape app.
struct OrientationInfo {
    uint8_t rotation = 0;    // Surface.ROTATION_* (0/1/2/3 = 0/90/180/270)
    bool landscape = false;  // rotation is 90 or 270 (the panel is being viewed sideways)
};

// Now-playing media snapshot (§4.4 MEDIA event / GET_MEDIA_INFO). The agent
// pushes this on every state/track change; the Tab5 re-fetches art + rendered
// title/artist (GET_MEDIA_RENDER) only when content_token changes (a state-only
// change — play<->pause — keeps the token, so no re-render).
struct MediaState {
    uint8_t state = 0;            // agent_link::MediaState enum (None/Playing/Paused/...)
    bool has_art = false;         // an album-art bitmap is available
    uint32_t content_token = 0;   // track-identity hash; 0 = no active session
    bool playing() const { return state == 1 /*kMediaPlaying*/; }
    bool active() const { return state == 1 || state == 2 || state == 3; }  // playing/paused/buffering
};

// One JPEG strip (§5.2): the rectangle on the Tab5 panel (x,y,w,h, all 16px
// multiples) plus the JPEG bytes that decode into it. `frame_start`/`frame_end`
// mirror the frame-layer FLAGS — present the framebuffer after the frame_end
// strip is decoded (§5.4). `jpeg` points into the Link's rx buffer and is only
// valid for the duration of the on_video_strip call.
struct VideoStrip {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    const uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    bool frame_start = false;
    bool frame_end = false;
};

class Link;

// Link lifecycle delegate — link establishment + teardown. The link OWNER
// (app::AgentClient) implements it and passes it to open(); it outlives the
// individual features. Callbacks fire on the reader thread.
class LinkLifecycleListener {
public:
    virtual ~LinkLifecycleListener() = default;

    // The HELLO handshake completed: the agent's HELLO was received, proto
    // matched, and our response was sent. Fires once, before any media — the
    // agent_link layer is now established (READY). The owner can now let features
    // register a channel listener and call Link::start_mirror(). The Link* first
    // arg lets one listener serve several links.
    virtual void on_link_hello(Link* link, const AgentInfo& info) = 0;

    // The link closed (peer/our close(), Client::close() teardown, or a protocol
    // error). Fires exactly once; err == Protocol on a framing/proto-mismatch
    // error, otherwise Ok.
    virtual void on_link_close(Link* link, adb::Error err) = 0;
};

// Video channel delegate — the JPEG mirror stream (TYPE=JPEG). A feature
// registers it with Link::set_video_listener() while it wants frames and clears
// it (set_video_listener({})) to detach; the link survives. Held weakly (lock()ed
// before each dispatch), so dropping the feature's shared_ptr also detaches.
// Callbacks fire on the reader thread.
class VideoListener {
public:
    virtual ~VideoListener() = default;

    // The agent accepted MIRROR_START (its response, §4.4 result): the video
    // stream is about to flow. Optional. Fires once per start_mirror().
    virtual void on_mirror_started(Link* /*link*/, const MirrorInfo& /*info*/) {}

    // The source device's orientation (the ORIENTATION event, §4.4): sent once
    // when the stream starts and again whenever the device rotates. Optional —
    // lay out the overlay UI for portrait vs landscape. Fires on the reader thread.
    virtual void on_orientation(Link* /*link*/, const OrientationInfo& /*info*/) {}

    // One JPEG strip of the video stream (§5). Decode `strip.jpeg` into the
    // (x,y,w,h) region; present the framebuffer once strip.frame_end is decoded
    // (§5.4). The decode + framebuffer seam is the listener's job, so the Link
    // stays free of libjpeg / HW-JPEG (test: host libjpeg + memory FB; app: P4 HW
    // JPEG + bsp FB).
    virtual void on_video_strip(Link* /*link*/, const VideoStrip& /*strip*/) = 0;
};

// Audio channel delegate — the PCM mirror stream (TYPE=AUDIO, §6). The AUDIO
// analogue of VideoListener: a feature registers it with Link::set_audio_listener()
// while it wants audio and clears it ({}) to detach; the link survives. Held weakly
// (lock()ed before each dispatch). Callbacks fire on the reader thread, so NEVER
// block here — copy the PCM into a ring and let an audio task drain it (§6.3);
// blocking would stall the per-A_WRTE flow control that gates the video stream.
class AudioListener {
public:
    virtual ~AudioListener() = default;

    // The MIRROR_START response carried the §6.2 audio tail: the audio stream is
    // about to flow with `info`'s format. Open the audio sink (bsp_audio_open) from
    // this. Fires once per start_mirror() that requested (and got) AUDIO.
    virtual void on_audio_started(Link* /*link*/, const AudioInfo& /*info*/) {}

    // One AUDIO frame = one codec unit (§6.3): for codec=PCM, `pcm`/`len` is a raw
    // interleaved 16-bit-LE PCM chunk. `pcm` points into the Link's rx buffer and is
    // valid only for the duration of this call — copy what you keep.
    virtual void on_audio_data(Link* /*link*/, const uint8_t* /*pcm*/, size_t /*len*/) = 0;
};

// Media channel delegate — now-playing notifications (the MEDIA event, §4.4). A
// feature (the DeviceScreen media card) registers it with Link::set_media_listener()
// while it shows the card and clears it ({}) to detach; the link survives. Held
// weakly (lock()ed before each dispatch). Callbacks fire on the reader thread —
// marshal to LVGL yourself. Album art + rendered title/artist are fetched
// on-demand with Link::request(kCmdGetMediaRender, ...); this channel only carries
// the small state/token notification.
class MediaListener {
public:
    virtual ~MediaListener() = default;

    // The now-playing state changed (sent once when the link binds the sink and on
    // every subsequent change). On a content_token change, fetch the art + rendered
    // text; a state-only change (play<->pause) just updates the transport glyph.
    virtual void on_media_update(Link* /*link*/, const MediaState& /*state*/) = 0;
};

class Link : public adb::StreamListener,
             public std::enable_shared_from_this<Link> {
public:
    ~Link() override;

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Open the agent link over an Online client: A_OPEN
    // localabstract:tab5adb-agent and start the framing parser, answering the
    // agent's HELLO with `cfg`. `listener` (the owner) gets HELLO / close. Returns
    // a shared_ptr immediately (before the device's A_OKAY); nullptr if the client
    // is not Online or the stream can't open. Hold the returned shared_ptr to keep
    // the link alive; drop it (and the listener's shared_ptr) to detach.
    static std::shared_ptr<Link> open(std::shared_ptr<adb::Client> client,
                                      std::weak_ptr<LinkLifecycleListener> listener,
                                      const HelloConfig& cfg = {});

    bool is_open() const;

    // Register (or clear, with an empty weak_ptr) the video-channel listener. Held
    // weakly. Callable from any thread; the new listener takes effect for the next
    // strip dispatched on the reader thread.
    void set_video_listener(std::weak_ptr<VideoListener> video);

    // Register (or clear, with an empty weak_ptr) the audio-channel listener. Held
    // weakly. Callable from any thread; takes effect for the next AUDIO frame /
    // on_audio_started dispatched on the reader thread. The AUDIO analogue of
    // set_video_listener.
    void set_audio_listener(std::weak_ptr<AudioListener> audio);

    // Register (or clear, with an empty weak_ptr) the media-channel listener. Held
    // weakly. Callable from any thread; takes effect for the next MEDIA event
    // dispatched on the reader thread. The agent pushes the current state right
    // after the link binds, so registering before/around on_link_hello gets the
    // initial state without a separate GET_MEDIA_INFO.
    void set_media_listener(std::weak_ptr<MediaListener> media);

    // Start mirroring: send MIRROR_START (§4.4) with `cfg` (panel size / scale
    // mode / streams). Non-blocking — the agent's response arrives as
    // on_mirror_started and JPEG strips then flow as on_video_strip (to the video
    // listener), both on the reader thread. Call after on_link_hello (READY).
    // Returns QueueFull on backpressure, StreamClosed if the link is down, Ok
    // otherwise.
    adb::Error start_mirror(const MirrorConfig& cfg = {});

    // Stop mirroring: send MIRROR_STOP (§4.4). The agent stops the JPEG stream and
    // returns to READY; the LINK STAYS OPEN, so a later start_mirror() resumes it.
    // Non-blocking, idempotent on the wire (the agent treats stop-when-stopped as
    // OK). The feature should also clear its video listener to stop receiving any
    // strips still in flight. Returns StreamClosed if the link is down, Ok otherwise.
    adb::Error stop_mirror();

    // One-shot control request completion (the `adb` one-shot archetype). Fires
    // exactly once on the reader thread. err == Ok means a response arrived:
    // `status` is the wire status (§4.5) and result/len its result bytes (valid
    // only for the duration of the call — copy what you keep). err != Ok is a
    // transport-level failure: StreamClosed (the link dropped, also fired for
    // every pending request on close) or Timeout. Marshalling to LVGL is the
    // caller's job, as with every agent_link callback.
    using RequestCallback = std::function<void(adb::Error err, uint8_t status,
                                               const uint8_t* result, size_t len)>;

    // Send a generic CONTROL_REQUEST (§4.1) — the typed siblings of
    // start_mirror() for the registry's one-shot commands (GET_APP_LIST,
    // GET_APP_ICON, ...). req_id correlation is handled here; concurrent
    // requests are fine. Non-blocking, callable from any thread. Timeouts are
    // swept lazily (on link traffic / the next request), so an expired callback
    // can fire later than deadline + idle time; the link close fails everything
    // promptly. Returns StreamClosed/QueueFull like the other senders — on a
    // non-Ok return `cb` is NOT kept (it never fires).
    adb::Error request(uint8_t cmd, const uint8_t* args, size_t args_len,
                       RequestCallback cb, uint32_t timeout_ms = 3000);

    // Inject one key event on the source device (TYPE=INPUT, §4.7). The agent
    // forwards `keycode` (an Android KeyEvent.KEYCODE_* value, e.g. kKeyBack) to
    // the hidden InputManager.injectInputEvent. Fire-and-forget: non-blocking, no
    // response, no state change on the link. Callable from any thread (e.g. an
    // overlay button on the LVGL thread). Returns StreamClosed if the link is down.
    adb::Error inject_key(uint32_t keycode, uint8_t action, uint32_t repeat = 0,
                          uint32_t meta = 0);

    // Inject a key tap = down then up (§4.7). The convenience used by the overlay's
    // power / volume / nav buttons. Returns the first non-Ok of the two sends.
    adb::Error tap_key(uint32_t keycode);

    // Inject one per-pointer touch transition on the source device (TYPE=INPUT,
    // input_type=TOUCH, §4.7). `action` is kTouchDown/kTouchMove/kTouchUp,
    // `pointer_id` is the source controller's track id, and (x,y) are Tab5 PANEL
    // coordinates — the agent inverts the mirror geometry to the source's logical
    // display coords and assembles the multi-pointer MotionEvent itself. Like
    // inject_key: fire-and-forget, non-blocking, no response, callable from any
    // thread (e.g. the DisplayManager touch task). Returns StreamClosed if down.
    adb::Error inject_touch(uint8_t action, uint8_t pointer_id, uint16_t x,
                            uint16_t y);

    // One per-pointer touch transition for inject_touch_batch.
    struct TouchSample {
        uint8_t action;      // kTouchDown / kTouchMove / kTouchUp
        uint8_t pointer_id;  // source touch-controller track id
        uint16_t x, y;       // Tab5 panel coords [px]
    };

    // Inject several per-pointer touch transitions in ONE INPUT frame
    // (input_type=kInputTouchBatch, §4.7) — the agent replays them in order through
    // the same per-pointer state machine as inject_touch, so it is semantically a
    // run of inject_touch calls but a single A_WRTE. The caller batches the samples
    // that pile up while the link is mid-round-trip (see tx_pending_bytes) and
    // flushes them when it goes idle, cutting a fast drag from one frame per touch
    // sample to ~one per RTT on a slow link. Fire-and-forget like inject_touch.
    // `n` must be 1..kTouchBatchMax. Returns StreamClosed if down, QueueFull on
    // backpressure, Ok otherwise.
    adb::Error inject_touch_batch(const TouchSample* samples, size_t n);

    // Bytes still un-acknowledged on the link's writer (queued + in flight); 0 =
    // the link is idle. The caller uses this to decide when to flush a touch batch
    // (idle = flush now). Callable from any thread. 0 if the link is down.
    size_t tx_pending_bytes() const;

    // End the link (A_CLSE the stream). Idempotent. on_link_close follows from
    // the reader thread.
    void close();

    // adb::StreamListener — reader thread.
    void on_stream_data(adb::Stream* st, const uint8_t* data, size_t len) override;
    void on_stream_close(adb::Stream* st, adb::Error err) override;

private:
    Link(std::weak_ptr<LinkLifecycleListener> listener, const HelloConfig& cfg);

    // Parse loop (reader thread): accumulate bytes, dispatch whole frames.
    void feed(const uint8_t* data, size_t len);
    void on_frame(const FrameHeader& h, const uint8_t* payload);
    void handle_control_request(const uint8_t* payload, size_t len);
    void handle_control_response(const uint8_t* payload, size_t len);
    void handle_event(const uint8_t* payload, size_t len);
    void handle_jpeg(const FrameHeader& h, const uint8_t* payload);
    void handle_audio(const FrameHeader& h, const uint8_t* payload);
    void send_hello_response(uint8_t req_id, uint8_t status);
    void fail(adb::Error err);          // protocol error: close + notify
    void fire_close_once(adb::Error err);

    // Generic request bookkeeping: take the pending entry for req_id (empty cmd
    // if none), expire stale entries, fail everything on close. Callbacks always
    // run outside req_mtx_.
    struct PendingRequest {
        uint8_t cmd = 0;
        RequestCallback cb;
        std::chrono::steady_clock::time_point deadline;
    };
    void sweep_expired_requests();  // reader thread / request() callers
    void fail_all_requests(adb::Error err);

    std::shared_ptr<adb::Client> client_;  // kept alive for the link's lifetime
    std::shared_ptr<adb::Stream> stream_;
    std::weak_ptr<LinkLifecycleListener> listener_;  // owner: HELLO + close
    // Video channel listener (the feature). Set/cleared from any thread, read on
    // the reader thread, so guarded by video_mtx_.
    std::weak_ptr<VideoListener> video_;
    mutable std::mutex video_mtx_;
    // Audio channel listener (the feature). Same model as video_: set/cleared from
    // any thread, read on the reader thread, guarded by audio_mtx_.
    std::weak_ptr<AudioListener> audio_;
    mutable std::mutex audio_mtx_;
    // Media channel listener (the feature). Same model as video_/audio_: set/cleared
    // from any thread, read on the reader thread, guarded by media_mtx_.
    std::weak_ptr<MediaListener> media_;
    mutable std::mutex media_mtx_;
    HelloConfig cfg_;

    std::vector<uint8_t> rx_;  // frame accumulator (reader thread only)
    // Outgoing frame counter. Touched by the reader thread (HELLO response), the
    // app thread (start_mirror), and the touch task thread (inject_touch /
    // inject_key), so atomic; each stream_->write() enqueues a whole frame, so
    // frames never interleave on the wire.
    std::atomic<uint8_t> tx_seq_{0};
    bool hello_done_ = false;
    std::atomic<bool> close_notified_{false};  // on_link_close fires once

    // In-flight generic requests, keyed by req_id (guarded by req_mtx_; ids
    // 0x10.. cycle, clear of the fixed mirror ids 0x02/0x03).
    std::mutex req_mtx_;
    std::vector<std::pair<uint8_t, PendingRequest>> pending_;
    uint8_t next_req_id_ = 0x10;
};

}  // namespace agent_link
