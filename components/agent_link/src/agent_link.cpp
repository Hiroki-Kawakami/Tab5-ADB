#include "agent_link.hpp"

#include <cstring>

#include "adb_client.hpp"  // adb::Client, adb::ConnectionState

namespace agent_link {

namespace {
constexpr const char* kSocket = "localabstract:tab5adb-agent";
}  // namespace

Link::Link(std::weak_ptr<LinkListener> listener, const HelloConfig& cfg)
    : listener_(std::move(listener)), cfg_(cfg) {}

Link::~Link() {
    // close() the stream so its writer task joins; the stream holds the listener
    // (this Link) weakly, so no callback runs once our shared_ptr is gone.
    if (stream_) stream_->close();
}

std::shared_ptr<Link> Link::open(std::shared_ptr<adb::Client> client,
                                 std::weak_ptr<LinkListener> listener,
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
        // JPEG / AUDIO / EVENT arrive in later slices; unknown TYPEs are ignored
        // for forward compatibility (§3.1).
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
    info.source_width = rd_u16(a + 4);
    info.source_height = rd_u16(a + 6);
    info.video_codec = a[8];
    // a[9] reserved.

    if (info.proto_version != kProtoVersion) {
        send_hello_response(req_id, kStatusEnotsup);  // §4.4: mismatch -> ENOTSUP
        fail(adb::Error::Protocol);                   // ... then close the stream
        return;
    }

    send_hello_response(req_id, kStatusOk);
    hello_done_ = true;
    if (auto l = listener_.lock()) l->on_link_hello(this, info);
}

void Link::send_hello_response(uint8_t req_id, uint8_t status) {
    // CONTROL_RESPONSE payload (§4.2): cmd, req_id, status, then result (§4.4),
    // which is only present/valid on status == OK.
    uint8_t payload[3 + kHelloResultLen];
    payload[0] = kCmdHello;
    payload[1] = req_id;
    payload[2] = status;
    uint8_t* r = payload + 3;
    wr_u32(r + 0, cfg_.max_payload);
    wr_u16(r + 4, cfg_.target_width);
    wr_u16(r + 6, cfg_.target_height);
    r[8] = kProtoVersion;
    r[9] = cfg_.scale_mode;
    r[10] = 0;
    r[11] = 0;

    const size_t plen = (status == kStatusOk) ? sizeof(payload) : 3;

    uint8_t frame[kFrameHeaderSize + sizeof(payload)];
    write_header(frame, kTypeControlResponse, /*flags=*/0, tx_seq_++,
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
