#include "adb_mirroring_screen.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "adb.hpp"  // adb::Client, adb::Shell, adb::Sync, adb::Error, adb::to_string
#include "adb_app.hpp"
#include "agent/agent_jar.h"
#include "bsp.h"
#include "jpeg_fullrange_decode.h"
#include "lvgl.hpp"
#include "screen_manager.hpp"

namespace {

constexpr const char* kRemoteJar = "/data/local/tmp/tab5adb-agent.jar";
// Launch the agent with app_process (shell uid 2000, reaches hidden APIs); no
// --test-pattern, so it mirrors the live screen via SurfaceControl capture.
constexpr const char* kLaunchCmd =
    "CLASSPATH=/data/local/tmp/tab5adb-agent.jar app_process / com.tab5adb.agent.Server";
constexpr const char* kKillCmd = "pkill -f com.tab5adb.agent.Server; true";

}  // namespace

// ---------------------------------------------------------------------------
// MirrorLauncher — owns the multi-step "get the agent running and linked"
// sequence on a private worker task, and forwards the agent_link callbacks to
// the screen (held weakly). It is the SyncListener/ShellListener for the push +
// app_process launch and the LinkListener for the link, so the screen only sees
// the UI-facing callbacks. Lifetime mirrors the adb sessions: the screen owns a
// shared_ptr and join()s the task in stop() before it is destroyed.
// ---------------------------------------------------------------------------
class MirrorLauncher : public agent_link::LinkListener,
                       public adb::SyncListener,
                       public adb::ShellListener,
                       public std::enable_shared_from_this<MirrorLauncher> {
public:
    static std::shared_ptr<MirrorLauncher> start(
        std::shared_ptr<adb::Client> client,
        std::weak_ptr<agent_link::LinkListener> ui,
        std::function<void(std::string)> on_status) {
        auto self = std::shared_ptr<MirrorLauncher>(new MirrorLauncher(
            std::move(client), std::move(ui), std::move(on_status)));
        self->done_ = xSemaphoreCreateBinary();
        TaskHandle_t task = nullptr;
        xTaskCreate(&MirrorLauncher::trampoline, "mirror_launch", 8192, self.get(),
                    5, &task);
        self->task_ = task;
        return self;
    }

    ~MirrorLauncher() override { stop(); }

    // LVGL thread (onExit/dtor): signal the task to bail, close the link/shell/
    // sync, and join the task so nothing runs after the screen is destroyed.
    void stop() {
        std::shared_ptr<adb::Sync> sync;
        std::shared_ptr<adb::Shell> shell;
        std::shared_ptr<agent_link::Link> link;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
            sync = sync_;
            shell = shell_;
            link = link_;
        }
        cv_.notify_all();
        if (link) link->close();
        if (shell) shell->close();
        if (sync) sync->close();

        bool join;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            join = done_ && !joined_ &&
                   xTaskGetCurrentTaskHandle() != static_cast<TaskHandle_t>(task_);
            if (join) joined_ = true;
        }
        if (join) xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), portMAX_DELAY);
    }

    // --- agent_link::LinkListener (reader thread): record + forward to the UI ---
    void on_link_hello(agent_link::Link* l, const agent_link::AgentInfo& i) override {
        { std::lock_guard<std::mutex> lk(mtx_); hello_ = true; }
        cv_.notify_all();
        if (auto s = ui_.lock()) s->on_link_hello(l, i);
    }
    void on_mirror_started(agent_link::Link* l, const agent_link::MirrorInfo& i) override {
        if (auto s = ui_.lock()) s->on_mirror_started(l, i);
    }
    void on_video_strip(agent_link::Link* l, const agent_link::VideoStrip& s) override {
        if (auto u = ui_.lock()) u->on_video_strip(l, s);
    }
    void on_link_close(agent_link::Link* l, adb::Error e) override {
        { std::lock_guard<std::mutex> lk(mtx_); link_closed_ = true; }
        cv_.notify_all();
        if (auto s = ui_.lock()) s->on_link_close(l, e);
    }

    // --- adb::SyncListener / ShellListener (push + agent stdout) ---
    void on_sync_close(adb::Sync*, adb::Error) override {}
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override {
        std::fwrite("agent| ", 1, 7, stdout);
        std::fwrite(d, 1, n, stdout);
        std::fflush(stdout);
    }
    void on_shell_close(adb::Shell*, adb::Error) override {}

private:
    MirrorLauncher(std::shared_ptr<adb::Client> client,
                   std::weak_ptr<agent_link::LinkListener> ui,
                   std::function<void(std::string)> on_status)
        : client_(std::move(client)), ui_(std::move(ui)),
          on_status_(std::move(on_status)) {}

    static void trampoline(void* arg) { static_cast<MirrorLauncher*>(arg)->run(); }

    void status(const std::string& s) { if (on_status_) on_status_(s); }
    bool stopping() { std::lock_guard<std::mutex> lk(mtx_); return stop_; }

    // Wait until pred() (or stop) is true, or ms elapses. pred reads members
    // guarded by mtx_, which the wait holds.
    template <class Pred>
    void wait_for(Pred pred, int ms) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(ms),
                     [&] { return stop_ || pred(); });
    }
    void sleep_ms(int ms) { wait_for([] { return false; }, ms); }

    void run() {
        // 1. Kill any stale agent from a previous session.
        status("Stopping previous agent...");
        client_->exec(kKillCmd, [w = weak_from_this()](adb::Error, const std::string&) {
            if (auto s = w.lock()) { std::lock_guard<std::mutex> lk(s->mtx_); s->kill_done_ = true; s->cv_.notify_all(); }
        });
        wait_for([this] { return kill_done_; }, 3000);
        if (stopping()) return finish();

        // 2. Push the embedded agent jar.
        status("Pushing agent...");
        if (!push_jar()) {
            if (!stopping()) status("Failed to push agent");
            return finish();
        }
        if (stopping()) return finish();

        // 3. Launch it with app_process; its stdout/stderr stream over this shell.
        status("Launching agent...");
        shell_ = client_->open_shell(weak_from_this(), kLaunchCmd);

        // 4. Open the link, retrying until the agent is listening (protocol.md §2.2).
        status("Connecting to agent...");
        for (int attempt = 0; attempt < 40 && !hello_ && !stopping(); ++attempt) {
            { std::lock_guard<std::mutex> lk(mtx_); link_closed_ = false; }
            auto link = agent_link::Link::open(client_, weak_from_this());
            if (!link) { sleep_ms(200); continue; }
            { std::lock_guard<std::mutex> lk(mtx_); link_ = link; }
            wait_for([this] { return hello_ || link_closed_; }, 1500);
            if (hello_) break;
            { std::lock_guard<std::mutex> lk(mtx_); link_.reset(); }  // detach + close
            link->close();
            sleep_ms(300);
        }

        if (hello_ && !stopping()) {
            status("Starting mirror...");
            link_->start_mirror();  // 720x1280 fit, video (default MirrorConfig)
        } else if (!hello_ && !stopping()) {
            status("Agent did not respond");
        }
        finish();
    }

    bool push_jar() {
        sync_ = client_->open_sync(weak_from_this());
        if (!sync_) return false;
        auto off = std::make_shared<size_t>(0);
        bool ok = false;
        push_done_ = false;
        sync_->push(
            kRemoteJar, 0644, /*mtime=*/0,
            [off](uint8_t* b, size_t cap) -> int {
                size_t rem = agent_jar_len - *off;
                size_t n = std::min(cap, rem);
                std::memcpy(b, agent_jar + *off, n);
                *off += n;
                return static_cast<int>(n);  // 0 at EOF
            },
            [w = weak_from_this()](adb::Error e) {
                if (auto s = w.lock()) { std::lock_guard<std::mutex> lk(s->mtx_); s->push_err_ = e; s->push_done_ = true; s->cv_.notify_all(); }
            });
        wait_for([this] { return push_done_; }, 15000);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ok = push_done_ && push_err_ == adb::Error::Ok;
        }
        sync_->close();
        { std::lock_guard<std::mutex> lk(mtx_); sync_.reset(); }
        return ok;
    }

    void finish() {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
        vTaskDelete(nullptr);
    }

    std::shared_ptr<adb::Client> client_;
    std::weak_ptr<agent_link::LinkListener> ui_;
    std::function<void(std::string)> on_status_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool kill_done_ = false;
    bool push_done_ = false;
    adb::Error push_err_ = adb::Error::Transport;
    bool hello_ = false;
    bool link_closed_ = false;

    std::shared_ptr<adb::Shell> shell_;
    std::shared_ptr<adb::Sync> sync_;
    std::shared_ptr<agent_link::Link> link_;

    void* task_ = nullptr;
    void* done_ = nullptr;
    bool joined_ = false;
};

// ---------------------------------------------------------------------------
// ADBMirroringScreen
// ---------------------------------------------------------------------------

ADBMirroringScreen::ADBMirroringScreen() = default;

ADBMirroringScreen::~ADBMirroringScreen() {
    if (launcher_) { launcher_->stop(); launcher_.reset(); }
    free_decoder();
}

void ADBMirroringScreen::build() {
    // The LVGL root stays a static black, clickable surface — no widgets are
    // composited over the stream, and nothing invalidates it after build, so LVGL
    // leaves the framebuffers alone while the mirror owns them. A tap pops back.
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_fn(root_, LV_EVENT_CLICKED,
                        [](lv_event_t*) { screen_manager.pop(); });

    // Grab the two bsp framebuffers (we decode strips straight into them) and
    // present black until the first frame. The flush pushes the cleared buffers to
    // the panel (and, on device, to PSRAM) so the letterbox stays black — strips
    // never touch those pixels again.
    fb_[0] = static_cast<uint16_t*>(bsp_display_get_frame_buffer(0));
    fb_[1] = static_cast<uint16_t*>(bsp_display_get_frame_buffer(1));
    for (int i = 0; i < 2; ++i) {
        if (fb_[i]) std::memset(fb_[i], 0, (size_t)PANEL_W * PANEL_H * 2);
        bsp_display_flush(i);
    }
    back_ = 0;

    // Kick off the launch sequence. The launcher forwards link callbacks to this
    // screen (held weakly) and logs progress (no on-screen status — no overlay).
    std::weak_ptr<agent_link::LinkListener> ui(
        std::static_pointer_cast<agent_link::LinkListener>(
            std::static_pointer_cast<ADBMirroringScreen>(shared_from_this())));
    auto status_cb = [](std::string t) {
        std::printf("mirror: %s\n", t.c_str());
        std::fflush(stdout);
    };

    auto client = app::adb_client_shared();
    if (client && client->state() == adb::ConnectionState::Online) {
        launcher_ = MirrorLauncher::start(std::move(client), ui, std::move(status_cb));
    } else {
        std::printf("mirror: not connected\n");
    }
}

void ADBMirroringScreen::onExit() {
    if (launcher_) { launcher_->stop(); launcher_.reset(); }
}

void ADBMirroringScreen::on_link_hello(agent_link::Link*,
                                       const agent_link::AgentInfo&) {}

void ADBMirroringScreen::on_mirror_started(agent_link::Link*,
                                           const agent_link::MirrorInfo&) {}

void ADBMirroringScreen::on_video_strip(agent_link::Link*,
                                        const agent_link::VideoStrip& strip) {
    // Reader thread (strips serialized here): decode straight into the back
    // framebuffer, then present it and swap once the frame is complete. The
    // displayed buffer is the other one, so it is never mid-write — no tearing,
    // no LVGL involvement.
    uint16_t* dst = fb_[back_];
    if (!dst) return;
    decode_strip(strip, dst);
    if (!strip.frame_end) return;
    bsp_display_flush(back_);
    back_ ^= 1;
}

void ADBMirroringScreen::on_link_close(agent_link::Link*, adb::Error err) {
    std::printf("mirror: link closed (%s)\n", adb::to_string(err));
    std::fflush(stdout);
}

bool ADBMirroringScreen::decode_strip(const agent_link::VideoStrip& s, uint16_t* dst) {
    if (!dst) return false;
    if (s.w == 0 || s.h == 0 || s.x + s.w > PANEL_W || s.y + s.h > PANEL_H) return false;
    // The agent always sends full-panel-width frames (fit/letterbox baked in), so a
    // strip is the full panel width and decodes tightly-packed straight into its
    // framebuffer row band — its width equals the framebuffer pitch, so "packed" IS
    // "in place". A narrower strip can't be placed without a stride the P4 HW JPEG
    // decoder lacks, so drop it rather than misrender.
    if (s.x != 0 || s.w != PANEL_W) return false;

    // Lazily create the engine.
    if (!jpeg_) {
        jpeg_decode_engine_cfg_t ecfg = {};
        ecfg.timeout_ms = 1000;
        if (jpeg_new_decoder_engine(&ecfg, &jpeg_) != ESP_OK) { jpeg_ = nullptr; return false; }
    }

    // The device HW JPEG decoder needs the bitstream in a DMA-capable buffer;
    // grow it to fit the largest strip seen.
    if (s.jpeg_len > in_cap_) {
        if (in_buf_) free(in_buf_);
        jpeg_decode_memory_alloc_cfg_t icfg = {};
        icfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
        size_t got = 0;
        in_buf_ = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(s.jpeg_len, &icfg, &got));
        if (!in_buf_) { in_cap_ = 0; return false; }
        in_cap_ = got;
    }
    std::memcpy(in_buf_, s.jpeg, s.jpeg_len);

    // Full-width strip → tightly-packed decode lands in place: the destination is
    // the framebuffer row band starting at row s.y (s.x == 0), and the decoded
    // s.w(==PANEL_W)×s.h picture is exactly PANEL_W*s.h pixels contiguous — no
    // scratch, no blit, no stride. (device: P4 HW JPEG straight to PSRAM; host:
    // libjpeg straight to the buffer.)
    jpeg_decode_cfg_t dcfg = {};
    dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR, not RGB: the panel is driven R-in-high-bits RGB565, and on the P4 HW
    // JPEG decoder that byte order is produced by the BGR scramble — the RGB enum's
    // RGB565 scramble mis-packs the 16-bit pixel (greens split across the byte
    // boundary) and the image comes out as rainbow noise. Matches the proven
    // Tab5-Screen-Streamer decoder.
    dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint16_t* out = dst + (size_t)s.y * PANEL_W;  // s.x == 0
    uint32_t out_size = 0;
    return jpeg_decoder_process_full_range(
               jpeg_, &dcfg, in_buf_, (uint32_t)s.jpeg_len,
               reinterpret_cast<uint8_t*>(out), (uint32_t)((size_t)s.w * s.h * 2),
               &out_size) == ESP_OK;
}

void ADBMirroringScreen::free_decoder() {
    if (in_buf_) { free(in_buf_); in_buf_ = nullptr; in_cap_ = 0; }
    if (jpeg_) { jpeg_del_decoder_engine(jpeg_); jpeg_ = nullptr; }
}
