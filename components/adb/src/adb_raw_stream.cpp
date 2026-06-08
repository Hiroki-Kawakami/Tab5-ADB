#include "adb_raw_stream.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "adb_connection.hpp"  // adb::AdbConnection
#include "adb_stream.hpp"      // adb::AdbStream

namespace adb {

namespace {

// Backpressure cap: unsent bytes beyond this make write() return QueueFull
// rather than growing the queue without bound (same policy as Shell).
constexpr size_t kWriteQueueCap = 64 * 1024;

// The writer task only calls into AdbStream::write()/close(); no crypto here.
constexpr uint32_t kWriterStack = 4096;

}  // namespace

Stream::Stream(std::weak_ptr<StreamListener> listener)
    : listener_(std::move(listener)) {}

std::shared_ptr<Stream> Stream::create(AdbConnection* conn,
                                       const std::string& service,
                                       std::weak_ptr<StreamListener> listener) {
    auto st = std::shared_ptr<Stream>(new Stream(std::move(listener)));
    std::weak_ptr<Stream> weak = st;

    // The stream callbacks fire on the reader thread and hold only a weak ref, so
    // the connection's stream map does not keep the Stream alive — its owner does.
    auto stream = conn->open_stream(
        service,
        [weak](const uint8_t* d, size_t n) {
            if (auto s = weak.lock()) s->handle_data(d, n);
        },
        [weak]() {
            if (auto s = weak.lock()) s->handle_close();
        });
    if (!stream) return nullptr;  // offline / could not open

    st->stream_ = std::move(stream);
    st->wake_ = xSemaphoreCreateBinary();
    st->done_ = xSemaphoreCreateBinary();
    TaskHandle_t task = nullptr;
    xTaskCreate(&Stream::writer_trampoline, "adb_stream", kWriterStack, st.get(), 5,
                &task);
    st->writer_task_ = task;
    return st;
}

Stream::~Stream() {
    close();  // joins the writer task; no callback runs after this returns
    if (wake_) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(wake_));
        wake_ = nullptr;
    }
}

void Stream::writer_trampoline(void* arg) {
    static_cast<Stream*>(arg)->writer_loop();
}

void Stream::writer_loop() {
    for (;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(wake_), portMAX_DELAY);
        for (;;) {
            std::vector<uint8_t> chunk;
            {
                std::lock_guard<std::mutex> lk(q_mtx_);
                if (queue_.empty()) {
                    if (closing_) goto done;  // stop requested and drained
                    break;                    // back to wait on wake_
                }
                chunk = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= chunk.size();
            }
            if (!stream_->write(chunk.data(), chunk.size())) goto done;  // closed
        }
    }
done:
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}

Error Stream::write(const uint8_t* data, size_t len) {
    if (len == 0) return Error::Ok;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        if (closing_) return Error::StreamClosed;
        // Reject only once something is already queued, so a single oversized
        // write still goes through on an empty queue.
        if (queued_bytes_ + len > kWriteQueueCap && !queue_.empty()) {
            return Error::QueueFull;
        }
        queue_.emplace_back(data, data + len);
        queued_bytes_ += len;
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));
    return Error::Ok;
}

bool Stream::is_open() const { return stream_ && stream_->is_open(); }

void Stream::handle_data(const uint8_t* d, size_t n) {
    if (auto l = listener_.lock()) l->on_stream_data(this, d, n);
}

void Stream::handle_close() {
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        closing_ = true;  // wake the writer so it observes the stop and exits
    }
    if (wake_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));

    if (close_notified_.exchange(true)) return;  // exactly once
    if (auto l = listener_.lock()) l->on_stream_close(this, Error::Ok);
}

void Stream::close() {
    SemaphoreHandle_t done = nullptr;
    bool send_close = false;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        if (!closing_) {
            closing_ = true;
            send_close = true;  // first close(): politely A_CLSE the stream
        }
        if (wake_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));
        bool on_writer = writer_task_ &&
                         xTaskGetCurrentTaskHandle() ==
                             static_cast<TaskHandle_t>(writer_task_);
        if (!joined_ && !on_writer && done_) {
            done = static_cast<SemaphoreHandle_t>(done_);
            joined_ = true;
        }
    }
    if (send_close && stream_) stream_->close();  // sends A_CLSE
    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);  // wait for the writer task to exit
        vSemaphoreDelete(done);
        done_ = nullptr;
    }
}

}  // namespace adb
