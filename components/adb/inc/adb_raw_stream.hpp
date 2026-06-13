// Stream — a generic, service-agnostic ADB stream multiplexed over the
// connection. It is the raw building block under the typed sessions: where
// Shell knows `shell:` PTY semantics and Sync knows the `sync:` sub-protocol,
// a Stream just opens an arbitrary service (the caller passes the service
// string) and exposes bidirectional bytes. Higher layers — including
// app-specific ones in *other* components (e.g. agent_link, which speaks the
// tab5adb-agent wire protocol) — build on this without depending on
// embedded_adb directly, keeping the dependency arrow pointing at `adb`.
//
// write() is non-blocking on any thread: it enqueues to an internal writer task
// that owns the blocking embedded_adb primitive (one A_WRTE per A_OKAY), the
// same model as Shell. on_stream_data / on_stream_close fire on the reader
// thread. See README.md for the cross-cutting contract.
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
class Stream;

// Generic stream delegate. Both callbacks fire on the reader thread. The Stream*
// first argument lets one listener instance serve several streams.
class StreamListener {
public:
    virtual ~StreamListener() = default;

    // Bytes from the service, as the peer sent them (no framing imposed here).
    virtual void on_stream_data(Stream* st, const uint8_t* data, size_t len) = 0;

    // The stream closed (peer, our close(), or Client::close() teardown). Fires
    // exactly once; no on_stream_data follows it. err is Ok in v1.
    virtual void on_stream_close(Stream* st, Error err) = 0;
};

class Stream {
public:
    ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Non-blocking, callable from any thread. Enqueues to the writer task:
    //   Ok / StreamClosed (closed) / QueueFull (backpressure cap hit).
    Error write(const uint8_t* data, size_t len);
    Error write(const std::string& s) {
        return write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    bool is_open() const;

    // Bytes not yet acknowledged by the peer: the queued bytes still waiting for
    // the writer task PLUS the one frame it currently has in flight (dequeued but
    // its blocking embedded_adb write — one A_WRTE per A_OKAY — has not returned).
    // So pending_bytes() == 0 means the link is genuinely idle (queue empty and
    // the last frame's A_OKAY received), the precise "channel free" signal a caller
    // needs to coalesce/batch low-value frames (e.g. touch MOVEs) without piling
    // them up behind the link. Callable from any thread.
    size_t pending_bytes() const;

    // Send A_CLSE and stop the writer task. Idempotent. Blocks until the writer
    // task has exited, except when called from the reader thread (e.g. inside
    // on_stream_close), where it only signals the stop.
    void close();

private:
    friend class Client;
    explicit Stream(std::weak_ptr<StreamListener> listener);

    // Factory used by Client::open_stream: A_OPENs `service` and wires the stream
    // callbacks + writer task. nullptr if the stream can't open.
    static std::shared_ptr<Stream> create(AdbConnection* conn,
                                          const std::string& service,
                                          std::weak_ptr<StreamListener> listener);

    static void writer_trampoline(void* arg);
    void writer_loop();
    void handle_data(const uint8_t* d, size_t n);  // stream on_data (reader thread)
    void handle_close();                            // stream on_close (reader thread)

    std::shared_ptr<AdbStream> stream_;

    // Held weakly: the owner (e.g. an agent_link::Link) owns the listener's
    // lifetime; each dispatch lock()s it, so dropping the owner detaches.
    std::weak_ptr<StreamListener> listener_;

    mutable std::mutex q_mtx_;  // guards queue_/queued_bytes_/closing_ + close bookkeeping
    std::deque<std::vector<uint8_t>> queue_;
    size_t queued_bytes_ = 0;
    size_t in_flight_bytes_ = 0;  // the frame the writer is currently blocked sending
    bool closing_ = false;
    std::atomic<bool> close_notified_{false};  // on_stream_close fires once

    void* wake_ = nullptr;         // binary SemaphoreHandle_t: work or stop signal
    void* done_ = nullptr;         // binary SemaphoreHandle_t given when writer exits
    void* writer_task_ = nullptr;  // writer TaskHandle_t (compared, not dereferenced)
    bool joined_ = false;          // the done_ wait has been consumed
};

}  // namespace adb
