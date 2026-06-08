#include "adb_sync.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "adb_connection.hpp"     // adb::AdbConnection
#include "adb_stream.hpp"         // adb::AdbStream
#include "adb_sync_protocol.hpp"  // adb::sync::ID_*

namespace adb {

namespace {

constexpr uint32_t kWorkerStack = 4096;  // framing + memcpy only, no crypto

void put_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

Sync::Sync(std::weak_ptr<SyncListener> listener) : listener_(std::move(listener)) {}

std::shared_ptr<Sync> Sync::create(AdbConnection* conn,
                                   std::weak_ptr<SyncListener> listener) {
    auto sy = std::shared_ptr<Sync>(new Sync(std::move(listener)));
    std::weak_ptr<Sync> weak = sy;

    // Stream callbacks hold only a weak ref so the connection's stream map does
    // not keep the Sync alive — the app's shared_ptr owns its lifetime.
    auto stream = conn->open_stream(
        "sync:",
        [weak](const uint8_t* d, size_t n) {
            if (auto s = weak.lock()) s->handle_data(d, n);
        },
        [weak]() {
            if (auto s = weak.lock()) s->handle_close();
        });
    if (!stream) return nullptr;  // offline / could not open

    sy->stream_ = std::move(stream);
    // One A_WRTE carries an 8-byte sync header + data and must fit max_payload;
    // also never exceed one sync DATA chunk.
    uint32_t mp = conn->max_payload();
    size_t room = mp > 8 ? mp - 8 : 0;
    sy->chunk_cap_ = std::min<size_t>(sync::DATA_MAX, room);
    sy->done_ = xSemaphoreCreateBinary();
    TaskHandle_t task = nullptr;
    xTaskCreate(&Sync::worker_trampoline, "adb_sync", kWorkerStack, sy.get(), 5,
                &task);
    sy->worker_task_ = task;
    return sy;
}

Sync::~Sync() { close(); }  // joins the worker; no callback runs after this

// --- queue / worker ---------------------------------------------------------

void Sync::enqueue(Op op) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!stopped_ && !stream_closed_) {
            queue_.push_back(std::move(op));
            cv_.notify_all();
            return;
        }
    }
    // Session already going down: fire the op's completion synchronously with
    // Cancelled (mirrors exec's synchronous NotConnected). Exactly-once holds.
    op(false);
}

void Sync::worker_trampoline(void* arg) { static_cast<Sync*>(arg)->worker_loop(); }

void Sync::worker_loop() {
    for (;;) {
        Op op;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] {
                return !queue_.empty() || stopped_ || stream_closed_;
            });
            if (queue_.empty()) break;  // stop/close with nothing left to do
            op = std::move(queue_.front());
            queue_.pop_front();
        }
        op(true);  // executes do_stat/do_push; fires its completion exactly once
    }

    // Drain anything still queued so every completion fires (with Cancelled).
    for (;;) {
        Op op;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty()) break;
            op = std::move(queue_.front());
            queue_.pop_front();
        }
        op(false);
    }

    fire_close_once(Error::Ok);
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}

// --- operations -------------------------------------------------------------

void Sync::stat(const std::string& path, std::function<void(Error, FileStat)> cb) {
    enqueue([this, path, cb = std::move(cb)](bool alive) {
        if (alive) do_stat(path, cb);
        else if (cb) cb(Error::Cancelled, FileStat{});
    });
}

void Sync::list(const std::string& path,
                std::function<void(Error, std::vector<DirEntry>)> cb) {
    enqueue([this, path, cb = std::move(cb)](bool alive) {
        if (alive) do_list(path, cb);
        else if (cb) cb(Error::Cancelled, {});
    });
}

void Sync::push(const std::string& remote_path, uint32_t perm, uint32_t mtime,
                SyncSource source, std::function<void(Error)> cb) {
    enqueue([this, remote_path, perm, mtime, source = std::move(source),
             cb = std::move(cb)](bool alive) {
        if (alive) do_push(remote_path, perm, mtime, source, cb);
        else if (cb) cb(Error::Cancelled);
    });
}

void Sync::do_stat(const std::string& path, std::function<void(Error, FileStat)> cb) {
    if (!write_request(sync::ID_STAT, path)) {
        if (cb) cb(Error::StreamClosed, FileStat{});
        return;
    }
    uint8_t buf[16];  // sync_stat_v1: id, mode, size, mtime
    if (!read_exact(buf, sizeof(buf))) {
        if (cb) cb(Error::StreamClosed, FileStat{});
        return;
    }
    if (get_le32(buf) != sync::ID_STAT) {
        if (cb) cb(Error::Protocol, FileStat{});
        return;
    }
    FileStat st;
    st.mode = get_le32(buf + 4);
    st.size = get_le32(buf + 8);
    st.mtime = get_le32(buf + 12);
    if (cb) cb(Error::Ok, st);
}

void Sync::do_list(const std::string& path,
                   std::function<void(Error, std::vector<DirEntry>)> cb) {
    if (!write_request(sync::ID_LIST, path)) {
        if (cb) cb(Error::StreamClosed, {});
        return;
    }
    // LIST v1 streams a sync_dent_v1 (id,mode,size,mtime,namelen) + name per
    // entry, terminated by a same-shaped header with id == DONE.
    std::vector<DirEntry> entries;
    for (;;) {
        uint8_t hdr[20];
        if (!read_exact(hdr, sizeof(hdr))) {
            if (cb) cb(Error::StreamClosed, {});
            return;
        }
        uint32_t id = get_le32(hdr);
        if (id == sync::ID_DONE) break;
        if (id != sync::ID_DENT) {
            if (cb) cb(Error::Protocol, {});
            return;
        }
        DirEntry e;
        e.mode = get_le32(hdr + 4);
        e.size = get_le32(hdr + 8);
        e.mtime = get_le32(hdr + 12);
        uint32_t namelen = get_le32(hdr + 16);
        if (namelen > sync::MAX_PATH_LEN) {
            if (cb) cb(Error::Protocol, {});
            return;
        }
        std::string name(namelen, '\0');
        if (namelen && !read_exact(&name[0], namelen)) {
            if (cb) cb(Error::StreamClosed, {});
            return;
        }
        e.name = std::move(name);
        entries.push_back(std::move(e));
    }
    if (cb) cb(Error::Ok, std::move(entries));
}

void Sync::do_push(const std::string& path, uint32_t perm, uint32_t mtime,
                   SyncSource source, std::function<void(Error)> cb) {
    // SEND v1: the request "path" is "<path>,<st_mode>" (decimal). Mark it a
    // regular file so adbd creates a plain file with `perm` bits.
    std::string spec = path + "," + std::to_string((perm & 07777) | 0100000);
    if (!write_request(sync::ID_SEND, spec)) {
        if (cb) cb(Error::StreamClosed);
        return;
    }

    // Stream the source in capped DATA chunks (header + data in one A_WRTE).
    std::vector<uint8_t> msg(8 + chunk_cap_);
    for (;;) {
        int n = source ? source(msg.data() + 8, chunk_cap_) : 0;
        if (n < 0) {  // source aborted
            if (cb) cb(Error::Transport);
            return;
        }
        if (n == 0) break;  // EOF
        put_le32(msg.data(), sync::ID_DATA);
        put_le32(msg.data() + 4, static_cast<uint32_t>(n));
        if (!send_bytes(msg.data(), 8 + static_cast<size_t>(n))) {
            if (cb) cb(Error::StreamClosed);
            return;
        }
    }

    // DONE carries the mtime in its length field; no payload.
    if (!write_header(sync::ID_DONE, mtime)) {
        if (cb) cb(Error::StreamClosed);
        return;
    }

    // Status: OKAY (success) or FAIL + message.
    uint8_t status[8];
    if (!read_exact(status, sizeof(status))) {
        if (cb) cb(Error::StreamClosed);
        return;
    }
    uint32_t id = get_le32(status);
    uint32_t len = get_le32(status + 4);
    if (id == sync::ID_OKAY) {
        if (cb) cb(Error::Ok);
    } else if (id == sync::ID_FAIL) {
        std::vector<uint8_t> drain(len);  // consume the message so the pipe stays aligned
        if (len) read_exact(drain.data(), len);
        if (cb) cb(Error::Rejected);
    } else {
        if (cb) cb(Error::Protocol);
    }
}

// --- wire helpers (worker thread) -------------------------------------------

bool Sync::send_bytes(const uint8_t* data, size_t len) {
    // One AdbStream::write() == one A_WRTE; callers keep len <= max_payload.
    return stream_ && stream_->write(data, len);
}

bool Sync::write_request(uint32_t id, const std::string& path) {
    if (path.size() > sync::MAX_PATH_LEN) return false;
    std::vector<uint8_t> buf(8 + path.size());
    put_le32(buf.data(), id);
    put_le32(buf.data() + 4, static_cast<uint32_t>(path.size()));
    std::memcpy(buf.data() + 8, path.data(), path.size());
    return send_bytes(buf.data(), buf.size());
}

bool Sync::write_header(uint32_t id, uint32_t arg) {
    uint8_t buf[8];
    put_le32(buf, id);
    put_le32(buf + 4, arg);
    return send_bytes(buf, sizeof(buf));
}

bool Sync::read_exact(void* dst, size_t n) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t got = 0;
    std::unique_lock<std::mutex> lk(mtx_);
    while (got < n) {
        size_t avail = rx_.size() - rx_pos_;
        if (avail > 0) {
            size_t take = std::min(avail, n - got);
            std::memcpy(out + got, rx_.data() + rx_pos_, take);
            rx_pos_ += take;
            got += take;
            // Reclaim the consumed prefix once it dominates the buffer.
            if (rx_pos_ > 4096 && rx_pos_ * 2 > rx_.size()) {
                rx_.erase(rx_.begin(), rx_.begin() + rx_pos_);
                rx_pos_ = 0;
            }
            continue;
        }
        if (stream_closed_ || stopped_) return false;
        cv_.wait(lk);
    }
    return true;
}

// --- stream callbacks (reader thread) ---------------------------------------

void Sync::handle_data(const uint8_t* d, size_t n) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rx_.insert(rx_.end(), d, d + n);
    }
    cv_.notify_all();
}

void Sync::handle_close() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stream_closed_ = true;
    }
    cv_.notify_all();  // unblock a worker read / its outer wait -> shutdown path
}

// --- public lifecycle -------------------------------------------------------

bool Sync::is_open() const { return stream_ && stream_->is_open(); }

void Sync::fire_close_once(Error err) {
    if (close_notified_.exchange(true)) return;  // exactly once
    if (auto l = listener_.lock()) l->on_sync_close(this, err);
}

void Sync::close() {
    SemaphoreHandle_t done = nullptr;
    bool send_close = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!stopped_) {
            stopped_ = true;
            send_close = true;  // first close(): politely A_CLSE the stream
        }
        cv_.notify_all();  // wake the worker (parked or mid-read)
        // Never self-join: close() from a completion runs on the worker task.
        bool on_worker = worker_task_ &&
                         xTaskGetCurrentTaskHandle() ==
                             static_cast<TaskHandle_t>(worker_task_);
        if (!joined_ && !on_worker && done_) {
            done = static_cast<SemaphoreHandle_t>(done_);
            joined_ = true;
        }
    }
    if (send_close && stream_) stream_->close();  // sends A_CLSE
    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);  // wait for the worker to exit
        vSemaphoreDelete(done);
        done_ = nullptr;
    }
}

}  // namespace adb
