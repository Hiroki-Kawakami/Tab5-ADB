// An ADB stream: one A_OPEN'd service (shell:, sync:, ...) multiplexed over the
// connection. The connection's read loop drives the internal state (open/data/
// close); the owning thread uses write()/close()/wait_closed(). Data is
// delivered via the on_data callback from the read-loop thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace adb {

class AdbConnection;

class AdbStream {
public:
    using DataCb = std::function<void(const uint8_t* data, size_t len)>;
    using CloseCb = std::function<void()>;

    // Send data to the service. Blocks until the peer is ready (classic ADB flow
    // control: one outstanding A_WRTE per A_OKAY). Returns false if the stream
    // closed first.
    bool write(const uint8_t* data, size_t len);

    // Politely close the stream (sends A_CLSE).
    void close();

    // Block until the stream is closed by either side. timeout_ms < 0 = forever.
    // Returns true if closed, false on timeout.
    bool wait_closed(int timeout_ms = -1);

    uint32_t local_id() const { return local_id_; }
    bool is_open() const { return open_.load(); }
    bool is_closed() const { return closed_.load(); }
    bool was_opened() const { return ever_open_; }  // reached open at least once

private:
    friend class AdbConnection;
    AdbStream(AdbConnection* conn, uint32_t local_id, DataCb on_data, CloseCb on_close)
        : conn_(conn), local_id_(local_id),
          on_data_(std::move(on_data)), on_close_(std::move(on_close)) {}

    // Called by the connection's read loop.
    void on_okay(uint32_t remote_id);  // first OKAY = opened; later = write ack
    void deliver(const uint8_t* data, size_t len);
    void mark_closed();

    AdbConnection* conn_;
    uint32_t local_id_;
    uint32_t remote_id_ = 0;

    std::atomic<bool> open_{false};
    std::atomic<bool> closed_{false};
    bool ever_open_ = false;
    bool writable_ = false;  // peer ready to receive one A_WRTE

    DataCb on_data_;
    CloseCb on_close_;
    std::mutex m_;
    std::condition_variable cv_;
};

}  // namespace adb
