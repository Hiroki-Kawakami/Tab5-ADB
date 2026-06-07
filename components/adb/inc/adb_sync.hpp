// Sync — the `sync:` filesystem session multiplexed over the connection.
//
// The sync stream runs a small request/response sub-protocol (STAT/LIST/SEND/
// RECV) and only one request at a time, so Sync is a *session* whose *methods*
// are one-shot style: open one Sync per device, then issue stat()/push()/... ,
// each with a std::function completion.
//
// Threading refinement (see docs/sync.md): unlike the rest of the component,
// Sync's completions fire on a private *worker thread* it owns (the sync
// protocol is synchronous, so the worker issues a request and blocks for the
// response over an internal byte pipe fed by the reader thread). Still never the
// LVGL thread — the app marshals as usual.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adb_error.hpp"

namespace adb {

class AdbConnection;  // embedded_adb
class AdbStream;      // embedded_adb
class Client;
class Sync;

// Metadata of one path. mode == 0 means the path does not exist (the device's
// lstat failed) — there is no separate "not found" error.
struct FileStat {
    uint32_t mode = 0;   // unix st_mode; 0 = does not exist
    uint32_t size = 0;   // bytes (v1 is 32-bit)
    uint32_t mtime = 0;  // seconds since epoch
    bool exists() const { return mode != 0; }
    bool is_dir() const { return (mode & 0170000) == 0040000; }
    bool is_reg() const { return (mode & 0170000) == 0100000; }
};

// Sync session delegate. on_sync_close fires once, on the worker thread. The
// Sync* first argument lets one listener serve several sessions.
class SyncListener {
public:
    virtual ~SyncListener() = default;

    // The session ended (our close(), the peer, or Client::close() teardown).
    // Fires exactly once; no completion follows it. err is Ok in v1.
    virtual void on_sync_close(Sync* s, Error err) = 0;
};

// Pull-style byte source for push(): fill up to `cap` bytes into `buf`, return
//   >0  bytes produced
//    0  end of file (stop)
//   <0  abort the transfer (push completes with Error::Transport)
// Called repeatedly on the worker thread until it returns <= 0.
using SyncSource = std::function<int(uint8_t* buf, size_t cap)>;

class Sync {
public:
    ~Sync();

    Sync(const Sync&) = delete;
    Sync& operator=(const Sync&) = delete;

    // lstat one path (a symlink is not followed). Completion fires once.
    void stat(const std::string& path, std::function<void(Error, FileStat)> cb);

    // Tab5 -> Android: stream `source` to `remote_path`, creating/truncating it
    // with permission bits `perm` (e.g. 0644) and mtime `mtime` (epoch seconds).
    // Non-blocking; the source is pumped on the worker thread. Completion once.
    void push(const std::string& remote_path, uint32_t perm, uint32_t mtime,
              SyncSource source, std::function<void(Error)> cb);

    bool is_open() const;

    // End the session (A_CLSE) and join the worker. Idempotent. Safe from a
    // completion (runs on the worker thread, only signals there — no self-join).
    void close();

    // Stop referencing the listener; call before destroying it. Not from a
    // callback.
    void detach();

private:
    friend class Client;
    explicit Sync(SyncListener* listener);

    // Factory used by Client::open_sync: opens "sync:" and starts the worker.
    // nullptr if the stream can't open.
    static std::shared_ptr<Sync> create(AdbConnection* conn, SyncListener* listener);

    // One queued operation. run(true) executes it on the worker; run(false) is
    // the "session already gone" path — it fires the op's completion with
    // Error::Cancelled. Keeping completion-firing inside the op erases the
    // differing cb signatures (stat vs push) from the queue.
    using Op = std::function<void(bool alive)>;
    void enqueue(Op op);

    static void worker_trampoline(void* arg);
    void worker_loop();

    void do_stat(const std::string& path, std::function<void(Error, FileStat)> cb);
    void do_push(const std::string& path, uint32_t perm, uint32_t mtime,
                 SyncSource source, std::function<void(Error)> cb);

    // Wire helpers, all on the worker thread. Return false once the stream is
    // gone (or stop was requested), so a caller turns that into StreamClosed.
    bool send_bytes(const uint8_t* data, size_t len);   // one capped A_WRTE
    bool write_request(uint32_t id, const std::string& path);
    bool write_header(uint32_t id, uint32_t arg);       // 8-byte header, no payload
    bool read_exact(void* dst, size_t n);               // pull n bytes from the pipe

    // Stream callbacks (reader thread).
    void handle_data(const uint8_t* d, size_t n);
    void handle_close();

    void fire_close_once(Error err);

    std::shared_ptr<AdbStream> stream_;
    size_t chunk_cap_ = 0;  // max sync DATA bytes per A_WRTE (fits max_payload)

    std::mutex listener_mtx_;  // held during dispatch; detach() takes it too
    SyncListener* listener_;
    std::atomic<bool> close_notified_{false};  // on_sync_close fires once

    // Worker + byte pipe, all under mtx_/cv_.
    std::mutex mtx_;
    std::condition_variable cv_;  // notified on: op queued / rx bytes / stop
    std::deque<Op> queue_;
    std::vector<uint8_t> rx_;  // bytes from the reader thread, consumed at rx_pos_
    size_t rx_pos_ = 0;
    bool stream_closed_ = false;  // peer/teardown closed the stream
    bool stopped_ = false;        // close() requested

    void* worker_task_ = nullptr;  // worker TaskHandle_t (compared, not deref'd)
    void* done_ = nullptr;         // binary SemaphoreHandle_t given on worker exit
    bool joined_ = false;          // the done_ wait has been consumed
};

}  // namespace adb
