#include "apk_preview_screen.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "adb.hpp"
#include "adb_app.hpp"
#include "apk_info.hpp"
#include "modal.hpp"
#include "resources/resources.h"

void ApkPreviewScreen::build() {
    lv_obj_t *content = nullptr;
    app::preview_chrome(this, "APK", &content);

    struct stat st = {};
    if (stat(ref_.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        ref_.size = (uint32_t)st.st_size;
    }

    // Local parse on the LVGL thread: the manifest is a few tens of KB off the
    // SD card, well under a frame's worth of work.
    app::apkinfo::ApkInfo info;
    std::string parse_err;
    bool parsed = app::apkinfo::parse(ref_.path.c_str(), info, parse_err);

    app::preview_header(content, LUCIDE_PACKAGE,
                        info.label.empty() ? ref_.name() : info.label);

    auto info_box = lv_obj_create(content);
    lv_obj_set_size(info_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(info_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info_box, 16, 0);

    if (parsed) {
        app::preview_info_row(info_box, "Package", info.package);
        std::string version = info.version_name;
        if (info.version_code) {
            char code[24];
            snprintf(code, sizeof(code), " (%u)", (unsigned)info.version_code);
            version = (version.empty() ? "?" : version) + code;
        }
        app::preview_info_row(info_box, "Version", version);
        std::string sdk;
        if (info.min_sdk >= 0) sdk += "min " + std::to_string(info.min_sdk);
        if (info.target_sdk >= 0) {
            if (!sdk.empty()) sdk += " / ";
            sdk += "target " + std::to_string(info.target_sdk);
        }
        app::preview_info_row(info_box, "SDK", sdk);
    } else {
        // Not parseable as an APK; the file may still be installable (e.g. a
        // zip feature this parser skips), so only the info card degrades.
        app::preview_info_row(info_box, "Package", "Could not parse the APK.");
    }
    app::preview_info_row(info_box, "File", ref_.name());
    app::preview_info_row(info_box, "Size", app::format_size(ref_.size));
    app::preview_info_row(info_box, "Path", ref_.path);

    auto actions = lv_obj_create(content);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actions, 16, 0);

    adb::Client *client = app::adb_client();
    bool online = client && client->state() == adb::ConnectionState::Online;
    app::preview_action(actions, LUCIDE_PACKAGE_PLUS,
                        online ? "Install" : "Install (not connected)", online,
                        [this](lv_event_t *) { confirm_install(); });
}

void ApkPreviewScreen::confirm_install() {
    char text[160];
    snprintf(text, sizeof(text), "%s (%s) will be installed.",
             ref_.name().c_str(), app::format_size(ref_.size).c_str());
    app::modal_confirm(root_, "Install APK", text, "Install", false, [this]() {
        job_ = app::install_apk(root_, ref_.path, [](bool) {});
    });
}
