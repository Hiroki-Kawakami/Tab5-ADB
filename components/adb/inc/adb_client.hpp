// Client — one connected device, and the factory for ADB sessions / one-shots.
//
// The app-facing entry point of the `adb` component. Owns the whole connection
// lifecycle (RSA identity, USB transport, the reader task that runs the CNXN +
// AUTH handshake and the packet read loop) on top of embedded_adb's
// AdbConnection.
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
#include "adb_raw_stream.hpp"  // adb::Stream, adb::StreamListener
#include "adb_shell.hpp"       // adb::Shell, adb::ShellListener
#include "adb_sync.hpp"        // adb::Sync, adb::SyncListener

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
    // The listener is held weakly: the app owns its lifetime (drop the listener's
    // shared_ptr to detach — no callback fires after the weak ref expires).
    static std::shared_ptr<Client> connect_usb(std::weak_ptr<ClientListener> listener);

    // Same as connect_usb, but over ADB-over-TCP to host:port (a device already
    // listening via `adb tcpip` / wireless debugging). The RSA identity is the same
    // NVS key, so a device already authorized over USB connects with no prompt.
    static std::shared_ptr<Client> connect_tcp(const std::string& host, uint16_t port,
                                               std::weak_ptr<ClientListener> listener);

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

    // Session: open an interactive `shell:` (empty cmd -> a PTY shell) or run a
    // single command (cmd = "ls" -> shell:ls). Returns a shared_ptr immediately
    // (before the device's A_OKAY); use Shell::write()/close() to interact and
    // ShellListener for output/close. nullptr if the client is not Online.
    // The listener is held weakly (drop its shared_ptr to detach); it may serve
    // several shells. See docs/shell.md.
    std::shared_ptr<Shell> open_shell(std::weak_ptr<ShellListener> listener,
                                      const std::string& cmd = "");

    // Session: open the device's `sync:` filesystem service. Returns a
    // shared_ptr immediately (before the device's A_OKAY); use Sync::stat()/
    // push()/... to transfer files and SyncListener for the session close.
    // nullptr if the client is not Online. The listener is held weakly (drop its
    // shared_ptr to detach). See docs/sync.md.
    std::shared_ptr<Sync> open_sync(std::weak_ptr<SyncListener> listener);

    // Generic, service-agnostic stream: A_OPENs `service` (e.g.
    // "localabstract:tab5adb-agent") and returns a raw bidirectional byte stream.
    // This is the building block app-specific protocols in other components layer
    // on (so they depend on `adb`, not embedded_adb). Returns a shared_ptr
    // immediately (before the device's A_OKAY); StreamListener delivers data/close
    // on the reader thread. nullptr if the client is not Online.
    std::shared_ptr<Stream> open_stream(const std::string& service,
                                        std::weak_ptr<StreamListener> listener);

    // Stop the reader task and release the connection. Idempotent. Blocks until
    // the reader task has exited (so no callback fires after it returns) — except
    // when called from within a callback on the reader thread, where it only
    // signals the stop and returns without self-joining.
    void close();

private:
    // The transport to open is a thunk (USB vs TCP), resolved on the reader thread
    // so an enumeration/connect can be aborted cheaply by a concurrent close().
    using TransportFactory = std::function<std::unique_ptr<Transport>()>;
    Client(std::weak_ptr<ClientListener> listener, TransportFactory open_transport,
           uint32_t max_payload);
    static std::shared_ptr<Client> launch(std::weak_ptr<ClientListener> listener,
                                          TransportFactory open_transport,
                                          uint32_t max_payload);
    static void reader_trampoline(void* arg);
    void run();                       // the reader task body
    void set_state(ConnectionState s);  // dedupe + notify the listener

    // Held weakly; lock()ed before each on_state dispatch (see connect_usb).
    std::weak_ptr<ClientListener> listener_;
    TransportFactory open_transport_;       // opens the USB or TCP transport
    uint32_t advertised_max_payload_;       // our CNXN.arg1 (transport-dependent)
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
