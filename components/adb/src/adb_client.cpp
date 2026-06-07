#include "adb_client.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "embedded_adb.hpp"  // load_or_create_key, open_usb_transport, Transport

namespace adb {

namespace {

// usb_host DMA-allocs per payload on device, so keep the advertised CNXN maxdata
// modest there; the simulator (libusb) can afford the full ADB max.
#ifdef ESP_PLATFORM
constexpr uint32_t kMaxPayload = 16 * 1024;
#else
constexpr uint32_t kMaxPayload = 256 * 1024;
#endif

// Big stack: the first connect generates the RSA-2048 key (mbedTLS is stack-heavy).
constexpr uint32_t kReaderStack = 16384;

}  // namespace

Client::Client(std::weak_ptr<ClientListener> listener)
    : listener_(std::move(listener)) {}

std::shared_ptr<Client> Client::connect_usb(std::weak_ptr<ClientListener> listener) {
    auto c = std::shared_ptr<Client>(new Client(std::move(listener)));
    c->done_ = xSemaphoreCreateBinary();
    TaskHandle_t task = nullptr;
    // The reader task takes a raw pointer; ~Client -> close() joins it, so the
    // Client cannot be destroyed while the task is still running.
    xTaskCreate(&Client::reader_trampoline, "adb_client", kReaderStack, c.get(), 5,
                &task);
    c->task_ = task;
    return c;
}

Client::~Client() { close(); }

void Client::reader_trampoline(void* arg) { static_cast<Client*>(arg)->run(); }

void Client::run() {
    // Setup work that can run before we commit to a connection (so a close()
    // during key gen / enumeration aborts cheaply).
    auto key = load_or_create_key();
    std::unique_ptr<Transport> transport;
    if (key) transport = open_usb_transport();

    {
        std::lock_guard<std::mutex> lk(life_mtx_);
        // Build the connection under the lock so a concurrent close() either sees
        // closing_ here and we skip it, or sees conn_ and can stop() it. The
        // stop-before-run_blocking window is handled by AdbConnection::stop().
        if (!closing_ && key && transport) {
            conn_ = std::make_unique<AdbConnection>(std::move(transport),
                                                    std::move(*key), kMaxPayload);
            conn_->set_state_callback([this](ConnectionState s) { set_state(s); });
        }
    }

    if (conn_) {
        conn_->run_blocking();  // blocks until close()/stop() or a transport error
    }
    set_state(ConnectionState::Closed);  // terminal (deduped if already Closed)

    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}

void Client::set_state(ConnectionState s) {
    if (state_.exchange(s) == s) return;  // dedupe repeated states
    if (s == ConnectionState::Online && conn_) banner_ = conn_->banner();
    if (auto l = listener_.lock()) l->on_state(this, s);
}

void Client::exec(const std::string& cmd, ExecCb cb) {
    AdbConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(life_mtx_);
        if (!closing_) conn = conn_.get();
    }
    if (!conn || conn->state() != ConnectionState::Online) {
        if (cb) cb(Error::NotConnected, std::string());
        return;
    }

    // Accumulate output on the reader thread; complete once when the stream
    // closes (peer A_CLSE or connection teardown — both fire on_close exactly
    // once). The shared buffer / cb outlive this call via the stream's callbacks.
    auto buf = std::make_shared<std::string>();
    auto sink = std::make_shared<ExecCb>(std::move(cb));
    auto stream = conn->open_stream(
        "shell:" + cmd,
        [buf](const uint8_t* d, size_t n) {
            buf->append(reinterpret_cast<const char*>(d), n);
        },
        [buf, sink]() {
            if (*sink) (*sink)(Error::Ok, *buf);
        });
    if (!stream) {  // raced to not-Online between the check and open_stream
        if (*sink) (*sink)(Error::NotConnected, std::string());
    }
}

std::shared_ptr<Shell> Client::open_shell(std::weak_ptr<ShellListener> listener,
                                          const std::string& cmd) {
    AdbConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(life_mtx_);
        if (!closing_) conn = conn_.get();
    }
    if (!conn || conn->state() != ConnectionState::Online) return nullptr;
    return Shell::create(conn, cmd, std::move(listener));
}

std::shared_ptr<Sync> Client::open_sync(std::weak_ptr<SyncListener> listener) {
    AdbConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(life_mtx_);
        if (!closing_) conn = conn_.get();
    }
    if (!conn || conn->state() != ConnectionState::Online) return nullptr;
    return Sync::create(conn, std::move(listener));
}

void Client::close() {
    SemaphoreHandle_t done = nullptr;
    {
        std::lock_guard<std::mutex> lk(life_mtx_);
        if (!closing_) {
            closing_ = true;
            if (conn_) conn_->stop();  // robust even before run_blocking() loops
        }
        if (joined_) return;  // someone already waited for the task
        // Never self-join: close() from a callback runs on the reader thread.
        if (task_ && xTaskGetCurrentTaskHandle() == static_cast<TaskHandle_t>(task_)) {
            return;
        }
        if (done_) {
            done = static_cast<SemaphoreHandle_t>(done_);
            joined_ = true;
        }
    }
    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);  // wait for the reader task to exit
        vSemaphoreDelete(done);
        done_ = nullptr;
    }
}

}  // namespace adb
