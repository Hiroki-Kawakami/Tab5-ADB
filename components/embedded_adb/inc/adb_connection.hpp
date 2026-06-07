// ADB connection: the CNXN handshake + AUTH (RSA) state machine on top of a
// Transport. Reads packets in a loop and dispatches them; drives authentication
// using the host's RSA key. Stream multiplexing (A_OPEN/WRTE/...) is layered on
// top in adb_stream (P5).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "adb_crypto.hpp"
#include "adb_stream.hpp"
#include "adb_transport.hpp"

namespace adb {

enum class ConnectionState {
    Offline,       // not started
    Connecting,    // CNXN sent, awaiting response
    Authorizing,   // device challenged us; signature/pubkey in flight
    Unauthorized,  // public key sent; waiting for the user to tap "Allow"
    Online,        // authenticated; banner available
    Closed,        // transport error / stopped
};

const char* to_string(ConnectionState s);

class AdbConnection {
public:
    using StateCallback = std::function<void(ConnectionState)>;

    // advertised_max_payload is sent in CNXN.arg1 and caps per-A_WRTE payloads.
    // Keep it modest on device (the usb_host transport DMA-allocs per payload);
    // 256 KB is fine on the simulator (libusb).
    AdbConnection(std::unique_ptr<Transport> transport, RsaKey key,
                  uint32_t advertised_max_payload = 256 * 1024);
    ~AdbConnection();

    AdbConnection(const AdbConnection&) = delete;
    AdbConnection& operator=(const AdbConnection&) = delete;

    void set_state_callback(StateCallback cb) { state_cb_ = std::move(cb); }

    // Run the handshake + read loop on the CALLING thread until stop() or a
    // transport error. Sends CNXN first. Use this for headless / single-threaded
    // contexts (tests). P5/P7 add a task-spawning start() for the app.
    void run_blocking();

    // Ask the read loop to exit (callable from another thread / a callback).
    // Honored even if called *before* run_blocking() starts: a stop that lands in
    // that window makes run_blocking() return immediately instead of looping.
    void stop();

    ConnectionState state() const { return state_; }
    const std::string& banner() const { return banner_; }
    uint32_t max_payload() const { return max_payload_; }

    // Thread-safe packet send (guards the transport's OUT endpoint).
    bool send(const Packet& p);

    // Open a service stream (e.g. "shell:ls", "shell:" for an interactive shell).
    // Callbacks fire on the read-loop thread. Returns the stream immediately
    // (before the device's A_OKAY); use AdbStream::is_open()/write() to interact.
    // Requires the read loop to be running on another thread. nullptr if offline.
    std::shared_ptr<AdbStream> open_stream(const std::string& service,
                                           AdbStream::DataCb on_data,
                                           AdbStream::CloseCb on_close = nullptr);

    // Convenience: open a non-interactive service, collect all output until the
    // device closes it. Blocks; needs the read loop running on another thread.
    // Returns true if the stream opened (false = rejected / never opened).
    bool run_service(const std::string& service, std::string& output,
                     int timeout_ms = 10000);

private:
    friend class AdbStream;
    // Stream-side helpers (called by AdbStream).
    bool send_write(AdbStream* s, const uint8_t* data, size_t len);
    void send_close(AdbStream* s);
    std::shared_ptr<AdbStream> find_stream(uint32_t local_id);
    void set_state(ConnectionState s);
    void send_connect();
    void handle_packet(Packet& p);
    void on_auth(Packet& p);
    void on_connect(Packet& p);
    void on_stream_packet(Packet& p);

    std::unique_ptr<Transport> transport_;
    RsaKey key_;
    std::mutex write_mtx_;

    std::map<uint32_t, std::shared_ptr<AdbStream>> streams_;
    std::mutex streams_mtx_;
    uint32_t next_local_id_ = 1;

    StateCallback state_cb_;
    ConnectionState state_ = ConnectionState::Offline;
    std::string banner_;
    uint32_t max_payload_;  // our advertised CNXN.arg1, then negotiated min
    bool running_ = false;
    std::atomic<bool> stop_requested_{false};  // stop() before/while run_blocking()

    bool sent_signature_ = false;  // tried signing with our key
    bool sent_pubkey_ = false;     // sent the public key (prompted the user)
};

}  // namespace adb
