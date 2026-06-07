// Client — one connected device, and the factory for ADB sessions / one-shots.
//
// The app-facing entry point of the `adb` component. Owns the whole connection
// lifecycle (RSA identity, USB transport, the reader task that runs the CNXN +
// AUTH handshake and the packet read loop) on top of embedded_adb's
// AdbConnection. Replaces the provisional app/adb_session.cpp.
//
// Threading: all callbacks fire on the internal reader thread; marshalling to
// the UI/LVGL thread is the app's job. Methods are callable from any thread.
// See README.md for the cross-cutting contract (threading, lifetime, errors, the
// `self` arg) and docs/client.md for this surface's detail.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "adb_connection.hpp"  // adb::ConnectionState, adb::AdbConnection
#include "adb_error.hpp"       // adb::Error

namespace adb {

class Client;

// Connection-level delegate. Fires on the reader thread. The Client* first
// argument lets one listener instance serve multiple Clients (multi-device).
class ClientListener {
public:
    virtual ~ClientListener() = default;

    // State transitions, deduplicated:
    //   Connecting -> Authorizing -> Unauthorized -> Online, or Closed on
    //   teardown / error. banner() is valid once Online is observed.
    virtual void on_state(Client* c, ConnectionState state) = 0;
};

class Client {
public:
    // Asynchronously connect to the first USB Android device: load/create the RSA
    // identity, open the transport, run CNXN + AUTH on an internal reader task.
    // Returns immediately; progress arrives via ClientListener::on_state.
    // `listener` must outlive the returned Client.
    static std::shared_ptr<Client> connect_usb(ClientListener* listener);

    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ConnectionState state() const { return state_.load(); }
    const std::string& banner() const { return banner_; }

    // One-shot: run a single shell command and collect its output. Opens a
    // shell:<cmd> stream, accumulates until the device closes it, then fires `cb`
    // exactly once with the output. Non-blocking, callable from any thread; the
    // completion fires on the reader thread (except a synchronous NotConnected
    // when not Online). See docs/one-shots.md.
    using ExecCb = std::function<void(Error, const std::string& output)>;
    void exec(const std::string& cmd, ExecCb cb);

    // Stop the reader task and release the connection. Idempotent. Blocks until
    // the reader task has exited (so no callback fires after it returns) — except
    // when called from within a callback on the reader thread, where it only
    // signals the stop and returns without self-joining.
    void close();

private:
    explicit Client(ClientListener* listener);
    static void reader_trampoline(void* arg);
    void run();                       // the reader task body
    void set_state(ConnectionState s);  // dedupe + notify the listener

    ClientListener* listener_;
    std::unique_ptr<AdbConnection> conn_;  // created inside run()
    std::atomic<ConnectionState> state_{ConnectionState::Offline};
    std::string banner_;  // set on the reader thread before Online is notified

    std::mutex life_mtx_;     // guards closing_/joined_ and conn_ creation vs close
    bool closing_ = false;    // close() requested
    bool joined_ = false;     // the done_ wait has been consumed
    void* task_ = nullptr;    // reader TaskHandle_t (compared, never dereferenced)
    void* done_ = nullptr;    // binary SemaphoreHandle_t the task gives on exit
};

}  // namespace adb
