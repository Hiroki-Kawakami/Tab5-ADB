#include "adb_device_screen.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "adb.hpp"  // adb::Client
#include "adb_app.hpp"
#include "adb_file_manager_screen.hpp"
#include "adb_mirroring_screen.hpp"
#include "adb_shell_screen.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

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
}

void ADBDeviceScreen::onAppear() {
    // Low-rate, agent-free screen preview via `exec:screencap -p`. dst matches the
    // preview box (360 x 360/9*20 = 360x800) for a 1:1 render. Created/torn down on
    // appear/disappear (not enter/exit) so the capture+decode loop and its PSRAM
    // buffers don't keep running behind a pushed sub-screen (Mirroring/Shell/...).
    preview_ = ScreencapPreview::create(preview_image_, 360, 800);
    preview_->start();
}

void ADBDeviceScreen::onDisappear() {
    if (preview_) {
        preview_->stop();
        preview_.reset();
    }
}

void ADBDeviceScreen::createHeader() {
    // ---- Device summary, parsed from the CNXN banner (no live ADB calls) ----
    adb::Client *client = app::adb_client();
    static const std::string kNoBanner;
    const std::string &banner = client ? client->banner() : kNoBanner;
    std::string model = banner_field(banner, "ro.product.model");
    if (model.empty()) model = "ADB Device";
    std::string device = banner_field(banner, "ro.product.device");

    if (header_) lv_obj_delete(header_);
    header_ = lv_obj_create(root_);
    lv_obj_set_size(header_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(header_, 20, 0);
    lv_obj_add_flag(header_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(header_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_fn(header_, LV_EVENT_CLICKED, [](lv_event_t *) {
        // screen_manager.push(std::make_unique<PlaceholderScreen>("Device Info"));
    });
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto label = lv_label_create(header_);
    lv_label_set_text(label, model.c_str());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);

    auto chevron = lv_label_create(header_);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(chevron, lv_color_hex(0xe0e0e0), 0);
}

void ADBDeviceScreen::createPreviewContainer() {
    const int width = 360;
    if (preview_container_) lv_obj_delete(preview_container_);
    preview_container_ = lv_obj_create(control_container_);
    lv_obj_remove_style_all(preview_container_);
    lv_obj_set_size(preview_container_, width, LV_SIZE_CONTENT);
    preview_image_ = lv_image_create(preview_container_);
    lv_obj_set_size(preview_image_, LV_PCT(100), width / 9 * 20);
    lv_obj_set_style_bg_color(preview_image_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(preview_image_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(preview_image_, 12, 0);
}

void ADBDeviceScreen::createToolsContainer() {
    if (tools_container_) lv_obj_delete(tools_container_);
    tools_container_ = lv_obj_create(control_container_);
    lv_obj_remove_style_all(tools_container_);
    lv_obj_set_size(tools_container_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tools_container_, 1);
    lv_obj_set_flex_flow(tools_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tools_container_, 16, 0);

    auto tool_button = [this](const char *icon, const char *title, std::function<void(lv_event_t*)> callback){
        auto button = lv_button_create(tools_container_);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(button, 12, 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, callback);

        auto icon_label = lv_label_create(button);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);

        auto title_label = lv_label_create(button);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
    };
    tool_button(LUCIDE_SMARTPHONE, "Mirroring", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBMirroringScreen>());
    });
    tool_button(LUCIDE_SQUARE_TERMINAL, "Shell", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBShellScreen>());
    });
    tool_button(LUCIDE_FOLDER_CLOSED, "File Manager", [](lv_event_t*){
        screen_manager.push(std::make_shared<ADBFileManagerScreen>());
    });
    tool_button(LUCIDE_LAYOUT_GRID, "Apps", [](lv_event_t*){});
    tool_button(LUCIDE_LOGS, "Logcat", [](lv_event_t*){});
    tool_button(LUCIDE_POWER, "Power Menu", [](lv_event_t*){});
    tool_button(LUCIDE_UNPLUG, "Disconect", [](lv_event_t*){});
}
