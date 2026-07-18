#include "adb_logcat_screen.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adb_app.hpp"
#include "bsp.h"
#include "lvgl.hpp"
#include "modal.hpp"
#include "screen_manager.hpp"
#include "sysclock.hpp"
#include "resources/resources.h"

namespace {

// Layout (portrait 720x1280): nav bar, level/text filter row, recycled list.
// The on-screen keyboard overlays the list bottom while the filter textarea
// is focused.
constexpr int kNavH = 120;
constexpr int kFilterH = 72;
constexpr int kListH = PANEL_H - kNavH - kFilterH;
constexpr int kKeyboardH = 440;
constexpr int32_t kRowH = 24;  // hack_16 line + breathing room

// Ring sizing: ~2 MB of text (= roughly 15 k typical logcat lines) and a hard
// 16 K line cap, both PSRAM. A line longer than kMaxLineLen is truncated.
constexpr size_t kPoolCap = 2 * 1024 * 1024;
constexpr size_t kMaxLines = 16 * 1024;
constexpr size_t kMaxLineLen = 1024;

// Reader-thread FIFO cap (only trips if the LVGL thread stalls).
constexpr size_t kPendingCap = 256 * 1024;

constexpr const char *kMountPoint = "/sd";
constexpr size_t kWriteChunk = 16 * 1024;

// Text colors per level on the light theme (V D I W E F).
constexpr uint32_t kLevelColor[6] = {0x8a8a8a, 0x1565c0, 0x1a1a1a,
                                     0xe65100, 0xc62828, 0x8e24aa};
constexpr const char *kLevelLabel[5] = {"V", "D", "I", "W", "E"};

// Parse a `logcat -v threadtime` line: "MM-DD hh:mm:ss.mmm  PID  TID L Tag: msg".
// Returns the level index (0=V..5=F) and fills ts[19] with the 18-char
// timestamp, or returns -1 for lines without a header (continuation lines,
// "--------- beginning of ..." markers) which inherit the previous level.
int parse_level(const char *s, size_t len, char *ts) {
    auto digit = [&](size_t i) { return std::isdigit((unsigned char)s[i]) != 0; };
    if (len < 20) return -1;
    if (!(digit(0) && digit(1) && s[2] == '-' && digit(3) && digit(4) && s[5] == ' ' &&
          digit(6) && digit(7) && s[8] == ':' && digit(9) && digit(10) && s[11] == ':' &&
          digit(12) && digit(13) && s[14] == '.' && digit(15) && digit(16) && digit(17) &&
          s[18] == ' '))
        return -1;
    size_t i = 18;
    auto skip_spaces = [&] { while (i < len && s[i] == ' ') ++i; };
    auto skip_digits = [&] {
        size_t b = i;
        while (i < len && std::isdigit((unsigned char)s[i])) ++i;
        return i > b;
    };
    skip_spaces();
    if (!skip_digits()) return -1;  // pid
    skip_spaces();
    if (!skip_digits()) return -1;  // tid
    skip_spaces();
    if (i >= len) return -1;
    int level;
    switch (s[i]) {
        case 'V': level = 0; break;
        case 'D': level = 1; break;
        case 'I': level = 2; break;
        case 'W': level = 3; break;
        case 'E': level = 4; break;
        case 'F': level = 5; break;
        default: return -1;
    }
    if (i + 1 < len && s[i + 1] != ' ') return -1;
    memcpy(ts, s, 18);
    ts[18] = '\0';
    return level;
}

bool ci_contains(const char *hay, size_t len, const std::string &needle_lc) {
    size_t n = needle_lc.size();
    if (n == 0) return true;
    if (n > len) return false;
    for (size_t i = 0; i + n <= len; ++i) {
        size_t j = 0;
        while (j < n &&
               (char)std::tolower((unsigned char)hay[i + j]) == needle_lc[j])
            ++j;
        if (j == n) return true;
    }
    return false;
}

}  // namespace

// ---- LogRing ----

ADBLogcatScreen::LogRing::~LogRing() {
    if (pool_) heap_caps_free(pool_);
    if (lines_) heap_caps_free(lines_);
}

bool ADBLogcatScreen::LogRing::init() {
    pool_ = (char *)heap_caps_malloc(kPoolCap, MALLOC_CAP_SPIRAM);
    lines_ = (Line *)heap_caps_malloc(kMaxLines * sizeof(Line), MALLOC_CAP_SPIRAM);
    return pool_ && lines_;
}

void ADBLogcatScreen::LogRing::evict() {
    bytes_ -= lines_[head_].len;
    head_ = (head_ + 1) % kMaxLines;
    --count_;
}

size_t ADBLogcatScreen::LogRing::append(const char *text, size_t len, uint8_t level) {
    if (len > kMaxLineLen) len = kMaxLineLen;
    uint32_t need = (uint32_t)len + 1;  // + NUL terminator
    size_t evicted = 0;
    // Lines ahead of the write offset are always the oldest, so eviction from
    // the head matches storage-offset order on both branches below.
    if (w_ + need > kPoolCap) {
        // Wrap: abandon the tail, evicting the old lines still living there.
        while (count_ && lines_[head_].off >= w_) { evict(); ++evicted; }
        w_ = 0;
    }
    while (count_ && lines_[head_].off < w_ + need &&
           lines_[head_].off + lines_[head_].len + 1 > w_) {
        evict(); ++evicted;
    }
    if (count_ == kMaxLines) { evict(); ++evicted; }
    Line &l = lines_[(head_ + count_) % kMaxLines];
    l.seq = next_seq_++;
    l.off = w_;
    l.len = (uint16_t)len;
    l.level = level;
    memcpy(pool_ + w_, text, len);
    pool_[w_ + len] = '\0';
    w_ += need;
    bytes_ += len;
    ++count_;
    return evicted;
}

void ADBLogcatScreen::LogRing::clear() {
    head_ = count_ = 0;
    w_ = 0;
    bytes_ = 0;
}

uint32_t ADBLogcatScreen::LogRing::head_seq() const {
    return count_ ? lines_[head_].seq : next_seq_;
}

const ADBLogcatScreen::LogRing::Line &ADBLogcatScreen::LogRing::at(size_t idx) const {
    return lines_[(head_ + idx) % kMaxLines];
}

ADBLogcatScreen::SaveJob::~SaveJob() {
    if (buf) heap_caps_free(buf);
}

// ---- screen ----

ADBLogcatScreen::ADBLogcatScreen() = default;

ADBLogcatScreen::~ADBLogcatScreen() {
    // onExit normally runs first (pop()/load()), but guard against a destroy
    // without it. The shell holds the listener weakly, so the weak ref simply
    // expires as this screen dies.
    if (render_timer_) {
        lv_timer_delete(render_timer_);
        render_timer_ = nullptr;
    }
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

void ADBLogcatScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    // --- nav bar: Back + title + Pause/Clear/Save ---
    auto navigation = lv_obj_create(root_);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, PANEL_W, kNavH);
    lv_obj_set_pos(navigation, 0, 0);
    lv_obj_set_style_bg_color(navigation, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(navigation, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(navigation, 1, 0);
    lv_obj_set_style_border_color(navigation, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navigation, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(navigation, 24, 0);
    lv_obj_set_style_pad_column(navigation, 24, 0);

    auto back = lv_button_create(navigation);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [](lv_event_t *) { screen_manager.pop(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    auto title = lv_label_create(back);
    lv_label_set_text(title, "Logcat");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto pad = lv_obj_create(navigation);
    lv_obj_remove_style_all(pad);
    lv_obj_set_flex_grow(pad, 1);

    auto icon_button = [&](const char *icon, std::function<void(lv_event_t *)> cb) {
        auto button = lv_button_create(navigation);
        lv_obj_remove_style_all(button);
        lv_obj_set_style_pad_all(button, 16, 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, std::move(cb));
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(button, 12, 0);
        auto icon_label = lv_label_create(button);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
        lv_obj_center(icon_label);
        return icon_label;
    };
    pause_icon_ = icon_button(LUCIDE_PAUSE, [this](lv_event_t *) { toggle_pause(); });
    icon_button(LUCIDE_ERASER, [this](lv_event_t *) { clear_logs(); });
    icon_button(LUCIDE_SAVE, [this](lv_event_t *) { save_to_sd(); });

    // --- filter row: minimum-level toggle + substring textarea ---
    auto filter_row = lv_obj_create(root_);
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_size(filter_row, PANEL_W, kFilterH);
    lv_obj_set_pos(filter_row, 0, kNavH);
    lv_obj_remove_flag(filter_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(filter_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filter_row, 1, 0);
    lv_obj_set_style_border_color(filter_row, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(filter_row, 24, 0);
    lv_obj_set_style_pad_ver(filter_row, 8, 0);
    lv_obj_set_style_pad_column(filter_row, 12, 0);

    for (int l = 0; l < 5; ++l) {
        auto button = lv_button_create(filter_row);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 56, LV_PCT(100));
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        // The selected minimum level wears its own log color.
        lv_obj_set_style_bg_color(button, lv_color_hex(kLevelColor[l]), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(kLevelColor[l]), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, l](lv_event_t *) {
            set_min_level(l);
        });
        auto label = lv_label_create(button);
        lv_label_set_text(label, kLevelLabel[l]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_center(label);
        level_btns_[l] = button;
    }
    lv_obj_add_state(level_btns_[0], LV_STATE_CHECKED);

    filter_ta_ = lv_textarea_create(filter_row);
    lv_textarea_set_one_line(filter_ta_, true);
    lv_textarea_set_placeholder_text(filter_ta_, "Filter");
    lv_textarea_set_max_length(filter_ta_, 64);
    lv_obj_set_height(filter_ta_, LV_PCT(100));
    lv_obj_set_flex_grow(filter_ta_, 1);
    lv_obj_add_event_fn(filter_ta_, LV_EVENT_FOCUSED, [this](lv_event_t *) {
        show_keyboard();
    });
    // FOCUSED alone misses a re-tap: the keyboard widget drops
    // CLICK_FOCUSABLE, so typing on it never updates the indev's last-pressed
    // object and tapping the still-"focused" textarea again sends no FOCUSED.
    lv_obj_add_event_fn(filter_ta_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        show_keyboard();
    });
    lv_obj_add_event_fn(filter_ta_, LV_EVENT_DEFOCUSED, [this](lv_event_t *) {
        hide_keyboard();
    });

    // --- list: recycled rows over an invisible scroll extent ---
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_size(list_, PANEL_W, kListH);
    lv_obj_set_pos(list_, 0, kNavH + kFilterH);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t *) {
        if (!programmatic_scroll_) {
            // A user scroll: follow the tail iff the view is at the bottom.
            int32_t max_scroll =
                (int32_t)filtered_.size() * kRowH - lv_obj_get_height(list_);
            if (max_scroll < 0) max_scroll = 0;
            follow_tail_ = lv_obj_get_scroll_y(list_) >= max_scroll - kRowH;
            if (follow_tail_) {
                lv_obj_add_flag(jump_btn_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(jump_btn_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        update_rows(false);
    });

    extent_ = lv_obj_create(list_);
    lv_obj_remove_style_all(extent_);
    lv_obj_set_size(extent_, 1, 0);
    lv_obj_set_pos(extent_, 0, 0);

    status_ = lv_label_create(list_);
    lv_label_set_text(status_, "No logs.");
    lv_obj_set_style_text_color(status_, lv_color_hex(0x444444), 0);
    lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 80);

    ensure_pool();

    // --- floating "jump to live tail" button (hidden while following) ---
    jump_btn_ = lv_button_create(root_);
    lv_obj_remove_style_all(jump_btn_);
    lv_obj_set_size(jump_btn_, 80, 80);
    lv_obj_set_pos(jump_btn_, PANEL_W - 104, PANEL_H - 104);
    lv_obj_set_style_radius(jump_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(jump_btn_, lv_theme_get_color_primary(jump_btn_), 0);
    lv_obj_set_style_bg_opa(jump_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(jump_btn_, lv_color_hex(0x444444), LV_STATE_PRESSED);
    lv_obj_add_flag(jump_btn_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_fn(jump_btn_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        follow_tail_ = true;
        update_view();
    });
    auto jump_icon = lv_label_create(jump_btn_);
    lv_label_set_text(jump_icon, LUCIDE_ARROW_DOWN_TO_LINE);
    lv_obj_set_style_text_font(jump_icon, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(jump_icon, lv_color_white(), 0);
    lv_obj_center(jump_icon);

    // --- on-screen keyboard for the filter textarea (overlay, hidden) ---
    kb_ = lv_keyboard_create(root_);
    lv_obj_set_size(kb_, PANEL_W, kKeyboardH);
    // The keyboard widget pre-sets ALIGN_BOTTOM_MID, so set_pos would offset
    // from the bottom; re-align instead.
    lv_obj_align(kb_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_fn(kb_, LV_EVENT_READY, [this](lv_event_t *) { hide_keyboard(); });
    lv_obj_add_event_fn(kb_, LV_EVENT_CANCEL, [this](lv_event_t *) { hide_keyboard(); });

    if (!ring_.init()) {
        lv_label_set_text(status_, "Out of memory.");
        return;
    }

    // Appends only mark dirty_; this timer does the extent/scroll/rebind work,
    // so a logcat burst costs one render per tick instead of one per line.
    render_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
            auto *self = static_cast<ADBLogcatScreen *>(lv_timer_get_user_data(t));
            if (self->dirty_) {
                self->dirty_ = false;
                self->update_view();
            }
        },
        100, this);

    update_view();
    start_stream();
}

void ADBLogcatScreen::onExit() {
    // LVGL thread, before destruction (exited() is already set). Marshalled
    // lambdas already queued see exited() and skip the freed widgets; the
    // in-flight save job owns its buffer, so it survives the screen.
    if (render_timer_) {
        lv_timer_delete(render_timer_);
        render_timer_ = nullptr;
    }
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

// ---- streaming ----

void ADBLogcatScreen::start_stream() {
    adb::Client *client = app::adb_client();
    std::shared_ptr<adb::ShellListener> self(
        shared_from_this(), static_cast<adb::ShellListener *>(this));
    // First start tails the last 500 lines; a resume backfills the paused span
    // from the last timestamp we saw (same-millisecond lines may duplicate).
    std::string cmd = "logcat -v threadtime -T ";
    cmd += last_ts_.empty() ? std::string("500") : "'" + last_ts_ + "'";
    shell_ = client ? client->open_shell(self, cmd) : nullptr;
    if (!shell_) {
        append_marker("[not connected]");
        dirty_ = true;
        paused_ = true;
        update_pause_icon();
        return;
    }
    paused_ = false;
    update_pause_icon();
}

void ADBLogcatScreen::pause_stream() {
    if (shell_) {
        ++expected_closes_;  // this close's on_shell_close is ours, not an error
        shell_->close();
        shell_.reset();
    }
    paused_ = true;
    update_pause_icon();
}

void ADBLogcatScreen::toggle_pause() {
    if (paused_) {
        start_stream();
    } else {
        pause_stream();
    }
}

void ADBLogcatScreen::update_pause_icon() {
    lv_label_set_text(pause_icon_, paused_ ? LUCIDE_PLAY : LUCIDE_PAUSE);
}

void ADBLogcatScreen::on_shell_data(adb::Shell * /*sh*/, const uint8_t *data,
                                    size_t len) {
    // Reader thread: buffer in arrival order and schedule one coalesced flush.
    bool need_schedule;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (overflow_ || pending_out_.size() + len > kPendingCap) {
            overflow_ = true;  // drop until the next flush drains the buffer
        } else {
            pending_out_.append(reinterpret_cast<const char *>(data), len);
        }
        need_schedule = !flush_scheduled_;
        flush_scheduled_ = true;
    }
    if (!need_schedule) return;
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_stream();
    });
}

void ADBLogcatScreen::on_shell_close(adb::Shell * /*sh*/, adb::Error /*err*/) {
    // Reader thread. Route through the FIFO flags so the marker lands after
    // the last buffered output; the flush decides expected vs unexpected.
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        close_marker_ = true;
        flush_scheduled_ = true;
    }
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_stream();
    });
}

void ADBLogcatScreen::flush_stream() {
    std::string chunk;
    bool overflow, closed;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        chunk.swap(pending_out_);
        overflow = overflow_;
        closed = close_marker_;
        overflow_ = false;
        close_marker_ = false;
        flush_scheduled_ = false;
    }
    carry_.append(chunk);
    size_t start = 0;
    while (true) {
        size_t eol = carry_.find('\n', start);
        if (eol == std::string::npos) break;
        size_t len = eol - start;
        while (len && carry_[start + len - 1] == '\r') --len;
        append_line(carry_.data() + start, len);
        start = eol + 1;
    }
    carry_.erase(0, start);
    if (carry_.size() > 2 * kMaxLineLen) {
        // Pathological newline-free stream: don't grow the carry unbounded.
        append_line(carry_.data(), carry_.size());
        carry_.clear();
    }
    if (overflow) {
        carry_.clear();  // the partial line's tail was dropped with the rest
        append_marker("[output dropped]");
    }
    if (closed) {
        if (expected_closes_ > 0) {
            --expected_closes_;  // our own pause/exit close
        } else {
            // The logcat process died on its own.
            shell_.reset();
            paused_ = true;
            update_pause_icon();
            append_marker("[logcat terminated]");
        }
    }
    dirty_ = true;
}

void ADBLogcatScreen::push_ring(const char *line, size_t len, uint8_t level) {
    size_t evicted = ring_.append(line, len, level);
    if (evicted) {
        // Drop filtered entries that now point below the ring head, counting
        // them so the next render can keep a scrolled-back view anchored.
        uint32_t head = ring_.head_seq();
        while (!filtered_.empty() && filtered_.front() < head) {
            filtered_.pop_front();
            ++pending_evicted_;
        }
    }
    const LogRing::Line &l = ring_.at(ring_.count() - 1);
    if (line_matches(l)) filtered_.push_back(l.seq);
}

void ADBLogcatScreen::append_line(const char *line, size_t len) {
    char ts[19];
    int level = parse_level(line, len, ts);
    if (level < 0) {
        level = last_level_;  // continuation line
    } else {
        last_level_ = level;
        last_ts_ = ts;
    }
    push_ring(line, len, (uint8_t)level);
}

void ADBLogcatScreen::append_marker(const char *text) {
    push_ring(text, strlen(text), 3 /*W: visible by default*/);
}

// ---- filtering ----

bool ADBLogcatScreen::line_matches(const LogRing::Line &l) const {
    if (l.level < min_level_) return false;
    return ci_contains(ring_.text(l), l.len, filter_lc_);
}

void ADBLogcatScreen::rebuild_filtered() {
    filtered_.clear();
    pending_evicted_ = 0;
    for (size_t i = 0; i < ring_.count(); ++i) {
        const LogRing::Line &l = ring_.at(i);
        if (line_matches(l)) filtered_.push_back(l.seq);
    }
}

void ADBLogcatScreen::set_min_level(int level) {
    if (min_level_ == level) return;
    min_level_ = level;
    for (int l = 0; l < 5; ++l) {
        if (l == level) {
            lv_obj_add_state(level_btns_[l], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(level_btns_[l], LV_STATE_CHECKED);
        }
    }
    rebuild_filtered();
    follow_tail_ = true;
    update_view();
}

void ADBLogcatScreen::apply_text_filter() {
    std::string lc = lv_textarea_get_text(filter_ta_);
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (lc == filter_lc_) return;
    filter_lc_ = std::move(lc);
    rebuild_filtered();
    follow_tail_ = true;
    update_view();
}

void ADBLogcatScreen::show_keyboard() {
    lv_keyboard_set_textarea(kb_, filter_ta_);
    lv_obj_remove_flag(kb_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb_);
}

void ADBLogcatScreen::hide_keyboard() {
    lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(kb_, nullptr);
    lv_obj_remove_state(filter_ta_, LV_STATE_FOCUSED);
    apply_text_filter();
}

// ---- view ----

void ADBLogcatScreen::ensure_pool() {
    if (!pool_.empty()) return;
    size_t n = (size_t)(kListH / kRowH) + 3;  // partial top/bottom + lookbehind
    pool_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Row r = {};
        r.btn = lv_button_create(list_);
        lv_obj_remove_style_all(r.btn);
        lv_obj_set_size(r.btn, LV_PCT(100), kRowH);
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_hor(r.btn, 12, 0);
        lv_obj_set_style_bg_color(r.btn, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.btn, LV_OPA_COVER, LV_STATE_PRESSED);
        // Tapping a (clipped) row shows the full line; the handler reads the
        // slot's bound index at tap time (pool_ is created once, no realloc).
        size_t slot = i;
        lv_obj_add_event_fn(r.btn, LV_EVENT_CLICKED, [this, slot](lv_event_t *) {
            int idx = pool_[slot].data_idx;
            if (idx < 0 || idx >= (int)filtered_.size()) return;
            uint32_t seq = filtered_[idx];
            uint32_t head = ring_.head_seq();
            if (seq < head || seq - head >= ring_.count()) return;
            app::modal_message(root_, "Log", ring_.text(ring_.at(seq - head)));
        });
        r.label = lv_label_create(r.btn);
        lv_obj_set_width(r.label, LV_PCT(100));
        lv_label_set_long_mode(r.label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(r.label, R.font.hack_16, 0);
        lv_obj_align(r.label, LV_ALIGN_LEFT_MID, 0, 0);
        pool_.push_back(r);
    }
}

void ADBLogcatScreen::bind_row(Row &r, int idx) {
    uint32_t head = ring_.head_seq();
    if (idx < 0 || idx >= (int)filtered_.size() || filtered_[idx] < head ||
        filtered_[idx] - head >= ring_.count()) {
        r.data_idx = -1;
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    r.data_idx = idx;
    lv_obj_remove_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(r.btn, 0, idx * kRowH);
    const LogRing::Line &l = ring_.at(filtered_[idx] - head);
    lv_label_set_text(r.label, ring_.text(l));
    lv_obj_set_style_text_color(r.label, lv_color_hex(kLevelColor[l.level]), 0);
}

void ADBLogcatScreen::update_rows(bool force) {
    if (pool_.empty()) return;
    int32_t sy = lv_obj_get_scroll_y(list_);
    int first = (int)(sy / kRowH) - 1;  // one row of lookbehind above the fold
    if (first < 0) first = 0;
    if (!force && first == first_bound_) return;
    first_bound_ = first;
    for (size_t i = 0; i < pool_.size(); ++i) {
        bind_row(pool_[i], first + (int)i);
    }
}

void ADBLogcatScreen::update_view() {
    size_t count = filtered_.size();
    if (count == 0) {
        lv_label_set_text(status_, ring_.count() == 0 ? "No logs."
                                                      : "No matching logs.");
        lv_obj_remove_flag(status_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_height(extent_, (int32_t)count * kRowH);
    lv_obj_update_layout(list_);
    int32_t max_scroll = (int32_t)count * kRowH - lv_obj_get_height(list_);
    if (max_scroll < 0) max_scroll = 0;
    size_t evicted = pending_evicted_;
    pending_evicted_ = 0;
    programmatic_scroll_ = true;
    if (follow_tail_) {
        lv_obj_scroll_to_y(list_, max_scroll, LV_ANIM_OFF);
    } else {
        // Rows evicted from the front shift all content up: pull the scroll
        // position by the same amount so the view stays on the same lines.
        int32_t sy = lv_obj_get_scroll_y(list_) - (int32_t)evicted * kRowH;
        if (sy < 0) sy = 0;
        if (sy > max_scroll) sy = max_scroll;
        lv_obj_scroll_to_y(list_, sy, LV_ANIM_OFF);
    }
    programmatic_scroll_ = false;
    update_rows(true);
    if (follow_tail_) {
        lv_obj_add_flag(jump_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(jump_btn_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ADBLogcatScreen::clear_logs() {
    ring_.clear();
    filtered_.clear();
    pending_evicted_ = 0;
    follow_tail_ = true;
    update_view();
}

// ---- save to SD ----

void ADBLogcatScreen::save_to_sd() {
    if (save_card_) return;  // a save is already in flight
    if (ring_.count() == 0) {
        app::modal_message(root_, "Save", "No logs.");
        return;
    }
    if (!bsp_sd_is_mounted()) {
        esp_err_t err = bsp_sd_mount(kMountPoint, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            app::modal_message(root_, "Save failed", "SD card not found.");
            return;
        }
    }
    // logcat_YYYYMMDD_HHMMSS.txt once the clock is synced from the phone, else
    // the RTC-less logcat_NNN.txt fallback.
    std::string path = app::sysclock::dated_path(kMountPoint, "logcat", "txt");
    if (path.empty()) {
        app::modal_message(root_, "Save failed", "Too many log files.");
        return;
    }

    // Snapshot the whole ring (the stream keeps mutating it) into one PSRAM
    // buffer; the writer task owns it through the job.
    auto job = std::make_shared<SaveJob>();
    size_t total = ring_.text_bytes() + ring_.count();  // + '\n' per line
    job->buf = (char *)heap_caps_malloc(
        total, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!job->buf) {
        app::modal_message(root_, "Save failed", "Out of memory.");
        return;
    }
    size_t off = 0;
    for (size_t n = 0; n < ring_.count(); ++n) {
        const LogRing::Line &l = ring_.at(n);
        memcpy(job->buf + off, ring_.text(l), l.len);
        off += l.len;
        job->buf[off++] = '\n';
    }
    job->len = off;
    job->path = path;

    save_card_ = app::modal_open(root_);
    auto title = lv_label_create(save_card_);
    lv_label_set_text(title, "Saving log");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    auto text = lv_label_create(save_card_);
    lv_label_set_text(text, path.c_str());
    lv_obj_set_style_text_font(text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0x444444), 0);
    auto spinner = lv_spinner_create(save_card_);
    lv_obj_set_size(spinner, 64, 64);

    // One-shot writer task: a 2 MB write would stall the LVGL thread.
    auto *fn = new std::function<void()>([self = shared_from_this(), this, job]() {
        int fd = ::open(job->path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        bool ok = fd >= 0;
        size_t off = 0;
        while (ok && off < job->len) {
            size_t n = std::min(job->len - off, kWriteChunk);
            ssize_t w = ::write(fd, job->buf + off, n);
            if (w <= 0) { ok = false; break; }
            off += (size_t)w;
        }
        if (fd >= 0) ::close(fd);
        lv_async_call([self, this, job, ok]() {
            if (exited()) return;
            if (save_card_) {
                app::modal_close(save_card_);
                save_card_ = nullptr;
            }
            if (ok) {
                app::modal_message(root_, "Save",
                                   (job->path + " saved.").c_str());
            } else {
                app::modal_message(root_, "Save failed",
                                   ("Cannot write " + job->path).c_str());
            }
        });
    });
    BaseType_t created = xTaskCreate(
        [](void *arg) {
            auto *f = static_cast<std::function<void()> *>(arg);
            (*f)();
            delete f;
            vTaskDelete(nullptr);
        },
        "logcat_save", 6144, fn, 2, nullptr);
    if (created != pdPASS) {
        delete fn;  // drops the job (and its buffer) too
        app::modal_close(save_card_);
        save_card_ = nullptr;
        app::modal_message(root_, "Save failed", "Cannot start the writer.");
    }
}
