#include "adb_device_screen.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

#include "adb_app.hpp"
#include "adb_session.hpp"

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

void strip_eol(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// Background: pull a few props over a shell stream, then update the label on the
// LVGL thread. The screen is terminal (no back nav) so the label stays valid.
void props_worker(void *arg) {
    auto *label = static_cast<lv_obj_t *>(arg);
    auto *conn = app::adb_connection();
    std::string text;
    if (conn) {
        auto getprop = [&](const char *prop) {
            std::string out;
            conn->run_service(std::string("shell:getprop ") + prop, out, 5000);
            strip_eol(out);
            return out;
        };
        text = "Android " + getprop("ro.build.version.release") +
               "  (SDK " + getprop("ro.build.version.sdk") + ")\n" +
               "Manufacturer: " + getprop("ro.product.manufacturer") + "\n" +
               "Serial: " + getprop("ro.serialno");
    } else {
        text = "(no connection)";
    }
    lv_async_call([label, text]() { lv_label_set_text(label, text.c_str()); });
    vTaskDelete(nullptr);
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

    const std::string &banner = app::adb_banner();

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

    xTaskCreate(props_worker, "adb_props", 8192, props_label_, 5, nullptr);
}
