#include "adb_app_manager_screen.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_heap_caps.h"

#include "adb_app.hpp"
#include "adb_app_detail_screen.hpp"
#include "agent_client.hpp"
#include "agent_link_protocol.hpp"  // kCmdGetApp*, kAppFlag*, rd_u16/wr_u16
#include "modal.hpp"
#include "sd_file_browser_screen.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

// The pm fallback: one round trip for everything the screen shows — third-party
// + system + disabled package names, separated so one exec output parses into
// the three sets. `pm list packages` emits "package:<name>" lines.
constexpr const char *kListCmd =
    "pm list packages -3 2>/dev/null; echo ---SEP---; "
    "pm list packages -s 2>/dev/null; echo ---SEP---; "
    "pm list packages -d 2>/dev/null";

void parse_sections(const std::string &out, std::vector<std::string> *user,
                    std::vector<std::string> *system, std::set<std::string> *disabled) {
    int section = 0;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "---SEP---") {
            ++section;
            continue;
        }
        if (line.rfind("package:", 0) != 0) continue;
        std::string pkg = line.substr(8);
        if (pkg.empty()) continue;
        switch (section) {
            case 0: user->push_back(std::move(pkg)); break;
            case 1: system->push_back(std::move(pkg)); break;
            default: disabled->insert(std::move(pkg)); break;
        }
    }
    std::sort(user->begin(), user->end());
    std::sort(system->begin(), system->end());
}

constexpr int32_t kRowH = 81;  // 80px row + 1px bottom-border separator

std::string fmt_size(size_t bytes) {
    char buf[32];
    if (bytes < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / 1048576.0);
    }
    return buf;
}

}  // namespace

void ADBAppManagerScreen::build() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_pad_row(root_, 0, 0);

    auto navigation = lv_obj_create(root_);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(navigation, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(navigation, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(navigation, 1, 0);
    lv_obj_set_style_border_color(navigation, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navigation, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(navigation, 24, 0);
    lv_obj_set_style_pad_column(navigation, 24, 0);

    auto back = lv_button_create(navigation);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [](lv_event_t*){ screen_manager.pop(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    auto title = lv_label_create(back);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto pad = lv_obj_create(navigation);
    lv_obj_remove_style_all(pad);
    lv_obj_set_flex_grow(pad, 1);

    auto install_button = lv_button_create(navigation);
    lv_obj_remove_style_all(install_button);
    lv_obj_set_style_pad_all(install_button, 16, 0);
    lv_obj_add_event_fn(install_button, LV_EVENT_CLICKED, [this](lv_event_t*){ pick_apk(); });
    lv_obj_set_style_bg_color(install_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(install_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(install_button, 12, 0);
    auto install_icon = lv_label_create(install_button);
    lv_label_set_text(install_icon, LUCIDE_PACKAGE_PLUS);
    lv_obj_set_style_text_font(install_icon, R.font.lucide_40, 0);
    lv_obj_center(install_icon);

    auto refresh_button = lv_button_create(navigation);
    lv_obj_remove_style_all(refresh_button);
    lv_obj_set_style_pad_all(refresh_button, 16, 0);
    lv_obj_add_event_fn(refresh_button, LV_EVENT_CLICKED, [this](lv_event_t*){ refresh(); });
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(refresh_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(refresh_button, 12, 0);
    auto refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LUCIDE_REFRESH_CW);
    lv_obj_set_style_text_font(refresh_icon, R.font.lucide_40, 0);
    lv_obj_center(refresh_icon);

    // ---- User / System filter toggle ----
    auto filter_row = lv_obj_create(root_);
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_size(filter_row, LV_PCT(100), 72);
    lv_obj_remove_flag(filter_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(filter_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filter_row, 1, 0);
    lv_obj_set_style_border_color(filter_row, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(filter_row, 24, 0);
    lv_obj_set_style_pad_ver(filter_row, 8, 0);
    lv_obj_set_style_pad_column(filter_row, 16, 0);

    auto filter_button = [this, filter_row](const char *text, Filter f) {
        auto button = lv_button_create(filter_row);
        lv_obj_remove_style_all(button);
        lv_obj_set_height(button, LV_PCT(100));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        // The active filter is shown with the theme primary color (set in
        // rebuild() via the CHECKED state).
        lv_obj_set_style_bg_color(button, lv_theme_get_color_primary(button), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, f](lv_event_t*){ set_filter(f); });
        auto label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return button;
    };
    user_btn_ = filter_button("User", Filter::User);
    system_btn_ = filter_button("System", Filter::System);

    // No layout: the recycled rows position themselves (lv_obj_set_pos) and
    // the invisible extent child defines the scroll range.
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t*){ update_rows(false); });

    extent_ = lv_obj_create(list_);
    lv_obj_remove_style_all(extent_);
    lv_obj_set_size(extent_, 1, 0);
    lv_obj_set_pos(extent_, 0, 0);
}

void ADBAppManagerScreen::onAppear() {
    // First show and every return from a sub-screen (detail actions / install
    // change the package set, so re-list).
    refresh();
}

void ADBAppManagerScreen::onExit() {
    // Abort an in-flight install: the transfer sees the abort on its worker
    // thread and ends quietly (the progress dialog dies with root_; the job
    // releases the file/buffer when the last ref drops).
    if (job_) {
        job_->abort();
        job_.reset();
    }
}

void ADBAppManagerScreen::set_filter(Filter f) {
    if (filter_ == f) return;
    filter_ = f;
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    rebuild();
}

void ADBAppManagerScreen::refresh() {
    adb::Client *client = app::adb_client();
    if (!client) {
        loading_ = false;
        error_ = "Not connected.";
        rebuild();
        return;
    }
    loading_ = true;
    error_.clear();
    rebuild();
    uint32_t gen = ++load_gen_;

    // Normal mode with an APPINFO-capable agent: one GET_APP_LIST request gives
    // labels + flags in agent-sorted order. Anything short of that — Limited
    // mode, a dropped link, a refused request — falls back to the pm exec path
    // (same gen, so the stale guard covers both).
    auto link = app::agent_client().link();
    if (!link || !(app::agent_client().agent_caps() & agent_link::kCapAppInfo)) {
        refresh_via_pm(gen);
        return;
    }
    auto cb = [self = std::static_pointer_cast<ADBAppManagerScreen>(shared_from_this()),
               gen](adb::Error err, uint8_t status, const uint8_t *result, size_t len) {
        // Reader thread: parse here so the LVGL thread only swaps vectors in.
        struct Parsed {
            std::vector<AppEntry> user, system;
            std::set<std::string> disabled;
            bool ok = false;
        };
        auto box = std::make_shared<Parsed>();
        if (err == adb::Error::Ok && status == agent_link::kStatusOk && len >= 2) {
            size_t off = 2;
            uint16_t count = agent_link::rd_u16(result);
            box->ok = true;
            for (uint16_t i = 0; i < count && box->ok; ++i) {
                if (off + 4 > len) { box->ok = false; break; }
                uint8_t flags = result[off];
                uint8_t pkg_len = result[off + 2];
                uint8_t label_len = result[off + 3];
                off += 4;
                if (off + pkg_len + label_len > len) { box->ok = false; break; }
                AppEntry e;
                e.pkg.assign(reinterpret_cast<const char *>(result + off), pkg_len);
                e.label.assign(reinterpret_cast<const char *>(result + off + pkg_len),
                               label_len);
                off += pkg_len + label_len;
                if (e.pkg.empty()) continue;
                if (flags & agent_link::kAppFlagDisabled) box->disabled.insert(e.pkg);
                if (flags & agent_link::kAppFlagSystem) box->system.push_back(std::move(e));
                else box->user.push_back(std::move(e));
            }
        }
        lv_async_call([self, gen, box]() {
            if (self->exited() || gen != self->load_gen_) return;
            if (!box->ok) {
                // Agent path failed: degrade to the pm listing (names only).
                self->refresh_via_pm(gen);
                return;
            }
            self->loading_ = false;
            self->user_pkgs_ = std::move(box->user);
            self->system_pkgs_ = std::move(box->system);
            self->disabled_ = std::move(box->disabled);
            self->rebuild();
        });
    };
    if (link->request(agent_link::kCmdGetAppList, nullptr, 0, std::move(cb)) !=
        adb::Error::Ok) {
        refresh_via_pm(gen);
    }
}

void ADBAppManagerScreen::refresh_via_pm(uint32_t gen) {
    adb::Client *client = app::adb_client();
    if (!client) {
        loading_ = false;
        error_ = "Not connected.";
        rebuild();
        return;
    }
    client->exec(kListCmd, [self = shared_from_this(), this, gen](
                               adb::Error err, const std::string &out) {
        // Reader thread: parse + sort here so the LVGL thread only swaps the
        // result vectors in.
        struct Parsed {
            std::vector<std::string> user, system;
            std::set<std::string> disabled;
        };
        auto box = std::make_shared<Parsed>();
        if (err == adb::Error::Ok) {
            parse_sections(out, &box->user, &box->system, &box->disabled);
        }
        lv_async_call([self, this, gen, err, box]() {
            if (exited() || gen != load_gen_) return;
            loading_ = false;
            if (err != adb::Error::Ok) {
                error_ = std::string("Error: ") + adb::to_string(err);
                rebuild();
                return;
            }
            auto to_entries = [](std::vector<std::string> &pkgs) {
                std::vector<AppEntry> v;
                v.reserve(pkgs.size());
                for (auto &p : pkgs) v.push_back({std::move(p), ""});
                return v;
            };
            user_pkgs_ = to_entries(box->user);
            system_pkgs_ = to_entries(box->system);
            disabled_ = std::move(box->disabled);
            rebuild();
        });
    });
}

void ADBAppManagerScreen::rebuild() {
    if (filter_ == Filter::User) {
        lv_obj_add_state(user_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(system_btn_, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(system_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(user_btn_, LV_STATE_CHECKED);
    }

    if (status_) {
        lv_obj_delete(status_);
        status_ = nullptr;
    }
    auto status_label = [this](const char *text) {
        status_ = lv_label_create(list_);
        lv_label_set_text(status_, text);
        lv_obj_set_style_text_color(status_, lv_color_hex(0x444444), 0);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 80);
    };

    size_t count = 0;
    if (loading_) {
        status_ = lv_spinner_create(list_);
        lv_obj_set_size(status_, 80, 80);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 80);
    } else if (!error_.empty()) {
        status_label(error_.c_str());
    } else if (filtered().empty()) {
        status_label("No apps.");
    } else {
        count = filtered().size();
    }

    ensure_pool();
    lv_obj_set_height(extent_, (int32_t)count * kRowH);
    // Keep the scroll position across a re-list (onAppear), clamped when the
    // list shrank below it.
    lv_obj_update_layout(list_);
    int32_t max_scroll = (int32_t)count * kRowH - lv_obj_get_height(list_);
    if (max_scroll < 0) max_scroll = 0;
    if (lv_obj_get_scroll_y(list_) > max_scroll) {
        lv_obj_scroll_to_y(list_, max_scroll, LV_ANIM_OFF);
    }
    update_rows(true);
}

void ADBAppManagerScreen::ensure_pool() {
    if (!pool_.empty()) return;
    lv_obj_update_layout(list_);
    int32_t h = lv_obj_get_height(list_);
    if (h <= 0) h = 1088;  // pre-layout fallback: panel minus nav + filter row
    size_t n = (size_t)(h / kRowH) + 3;  // partial top/bottom + one of lookbehind
    pool_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Row r = {};
        r.btn = lv_button_create(list_);
        lv_obj_remove_style_all(r.btn);
        lv_obj_set_size(r.btn, LV_PCT(100), kRowH);
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_side(r.btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r.btn, 1, 0);
        lv_obj_set_style_border_color(r.btn, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_pad_hor(r.btn, 24, 0);
        lv_obj_set_style_pad_column(r.btn, 24, 0);
        lv_obj_set_flex_flow(r.btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r.btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(r.btn, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.btn, LV_OPA_COVER, LV_STATE_PRESSED);
        // The handler reads the slot's bound index at tap time (the pool_
        // vector is created once and never reallocates).
        size_t slot = i;
        lv_obj_add_event_fn(r.btn, LV_EVENT_CLICKED, [this, slot](lv_event_t*){
            int idx = pool_[slot].data_idx;
            if (idx < 0 || idx >= (int)filtered().size()) return;
            const std::string &pkg = filtered()[idx].pkg;
            screen_manager.push(std::make_shared<ADBAppDetailScreen>(
                pkg, filter_ == Filter::System, disabled_.count(pkg) != 0));
        });

        // Two icon widgets per row: the fetched launcher icon (lv_image) when
        // cached, the package glyph until then — bind_row toggles their HIDDEN
        // flags (flex skips hidden children, so they share the slot).
        r.img = lv_image_create(r.btn);
        lv_obj_set_size(r.img, kIconPx, kIconPx);
        lv_obj_add_flag(r.img, LV_OBJ_FLAG_HIDDEN);
        r.icon = lv_label_create(r.btn);
        lv_label_set_text(r.icon, LUCIDE_PACKAGE);
        lv_obj_set_style_text_font(r.icon, R.font.lucide_40, 0);
        r.name = lv_label_create(r.btn);
        lv_obj_set_flex_grow(r.name, 1);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        r.tag = lv_label_create(r.btn);
        lv_label_set_text(r.tag, "disabled");
        lv_obj_set_style_text_color(r.tag, lv_color_hex(0xb0b0b0), 0);
        lv_obj_set_style_text_font(r.tag, &lv_font_montserrat_20, 0);
        pool_.push_back(r);
    }
}

void ADBAppManagerScreen::bind_row(Row &r, int idx) {
    if (idx < 0 || idx >= (int)filtered().size()) {
        r.data_idx = -1;
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    r.data_idx = idx;
    lv_obj_remove_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(r.btn, 0, idx * kRowH);
    const AppEntry &app = filtered()[idx];
    bool disabled = disabled_.count(app.pkg) != 0;
    lv_label_set_text(r.name, app.display().c_str());
    lv_color_t color = disabled ? lv_color_hex(0xb0b0b0) : lv_color_black();
    lv_obj_set_style_text_color(r.icon, color, 0);
    lv_obj_set_style_text_color(r.name, color, 0);
    auto it = icons_.find(app.pkg);
    if (it != icons_.end() && it->second.buf) {
        lv_image_set_src(r.img, &it->second.dsc);
        lv_obj_set_style_image_opa(r.img, disabled ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_remove_flag(r.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r.icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r.icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (disabled) {
        lv_obj_remove_flag(r.tag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r.tag, LV_OBJ_FLAG_HIDDEN);
    }
}

void ADBAppManagerScreen::update_rows(bool force) {
    if (pool_.empty()) return;
    int32_t sy = lv_obj_get_scroll_y(list_);
    int first = (int)(sy / kRowH) - 1;  // one row of lookbehind above the fold
    if (first < 0) first = 0;
    if (!force && first == first_bound_) return;
    first_bound_ = first;
    for (size_t i = 0; i < pool_.size(); ++i) {
        bind_row(pool_[i], first + (int)i);
    }
    pump_icons();
}

void ADBAppManagerScreen::pump_icons() {
    // Fetch launcher icons for the rows on screen, a few at a time (scrolling
    // rebinds constantly — the inflight cap keeps a fast fling from queueing
    // every row it passed). Gated on the agent's APPINFO capability, so the
    // Limited-mode / pm-fallback list keeps its glyphs without ever requesting.
    if (!(app::agent_client().agent_caps() & agent_link::kCapAppInfo)) return;
    for (const Row &r : pool_) {
        if (icon_pending_.size() >= kMaxIconInflight) break;
        if (icons_.size() + icon_pending_.size() >= kMaxIconCache) break;
        if (r.data_idx < 0 || r.data_idx >= (int)filtered().size()) continue;
        const std::string &pkg = filtered()[r.data_idx].pkg;
        if (icons_.count(pkg) || icon_pending_.count(pkg)) continue;
        fetch_icon(pkg);
    }
}

void ADBAppManagerScreen::fetch_icon(const std::string &pkg) {
    auto link = app::agent_client().link();
    if (!link || pkg.size() > 255) return;
    // GET_APP_ICON args (§4.4): size_px + reserved + the package name.
    uint8_t args[4 + 255];
    agent_link::wr_u16(args, kIconPx);
    agent_link::wr_u16(args + 2, 0);
    memcpy(args + 4, pkg.data(), pkg.size());
    auto cb = [self = std::static_pointer_cast<ADBAppManagerScreen>(shared_from_this()),
               pkg](adb::Error err, uint8_t status, const uint8_t *result, size_t len) {
        // Reader thread: validate + copy the pixels to PSRAM, then marshal.
        uint8_t *buf = nullptr;
        uint16_t w = 0, h = 0;
        if (err == adb::Error::Ok && status == agent_link::kStatusOk &&
            len >= agent_link::kAppIconHeaderLen) {
            w = agent_link::rd_u16(result);
            h = agent_link::rd_u16(result + 2);
            size_t pixels = (size_t)w * h * 4;
            if (w && h && result[4] == agent_link::kAppIconFormatArgb8888 &&
                len >= agent_link::kAppIconHeaderLen + pixels) {
                buf = static_cast<uint8_t *>(heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM));
                if (buf) memcpy(buf, result + agent_link::kAppIconHeaderLen, pixels);
            }
        }
        // A definitive agent refusal (unknown pkg / drawing failure) must cache a
        // "no icon" marker, or pump_icons would re-request the same package
        // forever; transport failures (timeout / link drop) stay retryable.
        const bool refused = err == adb::Error::Ok && status != agent_link::kStatusOk;
        lv_async_call([self, pkg, buf, w, h, refused]() {
            if (self->exited()) {
                if (buf) heap_caps_free(buf);
                return;
            }
            self->icon_pending_.erase(pkg);
            if (refused && !buf) self->icons_[pkg] = IconEntry{};  // glyph stays
            if (buf) {
                IconEntry e;
                e.buf = buf;
                e.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                e.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
                e.dsc.header.w = w;
                e.dsc.header.h = h;
                e.dsc.header.stride = w * 4;
                e.dsc.data = buf;
                e.dsc.data_size = (size_t)w * h * 4;
                self->icons_[pkg] = e;
                // Refresh whichever row currently shows this package.
                for (Row &r : self->pool_) {
                    if (r.data_idx >= 0 && r.data_idx < (int)self->filtered().size() &&
                        self->filtered()[r.data_idx].pkg == pkg) {
                        self->bind_row(r, r.data_idx);
                    }
                }
            }
            self->pump_icons();  // a slot freed up — keep the visible rows coming
        });
    };
    if (link->request(agent_link::kCmdGetAppIcon, args, 4 + pkg.size(),
                      std::move(cb)) == adb::Error::Ok) {
        icon_pending_.insert(pkg);
    }
}

ADBAppManagerScreen::~ADBAppManagerScreen() {
    for (auto &it : icons_) heap_caps_free(it.second.buf);
}

// ---- APK install flow ----

void ADBAppManagerScreen::pick_apk() {
    auto weak = std::weak_ptr<ADBAppManagerScreen>(
        std::static_pointer_cast<ADBAppManagerScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<SDFileBrowserScreen>(SDFileBrowserScreen::Pick{
        ".apk",
        [weak](const std::string &path) {
            // Fires on the LVGL thread after the picker popped (this screen is
            // on top again).
            if (auto self = weak.lock(); self && !self->exited()) {
                self->confirm_install(path);
            }
        }}));
}

void ADBAppManagerScreen::confirm_install(const std::string &path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        app::modal_message(root_, "Install failed", "Cannot read the file.");
        return;
    }
    std::string name = path.substr(path.rfind('/') + 1);
    char text[160];
    snprintf(text, sizeof(text), "%s (%s) will be installed.",
             name.c_str(), fmt_size((size_t)st.st_size).c_str());
    app::modal_confirm(root_, "Install APK", text, "Install", false,
                       [this, path]() { start_install(path); });
}

void ADBAppManagerScreen::start_install(const std::string &path) {
    // The shared install flow owns the progress dialog / push / pm install;
    // a successful install changes the package set, so re-list.
    auto weak = std::weak_ptr<ADBAppManagerScreen>(
        std::static_pointer_cast<ADBAppManagerScreen>(shared_from_this()));
    job_ = app::install_apk(root_, path, [weak](bool ok) {
        if (auto self = weak.lock(); self && !self->exited()) {
            if (ok) self->refresh();
        }
    });
}
