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
//     features. Future AUDIO / EVENT channels add the same kind of set_* setter.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
// this) and a video-capable sink. Link-only — no mirror params here.
struct HelloConfig {
    uint32_t max_payload = 256 * 1024;
    uint16_t capabilities = kCapVideo;  // features Tab5 can accept (§4.6)
};

// Mirror parameters Tab5 hands the agent in MIRROR_START (protocol.md §4.4 args).
// Defaults match the Tab5 panel + fit mode, video only.
struct MirrorConfig {
    uint16_t target_width = 720;
    uint16_t target_height = 1280;
    uint8_t scale_mode = kScaleFit;
    uint8_t streams = kCapVideo;  // which streams to start (§4.6 bit assignment)
};

// The agent's MIRROR_START response (protocol.md §4.4 result): the source it is
// actually streaming.
struct MirrorInfo {
    uint16_t source_width = 0;   // physical source width [px] (informational)
    uint16_t source_height = 0;  // physical source height [px] (informational)
    uint8_t video_codec = 0;     // 0x01 = JPEG(YUV420)
};

// The source device's logical orientation (§4.4 ORIENTATION event). The Tab5
// keeps mirroring the device's natural-orientation framebuffer either way (§5.1);
// this only tells the feature how to lay out its overlay UI (portrait vs
// landscape), since the user physically turns the Tab5 to view a landscape app.
struct OrientationInfo {
    uint8_t rotation = 0;    // Surface.ROTATION_* (0/1/2/3 = 0/90/180/270)
    bool landscape = false;  // rotation is 90 or 270 (the panel is being viewed sideways)
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
    void send_hello_response(uint8_t req_id, uint8_t status);
    void fail(adb::Error err);          // protocol error: close + notify
    void fire_close_once(adb::Error err);

    std::shared_ptr<adb::Client> client_;  // kept alive for the link's lifetime
    std::shared_ptr<adb::Stream> stream_;
    std::weak_ptr<LinkLifecycleListener> listener_;  // owner: HELLO + close
    // Video channel listener (the feature). Set/cleared from any thread, read on
    // the reader thread, so guarded by video_mtx_.
    std::weak_ptr<VideoListener> video_;
    mutable std::mutex video_mtx_;
    HelloConfig cfg_;

    std::vector<uint8_t> rx_;  // frame accumulator (reader thread only)
    // Outgoing frame counter. Touched by the reader thread (HELLO response) and
    // the app thread (start_mirror), so atomic; each stream_->write() enqueues a
    // whole frame, so frames never interleave on the wire.
    std::atomic<uint8_t> tx_seq_{0};
    bool hello_done_ = false;
    std::atomic<bool> close_notified_{false};  // on_link_close fires once
};

}  // namespace agent_link
