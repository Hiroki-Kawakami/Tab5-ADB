#include "adb_device_screen.hpp"

#include <memory>
#include <string>
#include <vector>

#include "adb.hpp"  // adb::Client, adb::Error
#include "adb_app.hpp"
#include "adb_shell_screen.hpp"
#include "screen_manager.hpp"

namespace {

// Extract a value from a banner "device::key=val;key=val;..." string.
std::string banner_field(const std::string &banner, const std::string &key) {
    auto sep = banner.find("::");
    std::string body = sep == std::string::npos ? banner : banner.substr(sep + 2);
    std::string needle = key + "=";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "?";
    pos += needle.size();
    auto end = body.find(';', pos);
    return body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// Split shell output into lines, dropping \r so \n / \r\n both work.
std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : s) {
        if (ch == '\n') { lines.push_back(cur); cur.clear(); }
        else if (ch != '\r') cur += ch;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace

void ADBDeviceScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root_, 24, 0);
    lv_obj_set_style_pad_row(root_, 16, 0);

    adb::Client *client = app::adb_client();
    static const std::string kNoBanner;
    const std::string &banner = client ? client->banner() : kNoBanner;

    lv_obj_t *title = lv_label_create(root_);
    lv_label_set_text(title, "Connected");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    auto add_row = [&](const char *caption, const std::string &value) {
        lv_obj_t *l = lv_label_create(root_);
        lv_label_set_text(l, (std::string(caption) + value).c_str());
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_width(l, PANEL_W - 48);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    };

    add_row("Model: ", banner_field(banner, "ro.product.model"));
    add_row("Name: ", banner_field(banner, "ro.product.name"));
    add_row("Device: ", banner_field(banner, "ro.product.device"));

    // Live props (filled in asynchronously).
    props_label_ = lv_label_create(root_);
    lv_label_set_text(props_label_, "Reading device properties...");
    lv_obj_set_style_text_color(props_label_, lv_color_hex(0xb0bec5), 0);
    lv_obj_set_width(props_label_, PANEL_W - 48);
    lv_label_set_long_mode(props_label_, LV_LABEL_LONG_WRAP);

    // One exec (commands chained with ';') returns four newline-separated values.
    // The completion fires on the reader thread, so marshal the label update to
    // LVGL. The screen is terminal (no back nav), so props_label_ stays valid.
    lv_obj_t *label = props_label_;
    if (!client) {
        lv_label_set_text(label, "(no connection)");
        return;
    }

    // Open the interactive terminal (ADBShellScreen) on top.
    lv_obj_t *term_btn = lv_button_create(root_);
    lv_obj_set_size(term_btn, 260, 80);
    lv_obj_t *term_lbl = lv_label_create(term_btn);
    lv_label_set_text(term_lbl, LV_SYMBOL_KEYBOARD " Open Terminal");
    lv_obj_center(term_lbl);
    lv_obj_add_event_fn(term_btn, LV_EVENT_CLICKED, [](lv_event_t *) {
        screen_manager.push(std::make_unique<ADBShellScreen>());
    });

    client->exec(
        "getprop ro.build.version.release; getprop ro.build.version.sdk; "
        "getprop ro.product.manufacturer; getprop ro.serialno",
        [label](adb::Error err, const std::string &out) {
            std::string text;
            if (err == adb::Error::Ok) {
                auto lines = split_lines(out);
                auto field = [&](size_t i) {
                    return i < lines.size() ? lines[i] : std::string("?");
                };
                text = "Android " + field(0) + "  (SDK " + field(1) + ")\n" +
                       "Manufacturer: " + field(2) + "\n" +
                       "Serial: " + field(3);
            } else {
                text = "(failed to read properties)";
            }
            lv_async_call([label, text]() { lv_label_set_text(label, text.c_str()); });
        });
}
