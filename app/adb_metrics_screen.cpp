#include "adb_metrics_screen.hpp"

#include <cctype>
#include <cstdio>

#include "adb_app.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace devinfo = app::devinfo;

namespace {

// One frame per second, delimited by @@@. The grep keeps /proc/stat to the cpu
// lines and dumpsys battery to level/temperature, so a frame stays ~1 KB.
const char *kStreamCmd =
    "while true; do echo @@@; "
    "grep ^cpu /proc/stat; "
    "grep -E 'MemTotal|MemAvailable' /proc/meminfo; "
    "cat /proc/loadavg; "
    "dumpsys battery | grep -E 'temperature:|level:'; "
    "sleep 1; done";

constexpr size_t kPendingCap = 64 * 1024;
constexpr int kChartPoints = 60;  // ~1 minute at the 1 s tick

// "0.47 0.58 0.54 1/3733 6728" -> "0.47 0.58 0.54"
std::string loadavg_of(const std::string &frame) {
    size_t pos = 0;
    while (pos < frame.size()) {
        size_t nl = frame.find('\n', pos);
        std::string line = frame.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? frame.size() : nl + 1;
        if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
        if (line.find('/') == std::string::npos || line.find('.') == std::string::npos) continue;
        // first three whitespace-separated tokens
        size_t sp = 0;
        for (int i = 0; i < 3 && sp != std::string::npos; ++i) sp = line.find(' ', sp + 1);
        return sp == std::string::npos ? line : line.substr(0, sp);
    }
    return "";
}

lv_obj_t *metric_card(lv_obj_t *parent) {
    auto c = lv_obj_create(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    return c;
}

// Card header: icon + title, then a right-aligned value label (returned).
lv_obj_t *card_head(lv_obj_t *card, const char *icon, const char *title) {
    auto head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    auto icon_label = lv_label_create(head);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0x666666), 0);
    auto title_label = lv_label_create(head);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x888888), 0);
    lv_obj_set_flex_grow(title_label, 1);
    auto value = lv_label_create(head);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_28, 0);
    return value;
}

lv_obj_t *history_chart(lv_obj_t *card, lv_color_t color, lv_chart_series_t **ser) {
    auto chart = lv_chart_create(card);
    lv_obj_set_size(chart, LV_PCT(100), 180);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart, 5, 7);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);  // line only, no dots
    *ser = lv_chart_add_series(chart, color, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, *ser, LV_CHART_POINT_NONE);
    return chart;
}

}  // namespace

void ADBMetricsScreen::build() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
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
    lv_label_set_text(title, "Metrics");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto content = lv_obj_create(root_);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 24, 0);
    lv_obj_set_style_pad_row(content, 24, 0);

    // ---- CPU: total % + 60 s history + per-core bars ----
    auto cpu_card = metric_card(content);
    cpu_pct_label_ = card_head(cpu_card, LUCIDE_CPU, "CPU");
    cpu_chart_ = history_chart(cpu_card, lv_palette_main(LV_PALETTE_BLUE), &cpu_ser_);
    cores_box_ = lv_obj_create(cpu_card);
    lv_obj_remove_style_all(cores_box_);
    lv_obj_set_size(cores_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cores_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cores_box_, 8, 0);

    // ---- Memory: used/total + 60 s history (% of total) ----
    auto mem_card = metric_card(content);
    mem_label_ = card_head(mem_card, LUCIDE_MEMORY_STICK, "Memory");
    mem_chart_ = history_chart(mem_card, lv_palette_main(LV_PALETTE_GREEN), &mem_ser_);

    // ---- the text rows ----
    auto misc_card = metric_card(content);
    auto text_row = [&misc_card](const char *key) {
        auto box = lv_obj_create(misc_card);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto k = lv_label_create(box);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x888888), 0);
        lv_obj_set_flex_grow(k, 1);
        auto v = lv_label_create(box);
        lv_label_set_text(v, "-");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
        return v;
    };
    load_label_ = text_row("Load average");
    batt_label_ = text_row("Battery");
}

void ADBMetricsScreen::onAppear() { start_stream(); }

void ADBMetricsScreen::onDisappear() { stop_stream(); }

void ADBMetricsScreen::start_stream() {
    adb::Client *client = app::adb_client();
    std::shared_ptr<adb::ShellListener> self(
        shared_from_this(), static_cast<adb::ShellListener *>(this));
    // Fresh baseline: the first frame only seeds prev_cpu_, the second one draws.
    acc_.clear();
    prev_cpu_ = {};
    shell_ = client ? client->open_shell(self, kStreamCmd) : nullptr;
    if (!shell_) lv_label_set_text(cpu_pct_label_, "n/a");
}

void ADBMetricsScreen::stop_stream() {
    if (!shell_) return;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        ++expected_closes_;  // this close's on_shell_close is ours, not an error
    }
    shell_->close();
    shell_.reset();
}

void ADBMetricsScreen::on_shell_data(adb::Shell * /*sh*/, const uint8_t *data, size_t len) {
    // Reader thread: buffer and coalesce one flush (lv_async_call is LIFO, so
    // never one async per chunk).
    bool need_schedule;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (overflow_ || pending_.size() + len > kPendingCap) {
            overflow_ = true;  // drop until the next flush drains; frames resync on @@@
        } else {
            pending_.append(reinterpret_cast<const char *>(data), len);
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

void ADBMetricsScreen::on_shell_close(adb::Shell * /*sh*/, adb::Error /*err*/) {
    // Reader thread. Expected (our own stop_stream) closes are silent; an
    // unexpected one freezes the charts and says so.
    bool expected;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        expected = expected_closes_ > 0;
        if (expected) --expected_closes_;
    }
    if (expected) return;
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        lv_label_set_text(cpu_pct_label_, "stopped");
    });
}

void ADBMetricsScreen::flush_stream() {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        chunk.swap(pending_);
        overflow_ = false;
        flush_scheduled_ = false;
    }
    acc_ += chunk;
    if (acc_.size() > kPendingCap) acc_.erase(0, acc_.size() - kPendingCap);

    // A frame is complete once the *next* @@@ marker arrives. Parse only the
    // last complete frame (latest wins — older ones are stale by definition)
    // and keep from the final marker on as the in-progress frame.
    size_t last = acc_.rfind("@@@\n");
    if (last == std::string::npos || last == 0) return;
    size_t prev = acc_.rfind("@@@\n", last - 1);
    std::string frame = acc_.substr(prev == std::string::npos ? 0 : prev + 4,
                                    last - (prev == std::string::npos ? 0 : prev + 4));
    acc_.erase(0, last);
    apply_frame(frame);
}

void ADBMetricsScreen::apply_frame(const std::string &frame) {
    // ---- CPU ----
    devinfo::CpuTimes now = devinfo::parse_proc_stat(frame);
    std::vector<int> usage = devinfo::cpu_usage(prev_cpu_, now);
    if (!now.cores.empty()) prev_cpu_ = std::move(now);
    if (!usage.empty()) {
        lv_label_set_text_fmt(cpu_pct_label_, "%d%%", usage[0]);
        lv_chart_set_next_value(cpu_chart_, cpu_ser_, usage[0]);
        ensure_core_rows(usage.size() - 1);
        for (size_t i = 1; i < usage.size(); ++i) {
            lv_bar_set_value(core_rows_[i - 1].bar, usage[i], LV_ANIM_OFF);
            lv_label_set_text_fmt(core_rows_[i - 1].pct, "%d%%", usage[i]);
        }
    }

    // ---- Memory ----
    devinfo::Mem mem = devinfo::parse_meminfo(frame);
    if (mem.total_kb > 0) {
        int64_t used = mem.total_kb - mem.avail_kb;
        lv_label_set_text(mem_label_, (devinfo::format_kb(used) + " / " +
                                       devinfo::format_kb(mem.total_kb)).c_str());
        lv_chart_set_next_value(mem_chart_, mem_ser_, (int32_t)(used * 100 / mem.total_kb));
    }

    // ---- text rows ----
    std::string load = loadavg_of(frame);
    if (!load.empty()) lv_label_set_text(load_label_, load.c_str());
    devinfo::Battery bat = devinfo::parse_dumpsys_battery(frame);
    if (bat.level >= 0) {
        std::string s = std::to_string(bat.level) + "%";
        if (bat.temp_dC > 0) s += " \xE2\x80\xA2 " + devinfo::format_temp_dC(bat.temp_dC);
        lv_label_set_text(batt_label_, s.c_str());
    }
}

void ADBMetricsScreen::ensure_core_rows(size_t n) {
    while (core_rows_.size() < n) {
        auto box = lv_obj_create(cores_box_);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(box, 12, 0);
        auto name = lv_label_create(box);
        lv_label_set_text_fmt(name, "cpu%d", (int)core_rows_.size());
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0x888888), 0);
        lv_obj_set_width(name, 80);
        auto bar = lv_bar_create(box);
        lv_obj_set_height(bar, 12);
        lv_obj_set_flex_grow(bar, 1);
        lv_bar_set_range(bar, 0, 100);
        auto pct = lv_label_create(box);
        lv_label_set_text(pct, "0%");
        lv_obj_set_style_text_font(pct, &lv_font_montserrat_20, 0);
        lv_obj_set_width(pct, 70);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
        core_rows_.push_back({bar, pct});
    }
}
