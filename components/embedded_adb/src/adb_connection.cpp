#include "adb_connection.hpp"

#include "esp_log.h"

#include <cinttypes>
#include <cstring>

namespace adb {

static const char* TAG = "adb/conn";

// Host identity sent in CNXN. Trailing NUL matches what devices expect; they
// strip it when parsing the banner.
static const char kHostBanner[] = "host::features=shell_v2,cmd";

const char* to_string(ConnectionState s) {
    switch (s) {
        case ConnectionState::Offline: return "Offline";
        case ConnectionState::Connecting: return "Connecting";
        case ConnectionState::Authorizing: return "Authorizing";
        case ConnectionState::Unauthorized: return "Unauthorized";
        case ConnectionState::Online: return "Online";
        case ConnectionState::Closed: return "Closed";
    }
    return "?";
}

AdbConnection::AdbConnection(std::unique_ptr<Transport> transport, RsaKey key,
                             uint32_t advertised_max_payload)
    : transport_(std::move(transport)), key_(std::move(key)),
      max_payload_(advertised_max_payload) {}

AdbConnection::~AdbConnection() { stop(); }

void AdbConnection::set_state(ConnectionState s) {
    if (state_ == s) return;
    state_ = s;
    ESP_LOGI(TAG, "state -> %s", to_string(s));
    if (state_cb_) state_cb_(s);
}

bool AdbConnection::send(const Packet& p) {
    std::lock_guard<std::mutex> lk(write_mtx_);
    return transport_ && transport_->write_packet(p);
}

void AdbConnection::send_connect() {
    std::vector<uint8_t> payload(kHostBanner, kHostBanner + sizeof(kHostBanner));  // incl NUL
    auto p = Packet::make(A_CNXN, A_VERSION, max_payload_, std::move(payload));
    send(p);
    set_state(ConnectionState::Connecting);
}

void AdbConnection::run_blocking() {
    if (!transport_ || stop_requested_.load()) {
        set_state(ConnectionState::Closed);
        return;
    }
    running_ = true;
    send_connect();

    while (running_ && !stop_requested_.load()) {
        Packet p;
        IoResult r = transport_->read_packet(p);
        if (r == IoResult::Timeout) {
            continue;  // idle bus (e.g. waiting for the user to authorize)
        }
        if (r == IoResult::Error) {
            ESP_LOGE(TAG, "read error; closing");
            break;
        }
        handle_packet(p);
    }

    running_ = false;

    // Close any still-open streams so their owners get a terminal callback
    // (one-shot completions / session on_close fire exactly once on teardown).
    // Copy out and clear under the lock, then notify outside it so an on_close
    // that re-enters the connection can't deadlock on streams_mtx_.
    std::vector<std::shared_ptr<AdbStream>> pending;
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        for (auto& kv : streams_) pending.push_back(kv.second);
        streams_.clear();
    }
    for (auto& s : pending) s->mark_closed();

    set_state(ConnectionState::Closed);
}

void AdbConnection::stop() {
    stop_requested_.store(true);
    running_ = false;
}

void AdbConnection::handle_packet(Packet& p) {
    switch (p.header.command) {
        case A_CNXN: on_connect(p); break;
        case A_AUTH: on_auth(p); break;
        case A_STLS:
            ESP_LOGW(TAG, "device requested TLS (A_STLS) — not supported yet");
            running_ = false;
            break;
        case A_OPEN:
        case A_OKAY:
        case A_WRTE:
        case A_CLSE:
            on_stream_packet(p);
            break;
        default:
            ESP_LOGW(TAG, "unknown command 0x%08" PRIx32, p.header.command);
            break;
    }
}

void AdbConnection::on_auth(Packet& p) {
    if (p.header.arg0 != ADB_AUTH_TOKEN) {
        ESP_LOGW(TAG, "unexpected AUTH arg0=%" PRIu32, p.header.arg0);
        return;
    }
    set_state(ConnectionState::Authorizing);

    if (!sent_signature_) {
        // First challenge: sign the token with our key.
        std::vector<uint8_t> sig;
        if (key_.sign_token(p.payload.data(), p.payload.size(), sig)) {
            send(Packet::make(A_AUTH, ADB_AUTH_SIGNATURE, 0, std::move(sig)));
            sent_signature_ = true;
            ESP_LOGI(TAG, "sent signature");
        } else {
            ESP_LOGE(TAG, "sign_token failed");
            running_ = false;
        }
        return;
    }

    if (!sent_pubkey_) {
        // Device didn't recognize our signature: offer our public key so the user
        // can authorize it. This triggers the on-device "Allow USB debugging?".
        std::string pub;
        if (key_.android_public_key(pub)) {
            std::vector<uint8_t> payload(pub.begin(), pub.end());
            payload.push_back('\0');
            send(Packet::make(A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, std::move(payload)));
            sent_pubkey_ = true;
            set_state(ConnectionState::Unauthorized);
            ESP_LOGI(TAG, "sent public key — waiting for user to authorize");
        } else {
            ESP_LOGE(TAG, "android_public_key failed");
            running_ = false;
        }
        return;
    }

    // Already sent the pubkey; the device re-sends TOKEN while it waits for the
    // user. Nothing to do but keep waiting.
    ESP_LOGD(TAG, "awaiting authorization (token re-sent)");
}

std::shared_ptr<AdbStream> AdbConnection::find_stream(uint32_t local_id) {
    std::lock_guard<std::mutex> lk(streams_mtx_);
    auto it = streams_.find(local_id);
    return it == streams_.end() ? nullptr : it->second;
}

void AdbConnection::on_stream_packet(Packet& p) {
    // For packets from the device: arg0 = device's id (our "remote"), arg1 = the
    // id we assigned ("local").
    uint32_t remote = p.header.arg0;
    uint32_t local = p.header.arg1;

    switch (p.header.command) {
        case A_OPEN:
            // Device wants to open a stream to us (reverse). We are a client and
            // host no services: reject it.
            send(Packet::make(A_CLSE, 0, remote));
            break;

        case A_OKAY:
            if (auto s = find_stream(local)) s->on_okay(remote);
            break;

        case A_WRTE:
            if (auto s = find_stream(local)) {
                s->deliver(p.payload.data(), p.payload.size());
                send(Packet::make(A_OKAY, local, remote));  // ack: ready for more
            } else {
                send(Packet::make(A_CLSE, 0, remote));  // unknown stream
            }
            break;

        case A_CLSE:
            if (auto s = find_stream(local)) {
                s->mark_closed();
                {
                    std::lock_guard<std::mutex> lk(streams_mtx_);
                    streams_.erase(local);
                }
                send(Packet::make(A_CLSE, local, remote));  // confirm close
            }
            break;
    }
}

std::shared_ptr<AdbStream> AdbConnection::open_stream(const std::string& service,
                                                     AdbStream::DataCb on_data,
                                                     AdbStream::CloseCb on_close) {
    if (state_ != ConnectionState::Online) return nullptr;

    uint32_t local;
    std::shared_ptr<AdbStream> s;
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        local = next_local_id_++;
        s = std::shared_ptr<AdbStream>(
            new AdbStream(this, local, std::move(on_data), std::move(on_close)));
        streams_[local] = s;
    }

    std::vector<uint8_t> payload(service.begin(), service.end());
    payload.push_back('\0');  // services are NUL-terminated on the wire
    send(Packet::make(A_OPEN, local, 0, std::move(payload)));
    return s;
}

bool AdbConnection::run_service(const std::string& service, std::string& output,
                               int timeout_ms) {
    std::string acc;
    std::mutex acc_mtx;
    auto s = open_stream(service,
                         [&](const uint8_t* d, size_t n) {
                             std::lock_guard<std::mutex> lk(acc_mtx);
                             acc.append(reinterpret_cast<const char*>(d), n);
                         },
                         nullptr);
    if (!s) return false;

    if (!s->wait_closed(timeout_ms)) {
        s->close();  // timed out — close and fall through with what we have
    }
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        streams_.erase(s->local_id());
    }
    {
        std::lock_guard<std::mutex> lk(acc_mtx);
        output = std::move(acc);
    }
    return s->was_opened();
}

bool AdbConnection::send_write(AdbStream* s, const uint8_t* data, size_t len) {
    std::vector<uint8_t> v(data, data + len);
    return send(Packet::make(A_WRTE, s->local_id_, s->remote_id_, std::move(v)));
}

void AdbConnection::send_close(AdbStream* s) {
    send(Packet::make(A_CLSE, s->local_id_, s->remote_id_));
}

void AdbConnection::on_connect(Packet& p) {
    // arg0 = device version, arg1 = device max payload. Negotiate the smaller.
    uint32_t their_max = p.header.arg1;
    if (their_max && their_max < max_payload_) max_payload_ = their_max;
    banner_.assign(reinterpret_cast<const char*>(p.payload.data()), p.payload.size());
    // strip a trailing NUL if present
    if (!banner_.empty() && banner_.back() == '\0') banner_.pop_back();
    ESP_LOGI(TAG, "CONNECTED maxdata=%" PRIu32 " banner=%s", max_payload_, banner_.c_str());
    set_state(ConnectionState::Online);
}

}  // namespace adb
