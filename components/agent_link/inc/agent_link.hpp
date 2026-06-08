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
// Threading: the frame parser runs on adb's reader thread, so LinkListener
// callbacks fire there; marshalling to LVGL is the app's job (the headless test
// has no LVGL and uses them directly).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "adb_error.hpp"          // adb::Error
#include "adb_raw_stream.hpp"     // adb::Stream, adb::StreamListener
#include "agent_link_protocol.hpp"  // FrameHeader, wire constants

namespace adb {
class Client;
}

namespace agent_link {

// The agent's HELLO (its self-introduction, protocol.md §4.4 request args).
struct AgentInfo {
    uint8_t proto_version = 0;
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    uint8_t version_patch = 0;
    uint16_t source_width = 0;   // physical source width [px] (informational)
    uint16_t source_height = 0;  // physical source height [px] (informational)
    uint8_t video_codec = 0;     // 0x01 = JPEG(YUV420)
};

// Tab5's HELLO response params (protocol.md §4.4 response result). Defaults match
// the Tab5 panel; max_payload defaults to the simulator/libusb value (device
// advertises 16 KiB, so the app overrides this).
struct HelloConfig {
    uint32_t max_payload = 256 * 1024;
    uint16_t target_width = 720;
    uint16_t target_height = 1280;
    uint8_t scale_mode = 0;  // 0 = fit (default), 1 = fill (§5.3)
};

class Link;

// Link delegate. Callbacks fire on the reader thread.
class LinkListener {
public:
    virtual ~LinkListener() = default;

    // The HELLO handshake completed: the agent's HELLO was received, proto
    // matched, and our response was sent. Fires once, before any media. The
    // Link* first arg lets one listener serve several links.
    virtual void on_link_hello(Link* link, const AgentInfo& info) = 0;

    // The link closed (peer/our close(), Client::close() teardown, or a protocol
    // error). Fires exactly once; err == Protocol on a framing/proto-mismatch
    // error, otherwise Ok.
    virtual void on_link_close(Link* link, adb::Error err) = 0;
};

class Link : public adb::StreamListener,
             public std::enable_shared_from_this<Link> {
public:
    ~Link() override;

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Open the agent link over an Online client: A_OPEN
    // localabstract:tab5adb-agent and start the framing parser, answering the
    // agent's HELLO with `cfg`. Returns a shared_ptr immediately (before the
    // device's A_OKAY); nullptr if the client is not Online or the stream can't
    // open. Hold the returned shared_ptr to keep the link alive; drop it (and the
    // listener's shared_ptr) to detach.
    static std::shared_ptr<Link> open(std::shared_ptr<adb::Client> client,
                                      std::weak_ptr<LinkListener> listener,
                                      const HelloConfig& cfg = {});

    bool is_open() const;

    // End the link (A_CLSE the stream). Idempotent. on_link_close follows from
    // the reader thread.
    void close();

    // adb::StreamListener — reader thread.
    void on_stream_data(adb::Stream* st, const uint8_t* data, size_t len) override;
    void on_stream_close(adb::Stream* st, adb::Error err) override;

private:
    Link(std::weak_ptr<LinkListener> listener, const HelloConfig& cfg);

    // Parse loop (reader thread): accumulate bytes, dispatch whole frames.
    void feed(const uint8_t* data, size_t len);
    void on_frame(const FrameHeader& h, const uint8_t* payload);
    void handle_control_request(const uint8_t* payload, size_t len);
    void send_hello_response(uint8_t req_id, uint8_t status);
    void fail(adb::Error err);          // protocol error: close + notify
    void fire_close_once(adb::Error err);

    std::shared_ptr<adb::Client> client_;  // kept alive for the link's lifetime
    std::shared_ptr<adb::Stream> stream_;
    std::weak_ptr<LinkListener> listener_;
    HelloConfig cfg_;

    std::vector<uint8_t> rx_;  // frame accumulator (reader thread only)
    uint8_t tx_seq_ = 0;       // outgoing frame counter (reader thread only)
    bool hello_done_ = false;
    std::atomic<bool> close_notified_{false};  // on_link_close fires once
};

}  // namespace agent_link
