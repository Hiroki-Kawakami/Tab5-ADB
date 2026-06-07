// Shell — an interactive `shell:` session multiplexed over the connection.
//
// A long-lived counterpart to the `exec` one-shot: where exec collects one
// command's output and completes, a Shell stays open so the app can stream
// keystrokes in and output out (the building block of an on-device terminal).
//
// write() is non-blocking on any thread: it enqueues to an internal writer task
// that owns the blocking embedded_adb primitive (one A_WRTE per A_OKAY). This is
// why Shell lives in `adb` (free to spawn a FreeRTOS task) rather than in the
// thread-agnostic engine. See README.md for the cross-cutting contract and
// docs/shell.md for this surface's detail.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adb_error.hpp"

namespace adb {

class AdbConnection;  // embedded_adb
class AdbStream;      // embedded_adb
class Client;
class Shell;

// Shell session delegate. Both callbacks fire on the reader thread. The Shell*
// first argument lets one listener instance serve several shells (e.g. a screen
// driving multiple terminals on one device).
class ShellListener {
public:
    virtual ~ShellListener() = default;

    // Output bytes from the device (stdout+stderr merged, as adbd emits them).
    virtual void on_shell_data(Shell* sh, const uint8_t* data, size_t len) = 0;

    // The stream closed (peer exit, our close(), or Client::close() teardown).
    // Fires exactly once; no on_shell_data follows it. err is Ok in v1.
    virtual void on_shell_close(Shell* sh, Error err) = 0;
};

class Shell {
public:
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Non-blocking, callable from any thread. Enqueues to the writer task:
    //   Ok / StreamClosed (closed) / QueueFull (backpressure cap hit).
    Error write(const uint8_t* data, size_t len);
    Error write(const std::string& s) {
        return write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    bool is_open() const;

    // Send A_CLSE and stop the writer task. Idempotent. Blocks until the writer
    // task has exited, except when called from the reader thread (e.g. inside
    // on_shell_close), where it only signals the stop.
    void close();

private:
    friend class Client;
    explicit Shell(std::weak_ptr<ShellListener> listener);

    // Factory used by Client::open_shell: opens "shell:<cmd>" and wires the
    // stream callbacks + writer task. nullptr if the stream can't open.
    static std::shared_ptr<Shell> create(AdbConnection* conn,
                                          const std::string& cmd,
                                          std::weak_ptr<ShellListener> listener);

    static void writer_trampoline(void* arg);
    void writer_loop();          // drains queue_ via the blocking AdbStream::write
    void handle_data(const uint8_t* d, size_t n);  // stream on_data (reader thread)
    void handle_close();                            // stream on_close (reader thread)

    std::shared_ptr<AdbStream> stream_;

    // Held weakly: the app owns the listener's lifetime (e.g. a screen). Each
    // dispatch lock()s it; if the listener is gone the callback is skipped, and
    // close() never has to race the listener's destruction.
    std::weak_ptr<ShellListener> listener_;

    std::mutex q_mtx_;  // guards queue_/queued_bytes_/closing_ + the close bookkeeping
    std::deque<std::vector<uint8_t>> queue_;
    size_t queued_bytes_ = 0;
    bool closing_ = false;
    std::atomic<bool> close_notified_{false};  // on_shell_close fires once

    void* wake_ = nullptr;         // binary SemaphoreHandle_t: work or stop signal
    void* done_ = nullptr;         // binary SemaphoreHandle_t given when writer exits
    void* writer_task_ = nullptr;  // writer TaskHandle_t (compared, not dereferenced)
    bool joined_ = false;          // the done_ wait has been consumed
};

}  // namespace adb
