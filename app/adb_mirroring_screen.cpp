#include "adb_mirroring_screen.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "adb.hpp"  // adb::Error, adb::to_string
#include "adb_app.hpp"
#include "agent_client.hpp"
#include "display_manager.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "jpeg_fullrange_decode.h"
#include "lvgl.hpp"
#include "resources/resources.h"  // R.font.lucide_40, LUCIDE_*
#include "screen_manager.hpp"

// ---------------------------------------------------------------------------
// ADBMirroringScreen
// ---------------------------------------------------------------------------

ADBMirroringScreen::ADBMirroringScreen() = default;

ADBMirroringScreen::~ADBMirroringScreen() {
    // onExit normally runs first on pop and does the orderly teardown; this is the
    // idempotent backstop. Stop the producer (clear our video listener so no more
    // on_video_strip arrives) before joining the consumer.
    if (auto l = app::agent_client().link()) l->set_video_listener({});
    if (decode_task_) {
        decode_stop_ = true;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    if (poll_timer_) { lv_timer_delete(static_cast<lv_timer_t*>(poll_timer_)); poll_timer_ = nullptr; }
    if (overlay_active_) { display_manager.exit_overlay(); overlay_active_ = false; }
    if (decode_done_) { vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_)); decode_done_ = nullptr; }
    if (ready_q_) { vQueueDelete(static_cast<QueueHandle_t>(ready_q_)); ready_q_ = nullptr; }
    if (free_q_) { vQueueDelete(static_cast<QueueHandle_t>(free_q_)); free_q_ = nullptr; }
    for (auto& s : slots_) { if (s.buf) { heap_caps_free(s.buf); s.buf = nullptr; } }
    free_decoder();
}

void ADBMirroringScreen::build() {
    // The LVGL root on the MAIN display: black, and (until the mirror starts) the
    // host of a centered "Connecting…" label rendered by the normal LVGL runtime.
    // Once mirroring starts we switch to DM overlay mode, after which the root
    // never invalidates so the main display leaves the framebuffers to the mirror.
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    waiting_label_ = lv_label_create(root_);
    lv_obj_t* wl = static_cast<lv_obj_t*>(waiting_label_);
    lv_label_set_text(wl, "Connecting to agent...");
    lv_obj_set_style_text_color(wl, lv_color_white(), 0);
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_28, 0);
    lv_obj_center(wl);

    // Grab the bsp framebuffers (we decode strips straight into them). They are
    // cleared to black in start_mirror_ui via enter_overlay(clear_framebuffers) —
    // once the main display is isolated from them — so the letterbox stays black.
    for (int i = 0; i < kFbCount; ++i)
        fb_[i] = static_cast<uint8_t*>(display_manager.framebuffer(i));
    back_ = 0;
    front_ = -1;

    // Receive/decode split: the reader thread fills frame slots (free_q_) and the
    // decode task drains finished frames (ready_q_), so the blocking HW-JPEG decode
    // runs off the reader thread. The decode task idles (no frames) until the
    // mirror starts. Hand all slots to the producer to start.
    free_q_ = xQueueCreate(kSlots, sizeof(int));
    ready_q_ = xQueueCreate(1, sizeof(int));
    for (int i = 0; i < kSlots; ++i) xQueueSend(static_cast<QueueHandle_t>(free_q_), &i, 0);
    decode_done_ = xSemaphoreCreateBinary();
    decode_stop_ = false;
    TaskHandle_t dt = nullptr;
    xTaskCreate(&ADBMirroringScreen::decode_trampoline, "mirror_decode", 8192, this, 6, &dt);
    decode_task_ = dt;
}

void ADBMirroringScreen::onEnter() {
    // Ensure the tab5adb-agent is connected (lazy: launched on first use, reused on
    // re-entry). If it is already live there is no wait — start the mirror straight
    // away; otherwise keep the "Connecting…" label up until ensure_connected's LVGL
    // -thread callback fires.
    if (app::agent_client().ready()) {
        start_mirror_ui();
        return;
    }
    std::weak_ptr<ADBMirroringScreen> self =
        std::static_pointer_cast<ADBMirroringScreen>(shared_from_this());
    app::agent_client().ensure_connected([self](bool ok) {
        auto s = self.lock();
        if (!s || s->exited()) return;  // navigated away while connecting
        if (ok) {
            s->start_mirror_ui();
        } else if (s->waiting_label_) {
            lv_label_set_text(static_cast<lv_obj_t*>(s->waiting_label_),
                              "Agent connection failed");
        }
    });
}

void ADBMirroringScreen::start_mirror_ui() {
    // Drop the waiting label, switch the DisplayManager into overlay mode (the
    // mirror owns the framebuffers; a small LVGL display renders the control strip
    // DM composites at flush time), register as the link's video listener, and
    // MIRROR_START. Build the strip BEFORE start_mirror so any on_orientation /
    // on_video_strip callback finds an overlay to drive.
    if (waiting_label_) { lv_obj_delete(static_cast<lv_obj_t*>(waiting_label_)); waiting_label_ = nullptr; }

    apply_overlay(cur_rot_, /*first=*/true);  // cur_rot_ defaults to 0 (portrait)
    display_manager.set_overlay_visible(true);

    // Poll the raw panel touch on the LVGL thread: a swipe out of the bottom-left
    // corner reveals a hidden strip. touch_point + the indev read both run on the
    // LVGL thread, so the cached state is consistent.
    poll_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<ADBMirroringScreen*>(lv_timer_get_user_data(t))->poll_touch();
        },
        30, this);

    // Register on the established link, then start mirroring. The video channel
    // (on_mirror_started / on_video_strip / on_orientation) fires on the adb reader
    // thread.
    auto l = app::agent_client().link();
    if (!l) {
        std::printf("mirror: link gone after connect\n");
        return;
    }
    l->set_video_listener(std::static_pointer_cast<agent_link::VideoListener>(
        std::static_pointer_cast<ADBMirroringScreen>(shared_from_this())));
    l->start_mirror();  // 720x1280 fit, video (default MirrorConfig)
}

void ADBMirroringScreen::apply_overlay(uint8_t rot, bool first) {
    // enter_overlay is re-entrant: `first` only chooses whether to clear the
    // framebuffers (black letterbox on the initial entry) and whether to restore
    // visibility (a rotation-change rebuild keeps the strip shown if it was).
    // Footprint = the cross x len strip flush against the viewer's bottom-left
    // corner; enter_overlay derives the (rotation-swapped) content size from it.
    bool was_visible = !first && display_manager.overlay_visible();
    bool right, bottom;
    anchor_corner(rot, &right, &bottom);
    int x1 = right ? PANEL_W - kStripCross : 0;
    int y1 = bottom ? PANEL_H - kStripLen : 0;
    lv_area_t rect = {x1, y1, x1 + kStripCross - 1, y1 + kStripLen - 1};

    DisplayManager::OverlayConfig cfg{rect, view_rot(rot) * 90, 1.0f, /*clear=*/first};
    lv_obj_t* scr = display_manager.enter_overlay(cfg);
    overlay_active_ = true;
    cur_rot_ = rot;
    if (scr) build_overlay_buttons(scr, rot_landscape(rot));
    if (was_visible) display_manager.set_overlay_visible(true);
}

void ADBMirroringScreen::build_overlay_buttons(lv_obj_t* scr, bool land) {
    // The control items read from the corner outward (SEP = a group separator
    // line). This single order IS the landscape (left->right) layout; portrait
    // (top->bottom) is the same list reversed:
    //   landscape: Hide | Back Home Recents | Vol- Vol+ Power | OpMode DispMode End
    //   portrait : End DispMode OpMode | Power Vol+ Vol- | Recents Home Back | Hide
    // Icons are placeholders (lucide_40) pending final selection.
    enum { HIDE, BACK, HOME, RECENTS, VOLDN, VOLUP, POWER, OPMODE, DISPMODE, END, SEP = -1 };
    static const int kOrder[] = {
        HIDE, SEP, BACK, HOME, RECENTS, SEP, VOLDN, VOLUP, POWER, SEP, OPMODE, DISPMODE, END,
    };
    constexpr int kN = sizeof(kOrder) / sizeof(kOrder[0]);

    auto icon_of = [](int id) -> const char* {
        switch (id) {
            case HIDE:     return LUCIDE_CHEVRON_DOWN;         // hide the strip
            case BACK:     return LUCIDE_ARROW_LEFT;           // Android Back
            case HOME:     return LUCIDE_CIRCLE;               // Android Home
            case RECENTS:  return LUCIDE_SQUARE;               // Android Recents
            case VOLDN:    return LUCIDE_VOLUME_1;             // volume down
            case VOLUP:    return LUCIDE_VOLUME_2;             // volume up
            case POWER:    return LUCIDE_POWER;                // power
            case OPMODE:   return LUCIDE_POINTER;              // touch-control toggle
            case DISPMODE: return LUCIDE_MAXIMIZE;             // fit/fill toggle
            default:       return LUCIDE_X;                    // END = end mirroring
        }
    };

    // The overlay content rect is composited opaquely (PPA SRM), so make the strip
    // background fill it (the panel area outside the strip stays black = the memset
    // overlay buffer).
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t* cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_side(cont, (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cont, kPad, 0);
    lv_obj_set_flex_flow(cont, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, kGap, 0);
    lv_obj_set_style_pad_column(cont, kGap, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < kN; ++i) {
        int id = kOrder[land ? i : (kN - 1 - i)];  // portrait = reversed order
        if (id == SEP) {
            // A thin line spanning the strip across its short axis.
            lv_obj_t* s = lv_obj_create(cont);
            lv_obj_remove_style_all(s);
            lv_obj_set_size(s, land ? kSep : LV_PCT(100), land ? LV_PCT(100) : kSep);
            lv_obj_set_style_bg_color(s, lv_color_hex(0x4A4A4A), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            continue;
        }
        lv_obj_t* b = lv_button_create(cont);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, kBtn, kBtn);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x4A4A4A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, icon_of(id));
        lv_obj_set_style_text_font(lbl, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        // The device buttons inject a key tap on the source over the agent link
        // (agent_link::Link::tap_key, TYPE=INPUT). The link is alive whenever the
        // mirror is streaming, and tap_key is non-blocking, so it is safe to call
        // straight from the LVGL event.
        auto wire_key = [](lv_obj_t* btn, uint32_t keycode) {
            lv_obj_add_event_fn(btn, LV_EVENT_CLICKED, [keycode](lv_event_t*) {
                if (auto l = app::agent_client().link()) l->tap_key(keycode);
            });
        };
        switch (id) {
            case HIDE:
                lv_obj_add_event_fn(b, LV_EVENT_CLICKED, [](lv_event_t*) {
                    display_manager.set_overlay_visible(false);
                });
                break;
            case BACK:     wire_key(b, agent_link::kKeyBack); break;
            case HOME:     wire_key(b, agent_link::kKeyHome); break;
            case RECENTS:  wire_key(b, agent_link::kKeyAppSwitch); break;
            case VOLDN:    wire_key(b, agent_link::kKeyVolumeDown); break;
            case VOLUP:    wire_key(b, agent_link::kKeyVolumeUp); break;
            case POWER:    wire_key(b, agent_link::kKeyPower); break;
            case END:
                // Defer the pop: it runs onExit -> exit_overlay, which deletes this
                // overlay (and this button) — don't tear it down from its own event.
                lv_obj_add_event_fn(b, LV_EVENT_CLICKED,
                                    [](lv_event_t*) { lv_async_call([] { screen_manager.pop(); }); });
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF8888), 0);
                break;
            default:
                // OpMode / DispMode land once their UI is final.
                break;
        }
    }
}

bool ADBMirroringScreen::in_corner(int px, int py, uint8_t rot) {
    bool right, bottom;
    anchor_corner(rot, &right, &bottom);
    return (right ? px > PANEL_W - kCornerSwipe : px < kCornerSwipe) &&
           (bottom ? py > PANEL_H - kCornerSwipe : py < kCornerSwipe);
}

void ADBMirroringScreen::poll_touch() {
    bsp_touch_point_t p;
    bool pressed = display_manager.touch_point(&p);
    bool visible = display_manager.overlay_visible();

    if (pressed && !touch_prev_) {
        // Press edge: arm a reveal swipe if it starts in the corner while hidden.
        if (!visible && in_corner(p.x, p.y, cur_rot_)) {
            swipe_active_ = true;
            swipe_x0_ = p.x;
            swipe_y0_ = p.y;
        }
    } else if (pressed && swipe_active_ && !visible) {
        int dx = p.x - swipe_x0_, dy = p.y - swipe_y0_;
        if (dx * dx + dy * dy >= kSwipeThresh * kSwipeThresh) {
            display_manager.set_overlay_visible(true);  // revealed
            swipe_active_ = false;
        }
    } else if (!pressed) {
        swipe_active_ = false;
    }
    touch_prev_ = pressed;
}

void ADBMirroringScreen::on_orientation(agent_link::Link*,
                                        const agent_link::OrientationInfo& info) {
    // Reader thread: marshal the layout switch to the LVGL thread (overlay rebuild
    // touches LVGL + the DM overlay). Skip if we navigated away or nothing changed.
    // Rebuild on any rotation-code change (90<->270 flips the anchor corner + PPA
    // angle even though both are "landscape").
    uint8_t rot = info.rotation & 3;
    std::weak_ptr<ADBMirroringScreen> self =
        std::static_pointer_cast<ADBMirroringScreen>(shared_from_this());
    lv_async_call([self, rot] {
        auto s = self.lock();
        if (!s || s->exited() || !s->overlay_active_) return;
        if (rot == s->cur_rot_) return;
        s->apply_overlay(rot, /*first=*/false);  // rebuild: keeps visibility
    });
}

void ADBMirroringScreen::onExit() {
    // Stop the mirror but KEEP the agent link alive for next time (the AgentClient
    // contract): MIRROR_STOP tells the agent to stop streaming, and clearing our
    // video listener stops any strip still in flight from reaching us. The link /
    // agent process stay connected so re-entering this screen resumes instantly.
    if (auto l = app::agent_client().link()) {
        l->stop_mirror();
        l->set_video_listener({});  // detach the producer (no more on_video_strip)
    }
    // Producer detached; join the decode task before the framebuffers are reclaimed
    // by the previous screen's re-render, so it can't flush into a buffer being
    // reused.
    if (decode_task_) {
        decode_stop_ = true;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    // Both threads are stopped, so no DM.flush can race the teardown: drop the
    // overlay (restores the indev to the main display + frees the overlay display)
    // and the toggle timer. The previous screen's re-render reclaims the panel.
    if (poll_timer_) { lv_timer_delete(static_cast<lv_timer_t*>(poll_timer_)); poll_timer_ = nullptr; }
    if (overlay_active_) { display_manager.exit_overlay(); overlay_active_ = false; }
}

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

void ADBMirroringScreen::decode_trampoline(void* arg) {
    static_cast<ADBMirroringScreen*>(arg)->decode_loop();
}

void ADBMirroringScreen::decode_loop() {
    // Consumer: drain finished frames, HW-decode each strip straight into the back
    // framebuffer, and present it via DisplayManager.flush() — which composites the
    // overlay strip (if visible) before presenting. The decode task owns fb_/back_
    // exclusively (DM.flush takes the LVGL lock only for the compose), so the double
    // buffer needs no lock of its own.
    //
    // The last decoded frame is RETAINED (held) instead of recycled immediately: it
    // is re-decoded to erase the strip when the overlay is hidden while the video
    // is static (no new frame arrives to repaint the region the strip covered) —
    // the Tab5-Screen-Streamer reveal trick.
    int  held = -1;          // last decoded slot, kept out of free_q_ for re-decode
    bool last_visible = false;  // overlay state of the last presented frame

    auto decode_all = [&](int slot, uint8_t* dst) -> bool {
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
                // Strip just hidden over a static frame: re-decode the last frame so
                // the band it covered shows the video again (flush won't compose).
                if (decode_all(held, fb_[back_])) {
                    display_manager.flush(back_);
                    front_ = back_;
                    back_ = (back_ + 1) % kFbCount;
                }
                last_visible = false;
            } else if (visible && front_ >= 0) {
                // Strip up with no fresh video: recomposite it onto the displayed
                // buffer in place (no swap) so it stays responsive (e.g. on show).
                display_manager.flush(front_);
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
            display_manager.flush(back_);  // composites the strip if visible
            front_ = back_;
            back_ = (back_ + 1) % kFbCount;
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
                                    uint16_t h, uint8_t* dst) {
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
    const bool rgb888 = (display_manager.format() == BSP_PIXEL_FORMAT_RGB888);
    const size_t bpp = rgb888 ? 3 : 2;

    jpeg_decode_cfg_t dcfg = {};
    dcfg.output_format = rgb888 ? JPEG_DECODE_OUT_FORMAT_RGB888
                                : JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR, not RGB, for both depths: the framebuffer byte order is LVGL's native
    // RGB888 (B,G,R) / R-in-high-bits RGB565, and on the P4 HW JPEG decoder that
    // packing is the one the BGR scramble produces — the RGB enum mis-orders the
    // bytes and the image comes out as colour-swapped / rainbow noise. Matches the
    // proven Tab5-Screen-Streamer 565 path; the host shim packs B,G,R the same way.
    // (The 888 device scramble is still the remaining real-HW verification.)
    dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint8_t* out = dst + (size_t)y * PANEL_W * bpp;  // x == 0
    uint32_t outbuf = (uint32_t)((size_t)PANEL_W * h * bpp);
    uint32_t out_size = 0;
    return jpeg_decoder_process_full_range(jpeg_, &dcfg, jpeg, len,
               out, outbuf, &out_size) == ESP_OK;
}

void ADBMirroringScreen::free_decoder() {
    if (jpeg_) { jpeg_del_decoder_engine(jpeg_); jpeg_ = nullptr; }
}
