#include "agent_link.hpp"

#include <cstring>

#include "adb_client.hpp"  // adb::Client, adb::ConnectionState

namespace agent_link {

namespace {
constexpr const char* kSocket = "localabstract:tab5adb-agent";
}  // namespace

Link::Link(std::weak_ptr<LinkLifecycleListener> listener, const HelloConfig& cfg)
    : listener_(std::move(listener)), cfg_(cfg) {}

Link::~Link() {
    // close() the stream so its writer task joins; the stream holds the listener
    // (this Link) weakly, so no callback runs once our shared_ptr is gone.
    if (stream_) stream_->close();
}

std::shared_ptr<Link> Link::open(std::shared_ptr<adb::Client> client,
                                 std::weak_ptr<LinkLifecycleListener> listener,
                                 const HelloConfig& cfg) {
    if (!client || client->state() != adb::ConnectionState::Online) return nullptr;

    auto link = std::shared_ptr<Link>(new Link(std::move(listener), cfg));
    link->client_ = std::move(client);

    // The Link is the StreamListener (held weakly by the stream); dropping the
    // app's shared_ptr to the Link detaches the callbacks racelessly.
    auto stream = link->client_->open_stream(
        kSocket, std::weak_ptr<adb::StreamListener>(link));
    if (!stream) return nullptr;
    link->stream_ = std::move(stream);
    return link;
}

bool Link::is_open() const { return stream_ && stream_->is_open(); }

void Link::set_video_listener(std::weak_ptr<VideoListener> video) {
    std::lock_guard<std::mutex> lk(video_mtx_);
    video_ = std::move(video);
}

adb::Error Link::start_mirror(const MirrorConfig& cfg) {
    if (!stream_ || !stream_->is_open()) return adb::Error::StreamClosed;

    // CONTROL_REQUEST payload (§4.1): cmd, req_id, then MIRROR_START args (§4.4).
    // req_id 0x02 (HELLO used 0x01); only one MIRROR_START is ever in flight, so
    // the response is matched by cmd alone.
    uint8_t payload[2 + kMirrorStartArgsLen];
    payload[0] = kCmdMirrorStart;
    payload[1] = 0x02;
    uint8_t* a = payload + 2;
    wr_u16(a + 0, cfg.target_width);
    wr_u16(a + 2, cfg.target_height);
    a[4] = cfg.scale_mode;
    a[5] = cfg.streams;
    wr_u16(a + 6, 0);  // reserved

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeControlRequest, /*flags=*/0,
                 tx_seq_.fetch_add(1), static_cast<uint32_t>(sizeof(payload)));
    std::memcpy(frame + kFrameHeaderSize, payload, sizeof(payload));
    return stream_->write(frame, sizeof(frame));
}

adb::Error Link::stop_mirror() {
    if (!stream_ || !stream_->is_open()) return adb::Error::StreamClosed;

    // CONTROL_REQUEST payload (§4.1): cmd, req_id, no args (MIRROR_STOP, §4.4).
    // req_id 0x03; the response is matched by cmd alone.
    uint8_t payload[2];
    payload[0] = kCmdMirrorStop;
    payload[1] = 0x03;

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeControlRequest, /*flags=*/0,
                 tx_seq_.fetch_add(1), static_cast<uint32_t>(sizeof(payload)));
    std::memcpy(frame + kFrameHeaderSize, payload, sizeof(payload));
    return stream_->write(frame, sizeof(frame));
}

adb::Error Link::inject_key(uint32_t keycode, uint8_t action, uint32_t repeat,
                            uint32_t meta) {
    if (!stream_ || !stream_->is_open()) return adb::Error::StreamClosed;

    // INPUT payload (§4.7): input_type, then the INPUT_KEY args. No req_id / no
    // response — input is a one-way fire-and-forget channel (TYPE=INPUT).
    uint8_t payload[1 + kInputKeyArgsLen];
    payload[0] = kInputKey;
    payload[1] = action;
    wr_u32(payload + 2, keycode);
    wr_u32(payload + 6, repeat);
    wr_u32(payload + 10, meta);

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeInput, /*flags=*/0, tx_seq_.fetch_add(1),
                 static_cast<uint32_t>(sizeof(payload)));
    std::memcpy(frame + kFrameHeaderSize, payload, sizeof(payload));
    return stream_->write(frame, sizeof(frame));
}

adb::Error Link::tap_key(uint32_t keycode) {
    adb::Error e = inject_key(keycode, kKeyActionDown);
    if (e != adb::Error::Ok) return e;
    return inject_key(keycode, kKeyActionUp);
}

adb::Error Link::inject_touch(uint8_t action, uint8_t pointer_id, uint16_t x,
                              uint16_t y) {
    if (!stream_ || !stream_->is_open()) return adb::Error::StreamClosed;

    // INPUT payload (§4.7): input_type, then the INPUT_TOUCH args. Fire-and-forget
    // (TYPE=INPUT) — no req_id / no response. Coordinates are Tab5 panel coords.
    uint8_t payload[1 + kInputTouchArgsLen];
    payload[0] = kInputTouch;
    payload[1] = action;
    payload[2] = pointer_id;
    payload[3] = 0;  // reserved
    wr_u16(payload + 4, x);
    wr_u16(payload + 6, y);

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeInput, /*flags=*/0, tx_seq_.fetch_add(1),
                 static_cast<uint32_t>(sizeof(payload)));
    std::memcpy(frame + kFrameHeaderSize, payload, sizeof(payload));
    return stream_->write(frame, sizeof(frame));
}

void Link::close() {
    if (stream_) stream_->close();  // A_CLSE; on_stream_close drives on_link_close
}

void Link::on_stream_data(adb::Stream*, const uint8_t* data, size_t len) {
    feed(data, len);
}

void Link::on_stream_close(adb::Stream*, adb::Error err) { fire_close_once(err); }

void Link::feed(const uint8_t* data, size_t len) {
    rx_.insert(rx_.end(), data, data + len);

    size_t off = 0;
    while (rx_.size() - off >= kFrameHeaderSize) {
        FrameHeader h;
        if (!parse_header(rx_.data() + off, h)) {
            // MAGIC mismatch on a reliable stream = corruption/bug (§8): abort.
            fail(adb::Error::Protocol);
            return;
        }
        if (h.length > cfg_.max_payload) {
            fail(adb::Error::Protocol);
            return;
        }
        if (rx_.size() - off < kFrameHeaderSize + h.length) break;  // need more

        on_frame(h, rx_.data() + off + kFrameHeaderSize);
        if (close_notified_.load()) return;  // a handler tore the link down
        off += kFrameHeaderSize + h.length;
    }
    if (off > 0) rx_.erase(rx_.begin(), rx_.begin() + off);
}

void Link::on_frame(const FrameHeader& h, const uint8_t* payload) {
    switch (h.type) {
        case kTypeControlRequest:
            handle_control_request(payload, h.length);
            break;
        case kTypeControlResponse:
            handle_control_response(payload, h.length);
            break;
        case kTypeEvent:
            handle_event(payload, h.length);
            break;
        case kTypeJpeg:
            handle_jpeg(h, payload);
            break;
        // AUDIO arrives in a later slice; unknown TYPEs are ignored for forward
        // compatibility (§3.1).
        default:
            break;
    }
}

void Link::handle_control_request(const uint8_t* p, size_t len) {
    if (len < 2) {
        fail(adb::Error::Protocol);
        return;
    }
    const uint8_t cmd = p[0];
    const uint8_t req_id = p[1];
    if (cmd != kCmdHello) return;  // unknown cmd: ignore (forward compat, §4.4)

    if (len < 2 + kHelloArgsLen) {
        fail(adb::Error::Protocol);
        return;
    }
    const uint8_t* a = p + 2;
    AgentInfo info;
    info.proto_version = a[0];
    info.version_major = a[1];
    info.version_minor = a[2];
    info.version_patch = a[3];
    info.capabilities = rd_u16(a + 4);
    // a[6..7] reserved.

    if (info.proto_version != kProtoVersion) {
        send_hello_response(req_id, kStatusEnotsup);  // §4.4: mismatch -> ENOTSUP
        fail(adb::Error::Protocol);                   // ... then close the stream
        return;
    }

    send_hello_response(req_id, kStatusOk);
    hello_done_ = true;
    if (auto l = listener_.lock()) l->on_link_hello(this, info);
}

// CONTROL_RESPONSE (§4.2): replies to a Tab5-initiated request — MIRROR_START or
// MIRROR_STOP.
void Link::handle_control_response(const uint8_t* p, size_t len) {
    if (len < 3) {
        fail(adb::Error::Protocol);
        return;
    }
    const uint8_t cmd = p[0];
    const uint8_t status = p[2];

    // MIRROR_STOP ack: the agent is back in READY. The feature has already (or is
    // about to) clear its video listener, so there is nothing to dispatch; a
    // non-OK status doesn't tear the link down (stop is best-effort, the link
    // stays usable). Just consume it.
    if (cmd == kCmdMirrorStop) return;

    if (cmd != kCmdMirrorStart) return;  // unknown cmd: ignore (forward compat)

    if (status != kStatusOk) {  // agent refused MIRROR_START
        fail(adb::Error::Protocol);
        return;
    }
    if (len < 3 + kMirrorStartResultLen) {
        fail(adb::Error::Protocol);
        return;
    }
    const uint8_t* r = p + 3;
    MirrorInfo info;
    info.source_width = rd_u16(r + 0);
    info.source_height = rd_u16(r + 2);
    info.video_codec = r[4];
    // r[5..7] reserved.
    std::shared_ptr<VideoListener> v;
    { std::lock_guard<std::mutex> lk(video_mtx_); v = video_.lock(); }
    if (v) v->on_mirror_started(this, info);
}

// EVENT (§4.3): an async agent->Tab5 notification. payload = event(u8) + data.
// Unknown events are ignored (forward compat, §8). Dispatched to the video
// listener (orientation drives the feature's overlay layout).
void Link::handle_event(const uint8_t* p, size_t len) {
    if (len < 1) {
        fail(adb::Error::Protocol);
        return;
    }
    const uint8_t event = p[0];
    if (event != kEventOrientation) return;  // unknown event: ignore (forward compat)
    if (len < 1 + kOrientationDataLen) {
        fail(adb::Error::Protocol);
        return;
    }
    OrientationInfo info;
    info.rotation = p[1];
    info.landscape = rotation_is_landscape(info.rotation);
    // p[2..4] reserved.
    std::shared_ptr<VideoListener> v;
    { std::lock_guard<std::mutex> lk(video_mtx_); v = video_.lock(); }
    if (v) v->on_orientation(this, info);
}

// JPEG (§5.2): one strip = subheader (x,y,w,h) + JPEG bytes. The frame layer has
// already reassembled the whole strip into `payload`; hand it to the decode +
// framebuffer seam (the listener).
void Link::handle_jpeg(const FrameHeader& h, const uint8_t* payload) {
    if (h.length < kJpegSubheaderSize) {
        fail(adb::Error::Protocol);
        return;
    }
    JpegSubheader s;
    parse_jpeg_subheader(payload, s);
    VideoStrip strip;
    strip.x = s.x;
    strip.y = s.y;
    strip.w = s.w;
    strip.h = s.h;
    strip.jpeg = payload + kJpegSubheaderSize;
    strip.jpeg_len = h.length - kJpegSubheaderSize;
    strip.frame_start = (h.flags & kFlagFrameStart) != 0;
    strip.frame_end = (h.flags & kFlagFrameEnd) != 0;
    std::shared_ptr<VideoListener> v;
    { std::lock_guard<std::mutex> lk(video_mtx_); v = video_.lock(); }
    if (v) v->on_video_strip(this, strip);
}

void Link::send_hello_response(uint8_t req_id, uint8_t status) {
    // CONTROL_RESPONSE payload (§4.2): cmd, req_id, status, then result (§4.4),
    // which is only present/valid on status == OK. Link-only: proto / caps /
    // max_payload — no mirror params (those go in MIRROR_START).
    uint8_t payload[3 + kHelloResultLen];
    payload[0] = kCmdHello;
    payload[1] = req_id;
    payload[2] = status;
    uint8_t* r = payload + 3;
    r[0] = kProtoVersion;
    r[1] = 0;  // reserved
    wr_u16(r + 2, cfg_.capabilities);
    wr_u32(r + 4, cfg_.max_payload);

    const size_t plen = (status == kStatusOk) ? sizeof(payload) : 3;

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeControlResponse, /*flags=*/0, tx_seq_.fetch_add(1),
                 static_cast<uint32_t>(plen));
    std::memcpy(frame + kFrameHeaderSize, payload, plen);
    if (stream_) stream_->write(frame, kFrameHeaderSize + plen);
}

void Link::fail(adb::Error err) {
    if (stream_) stream_->close();
    fire_close_once(err);
}

void Link::fire_close_once(adb::Error err) {
    if (close_notified_.exchange(true)) return;
    if (auto l = listener_.lock()) l->on_link_close(this, err);
}

}  // namespace agent_link
