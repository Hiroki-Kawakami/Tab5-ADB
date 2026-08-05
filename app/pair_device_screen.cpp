#include "pair_device_screen.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include "adb_app.hpp"
#include "modal.hpp"
#include "screen_manager.hpp"

namespace {

const char *const kAddressKeyboardMap[] = {
    " ", "1", "2", "3", " ", "\n",
    " ", "4", "5", "6", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_LEFT, "7", "8", "9", LV_SYMBOL_RIGHT, "\n",
    LV_SYMBOL_CLOSE, ":", "0", ".", LV_SYMBOL_OK, "",
};

const char *const kCodeKeyboardMap[] = {
    " ", "1", "2", "3", " ", "\n",
    " ", "4", "5", "6", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_LEFT, "7", "8", "9", LV_SYMBOL_RIGHT, "\n",
    LV_SYMBOL_CLOSE, " ", "0", " ", LV_SYMBOL_OK, "",
};

constexpr lv_buttonmatrix_ctrl_t kKeyW1 =
    LV_BUTTONMATRIX_CTRL_WIDTH_1;
constexpr lv_buttonmatrix_ctrl_t kActionW1 =
    static_cast<lv_buttonmatrix_ctrl_t>(
        LV_KEYBOARD_CTRL_BUTTON_FLAGS |
        LV_BUTTONMATRIX_CTRL_WIDTH_1);
constexpr lv_buttonmatrix_ctrl_t kHideW1 =
    static_cast<lv_buttonmatrix_ctrl_t>(
        LV_BUTTONMATRIX_CTRL_HIDDEN |
        LV_BUTTONMATRIX_CTRL_WIDTH_1);

const lv_buttonmatrix_ctrl_t kAddressKeyboardCtrlMap[] = {
    kHideW1, kKeyW1, kKeyW1, kKeyW1, kHideW1,
    kHideW1, kKeyW1, kKeyW1, kKeyW1, kKeyW1,
    kKeyW1, kKeyW1, kKeyW1, kKeyW1, kKeyW1,
    kActionW1, kKeyW1, kKeyW1, kKeyW1, kActionW1,
};

const lv_buttonmatrix_ctrl_t kCodeKeyboardCtrlMap[] = {
    kHideW1, kKeyW1, kKeyW1, kKeyW1, kHideW1,
    kHideW1, kKeyW1, kKeyW1, kKeyW1, kKeyW1,
    kKeyW1, kKeyW1, kKeyW1, kKeyW1, kKeyW1,
    kActionW1, kHideW1, kKeyW1, kHideW1, kActionW1,
};

bool parse_pair_target(const std::string &value,
                       std::string &host, uint16_t &port) {
    std::string target = value;
    while (!target.empty() &&
           (target.front() == ' ' || target.front() == '\t')) {
        target.erase(target.begin());
    }
    while (!target.empty() &&
           (target.back() == ' ' || target.back() == '\t')) {
        target.pop_back();
    }
    size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 == target.size()) {
        return false;
    }
    host = target.substr(0, colon);
    uint32_t parsed_port = 0;
    for (size_t i = colon + 1; i < target.size(); ++i) {
        char c = target[i];
        if (c < '0' || c > '9') {
            return false;
        }
        parsed_port = parsed_port * 10 +
                      static_cast<uint32_t>(c - '0');
        if (parsed_port > 65535) {
            return false;
        }
    }
    if (parsed_port == 0) {
        return false;
    }
    port = static_cast<uint16_t>(parsed_port);
    return true;
}

bool valid_pairing_code(const std::string &code) {
    if (code.size() != 6) {
        return false;
    }
    for (char c : code) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

const char *pairing_error_message(adb::PairingError error) {
    switch (error) {
        case adb::PairingError::InvalidTarget:
            return "Enter the pairing address shown on Android as host:port.";
        case adb::PairingError::InvalidCode:
            return "Enter the current six-digit pairing code.";
        case adb::PairingError::Connect:
            return "Could not reach the pairing address. Check that both devices "
                   "are on the same Wi-Fi network.";
        case adb::PairingError::Timeout:
            return "Pairing timed out. Open a new pairing-code dialog on Android "
                   "and try again.";
        case adb::PairingError::Tls:
        case adb::PairingError::KeyExport:
            return "The secure pairing connection could not be established.";
        case adb::PairingError::Authentication:
            return "The pairing code was rejected. Generate a new code and try "
                   "again.";
        case adb::PairingError::Protocol:
            return "Android rejected the pairing exchange. Generate a new code "
                   "and try again.";
        case adb::PairingError::Crypto:
            return "The ADB identity or pairing encryption could not be prepared.";
        case adb::PairingError::Ok:
            return "";
    }
    return "Pairing failed.";
}

lv_obj_t *make_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void style_field(lv_obj_t *field) {
    lv_textarea_set_one_line(field, true);
    lv_obj_set_size(field, LV_PCT(100), 72);
    lv_obj_set_style_text_font(field, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_ver(field, 18, 0);
}

}  // namespace

void PairDeviceScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *navigation = lv_obj_create(root_);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(navigation, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(
        navigation, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(navigation, 1, 0);
    lv_obj_set_style_border_color(
        navigation, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        navigation, LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(navigation, 24, 0);
    lv_obj_set_style_pad_column(navigation, 24, 0);

    lv_obj_t *back = lv_button_create(navigation);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        back, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED,
                        [](lv_event_t *) { screen_manager.pop(); });
    lv_obj_set_style_bg_color(
        back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);

    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(
        back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);

    lv_obj_t *title_label = lv_label_create(back);
    lv_label_set_text(title_label, "Pair Device");
    lv_obj_set_style_text_font(
        title_label, &lv_font_montserrat_38, 0);

    lv_obj_t *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 28, 0);
    lv_obj_set_style_pad_row(body, 22, 0);

    lv_obj_t *instructions = make_card(body);
    lv_obj_t *instructions_title = lv_label_create(instructions);
    lv_label_set_text(
        instructions_title, "Pair using a six-digit code");
    lv_obj_set_style_text_font(
        instructions_title, &lv_font_montserrat_28, 0);

    lv_obj_t *instructions_text = lv_label_create(instructions);
    lv_obj_set_width(instructions_text, LV_PCT(100));
    lv_label_set_long_mode(
        instructions_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        instructions_text,
        "On Android, open Developer options > Wireless debugging > "
        "Pair device with pairing code. Enter the pairing address and "
        "code shown there.");
    lv_obj_set_style_text_font(
        instructions_text, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(
        instructions_text, lv_color_hex(0x455a64), 0);

    lv_obj_t *host_name = lv_label_create(instructions);
    lv_label_set_text(host_name, app::adb_host_name().c_str());
    lv_obj_set_style_text_font(
        host_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(
        host_name, lv_color_hex(0x1565c0), 0);

    wifi_status_ = lv_label_create(instructions);
    lv_obj_set_width(wifi_status_, LV_PCT(100));
    lv_label_set_long_mode(wifi_status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(
        wifi_status_, &lv_font_montserrat_18, 0);

    lv_obj_t *form = make_card(body);

    lv_obj_t *address_label = lv_label_create(form);
    lv_label_set_text(address_label, "Pairing address");
    lv_obj_set_style_text_font(
        address_label, &lv_font_montserrat_20, 0);

    address_ = lv_textarea_create(form);
    style_field(address_);
    lv_textarea_set_placeholder_text(
        address_, "192.168.x.x:xxxxx");
    lv_textarea_set_accepted_chars(address_, "0123456789.:");
    lv_textarea_set_max_length(address_, 63);

    lv_obj_t *code_label = lv_label_create(form);
    lv_label_set_text(code_label, "Six-digit code");
    lv_obj_set_style_text_font(
        code_label, &lv_font_montserrat_20, 0);

    code_ = lv_textarea_create(form);
    style_field(code_);
    lv_textarea_set_placeholder_text(code_, "Pairing code");
    lv_textarea_set_accepted_chars(code_, "0123456789");
    lv_textarea_set_max_length(code_, 6);
    lv_obj_set_style_text_align(code_, LV_TEXT_ALIGN_CENTER, 0);

    auto address_focus = [this](lv_event_t *) {
        show_keyboard(address_, false);
    };
    lv_obj_add_event_fn(
        address_, LV_EVENT_FOCUSED, address_focus);
    lv_obj_add_event_fn(
        address_, LV_EVENT_CLICKED, address_focus);

    auto code_focus = [this](lv_event_t *) {
        show_keyboard(code_, true);
    };
    lv_obj_add_event_fn(code_, LV_EVENT_FOCUSED, code_focus);
    lv_obj_add_event_fn(code_, LV_EVENT_CLICKED, code_focus);

    pair_button_ = lv_button_create(form);
    lv_obj_set_size(pair_button_, LV_PCT(100), 84);
    lv_obj_set_style_radius(pair_button_, 12, 0);
    lv_obj_set_style_opa(
        pair_button_, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *pair_label = lv_label_create(pair_button_);
    lv_label_set_text(pair_label, "Pair");
    lv_obj_set_style_text_font(
        pair_label, &lv_font_montserrat_24, 0);
    lv_obj_center(pair_label);
    lv_obj_add_event_fn(
        pair_button_, LV_EVENT_CLICKED,
        [this](lv_event_t *) { start_pairing(); });

    result_label_ = lv_label_create(form);
    lv_obj_set_width(result_label_, LV_PCT(100));
    lv_label_set_long_mode(result_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(
        result_label_, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(
        result_label_, lv_color_hex(0x607d8b), 0);
    lv_label_set_text(
        result_label_,
        "The pairing port is different from the connection port "
        "shown on the main Wireless debugging screen.");

    update_controls();
}

void PairDeviceScreen::onEnter() {
    std::weak_ptr<wifi::Listener> listener(
        std::shared_ptr<wifi::Listener>(
            shared_from_this(),
            static_cast<wifi::Listener *>(this)));
    wifi::manager().set_listener(listener);
}

void PairDeviceScreen::onAppear() {
    update_controls();
}

void PairDeviceScreen::on_wifi_state(
    const wifi::Status & /*status*/) {
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) {
            return;
        }
        update_controls();
    });
}

void PairDeviceScreen::show_keyboard(
    lv_obj_t *textarea, bool code_mode) {
    if (!keyboard_) {
        constexpr int kKeyboardHeight = 400;
        keyboard_ = lv_keyboard_create(root_);
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(
            keyboard_, PANEL_W, kKeyboardHeight);
        lv_obj_align(
            keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);

        lv_obj_add_event_fn(
            keyboard_, LV_EVENT_READY,
            [this](lv_event_t *) {
                lv_obj_t *target = keyboard_target_;
                lv_async_call([
                    self = shared_from_this(), this, target
                ]() {
                    if (exited()) {
                        return;
                    }
                    if (target == address_) {
                        show_keyboard(code_, true);
                    } else {
                        start_pairing();
                    }
                });
            });
        lv_obj_add_event_fn(
            keyboard_, LV_EVENT_CANCEL,
            [this](lv_event_t *) {
                lv_async_call([
                    self = shared_from_this(), this
                ]() {
                    if (!exited()) {
                        hide_keyboard();
                    }
                });
            });
    }

    keyboard_target_ = textarea;
    lv_keyboard_set_map(
        keyboard_, LV_KEYBOARD_MODE_USER_1,
        code_mode ? kCodeKeyboardMap : kAddressKeyboardMap,
        code_mode ? kCodeKeyboardCtrlMap :
                    kAddressKeyboardCtrlMap);
    lv_keyboard_set_mode(
        keyboard_, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_textarea(keyboard_, textarea);
    lv_obj_move_foreground(keyboard_);
}

void PairDeviceScreen::hide_keyboard() {
    if (!keyboard_) {
        return;
    }
    lv_obj_delete(keyboard_);
    keyboard_ = nullptr;
    keyboard_target_ = nullptr;
}

void PairDeviceScreen::start_pairing() {
    if (busy_) {
        return;
    }
    if (wifi::manager().status().state !=
        wifi::State::Connected) {
        app::modal_message(
            root_, "Wi-Fi required",
            "Connect Tab5 to the same Wi-Fi network as the "
            "Android device first.");
        return;
    }

    std::string host;
    uint16_t port = 0;
    if (!parse_pair_target(
            lv_textarea_get_text(address_), host, port)) {
        app::modal_message(
            root_, "Invalid pairing address",
            "Enter the address shown in Android's pairing-code "
            "dialog as host:port.");
        return;
    }

    std::string code = lv_textarea_get_text(code_);
    if (!valid_pairing_code(code)) {
        app::modal_message(
            root_, "Invalid pairing code",
            "Enter all six digits from Android's pairing-code dialog.");
        return;
    }

    hide_keyboard();
    lv_textarea_set_text(code_, "");
    busy_ = true;
    update_controls();
    open_progress();

    app::adb_pair_async(
        host, port, code,
        [self = shared_from_this(), this](
            adb::PairingResult result) {
            if (exited()) {
                return;
            }
            busy_ = false;
            close_progress();
            update_controls();

            if (result) {
                lv_label_set_text(
                    result_label_,
                    "Paired successfully. Return to Home and enter "
                    "the connection address from Android's main "
                    "Wireless debugging screen.");
                lv_obj_set_style_text_color(
                    result_label_, lv_color_hex(0x2e7d32), 0);
                app::modal_message(
                    root_, "Pairing complete",
                    "The ADB key is now authorized. Use the separate "
                    "connection address shown on Android to connect.");
                return;
            }

            const char *message =
                pairing_error_message(result.error);
            lv_label_set_text(result_label_, message);
            lv_obj_set_style_text_color(
                result_label_, lv_color_hex(0xc62828), 0);
            app::modal_message(
                root_, "Pairing failed", message);
        });
}

void PairDeviceScreen::update_controls() {
    wifi::Status status = wifi::manager().status();
    bool connected =
        status.state == wifi::State::Connected;

    if (wifi_status_) {
        if (connected) {
            lv_label_set_text_fmt(
                wifi_status_, "Wi-Fi: connected to %s",
                status.ssid.c_str());
            lv_obj_set_style_text_color(
                wifi_status_, lv_color_hex(0x2e7d32), 0);
        } else if (status.state == wifi::State::Connecting) {
            lv_label_set_text(
                wifi_status_, "Wi-Fi is connecting...");
            lv_obj_set_style_text_color(
                wifi_status_, lv_color_hex(0x607d8b), 0);
        } else {
            lv_label_set_text(
                wifi_status_,
                "Wi-Fi is not connected. Return to Settings first.");
            lv_obj_set_style_text_color(
                wifi_status_, lv_color_hex(0xc62828), 0);
        }
    }

    bool enabled = connected && !busy_;
    for (lv_obj_t *object :
         {address_, code_, pair_button_}) {
        if (!object) {
            continue;
        }
        if (enabled) {
            lv_obj_remove_state(object, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(object, LV_STATE_DISABLED);
        }
    }
}

void PairDeviceScreen::open_progress() {
    progress_card_ = app::modal_open(root_);

    lv_obj_t *header = lv_obj_create(progress_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(
        header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        header, LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);

    lv_obj_t *spinner = lv_spinner_create(header);
    lv_obj_set_size(spinner, 64, 64);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Pairing...");
    lv_obj_set_style_text_font(
        title, &lv_font_montserrat_28, 0);

    lv_obj_t *message = lv_label_create(progress_card_);
    lv_obj_set_width(message, LV_PCT(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        message,
        "Keep the pairing-code dialog open on Android.");
    lv_obj_set_style_text_font(
        message, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(
        message, lv_color_hex(0x455a64), 0);
}

void PairDeviceScreen::close_progress() {
    if (progress_card_) {
        app::modal_close(progress_card_);
        progress_card_ = nullptr;
    }
}
