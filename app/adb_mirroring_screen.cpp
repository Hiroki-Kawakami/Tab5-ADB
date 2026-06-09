#include "adb_mirroring_screen.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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
#include "display_manager.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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
    // Stop the producer (launcher closes the link → no more on_video_strip) before
    // the consumer, so no slot is filled while we tear the decode task down.
    if (launcher_) { launcher_->stop(); launcher_.reset(); }
    if (decode_task_) {
        decode_stop_ = true;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    // Idempotent with onExit (which normally runs first on pop): drop the toggle
    // timer + overlay mode if they somehow survived.
    if (poll_timer_) { lv_timer_delete(static_cast<lv_timer_t*>(poll_timer_)); poll_timer_ = nullptr; }
    display_manager.exit_overlay();
    if (decode_done_) { vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_)); decode_done_ = nullptr; }
    if (ready_q_) { vQueueDelete(static_cast<QueueHandle_t>(ready_q_)); ready_q_ = nullptr; }
    if (free_q_) { vQueueDelete(static_cast<QueueHandle_t>(free_q_)); free_q_ = nullptr; }
    for (auto& s : slots_) { if (s.buf) { heap_caps_free(s.buf); s.buf = nullptr; } }
    free_decoder();
}

void ADBMirroringScreen::build() {
    // The LVGL root on the MAIN display stays a static black surface — it renders
    // once at load then never invalidates, so the main display leaves the
    // framebuffers alone while the mirror owns them (the video and, in overlay
    // mode, the control bar are composited by the DisplayManager instead). It is
    // not clickable: navigation is the bar's Back button (set up in onEnter).
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    // Grab the two bsp framebuffers (we decode strips straight into them). They are
    // cleared to black in onEnter via enter_overlay(clear_framebuffers) — once the
    // main display is isolated from them — so the pre-stream / letterbox stays black.
    fb_[0] = static_cast<uint16_t*>(display_manager.framebuffer(0));
    fb_[1] = static_cast<uint16_t*>(display_manager.framebuffer(1));
    back_ = 0;

    // Receive/decode split: the reader thread fills frame slots (free_q_) and the
    // decode task drains finished frames (ready_q_), so the blocking HW-JPEG decode
    // runs off the reader thread. Hand all slots to the producer to start.
    free_q_ = xQueueCreate(kSlots, sizeof(int));
    ready_q_ = xQueueCreate(1, sizeof(int));
    for (int i = 0; i < kSlots; ++i) xQueueSend(static_cast<QueueHandle_t>(free_q_), &i, 0);
    decode_done_ = xSemaphoreCreateBinary();
    decode_stop_ = false;
    TaskHandle_t dt = nullptr;
    xTaskCreate(&ADBMirroringScreen::decode_trampoline, "mirror_decode", 8192, this, 6, &dt);
    decode_task_ = dt;

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

void ADBMirroringScreen::onEnter() {
    // Switch the DisplayManager into overlay mode: the mirror owns the
    // framebuffers (JPEG decode), and a small LVGL display renders the opaque
    // control bar that DM composites at flush time. The bar is a full-width strip
    // at the bottom of the panel (no rotation, scale 1).
    lv_area_t rect = {0, PANEL_H - kBarH, PANEL_W - 1, PANEL_H - 1};
    lv_obj_t* bar = display_manager.enter_overlay({rect, 0, 1.0f, /*clear=*/true});

    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 16, 0);
    lv_obj_t* back = lv_button_create(bar);
    lv_obj_set_height(back, kBarH - 32);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* lbl = lv_label_create(back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(lbl);
    // Defer the pop: it runs onExit -> exit_overlay, which deletes this overlay
    // display (and this button) — don't tear it down from inside its own event.
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED,
                        [](lv_event_t*) { lv_async_call([] { screen_manager.pop(); }); });

    display_manager.set_overlay_visible(true);

    // Poll the raw panel touch on the LVGL thread to toggle the bar when the user
    // taps the video area (outside the bar). touch_point + the indev read both run
    // on the LVGL thread, so the cached state is consistent.
    poll_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<ADBMirroringScreen*>(lv_timer_get_user_data(t))->poll_touch();
        },
        30, this);
}

void ADBMirroringScreen::poll_touch() {
    bsp_touch_point_t p;
    bool pressed = display_manager.touch_point(&p);
    if (pressed && !touch_prev_) {
        bool in_bar = (p.y >= PANEL_H - kBarH);  // full-width bottom strip
        if (!in_bar) display_manager.set_overlay_visible(!display_manager.overlay_visible());
    }
    touch_prev_ = pressed;
}

void ADBMirroringScreen::onExit() {
    // Stop both threads before the popped screen's framebuffers are reclaimed by
    // the previous screen's re-render: producer first (no more strips), then join
    // the decode task so it can't flush into a buffer that is being reused.
    if (launcher_) { launcher_->stop(); launcher_.reset(); }
    if (decode_task_) {
        decode_stop_ = true;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    // Both threads are stopped, so no DM.flush can race the teardown: drop the
    // overlay (restores the indev to the main display + frees the overlay display)
    // and the toggle timer. The previous screen's re-render reclaims the panel.
    if (poll_timer_) { lv_timer_delete(static_cast<lv_timer_t*>(poll_timer_)); poll_timer_ = nullptr; }
    display_manager.exit_overlay();
}

void ADBMirroringScreen::on_link_hello(agent_link::Link*,
                                       const agent_link::AgentInfo&) {}

void ADBMirroringScreen::on_mirror_started(agent_link::Link*,
                                           const agent_link::MirrorInfo&) {}

void ADBMirroringScreen::on_video_strip(agent_link::Link*,
                                        const agent_link::VideoStrip& strip) {
    // Reader thread (strips serialized here): only copy the strip into the current
    // fill slot and, on frame_end, hand the finished frame to the decode task. No
    // decode here, so the link acks immediately and the USB stream keeps flowing
    // while the decode task works in parallel.

    // Start of a frame: claim a free slot (reusing one we still hold from a frame
    // that never terminated). If none is free the decode task is behind — drop the
    // whole frame (latest-frame-wins backpressure), don't stall the reader.
    if (strip.frame_start) {
        if (fill_slot_ < 0) {
            int s;
            if (xQueueReceive(static_cast<QueueHandle_t>(free_q_), &s, 0) == pdTRUE) fill_slot_ = s;
        }
        fill_bad_ = (fill_slot_ < 0);
        if (fill_slot_ >= 0) {
            FrameSlot& f = slots_[fill_slot_];
            f.write_off = 0;
            f.strip_count = 0;
            f.bytes = 0;
        }
    }
    if (fill_slot_ < 0 || fill_bad_) return;  // dropping this frame

    FrameSlot& f = slots_[fill_slot_];
    // The agent always sends full-panel-width frames (fit/letterbox baked in), so a
    // strip is the full panel width and decodes tightly-packed straight into its
    // framebuffer row band — its width equals the framebuffer pitch, so "packed" is
    // "in place". A narrower strip can't be placed without a stride the P4 HW JPEG
    // decoder lacks, so drop the whole frame rather than misrender it.
    if (strip.x != 0 || strip.w != PANEL_W || strip.h == 0 ||
        strip.y + strip.h > PANEL_H || f.strip_count >= kMaxStrips || strip.jpeg_len == 0) {
        fill_bad_ = true;
        return;
    }

    // Grow the slot buffer to fit and concatenate the strip's JPEG bytes. The slot
    // lives in PSRAM, which the HW-JPEG input DMA reads directly (the fullrange
    // decoder syncs the input cache UNALIGNED), so no separate DMA-input copy and
    // no alignment of the per-strip offset is needed — this is one fewer copy than
    // a per-strip DMA bounce buffer.
    uint32_t need = f.write_off + strip.jpeg_len;
    if (need > f.cap) {
        uint32_t cap = (need + 0xFFFFu) & ~0xFFFFu;  // round up to 64 KiB
        auto* nb = static_cast<uint8_t*>(heap_caps_realloc(f.buf, cap, MALLOC_CAP_SPIRAM));
        if (!nb) { fill_bad_ = true; return; }
        f.buf = nb;
        f.cap = cap;
    }
    std::memcpy(f.buf + f.write_off, strip.jpeg, strip.jpeg_len);
    StripDesc& d = f.strips[f.strip_count++];
    d.y = strip.y;
    d.h = strip.h;
    d.off = f.write_off;
    d.len = strip.jpeg_len;
    f.write_off = need;
    f.bytes += strip.jpeg_len;

    if (!strip.frame_end) return;

    // Frame complete: publish it (latest-frame-wins). If a prior frame is still
    // queued unconsumed, reclaim its slot here so it is not leaked — whoever pulls
    // a slot out of ready_q_ owns recycling it, and these queue ops are atomic, so
    // the decode task and this reclaim can't both take the same frame.
    int finished = fill_slot_;
    fill_slot_ = -1;
    int stale;
    if (xQueueReceive(static_cast<QueueHandle_t>(ready_q_), &stale, 0) == pdTRUE) {
        xQueueSend(static_cast<QueueHandle_t>(free_q_), &stale, 0);
    }
    xQueueSend(static_cast<QueueHandle_t>(ready_q_), &finished, 0);
}

void ADBMirroringScreen::on_link_close(agent_link::Link*, adb::Error err) {
    std::printf("mirror: link closed (%s)\n", adb::to_string(err));
    std::fflush(stdout);
}

void ADBMirroringScreen::decode_trampoline(void* arg) {
    static_cast<ADBMirroringScreen*>(arg)->decode_loop();
}

void ADBMirroringScreen::decode_loop() {
    // Consumer: drain finished frames, HW-decode each strip straight into the back
    // framebuffer, and present it via DisplayManager.flush() — which composites the
    // overlay bar (if visible) before presenting. The decode task owns fb_/back_
    // exclusively (DM.flush takes the LVGL lock only for the compose), so the double
    // buffer needs no lock of its own.
    //
    // The last decoded frame is RETAINED (held) instead of recycled immediately: it
    // is re-decoded to erase the bar when the overlay is hidden while the video is
    // static (no new frame arrives to repaint the band the bar covered) — the
    // Tab5-Screen-Streamer reveal trick.
    int  held = -1;          // last decoded slot, kept out of free_q_ for re-decode
    bool last_visible = false;  // overlay state of the last presented frame

    auto decode_all = [&](int slot, uint16_t* dst) -> bool {
        FrameSlot& f = slots_[slot];
        if (!dst) return false;
        for (int i = 0; i < f.strip_count; ++i) {
            const StripDesc& d = f.strips[i];
            if (!decode_one(f.buf + d.off, d.len, d.y, d.h, dst)) return false;
        }
        return true;
    };

    while (!decode_stop_) {
        int slot;
        bool got = xQueueReceive(static_cast<QueueHandle_t>(ready_q_), &slot,
                                 pdMS_TO_TICKS(100)) == pdTRUE;
        bool visible = display_manager.overlay_visible();

        if (!got) {
            // No new frame within the timeout (static screen, or stream stalled).
            if (last_visible && !visible && held >= 0) {
                // Bar just hidden over a static frame: re-decode the last frame so
                // the band it covered shows the video again (flush won't compose).
                if (decode_all(held, fb_[back_])) {
                    display_manager.flush(back_);
                    back_ ^= 1;
                }
                last_visible = false;
            } else if (visible) {
                // Bar up with no fresh video: recomposite it onto the displayed
                // buffer in place (no swap) so it stays responsive (e.g. on show).
                display_manager.flush(back_ ^ 1);
                last_visible = true;
            }
            continue;  // re-check stop
        }

        FrameSlot& f = slots_[slot];
        // Present only a fully-decoded frame: a HW JPEG decode error would otherwise
        // flush a half-updated buffer (the back buffer also still holds an older
        // frame's pixels). Drop it and keep showing the last good frame instead.
        bool ok = decode_all(slot, fb_[back_]);
        if (ok) {
            display_manager.flush(back_);  // composites the bar if visible
            back_ ^= 1;
            last_visible = visible;
            ++stats_frames_;
        }
        stats_strips_ += f.strip_count;
        stats_bytes_ += f.bytes;
        // On success retain this frame for the reveal-on-hide path (recycling the
        // previous one); on a decode error drop it and keep the last good held.
        if (ok) {
            if (held >= 0) xQueueSend(static_cast<QueueHandle_t>(free_q_), &held, 0);
            held = slot;
        } else {
            xQueueSend(static_cast<QueueHandle_t>(free_q_), &slot, 0);
        }

        // Log throughput roughly once per second (decode task).
        int64_t now = esp_timer_get_time();
        if (stats_start_us_ == 0) { stats_start_us_ = now; continue; }
        if (now - stats_start_us_ >= 1000000) {
            double secs = (now - stats_start_us_) / 1e6;
            std::printf("mirror: %.1f fps (%.1f strips/s, %.1f KB/s)\n",
                        stats_frames_ / secs, stats_strips_ / secs,
                        stats_bytes_ / 1024.0 / secs);
            std::fflush(stdout);
            stats_start_us_ = now;
            stats_frames_ = 0;
            stats_strips_ = 0;
            stats_bytes_ = 0;
        }
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}

bool ADBMirroringScreen::decode_one(uint8_t* jpeg, uint32_t len, uint16_t y,
                                    uint16_t h, uint16_t* dst) {
    if (!jpeg || len == 0 || !dst) return false;

    // Lazily create the engine (decode task only).
    if (!jpeg_) {
        jpeg_decode_engine_cfg_t ecfg = {};
        ecfg.timeout_ms = 1000;
        if (jpeg_new_decoder_engine(&ecfg, &jpeg_) != ESP_OK) { jpeg_ = nullptr; return false; }
    }

    // Full-width strip → tightly-packed decode lands in place: the destination is
    // the framebuffer row band starting at row y (x == 0), and the decoded
    // PANEL_W×h picture is exactly PANEL_W*h pixels contiguous — no scratch, no
    // blit, no stride. (device: P4 HW JPEG straight to PSRAM; host: libjpeg.)
    jpeg_decode_cfg_t dcfg = {};
    dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR, not RGB: the panel is driven R-in-high-bits RGB565, and on the P4 HW
    // JPEG decoder that byte order is produced by the BGR scramble — the RGB enum's
    // RGB565 scramble mis-packs the 16-bit pixel (greens split across the byte
    // boundary) and the image comes out as rainbow noise. Matches the proven
    // Tab5-Screen-Streamer decoder.
    dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint16_t* out = dst + (size_t)y * PANEL_W;  // x == 0
    uint32_t outbuf = (uint32_t)((size_t)PANEL_W * h * 2);
    uint32_t out_size = 0;
    return jpeg_decoder_process_full_range(jpeg_, &dcfg, jpeg, len,
               reinterpret_cast<uint8_t*>(out), outbuf, &out_size) == ESP_OK;
}

void ADBMirroringScreen::free_decoder() {
    if (jpeg_) { jpeg_del_decoder_engine(jpeg_); jpeg_ = nullptr; }
}
