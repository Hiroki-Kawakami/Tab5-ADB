#include "adb_shell.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "adb_connection.hpp"  // adb::AdbConnection, ConnectionState
#include "adb_stream.hpp"      // adb::AdbStream

namespace adb {

namespace {

// Backpressure cap: unsent bytes beyond this make write() return QueueFull
// rather than growing the queue without bound (see docs/shell.md).
constexpr size_t kWriteQueueCap = 64 * 1024;

// The writer task only calls into AdbStream::write()/close(); no crypto here, so
// a small stack is plenty (unlike the reader task that generates the RSA key).
constexpr uint32_t kWriterStack = 4096;

}  // namespace

Shell::Shell(std::weak_ptr<ShellListener> listener)
    : listener_(std::move(listener)) {}

std::shared_ptr<Shell> Shell::create(AdbConnection* conn, const std::string& cmd,
                                     std::weak_ptr<ShellListener> listener) {
    auto sh = std::shared_ptr<Shell>(new Shell(std::move(listener)));
    std::weak_ptr<Shell> weak = sh;

    // The stream callbacks fire on the reader thread. They hold only a weak ref
    // so the stream's closure (kept alive by the connection's stream map) does
    // not keep the Shell alive — the app's shared_ptr owns its lifetime.
    auto stream = conn->open_stream(
        "shell:" + cmd,
        [weak](const uint8_t* d, size_t n) {
            if (auto s = weak.lock()) s->handle_data(d, n);
        },
        [weak]() {
            if (auto s = weak.lock()) s->handle_close();
        });
    if (!stream) return nullptr;  // offline / could not open

    sh->stream_ = std::move(stream);
    sh->wake_ = xSemaphoreCreateBinary();
    sh->done_ = xSemaphoreCreateBinary();
    TaskHandle_t task = nullptr;
    // The writer task takes a raw pointer; ~Shell -> close() joins it, so the
    // Shell cannot be destroyed while the task is still running.
    xTaskCreate(&Shell::writer_trampoline, "adb_shell", kWriterStack, sh.get(), 5,
                &task);
    sh->writer_task_ = task;
    return sh;
}

Shell::~Shell() {
    close();  // joins the writer task; no callback runs after this returns
    if (wake_) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(wake_));
        wake_ = nullptr;
    }
}

void Shell::writer_trampoline(void* arg) { static_cast<Shell*>(arg)->writer_loop(); }

void Shell::writer_loop() {
    for (;;) {
        // Wait for queued work or a stop signal. A binary semaphore collapses
        // multiple gives into one, so we always drain the whole queue per wake.
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
            // Blocks on ADB flow control (off the caller's thread). false once
            // the stream is closed -> nothing more we can send.
            if (!stream_->write(chunk.data(), chunk.size())) goto done;
        }
    }
done:
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}

Error Shell::write(const uint8_t* data, size_t len) {
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

bool Shell::is_open() const {
    return stream_ && stream_->is_open();
}

void Shell::handle_data(const uint8_t* d, size_t n) {
    if (auto l = listener_.lock()) l->on_shell_data(this, d, n);
}

void Shell::handle_close() {
    // Wake the writer so it observes the stop and exits even if it is parked on
    // wake_ with an empty queue.
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        closing_ = true;
    }
    if (wake_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));

    if (close_notified_.exchange(true)) return;  // exactly once
    if (auto l = listener_.lock()) l->on_shell_close(this, Error::Ok);
}

void Shell::close() {
    SemaphoreHandle_t done = nullptr;
    bool send_close = false;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        if (!closing_) {
            closing_ = true;
            send_close = true;  // first close(): politely A_CLSE the stream
        }
        if (wake_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));
        // Never self-join: close() from a callback runs on the reader thread, not
        // the writer task, so the guard below is purely defensive.
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
