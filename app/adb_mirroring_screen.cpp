#include "adb_mirroring_screen.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "adb.hpp"  // adb::Error, adb::to_string
#include "adb_app.hpp"
#include "agent_client.hpp"
#include "display_manager.hpp"
#include "esp_heap_caps.h"
#include "settings.hpp"  // app::audio_output_mode
#include "esp_timer.h"
#include "jpeg_decode_enhanced.h"
#include "lvgl.hpp"
#include "resources/resources.h"  // R.font.lucide_40, LUCIDE_*
#include "screen_manager.hpp"

// ---------------------------------------------------------------------------
// ADBMirroringScreen
// ---------------------------------------------------------------------------

ADBMirroringScreen::ADBMirroringScreen() = default;

ADBMirroringScreen::~ADBMirroringScreen() {
    // onExit normally runs first on pop and does the orderly stream/overlay teardown.
    // Only do it here as a backstop for the no-onExit path (the screen destroyed
    // without a navigation — e.g. AgentClient teardown at shutdown). When onExit DID
    // run (exited()), skip it: the screen is retired asynchronously (lv_async_call),
    // AFTER the next screen's onAppear has already re-registered its own video/touch
    // listeners (e.g. the device screen's AgentPreview) — clearing them here would
    // clobber that screen, dropping all its frames (a blank device-screen preview).
    if (!exited()) {
        if (auto l = app::agent_client().link()) l->set_video_listener({});
        release_all_pointers();
        display_manager.set_touch_listener({});
        if (decode_task_) {
            decode_stop_ = true;
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
            decode_task_ = nullptr;
        }
        if (overlay_active_) { display_manager.exit_overlay(); overlay_active_ = false; }
    }
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
    batch_touch_ = app::connection_is_tcp();
    touch_batch_.clear();
    has_last_touch_snapshot_ = false;

    // Observe raw touch (pushed from the BSP dispatch task): a swipe out of
    // the bottom-left corner reveals a hidden strip. on_touch fires off the LVGL
    // thread, but the swipe logic only flips DM flags (no LVGL access).
    display_manager.set_touch_listener(
        std::static_pointer_cast<DisplayManager::TouchListener>(
            std::static_pointer_cast<ADBMirroringScreen>(shared_from_this())));

    // Register on the established link, then start mirroring. The video channel
    // (on_mirror_started / on_video_strip / on_orientation) fires on the adb reader
    // thread.
    auto l = app::agent_client().link();
    if (!l) {
        std::printf("mirror: link gone after connect\n");
        return;
    }

    // Audio (§6): Tab5Only streams the device's audio to the Tab5 (the agent mutes
    // the phone via REMOTE_SUBMIX); PhoneOnly streams none. Set up the player BEFORE
    // start_mirror so its audio listener is registered when the agent answers — and
    // audio_on_ adds the AUDIO bit to the MIRROR_START streams (mirror_config_for).
    audio_on_ = (app::audio_output_mode() == app::AudioOutputMode::Tab5Only);
    if (audio_on_) {
        agent_audio_ = AgentAudio::create();
        if (agent_audio_) agent_audio_->start();  // registers the audio listener
        else audio_on_ = false;                    // no ring -> fall back to no audio
    }

    l->set_video_listener(std::static_pointer_cast<agent_link::VideoListener>(
        std::static_pointer_cast<ADBMirroringScreen>(shared_from_this())));
    l->start_mirror(mirror_config_for(disp_mode_));  // 720x1280, current display mode + audio

    // Tailor the DispMode button to the source's current resolution (hide it when
    // already panel-aspect / drop Adapt when a `wm size` override is set).
    query_disp_mode_availability();
}

// ---------------------------------------------------------------------------
// Display mode (DispMode button): Fit / Fill / Adapt
// ---------------------------------------------------------------------------

agent_link::MirrorConfig ADBMirroringScreen::mirror_config_for(int mode) const {
    agent_link::MirrorConfig cfg;  // 720x1280 panel, video (defaults)
    // Fit = letterbox, Fill = cover+crop, Adapt = the agent resizes the source itself
    // (kScaleAdapt → `wm size` to the panel aspect, restored agent-side on stop /
    // disconnect / shutdown) so a plain fit fills the panel with no letterbox/crop.
    cfg.scale_mode = (mode == kDispFill)  ? agent_link::kScaleFill
                   : (mode == kDispAdapt) ? agent_link::kScaleAdapt
                                          : agent_link::kScaleFit;
    // Keep AUDIO in the streams across DispMode restarts so the audio stream isn't
    // dropped when only the video scale/size changes (§6.1 Tab5Only).
    cfg.streams = agent_link::kCapVideo | (audio_on_ ? agent_link::kCapAudio : 0);
    // The TCP/Wi-Fi link is far slower and higher-latency than USB, so over TCP the
    // mirror is painful at the agent defaults: drop quality + frame rate and split
    // each frame into more strips (smaller JPEGs → less head-of-line latency per
    // frame). USB keeps the agent defaults (0 = quality 80 / uncapped / 4 strips).
    if (app::connection_is_tcp()) {
        cfg.jpeg_quality = 40;
        cfg.max_fps = 15;
        cfg.split_count = 16;
    }
    return cfg;
}

void ADBMirroringScreen::apply_disp_mode(int mode) {
    if (mode == disp_mode_) return;
    disp_mode_ = mode;
    // Every mode (incl. Adapt) is just a fresh MIRROR_START — the agent applies/
    // restores the Adapt `wm size` itself (kScaleAdapt), so the Tab5 never drives it.
    restart_mirror(mode);
}

void ADBMirroringScreen::restart_mirror(int mode) {
    // The agent restarts the stream in place on a fresh MIRROR_START (our video
    // listener stays registered, the decode pipeline keeps running). Non-blocking.
    if (auto l = app::agent_client().link()) l->start_mirror(mirror_config_for(mode));
}

namespace {
// Parsed `wm size` output: the device's physical resolution and, if a `wm size`
// override is active, the override. The CURRENT effective resolution is the override
// when present, else the physical.
struct WmSize {
    bool ok = false;            // physical size parsed
    int phys_w = 0, phys_h = 0;
    bool has_override = false;  // an "Override size:" line is present
    int ov_w = 0, ov_h = 0;
};

// Parse one "<Label> size: WxH" line at/after `label` in `out`.
bool parse_wm_line(const std::string& out, const char* label, int* w, int* h) {
    auto pos = out.find(label);
    if (pos == std::string::npos) return false;
    char fmt[40];
    std::snprintf(fmt, sizeof(fmt), "%s %%dx%%d", label);  // "<label> %dx%d"
    return std::sscanf(out.c_str() + pos, fmt, w, h) == 2 && *w > 0 && *h > 0;
}

WmSize parse_wm_size(const std::string& out) {
    WmSize s;
    s.ok = parse_wm_line(out, "Physical size:", &s.phys_w, &s.phys_h);
    s.has_override = parse_wm_line(out, "Override size:", &s.ov_w, &s.ov_h);
    return s;
}

// True when (w,h) is the Tab5 panel aspect (9:16 or 16:9), within tolerance — the
// long:short ratio equals PANEL_H:PANEL_W either way, so one check covers both.
bool is_panel_aspect(int w, int h) {
    int lo = std::min(w, h), hi = std::max(w, h);
    if (lo <= 0) return false;
    double panel = static_cast<double>(PANEL_H) / PANEL_W;  // 16/9
    return std::fabs(static_cast<double>(hi) / lo - panel) < 0.02;
}

}  // namespace

void ADBMirroringScreen::query_disp_mode_availability() {
    auto* c = app::adb_client();
    if (!c) return;  // keep the defaults (DispMode shown, full Fit/Fill/Adapt cycle)
    std::weak_ptr<ADBMirroringScreen> self =
        std::static_pointer_cast<ADBMirroringScreen>(shared_from_this());
    c->exec("wm size", [self](adb::Error e, const std::string& out) {
        // Reader thread: decide from the source's current resolution.
        bool enabled = true, adapt = true;
        if (e == adb::Error::Ok) {
            WmSize w = parse_wm_size(out);
            if (w.ok) {
                int ew = w.has_override ? w.ov_w : w.phys_w;  // current effective size
                int eh = w.has_override ? w.ov_h : w.phys_h;
                if (is_panel_aspect(ew, eh)) {
                    enabled = false;  // already panel-aspect: fit == fill == adapt, grey it
                } else if (w.has_override &&
                           (w.ov_w != w.phys_w || w.ov_h != w.phys_h)) {
                    adapt = false;  // a user `wm size` override is set: don't offer Adapt
                }
            }
        }
        lv_async_call([self, enabled, adapt] {
            auto s = self.lock();
            if (!s || s->exited()) return;
            bool need_rebuild = (s->dispmode_enabled_ != enabled) && s->overlay_active_;
            s->dispmode_enabled_ = enabled;
            s->adapt_allowed_ = adapt;  // read live by the DispMode click handler
            // The button is always present, so the layout is unchanged; only its
            // enabled/greyed appearance flips, so rebuild when that changes. The
            // cycle length is read live, so a disabled Adapt needs no rebuild.
            if (need_rebuild) s->apply_overlay(s->cur_rot_, /*first=*/false);
        });
    });
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
            case OPMODE: {
                // Touch-control toggle (§4.7): when on, touches over the mirror are
                // injected to the source device. Active = the LVGL theme's primary
                // blue (matching a default button); inactive = white. Turning it off
                // releases any still-down pointers so the source sees no stuck
                // finger. Capturing `this` is safe — the overlay (and this button)
                // is torn down in onExit before the screen frees, so the event can
                // only fire while the screen is alive.
                lv_color_t on_color = lv_palette_main(LV_PALETTE_BLUE);
                lv_obj_set_style_text_color(
                    lbl, passthrough_.load() ? on_color : lv_color_white(), 0);
                lv_obj_add_event_fn(b, LV_EVENT_CLICKED, [this, lbl, on_color](lv_event_t*) {
                    bool now = !passthrough_.load();
                    passthrough_.store(now);
                    if (!now) release_all_pointers();
                    lv_obj_set_style_text_color(lbl, now ? on_color : lv_color_white(), 0);
                });
                break;
            }
            case DISPMODE:
                if (!dispmode_enabled_) {
                    // Source already panel-aspect (fit == fill == adapt): show the
                    // button greyed-out and inert instead of hiding it. LV_STATE_DISABLED
                    // blocks the press/click in this LVGL build; the grey icon signals it.
                    lv_obj_add_state(b, LV_STATE_DISABLED);
                    lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
                    break;
                }
                // Cycle Fit -> Fill -> Adapt -> Fit, or Fit <-> Fill when Adapt is
                // disabled (a non-default `wm size` override we must not clobber). The
                // active mode is evident from the mirrored image, so the icon stays
                // put. Capturing `this` is safe — the overlay (and this button) is torn
                // down in onExit before the screen frees; adapt_allowed_ is read live.
                lv_obj_add_event_fn(b, LV_EVENT_CLICKED, [this](lv_event_t*) {
                    int n = adapt_allowed_ ? 3 : 2;
                    apply_disp_mode((disp_mode_ + 1) % n);
                });
                break;
            default:
                break;
        }
    }
}

bool ADBMirroringScreen::in_corner(int px, int py, uint8_t rot) {
    bool right, bottom;
    anchor_corner(rot, &right, &bottom);
    // Distance from the anchored corner along each axis (0 at the corner's edge).
    int dx = right ? (PANEL_W - 1 - px) : px;
    int dy = bottom ? (PANEL_H - 1 - py) : py;
    if (dx < 0 || dy < 0) return false;
    // The L = a band along the corner's horizontal edge OR its vertical edge.
    bool horiz = dy < kEdgeThick && dx < kEdgeReach;
    bool vert = dx < kEdgeThick && dy < kEdgeReach;
    return horiz || vert;
}

bool ADBMirroringScreen::in_overlay_footprint(int px, int py, uint8_t rot) {
    bool right, bottom;
    anchor_corner(rot, &right, &bottom);
    int x1 = right ? PANEL_W - kStripCross : 0;
    int y1 = bottom ? PANEL_H - kStripLen : 0;
    return px >= x1 && px < x1 + kStripCross && py >= y1 && py < y1 + kStripLen;
}

void ADBMirroringScreen::flush_touch_batch(agent_link::Link* link) {
    if (!link || touch_batch_.empty()) return;
    if (link->inject_touch_snapshot_batch(touch_batch_.data(), touch_batch_.size()) ==
        adb::Error::Ok) {
        touch_batch_.clear();
    }
}

void ADBMirroringScreen::submit_touch_snapshot(
    agent_link::Link* link, const agent_link::Link::TouchSnapshot& snapshot) {
    if (!link) {
        touch_batch_.clear();
        has_last_touch_snapshot_ = false;
        return;
    }

    auto same_points = [](const agent_link::Link::TouchSnapshot& a,
                          const agent_link::Link::TouchSnapshot& b) {
        if (a.point_count != b.point_count) return false;
        for (size_t i = 0; i < a.point_count; ++i) {
            if (a.points[i].pointer_id != b.points[i].pointer_id ||
                a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y) {
                return false;
            }
        }
        return true;
    };
    auto same_ids = [](const agent_link::Link::TouchSnapshot& a,
                       const agent_link::Link::TouchSnapshot& b) {
        if (a.point_count != b.point_count) return false;
        for (size_t i = 0; i < a.point_count; ++i) {
            if (a.points[i].pointer_id != b.points[i].pointer_id) return false;
        }
        return true;
    };

    if (!has_last_touch_snapshot_ && snapshot.point_count == 0) return;
    if (has_last_touch_snapshot_ && same_points(last_touch_snapshot_, snapshot)) {
        if (batch_touch_ && !touch_batch_.empty() && link->tx_pending_bytes() == 0) {
            flush_touch_batch(link);
        }
        return;
    }

    const bool edge = !has_last_touch_snapshot_ ||
                      !same_ids(last_touch_snapshot_, snapshot);
    if (!batch_touch_) {
        if (link->inject_touch_snapshot(snapshot) == adb::Error::Ok) {
            last_touch_snapshot_ = snapshot;
            has_last_touch_snapshot_ = true;
        }
        return;
    }

    if (touch_batch_.size() >= kSnapshotBatchMax) touch_batch_.erase(touch_batch_.begin());
    touch_batch_.push_back(snapshot);
    last_touch_snapshot_ = snapshot;
    has_last_touch_snapshot_ = true;
    if (edge || link->tx_pending_bytes() == 0) flush_touch_batch(link);
}

void ADBMirroringScreen::release_all_pointers() {
    std::lock_guard<std::mutex> lk(pass_mtx_);
    auto l = app::agent_client().link();
    bool had_pass = false;
    for (auto& p : pass_) {
        had_pass |= p.used && p.kind == PtKind::Pass;
        p.used = false;
    }
    if (!had_pass &&
        (!has_last_touch_snapshot_ || last_touch_snapshot_.point_count == 0)) return;

    agent_link::Link::TouchSnapshot snapshot{};
    snapshot.sample_time_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    submit_touch_snapshot(l.get(), snapshot);
}

void ADBMirroringScreen::on_touch(const bsp_touch_point_t* pts, int count) {
    const bool po = passthrough_.load();
    const bool visible = display_manager.overlay_visible();
    const uint8_t rot = cur_rot_;
    auto link = app::agent_client().link();

    std::lock_guard<std::mutex> lk(pass_mtx_);

    auto slot_of = [&](int id) -> int {
        for (int s = 0; s < kMaxPass; ++s)
            if (pass_[s].used && pass_[s].id == id) return s;
        return -1;
    };
    auto free_slot = [&]() -> int {
        for (int s = 0; s < kMaxPass; ++s)
            if (!pass_[s].used) return s;
        return -1;
    };

    bool seen[kMaxPass] = {false};

    for (int i = 0; i < count; ++i) {
        const int id = pts[i].id;
        const uint16_t x = static_cast<uint16_t>(pts[i].x);
        const uint16_t y = static_cast<uint16_t>(pts[i].y);
        int s = slot_of(id);

        if (s < 0) {
            // New pointer: classify. The reveal corner is reserved regardless of
            // mode (so the strip is always recoverable); passthrough only fires in
            // touch-control mode and outside a visible overlay strip.
            s = free_slot();
            if (s < 0) continue;  // more than kMaxPass fingers — ignore the extra
            ActivePtr& p = pass_[s];
            p.used = true; p.id = id; p.x = x; p.y = y; p.rx0 = x; p.ry0 = y;
            if (!visible && in_corner(x, y, rot)) {
                p.kind = PtKind::Reveal;
            } else if (po && !(visible && in_overlay_footprint(x, y, rot))) {
                p.kind = PtKind::Pass;
            } else {
                p.kind = PtKind::Ignore;  // overlay handles it, or passthrough off
            }
        } else {
            ActivePtr& p = pass_[s];
            p.x = x; p.y = y;
            if (p.kind == PtKind::Pass) {
                if (!po) {
                    p.used = false;
                    continue;
                }
            } else if (p.kind == PtKind::Reveal && !visible) {
                int dx = x - p.rx0, dy = y - p.ry0;
                if (dx * dx + dy * dy >= kSwipeThresh * kSwipeThresh) {
                    // Reveal, masking this same press from the indev until the
                    // finger lifts so the gesture isn't delivered as a tap on a
                    // freshly-shown button. consume FIRST (close the unmasked window).
                    display_manager.consume_overlay_touch();
                    display_manager.set_overlay_visible(true);
                    p.kind = PtKind::Ignore;  // gesture consumed
                }
            }
        }
        seen[s] = true;
    }

    for (int s = 0; s < kMaxPass; ++s) {
        if (!pass_[s].used || seen[s]) continue;
        pass_[s].used = false;
    }

    agent_link::Link::TouchSnapshot snapshot{};
    snapshot.sample_time_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    for (const auto& p : pass_) {
        if (!p.used || p.kind != PtKind::Pass) continue;
        auto& point = snapshot.points[snapshot.point_count++];
        point.pointer_id = static_cast<uint8_t>(p.id);
        point.x = p.x;
        point.y = p.y;
    }
    std::sort(snapshot.points, snapshot.points + snapshot.point_count,
              [](const auto& a, const auto& b) { return a.pointer_id < b.pointer_id; });
    submit_touch_snapshot(link.get(), snapshot);
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
    // Stop audio playback (clears its audio listener, closes bsp_audio); the link
    // stays connected for next time. Safe before or after the video detach.
    if (agent_audio_) {
        agent_audio_->stop();
        agent_audio_.reset();
    }
    audio_on_ = false;
    // Leaving while in Adapt: the agent restores the device resolution itself on the
    // MIRROR_STOP that stop_mirror() (below) sends — and on disconnect/shutdown if the
    // link is already gone — so the Tab5 issues no `wm size reset`.
    // Stop observing touch: clear our listener so no more on_touch fires (an
    // in-flight one keeps us alive via its weak lock). UP any still-down
    // passthrough pointer first (link still alive) so the source sees no stuck finger.
    release_all_pointers();
    display_manager.set_touch_listener({});
    // Producer detached; join the decode task before the framebuffers are reclaimed
    // by the previous screen's re-render, so it can't flush into a buffer being
    // reused.
    if (decode_task_) {
        decode_stop_ = true;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    // Both threads are stopped, so no DM.flush can race the teardown: drop the
    // overlay (restores the indev to the main display + frees the overlay display).
    // The previous screen's re-render reclaims the panel.
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
    // lives in PSRAM, which the HW-JPEG input DMA reads directly (the enhanced
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

    const bool rgb888 = (display_manager.format() == BSP_PIXEL_FORMAT_RGB888);
    const size_t bpp = rgb888 ? 3 : 2;

    // Lazily create the decoder (decode task only). The pixel format is baked
    // into the handle, which is fine: it is chosen at bsp_init and fixed for the
    // boot. ring_count = 0 — whole-frame decodes only, no strip ring (each strip
    // already lands straight in its framebuffer row band).
    if (!jpeg_) {
        jpeg_enh_strip_decoder_cfg_t cfg = {};
        cfg.decode.output_format = rgb888 ? JPEG_DECODE_OUT_FORMAT_RGB888
                                          : JPEG_DECODE_OUT_FORMAT_RGB565;
        // BGR, not RGB, for both depths: the framebuffer byte order is LVGL's native
        // RGB888 (B,G,R) / R-in-high-bits RGB565, and on the P4 HW JPEG decoder that
        // packing is the one the BGR scramble produces — the RGB enum mis-orders the
        // bytes and the image comes out as colour-swapped / rainbow noise. Matches the
        // proven Tab5-Screen-Streamer 565 path; the host shim packs B,G,R the same way.
        // (The 888 device scramble is still the remaining real-HW verification.)
        cfg.decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
        cfg.decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        cfg.decode.yuv_full_range = true;  // MJPEG/JFIF strips are full-range
        cfg.max_pic_w = PANEL_W;
        cfg.max_pic_h = PANEL_H;
        cfg.timeout_ms = 1000;
        if (jpeg_enh_strip_decoder_new(&cfg, &jpeg_) != ESP_OK) { jpeg_ = nullptr; return false; }
    }

    // Full-width strip → tightly-packed decode lands in place: the destination is
    // the framebuffer row band starting at row y (x == 0), and the decoded
    // PANEL_W×h picture is exactly PANEL_W*h pixels contiguous — no scratch, no
    // blit, no stride. (device: P4 HW JPEG straight to PSRAM; host: libjpeg.)
    uint8_t* out = dst + (size_t)y * PANEL_W * bpp;  // x == 0
    size_t outbuf = (size_t)PANEL_W * h * bpp;
    return jpeg_enh_decoder_process(jpeg_, jpeg, len, out, outbuf, nullptr) == ESP_OK;
}

void ADBMirroringScreen::free_decoder() {
    if (jpeg_) { jpeg_enh_strip_decoder_del(jpeg_); jpeg_ = nullptr; }
}
