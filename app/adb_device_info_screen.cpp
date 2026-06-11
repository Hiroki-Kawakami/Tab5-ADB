#include "adb_device_info_screen.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adb.hpp"
#include "adb_app.hpp"
#include "adb_metrics_screen.hpp"
#include "device_icons.hpp"
#include "device_info.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace devinfo = app::devinfo;

// Everything one chained exec returns, parsed on the adb reader thread.
struct DeviceInfoData {
    devinfo::PropList props;
    devinfo::Mem mem;
    devinfo::Storage storage;
    devinfo::Battery battery;
    devinfo::Wifi wifi;
    devinfo::Cellular cell;
    std::string kernel_version;  // /proc/version first line
    std::string wm_size, wm_density;
    int ncpu = 0;
    int max_freq_khz = 0;
    std::string uptime;
};

namespace {

const char *kInfoCmd =
    "getprop; echo ---SEP---; "
    "grep -E 'MemTotal|MemAvailable' /proc/meminfo; echo ---SEP---; "
    "df -k /data; echo ---SEP---; "
    "dumpsys battery; echo ---SEP---; "
    "cmd wifi status; echo ---SEP---; "
    "dumpsys telephony.registry | grep -E 'mServiceState=|mSignalStrength=' | head -4; "
    "echo ---SEP---; "
    "cat /proc/version; echo ---SEP---; "
    "wm size; wm density; echo ---SEP---; "
    "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -n | tail -1; "
    "ls /sys/devices/system/cpu/ | grep -cE '^cpu[0-9]+$'; echo ---SEP---; "
    "cat /proc/uptime";

// "Physical size: 1080x2340" (+ optional "Override size: ..." which wins, the
// `wm size` convention also used by the mirror's adapt mode).
std::string wm_value(const std::string &out, const char *kind) {
    std::string v;
    for (const char *prefix : {"Override ", "Physical "}) {
        std::string needle = std::string(prefix) + kind + ": ";
        size_t pos = out.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        size_t end = out.find('\n', pos);
        v = devinfo::trim(out.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (!v.empty()) return v;
    }
    return v;
}

DeviceInfoData parse_info(const std::string &out) {
    DeviceInfoData d;
    auto sec = devinfo::split_sections(out);
    auto section = [&sec](size_t i) -> const std::string & {
        static const std::string kEmpty;
        return i < sec.size() ? sec[i] : kEmpty;
    };
    d.props = devinfo::parse_getprop(section(0));
    d.mem = devinfo::parse_meminfo(section(1));
    d.storage = devinfo::parse_df(section(2));
    d.battery = devinfo::parse_dumpsys_battery(section(3));
    d.wifi = devinfo::parse_wifi_status(section(4));
    d.cell = devinfo::parse_cellular(devinfo::prop(d.props, "gsm.sim.state"), section(5));
    d.kernel_version = devinfo::first_line(section(6));
    d.wm_size = wm_value(section(7), "size");
    d.wm_density = wm_value(section(7), "density");
    // section 8: max cpufreq (kHz) then the core count, one per line
    {
        auto lines = section(8);
        d.max_freq_khz = std::atoi(lines.c_str());
        size_t nl = lines.find('\n');
        if (nl != std::string::npos) d.ncpu = std::atoi(lines.c_str() + nl + 1);
    }
    d.uptime = devinfo::format_uptime(section(9));
    return d;
}

std::string freq_ghz(int khz) {
    if (khz <= 0) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f GHz", khz / 1e6);
    return buf;
}

}  // namespace

void ADBDeviceInfoScreen::build() {
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
    lv_label_set_text(title, "Device Info");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    content_ = lv_obj_create(root_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_width(content_, LV_PCT(100));
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content_, 24, 0);
    lv_obj_set_style_pad_row(content_, 24, 0);

    info_box_ = lv_obj_create(content_);
    lv_obj_remove_style_all(info_box_);
    lv_obj_set_size(info_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(info_box_, 24, 0);
    auto spinner = lv_spinner_create(info_box_);
    lv_obj_set_size(spinner, 48, 48);

    load_info();
}

void ADBDeviceInfoScreen::load_info() {
    adb::Client *client = app::adb_client();
    if (!client) return;  // keep the spinner; nothing to fetch without a connection
    client->exec(kInfoCmd, [self = shared_from_this(), this](adb::Error err,
                                                             const std::string &out) {
        if (err != adb::Error::Ok || out.empty()) return;  // spinner stays
        auto data = std::make_shared<DeviceInfoData>(parse_info(out));
        lv_async_call([self, this, data]() {
            if (exited()) return;
            rebuild(*data);
        });
    });
}

void ADBDeviceInfoScreen::rebuild(const DeviceInfoData &d) {
    lv_obj_clean(info_box_);

    // ---- featured cards, two per row (672px content -> 324px each) ----
    auto cards = lv_obj_create(info_box_);
    lv_obj_remove_style_all(cards);
    lv_obj_set_size(cards, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(cards, 24, 0);
    lv_obj_set_style_pad_row(cards, 24, 0);

    auto card = [&cards](const char *icon, const char *title) {
        auto c = lv_obj_create(cards);
        lv_obj_set_size(c, 324, LV_SIZE_CONTENT);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 8, 0);
        auto head = lv_obj_create(c);
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
        return c;
    };
    auto main_line = [](lv_obj_t *c, const std::string &text) {
        auto l = lv_label_create(c);
        lv_label_set_text(l, text.empty() ? "-" : text.c_str());
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return l;
    };
    auto sub_line = [](lv_obj_t *c, const std::string &text) {
        auto l = lv_label_create(c);
        lv_label_set_text(l, text.c_str());
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x666666), 0);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return l;
    };
    auto p = [&d](const char *key) { return devinfo::prop(d.props, key); };

    {  // SoC
        auto c = card(LUCIDE_CPU, "SoC");
        std::string soc = devinfo::trim(p("ro.soc.manufacturer") + " " + p("ro.soc.model"));
        if (soc.empty()) soc = p("ro.board.platform");
        if (soc.empty()) soc = p("ro.hardware");
        main_line(c, soc);
        std::string platform = p("ro.board.platform");
        if (!platform.empty() && soc.find(platform) == std::string::npos)
            sub_line(c, "Platform: " + platform);
        std::string cpu_line;
        if (d.ncpu > 0) cpu_line = std::to_string(d.ncpu) + " cores";
        std::string freq = freq_ghz(d.max_freq_khz);
        if (!freq.empty()) cpu_line += (cpu_line.empty() ? "up to " : " \xE2\x80\xA2 up to ") + freq;
        if (!cpu_line.empty()) sub_line(c, cpu_line);
        std::string abis = p("ro.product.cpu.abilist");
        if (size_t comma = abis.find(','); comma != std::string::npos) abis.resize(comma);
        if (!abis.empty()) sub_line(c, "ABI: " + abis);
    }
    {  // Memory
        auto c = card(LUCIDE_MEMORY_STICK, "Memory");
        int gb = devinfo::marketed_ram_gb(d.mem.total_kb);
        main_line(c, gb ? std::to_string(gb) + " GB" : "");
        if (d.mem.total_kb > 0) sub_line(c, "Total " + devinfo::format_kb(d.mem.total_kb));
        if (d.mem.avail_kb > 0) sub_line(c, "Available " + devinfo::format_kb(d.mem.avail_kb));
    }
    {  // Storage
        auto c = card(LUCIDE_HARD_DRIVE, "Storage");
        if (d.storage.total_kb > 0) {
            main_line(c, devinfo::format_kb(d.storage.used_kb) + " / " +
                             devinfo::format_kb(d.storage.total_kb));
            auto bar = lv_bar_create(c);
            lv_obj_set_size(bar, LV_PCT(100), 12);
            lv_bar_set_range(bar, 0, 1000);
            lv_bar_set_value(bar, (int32_t)(d.storage.used_kb * 1000 / d.storage.total_kb),
                             LV_ANIM_OFF);
            sub_line(c, "Free " + devinfo::format_kb(d.storage.avail_kb));
            int gb = devinfo::marketed_storage_gb(d.storage.total_kb);
            if (gb) sub_line(c, "Marketed " + std::to_string(gb) + " GB");
        } else {
            main_line(c, "");
        }
    }
    {  // Battery
        auto c = card(devinfo::battery_icon(d.battery), "Battery");
        if (d.battery.level >= 0) {
            main_line(c, std::to_string(d.battery.level) + "% \xE2\x80\xA2 " +
                             d.battery.status_str());
            sub_line(c, std::string("Health: ") + d.battery.health_str());
            sub_line(c, "Temp: " + devinfo::format_temp_dC(d.battery.temp_dC) + " \xE2\x80\xA2 " +
                            std::to_string(d.battery.voltage_mV) + " mV");
            if (!d.battery.technology.empty() && d.battery.technology != "Unknown")
                sub_line(c, "Technology: " + d.battery.technology);
        } else {
            main_line(c, "");
        }
    }
    {  // Network
        auto c = card(devinfo::wifi_icon(d.wifi), "Network");
        const char *wifi_state =
            d.wifi.state == devinfo::Wifi::State::Connected      ? "Wi-Fi connected"
            : d.wifi.state == devinfo::Wifi::State::Disconnected ? "Wi-Fi not connected"
                                                                 : "Wi-Fi off";
        main_line(c, wifi_state);
        if (d.wifi.state == devinfo::Wifi::State::Connected) {
            sub_line(c, "RSSI " + std::to_string(d.wifi.rssi) + " dBm (" +
                            std::to_string(d.wifi.bars()) + "/4)");
            if (!d.wifi.ip.empty()) sub_line(c, "IP " + d.wifi.ip);
        }
        std::string cell = !d.cell.sim_present ? "Cellular: no SIM"
                           : !d.cell.in_service
                               ? "Cellular: out of service"
                               : "Cellular: signal " + std::to_string(d.cell.level) + "/4";
        sub_line(c, cell);
    }
    {  // System
        auto c = card(LUCIDE_INFO, "System");
        std::string ver = p("ro.build.version.release");
        main_line(c, ver.empty() ? "" : "Android " + ver);
        if (!p("ro.build.version.sdk").empty()) sub_line(c, "SDK " + p("ro.build.version.sdk"));
        if (!p("ro.build.version.security_patch").empty())
            sub_line(c, "Patch " + p("ro.build.version.security_patch"));
        if (!p("ro.build.id").empty()) sub_line(c, "Build " + p("ro.build.id"));
        std::string kernel = devinfo::short_kernel(d.kernel_version);
        if (!kernel.empty()) sub_line(c, "Kernel " + kernel);
    }

    // ---- Performance Metrics -> live CPU/memory screen ----
    auto button = lv_button_create(info_box_);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(button, 32, 0);
    lv_obj_set_style_pad_column(button, 24, 0);
    lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [](lv_event_t*) {
        screen_manager.push(std::make_shared<ADBMetricsScreen>());
    });
    auto button_icon = lv_label_create(button);
    lv_label_set_text(button_icon, LUCIDE_ACTIVITY);
    lv_obj_set_style_text_font(button_icon, R.font.lucide_40, 0);
    auto button_text = lv_label_create(button);
    lv_label_set_text(button_text, "Performance Metrics");
    lv_obj_set_style_text_font(button_text, &lv_font_montserrat_28, 0);

    // ---- the miscellaneous key-value list ----
    auto misc = lv_obj_create(info_box_);
    lv_obj_set_size(misc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(misc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(misc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(misc, 16, 0);
    auto row = [&misc](const char *key, const std::string &value) {
        if (value.empty()) return;
        auto box = lv_obj_create(misc);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, 4, 0);
        auto k = lv_label_create(box);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x888888), 0);
        auto v = lv_label_create(box);
        lv_label_set_text(v, value.c_str());
        lv_obj_set_width(v, LV_PCT(100));
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    };
    row("Manufacturer", p("ro.product.manufacturer"));
    row("Brand", p("ro.product.brand"));
    row("Model", p("ro.product.model"));
    row("Device", p("ro.product.device"));
    row("Product", p("ro.product.name"));
    row("Board", p("ro.product.board"));
    row("Hardware", p("ro.hardware"));
    row("Serial", p("ro.serialno"));
    row("Bootloader", p("ro.bootloader"));
    row("Baseband", p("gsm.version.baseband"));
    row("ABIs", p("ro.product.cpu.abilist"));
    std::string display = d.wm_size;
    if (!display.empty() && !d.wm_density.empty()) display += " @ " + d.wm_density + " dpi";
    row("Display", display);
    row("Uptime", d.uptime);
    row("Kernel", d.kernel_version);
    row("Fingerprint", p("ro.build.fingerprint"));
    row("Build date", p("ro.build.date"));
    row("Locale", p("persist.sys.locale"));
    row("Timezone", p("persist.sys.timezone"));
    row("SIM state", p("gsm.sim.state"));
}
