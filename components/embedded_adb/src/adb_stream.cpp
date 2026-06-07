#include "adb_stream.hpp"

#include <chrono>

#include "adb_connection.hpp"

namespace adb {

void AdbStream::on_okay(uint32_t remote_id) {
    bool first;
    {
        std::lock_guard<std::mutex> lk(m_);
        first = !ever_open_;
        if (first) {
            remote_id_ = remote_id;
            ever_open_ = true;
            open_.store(true);
        }
        writable_ = true;  // peer is ready for (the next) A_WRTE
    }
    cv_.notify_all();
}

void AdbStream::deliver(const uint8_t* data, size_t len) {
    if (on_data_) on_data_(data, len);
}

void AdbStream::mark_closed() {
    {
        std::lock_guard<std::mutex> lk(m_);
        closed_.store(true);
        open_.store(false);
        writable_ = false;
    }
    cv_.notify_all();
    if (on_close_) on_close_();
}

bool AdbStream::write(const uint8_t* data, size_t len) {
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return writable_ || closed_.load(); });
        if (closed_.load()) return false;
        writable_ = false;  // one outstanding A_WRTE until the next A_OKAY
    }
    return conn_->send_write(this, data, len);
}

void AdbStream::close() {
    if (closed_.load()) return;  // already closed by the peer
    // Send A_CLSE; the device confirms with its own A_CLSE, which marks us closed.
    conn_->send_close(this);
}

bool AdbStream::wait_closed(int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (timeout_ms < 0) {
        cv_.wait(lk, [this] { return closed_.load(); });
        return true;
    }
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [this] { return closed_.load(); });
}

}  // namespace adb
