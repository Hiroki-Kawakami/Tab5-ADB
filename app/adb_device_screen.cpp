#include "adb_device_screen.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "adb.hpp"  // adb::Client
#include "adb_app.hpp"
#include "adb_app_manager_screen.hpp"
#include "adb_device_info_screen.hpp"
#include "adb_file_manager_screen.hpp"
#include "adb_logcat_screen.hpp"
#include "adb_mirroring_screen.hpp"
#include "adb_screenshot_screen.hpp"
#include "adb_shell_screen.hpp"
#include "agent_client.hpp"
#include "device_icons.hpp"
#include "device_info.hpp"
#include "home_screen.hpp"
#include "media_session.hpp"
#include "modal.hpp"
#include "screen_manager.hpp"
#include "settings_screen.hpp"
#include "resources/resources.h"

namespace {

namespace devinfo = app::devinfo;
namespace mediainfo = app::mediainfo;

// Fire-and-forget a device-side shell command (power actions, key events).
// Reboot/shutdown drop the link, so the completion may never arrive — that is
// expected; the adb holder resets the UI to HomeScreen on Closed.
void run_device_command(const std::string &cmd) {
    if (auto *c = app::adb_client())
        c->exec(cmd, [](adb::Error, const std::string &) {});
}

// Extract a value from a banner "device::key=val;key=val;..." string.
std::string banner_field(const std::string &banner, const std::string &key) {
    auto sep = banner.find("::");
    std::string body = sep == std::string::npos ? banner : banner.substr(sep + 2);
    std::string needle = key + "=";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = body.find(';', pos);
    return body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// One chained exec fills the whole summary (AppManager-style ---SEP--- split).
const char *kSummaryCmd =
    "settings get global device_name; echo ---SEP---; "
    "getprop ro.build.version.release; echo ---SEP---; "
    "dumpsys battery; echo ---SEP---; "
    "df -k /data; echo ---SEP---; "
    "cmd wifi status; echo ---SEP---; "
    "getprop gsm.sim.state; echo ---SEP---; "
    "dumpsys telephony.registry | grep -E 'mServiceState=|mSignalStrength=' | head -4; "
    "echo ---SEP---; dumpsys media_session";

struct Summary {
    std::string name;     // user-set device name ("" = fall back to model)
    std::string version;  // Android release
    devinfo::Battery battery;
    devinfo::Storage storage;
    devinfo::Wifi wifi;
    devinfo::Cellular cell;
    mediainfo::NowPlaying media;
};

Summary parse_summary(const std::string &out) {
    Summary s;
    auto sec = devinfo::split_sections(out);
    auto section = [&sec](size_t i) -> const std::string & {
        static const std::string kEmpty;
        return i < sec.size() ? sec[i] : kEmpty;
    };
    s.name = devinfo::first_line(section(0));
    if (s.name == "null") s.name.clear();
    s.version = devinfo::first_line(section(1));
    s.battery = devinfo::parse_dumpsys_battery(section(2));
    s.storage = devinfo::parse_df(section(3));
    s.wifi = devinfo::parse_wifi_status(section(4));
    s.cell = devinfo::parse_cellular(section(5), section(6));
    s.media = mediainfo::parse_media_session(section(7));
    return s;
}

}  // namespace

void ADBDeviceScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root_, 24, 0);
    lv_obj_set_style_pad_row(root_, 24, 0);

    createHeader();
    control_container_ = lv_obj_create(root_);
    lv_obj_remove_style_all(control_container_);
    lv_obj_set_size(control_container_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(control_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(control_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(control_container_, 24, 0);
    createPreviewContainer();
    createToolsContainer();
    createMediaCard();
}

void ADBDeviceScreen::onAppear() {
    visible_ = true;
    // The preview is created/torn down on appear/disappear (not enter/exit) so its
    // stream and PSRAM buffers don't keep running behind a pushed sub-screen
    // (Mirroring/Shell/...).
    startPreview();

    // Live summary fields: fetch now, then every 10 s while the screen shows
    // (battery and signal move; returning from a sub-screen refreshes too).
    refreshSummary();
    summary_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
            static_cast<ADBDeviceScreen *>(lv_timer_get_user_data(t))->refreshSummary();
        },
        10000, this);
}

void ADBDeviceScreen::onDisappear() {
    visible_ = false;
    stopPreview();
    if (summary_timer_) {
        lv_timer_delete(summary_timer_);
        summary_timer_ = nullptr;
    }
    if (media_poll_timer_) {
        lv_timer_delete(media_poll_timer_);
        media_poll_timer_ = nullptr;
    }
}

void ADBDeviceScreen::startPreview() {
    if (app::agent_client().mode() == app::AgentClient::Mode::Normal) {
        // Normal mode: the mirror-stream preview over the agent link, in the same
        // 360x860 box as the screencap fallback: fixed 360 width, height following
        // the phone's aspect (the agent sizes the stream via scale_mode=aspect +
        // split_count=1, so nothing needs 16px alignment). The agent link usually
        // outlives the screens; if it dropped, re-establish it first (callback on
        // the LVGL thread).
        if (app::agent_client().ready()) {
            agent_preview_ = AgentPreview::create(preview_image_, 360, 860);
            agent_preview_->start();
            return;
        }
        app::agent_client().ensure_connected(
            [self = std::static_pointer_cast<ADBDeviceScreen>(shared_from_this())](bool ok) {
                if (self->exited() || !self->visible_ || self->agent_preview_) return;
                if (ok) {
                    self->agent_preview_ = AgentPreview::create(self->preview_image_, 360, 860);
                    self->agent_preview_->start();
                } else if (self->visible_ && !self->preview_) {
                    // Bring-up failed (mode flipped to Limited): degrade in place.
                    self->preview_ = ScreencapPreview::create(self->preview_image_, 360, 860);
                    self->preview_->start();
                }
            });
        return;
    }
    // Limited mode: the low-rate, agent-free `exec:screencap -p` preview. 360x860
    // is the bounding box each frame aspect-fits into (860 keeps sources up to
    // ~9:21.5 at the full 360 width; the lv_image resizes to hug each frame, so
    // the device's real aspect — and rotation — shows with no stretch or letterbox).
    preview_ = ScreencapPreview::create(preview_image_, 360, 860);
    preview_->start();
}

void ADBDeviceScreen::stopPreview() {
    if (preview_) {
        preview_->stop();
        preview_.reset();
    }
    if (agent_preview_) {
        agent_preview_->stop();
        agent_preview_.reset();
    }
}

void ADBDeviceScreen::onPreviewTapped() {
    if (app::agent_client().mode() == app::AgentClient::Mode::Normal) {
        screen_manager.push(std::make_shared<ADBMirroringScreen>());
    } else {
        app::modal_message(root_, "Mirroring unavailable",
                           "The tab5adb-agent could not be started on this device, "
                           "so screen mirroring is disabled.");
    }
}

void ADBDeviceScreen::createHeader() {
    // ---- Device summary: banner fields render immediately, live fields (name /
    // battery / network / storage) land via refreshSummary(). ----
    adb::Client *client = app::adb_client();
    static const std::string kNoBanner;
    const std::string &banner = client ? client->banner() : kNoBanner;
    model_ = banner_field(banner, "ro.product.model");
    if (model_.empty()) model_ = "ADB Device";

    if (header_) lv_obj_delete(header_);
    header_ = lv_obj_create(root_);
    lv_obj_set_size(header_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(header_, 20, 0);
    lv_obj_add_flag(header_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(header_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(header_, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_add_event_fn(header_, LV_EVENT_CLICKED, [](lv_event_t *) {
        screen_manager.push(std::make_shared<ADBDeviceInfoScreen>());
    });

    auto text_box = lv_obj_create(header_);
    lv_obj_remove_style_all(text_box);
    lv_obj_remove_flag(text_box, LV_OBJ_FLAG_CLICKABLE);  // taps land on header_
    lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text_box, 4, 0);
    lv_obj_set_flex_grow(text_box, 1);

    name_label_ = lv_label_create(text_box);
    lv_label_set_text(name_label_, model_.c_str());
    lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_28, 0);
    lv_obj_set_width(name_label_, LV_PCT(100));
    lv_label_set_long_mode(name_label_, LV_LABEL_LONG_DOT);

    sub_label_ = lv_label_create(text_box);
    lv_label_set_text(sub_label_, model_.c_str());
    lv_obj_set_style_text_font(sub_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sub_label_, lv_color_hex(0x888888), 0);
    lv_obj_set_width(sub_label_, LV_PCT(100));
    lv_label_set_long_mode(sub_label_, LV_LABEL_LONG_DOT);

    auto status_box = lv_obj_create(header_);
    lv_obj_remove_style_all(status_box);
    lv_obj_remove_flag(status_box, LV_OBJ_FLAG_CLICKABLE);  // taps land on header_
    lv_obj_set_size(status_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_box, 12, 0);

    auto status_icon = [&status_box](void) {
        auto icon = lv_label_create(status_box);
        lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon, devinfo::icon_inactive_color(), 0);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);  // shown once data lands
        return icon;
    };
    batt_icon_ = status_icon();
    batt_pct_ = lv_label_create(status_box);
    lv_obj_set_style_text_font(batt_pct_, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(batt_pct_, LV_OBJ_FLAG_HIDDEN);
    wifi_icon_ = status_icon();
    cell_icon_ = status_icon();

    auto chevron = lv_label_create(status_box);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(chevron, lv_color_hex(0xc0c0c0), 0);
}

void ADBDeviceScreen::refreshSummary() {
    adb::Client *client = app::adb_client();
    if (!client || summary_inflight_) return;
    summary_inflight_ = true;
    client->exec(kSummaryCmd, [self = shared_from_this(), this](adb::Error err,
                                                                const std::string &out) {
        // Reader thread: parse here, hand the LVGL thread a finished struct.
        auto sum = std::make_shared<Summary>();
        bool ok = err == adb::Error::Ok && !out.empty();
        if (ok) *sum = parse_summary(out);
        lv_async_call([self, this, sum, ok]() {
            if (exited()) return;
            summary_inflight_ = false;
            if (!ok) return;  // keep the previous (or banner placeholder) render

            lv_label_set_text(name_label_,
                              sum->name.empty() ? model_.c_str() : sum->name.c_str());

            // "Pixel 10 • Android 16 • 128 GB" — skip the parts that didn't parse.
            std::string sub = model_;
            if (!sum->version.empty()) sub += " \xE2\x80\xA2 Android " + sum->version;
            if (int gb = devinfo::marketed_storage_gb(sum->storage.total_kb))
                sub += " \xE2\x80\xA2 " + std::to_string(gb) + " GB";
            lv_label_set_text(sub_label_, sub.c_str());

            if (sum->battery.level >= 0) {
                lv_label_set_text(batt_icon_, devinfo::battery_icon(sum->battery));
                lv_obj_set_style_text_color(batt_icon_, devinfo::battery_color(sum->battery), 0);
                lv_label_set_text_fmt(batt_pct_, "%d%%", sum->battery.level);
                lv_obj_remove_flag(batt_icon_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(batt_pct_, LV_OBJ_FLAG_HIDDEN);
            }

            lv_label_set_text(wifi_icon_, devinfo::wifi_icon(sum->wifi));
            lv_obj_set_style_text_color(wifi_icon_,
                                        sum->wifi.state == devinfo::Wifi::State::Connected
                                            ? devinfo::icon_active_color()
                                            : devinfo::icon_inactive_color(), 0);
            lv_obj_remove_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);

            // No SIM -> no cellular icon at all; out of service -> greyed zero bars.
            if (sum->cell.sim_present) {
                lv_label_set_text(cell_icon_, devinfo::cellular_icon(sum->cell));
                lv_obj_set_style_text_color(cell_icon_,
                                            sum->cell.in_service ? devinfo::icon_active_color()
                                                                 : devinfo::icon_inactive_color(), 0);
                lv_obj_remove_flag(cell_icon_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(cell_icon_, LV_OBJ_FLAG_HIDDEN);
            }

            applyMedia(sum->media);
        });
    });
}

void ADBDeviceScreen::createMediaCard() {
    // A bordered "now playing" card filling the space under the preview/tools
    // row: [music icon] [title / artist] [<< | play-pause | >>]. Always present;
    // play-pause stays enabled even when idle (it resumes the last media-button
    // session), Prev/Next grey out without a current track (see applyMedia).
    // Media info rides the same `dumpsys media_session` section of the summary
    // fetch, and a control tap dispatches a media key via `cmd media_session`
    // (agent-free, so it works in Limited mode too).
    if (media_card_) lv_obj_delete(media_card_);
    media_card_ = lv_obj_create(root_);
    lv_obj_set_size(media_card_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(media_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(media_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(media_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(media_card_, 1, 0);
    lv_obj_set_style_border_color(media_card_, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(media_card_, 12, 0);
    lv_obj_set_style_pad_all(media_card_, 16, 0);
    lv_obj_set_flex_flow(media_card_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(media_card_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(media_card_, 16, 0);

    media_icon_ = lv_label_create(media_card_);
    lv_label_set_text(media_icon_, LUCIDE_MUSIC);
    lv_obj_set_style_text_font(media_icon_, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(media_icon_, lv_color_hex(0x888888), 0);

    auto text_box = lv_obj_create(media_card_);
    lv_obj_remove_style_all(text_box);
    lv_obj_remove_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_box, 1);  // takes the slack between icon and controls
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text_box, 2, 0);

    media_title_ = lv_label_create(text_box);
    lv_obj_set_style_text_font(media_title_, &lv_font_montserrat_20, 0);
    lv_obj_set_width(media_title_, LV_PCT(100));
    lv_label_set_long_mode(media_title_, LV_LABEL_LONG_DOT);

    media_artist_ = lv_label_create(text_box);
    lv_obj_set_style_text_font(media_artist_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(media_artist_, lv_color_hex(0x888888), 0);
    lv_obj_set_width(media_artist_, LV_PCT(100));
    lv_label_set_long_mode(media_artist_, LV_LABEL_LONG_DOT);

    auto controls = lv_obj_create(media_card_);
    lv_obj_remove_style_all(controls);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, 8, 0);

    auto ctrl_button = [controls, this](const char *icon, const char *key) {
        auto b = lv_button_create(controls);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 60, 60);
        lv_obj_set_style_radius(b, 30, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_fn(b, LV_EVENT_CLICKED,
                            [this, key](lv_event_t *) { dispatchMedia(key); });
        auto lbl = lv_label_create(b);
        lv_label_set_text(lbl, icon);
        lv_obj_set_style_text_font(lbl, R.font.lucide_40, 0);
        lv_obj_center(lbl);
        return b;
    };
    media_prev_btn_ = ctrl_button(LUCIDE_SKIP_BACK, "previous");
    media_play_btn_ = ctrl_button(LUCIDE_PLAY, "play-pause");
    media_play_icon_ = lv_obj_get_child(media_play_btn_, 0);
    media_next_btn_ = ctrl_button(LUCIDE_SKIP_FORWARD, "next");

    applyMedia(mediainfo::NowPlaying{});  // start in the idle state
}

void ADBDeviceScreen::applyMedia(const mediainfo::NowPlaying &np) {
    if (!media_card_) return;

    if (np.valid) {
        lv_label_set_text(media_title_,
                          np.title.empty() ? np.package.c_str() : np.title.c_str());
        // Prefer artist; fall back to album, then hide the second line entirely.
        const std::string &sub = !np.artist.empty() ? np.artist : np.album;
        lv_label_set_text(media_artist_, sub.c_str());
        sub.empty() ? lv_obj_add_flag(media_artist_, LV_OBJ_FLAG_HIDDEN)
                    : lv_obj_remove_flag(media_artist_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(media_icon_, lv_color_hex(0x444444), 0);
        lv_label_set_text(media_play_icon_, np.playing() ? LUCIDE_PAUSE : LUCIDE_PLAY);
    } else {
        // Idle: leave the labels blank (the controls keep the card's height).
        lv_label_set_text(media_title_, "");
        lv_obj_add_flag(media_artist_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(media_icon_, lv_color_hex(0x888888), 0);
        lv_label_set_text(media_play_icon_, LUCIDE_PLAY);
    }
    media_playing_ = np.valid && np.playing();

    // Play/pause is ALWAYS enabled: `dispatch` targets the system's media-button
    // session (the last app to play), so even with no active session a Play tap
    // resumes/wakes it. Prev/Next only make sense against a current track, so
    // they grey out when idle (a skip with no queue context would be a no-op).
    lv_obj_set_style_text_color(media_play_icon_, lv_color_hex(0x222222), 0);
    for (lv_obj_t *b : {media_prev_btn_, media_next_btn_}) {
        lv_color_t fg = np.active() ? lv_color_hex(0x222222) : lv_color_hex(0xc0c0c0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), fg, 0);
        np.active() ? lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE)
                    : lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
}

void ADBDeviceScreen::dispatchMedia(const char *key) {
    if (auto *c = app::adb_client())
        c->exec(std::string("cmd media_session dispatch ") + key,
                [](adb::Error, const std::string &) {});

    // Optimistic flip: a control tap almost always changes the play state, so
    // update the play/pause glyph NOW for instant feedback instead of waiting for
    // a re-fetch. play-pause toggles; next/previous keep playback running.
    media_playing_ = std::string(key) == "play-pause" ? !media_playing_ : true;
    lv_label_set_text(media_play_icon_, media_playing_ ? LUCIDE_PAUSE : LUCIDE_PLAY);

    // The device updates its session state asynchronously; re-fetch shortly after
    // so the glyph and track metadata reconcile with reality without waiting for
    // the 10 s summary tick. The timer is tracked + cancelled in onDisappear.
    if (media_poll_timer_) lv_timer_delete(media_poll_timer_);
    media_poll_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
            auto *self = static_cast<ADBDeviceScreen *>(lv_timer_get_user_data(t));
            self->media_poll_timer_ = nullptr;  // one-shot: deletes itself below
            lv_timer_delete(t);
            self->refreshMedia();
        },
        1000, this);
    lv_timer_set_repeat_count(media_poll_timer_, 1);
}

void ADBDeviceScreen::refreshMedia() {
    adb::Client *client = app::adb_client();
    if (!client || media_inflight_) return;
    media_inflight_ = true;
    client->exec("dumpsys media_session", [self = shared_from_this(), this](
                                              adb::Error err, const std::string &out) {
        auto np = std::make_shared<mediainfo::NowPlaying>();
        if (err == adb::Error::Ok) *np = mediainfo::parse_media_session(out);
        lv_async_call([self, this, np]() {
            if (exited()) return;
            media_inflight_ = false;
            applyMedia(*np);
        });
    });
}

void ADBDeviceScreen::createPreviewContainer() {
    const int width = 360;
    if (preview_container_) lv_obj_delete(preview_container_);
    preview_container_ = lv_obj_create(control_container_);
    lv_obj_remove_style_all(preview_container_);
    lv_obj_set_size(preview_container_, width, LV_SIZE_CONTENT);
    // Column: the preview image, then the Back/Home/Recents/Power nav row right
    // below it. Cross-axis centered so a narrower frame (very tall source hitting
    // the height cap, or a landscape-rotated device) stays centered in the
    // 360-wide column; the nav row follows the image's bottom edge.
    lv_obj_set_flex_flow(preview_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(preview_container_, 12, 0);
    preview_image_ = lv_image_create(preview_container_);
    // 9:20 placeholder until the first frame lands; the preview then resizes the
    // image to each frame's aspect-fitted size.
    lv_obj_set_size(preview_image_, width, width / 9 * 20);
    lv_obj_set_style_bg_color(preview_image_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(preview_image_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(preview_image_, 12, 0);
    // The preview doubles as the entry to the mirroring screen (Normal mode) /
    // the "why not" explainer (Limited mode). lv_image isn't clickable by default.
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_fn(preview_image_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        onPreviewTapped();
    });

    createNavBar();
}

void ADBDeviceScreen::createNavBar() {
    // [ Back | Home | Recents | Power ] directly under the preview, rendered as
    // one bordered bar with thin separators between the four flat icon buttons.
    // Back/Home/Recents grow to share the width evenly; Power is a fixed narrow
    // button pinned to the right. These are agent-independent (work in Limited
    // mode too), so they go over plain `adb shell input keyevent` — the slight
    // latency is fine for discrete taps, unlike the mirror overlay's low-latency
    // INPUT channel. Gesture-nav devices still get the 3 nav keys (KEYCODE_BACK/
    // HOME/APP_SWITCH work regardless of nav mode), same as the mirror overlay.
    auto nav = lv_obj_create(preview_container_);
    lv_obj_set_size(nav, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(nav, 0, 0);
    lv_obj_set_style_pad_column(nav, 0, 0);

    // Android KeyEvent.KEYCODE_* (also the `input keyevent` numbers).
    auto nav_button = [nav](const char *icon, bool grow, int keycode) {
        auto b = lv_button_create(nav);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 64);
        if (grow) {
            lv_obj_set_flex_grow(b, 1);  // Back/Home/Recents share the row evenly
        } else {
            lv_obj_set_width(b, 64);     // Power: fixed, narrow, right end
        }
        lv_obj_add_event_fn(b, LV_EVENT_CLICKED, [keycode](lv_event_t *) {
            if (auto *c = app::adb_client())
                c->exec("input keyevent " + std::to_string(keycode),
                        [](adb::Error, const std::string &) {});
        });
        lv_obj_set_style_bg_color(b, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        auto lbl = lv_label_create(b);
        lv_label_set_text(lbl, icon);
        lv_obj_set_style_text_font(lbl, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x444444), 0);
        lv_obj_center(lbl);
    };
    auto separator = [nav]() {
        auto sep = lv_obj_create(nav);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, 1, LV_PCT(100));
        lv_obj_set_style_bg_color(sep, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_margin_ver(sep, 8, 0);
    };
    nav_button(LUCIDE_ARROW_LEFT, /*grow=*/true, 4);    // Back
    separator();
    nav_button(LUCIDE_CIRCLE, /*grow=*/true, 3);        // Home
    separator();
    nav_button(LUCIDE_SQUARE, /*grow=*/true, 187);      // Recents (KEYCODE_APP_SWITCH)
    separator();
    nav_button(LUCIDE_POWER, /*grow=*/false, 26);       // Power
}

void ADBDeviceScreen::createToolsContainer() {
    if (tools_container_) lv_obj_delete(tools_container_);
    tools_container_ = lv_obj_create(control_container_);
    lv_obj_remove_style_all(tools_container_);
    lv_obj_set_size(tools_container_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(tools_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(tools_container_, 2, 0);
    lv_obj_set_flex_grow(tools_container_, 1);
    lv_obj_set_flex_flow(tools_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tools_container_, 16, 0);

    auto tool_button = [this](const char *icon, const char *title, std::function<void(lv_event_t*)> callback, bool danger = false){
        lv_color_t fg = danger ? lv_color_hex(0xd32f2f) : lv_color_black();
        auto button = lv_button_create(tools_container_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, danger ? fg : lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(button, 20, 0);
        lv_obj_set_style_pad_column(button, 12, 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, callback);

        auto icon_label = lv_label_create(button);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon_label, fg, 0);

        auto title_label = lv_label_create(button);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(title_label, fg, 0);
    };
    tool_button(LUCIDE_SQUARE_TERMINAL, "Shell", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBShellScreen>());
    });
    tool_button(LUCIDE_FOLDER_CLOSED, "File Manager", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBFileManagerScreen>());
    });
    tool_button(LUCIDE_LAYOUT_GRID, "Apps", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBAppManagerScreen>());
    });
    tool_button(LUCIDE_LOGS, "Logcat", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBLogcatScreen>());
    });
    tool_button(LUCIDE_CAMERA, "Screenshot", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBScreenshotScreen>());
    });
    tool_button(LUCIDE_POWER, "Power Menu", [this](lv_event_t*){ openPowerMenu(); });
    tool_button(LUCIDE_SETTINGS, "Settings", [](lv_event_t*){
        // Wi-Fi entry disabled while an ADB session is live — changing the
        // link's Wi-Fi mid-connection would disrupt it.
        screen_manager.push(std::make_shared<SettingsScreen>(false));
    });
    tool_button(LUCIDE_UNPLUG, "Disconnect", [this](lv_event_t*){
        app::modal_confirm(
            root_, "Disconnect", "Disconnect from this device?", "Disconnect",
            /*destructive=*/true, []{
                // Drop the connection (also tears down the agent link), then reset
                // the UI to the home screen. load() unwinds the whole screen stack.
                app::adb_disconnect();
                screen_manager.load(std::make_shared<HomeScreen>());
            });
    }, /*danger=*/true);
}

void ADBDeviceScreen::openPowerMenu() {
    // A vertical list of power actions sent to the device over plain `adb shell`
    // (agent-independent, work in both Normal and Limited mode). Reboot/shutdown
    // confirm first (and drop the link); sleep/wake fire immediately.
    auto card = app::modal_open(root_);
    auto title = lv_label_create(card);
    lv_label_set_text(title, "Power Menu");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    // One tappable row: icon + label. `confirm` gates the command behind a
    // destructive confirmation (reboot/shutdown); otherwise it runs immediately.
    auto action = [this, card](const char *icon, const char *text,
                               const char *confirm_text, std::string cmd,
                               bool confirm, bool destructive) {
        auto b = lv_button_create(card);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_PCT(100), 72);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xf0f0f0), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(b, 20, 0);
        lv_obj_set_style_pad_column(b, 16, 0);

        lv_color_t fg = destructive ? lv_color_hex(0xd32f2f) : lv_color_black();
        auto icon_label = lv_label_create(b);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon_label, fg, 0);
        auto text_label = lv_label_create(b);
        lv_label_set_text(text_label, text);
        lv_obj_set_style_text_font(text_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(text_label, fg, 0);

        lv_obj_t *parent = root_;
        lv_obj_add_event_fn(b, LV_EVENT_CLICKED,
                            [parent, card, text, confirm_text, cmd, confirm,
                             destructive](lv_event_t*) {
            // modal_close deletes the card tree — including THIS button, whose
            // lvgl++ DELETE cleanup frees this very lambda. So copy everything we
            // still need (incl. the parent ptr; `this`/captures become dangling)
            // onto the stack before closing.
            lv_obj_t *p = parent;
            std::string c = cmd;
            const char *t = text;
            const char *ct = confirm_text;
            bool needs_confirm = confirm, danger = destructive;
            app::modal_close(card);
            if (needs_confirm) {
                app::modal_confirm(p, t, ct, t, danger,
                                   [c]{ run_device_command(c); });
            } else {
                run_device_command(c);
            }
        });
    };

    action(LUCIDE_POWER_OFF, "Power off",
           "Power the device off now? The ADB connection will drop.",
           "reboot -p", /*confirm=*/true, /*destructive=*/true);
    action(LUCIDE_REFRESH_CW, "Restart",
           "Reboot the device now? The ADB connection will drop.",
           "reboot", /*confirm=*/true, /*destructive=*/true);
    action(LUCIDE_WRENCH, "Reboot to Recovery",
           "Reboot the device into recovery? The ADB connection will drop.",
           "reboot recovery", /*confirm=*/true, /*destructive=*/true);
    action(LUCIDE_HARD_DRIVE, "Reboot to Bootloader",
           "Reboot the device into the bootloader? The ADB connection will drop.",
           "reboot bootloader", /*confirm=*/true, /*destructive=*/true);
    action(LUCIDE_MOON, "Sleep (screen off)", nullptr,
           "input keyevent 223", /*confirm=*/false, /*destructive=*/false);
    action(LUCIDE_SUN, "Wake (screen on)", nullptr,
           "input keyevent 224", /*confirm=*/false, /*destructive=*/false);

    auto cancel = lv_button_create(card);
    lv_obj_set_size(cancel, LV_PCT(100), 72);
    lv_obj_set_style_radius(cancel, 12, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xe0e0e0), 0);
    auto cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_black(), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED,
                        [card](lv_event_t*){ app::modal_close(card); });
}
