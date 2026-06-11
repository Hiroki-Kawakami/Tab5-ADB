#pragma once
#include <functional>
#include <string>
#include "screen.hpp"

// Per-app detail + management. Info comes from one `dumpsys package <pkg>`
// exec (version / install times / code path); actions are exec one-shots:
// Launch (monkey), Force stop (am force-stop), Clear data (pm clear, confirm),
// Uninstall (pm uninstall, confirm, pops back on success — the app list
// re-lists in onAppear), and Enable / Disable (pm enable / pm disable-user,
// system apps; Enable is also offered for a disabled user app).
//
// exec completions fire on the adb reader thread and are marshalled to the
// LVGL thread with lv_async_call (self + exited() guards).
class ADBAppDetailScreen : public Screen {
public:
    ADBAppDetailScreen(std::string pkg, bool system_app, bool disabled);

    void build() override;

private:
    std::string pkg_;
    bool system_app_;
    bool disabled_;

    bool info_loaded_ = false;
    std::string version_, installed_, updated_, code_path_;

    lv_obj_t *content_{nullptr};
    lv_obj_t *info_box_{nullptr};
    lv_obj_t *actions_box_{nullptr};

    void load_info();       // dumpsys package -> rebuild_info()
    void rebuild_info();
    void rebuild_actions();
    // Run `cmd` and hand the trimmed output to on_done on the LVGL thread.
    void run_action(const std::string &cmd,
                    std::function<void(bool ok, std::string out)> on_done);
};
