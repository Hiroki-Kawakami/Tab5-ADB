#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "adb.hpp"
#include "file_transfer.hpp"  // app::TransferJob (the install flow)
#include "screen.hpp"

// Installed-app manager. In Normal mode the listing is one agent GET_APP_LIST
// request (human-readable labels, label-sorted, system/disabled flags) and the
// visible rows lazily fetch their launcher icons via GET_APP_ICON (raw
// ARGB8888, cached in PSRAM for the screen's lifetime); in Limited mode — or
// when the agent path fails — it falls back to the original one-exec `pm list
// packages` round trip (package names only, no icons). A User/System filter
// toggle picks the rendered set. Tapping a row opens the app detail screen;
// the nav bar's Install button picks a .apk off the Tab5 SD card
// (SDFileBrowserScreen pick mode) and installs it via the shared
// app::install_apk flow (file_transfer: Sync::push to /data/local/tmp with a
// progress dialog, then `pm install -r`).
//
// The list is **recycled**, not built per package: a fixed pool of row widgets
// (viewport / row height + spillover) is created once, an invisible extent
// object spans count*row_height to define the scroll range, and the scroll
// handler rebinds the pool to the visible index window (set label text / tag /
// y position — no object churn). A few hundred system packages therefore cost
// the same LVGL work as one screenful. While a listing is in flight the list
// shows a spinner (FileBrowser-style); the output is parsed and sorted on the
// adb reader thread so the LVGL thread only swaps the result vectors in.
//
// exec completions fire on the adb reader thread and are marshalled to the
// LVGL thread with lv_async_call (self + exited() guards); load_gen_ drops
// stale completions when Refresh is tapped faster than the device responds.
class ADBAppManagerScreen : public Screen {
public:
    ~ADBAppManagerScreen() override;  // frees the PSRAM icon cache

    void build() override;
    void onAppear() override;  // re-list after returning from detail/install
    void onExit() override;    // abort an in-flight install

private:
    enum class Filter { User, System };

    // One listed app: the package plus, on the agent path, its human label
    // (empty on the pm fallback / when unknown — the row shows pkg then).
    struct AppEntry {
        std::string pkg;
        std::string label;
        const std::string &display() const { return label.empty() ? pkg : label; }
    };

    // One recycled list row. The widgets are created once (ensure_pool) and
    // rebound while scrolling; data_idx is the bound index into filtered()
    // (-1 = hidden), read by the row's click handler.
    struct Row {
        lv_obj_t *btn;
        lv_obj_t *img;   // fetched launcher icon (shown when cached)
        lv_obj_t *icon;  // placeholder glyph (shown until the icon lands)
        lv_obj_t *name;
        lv_obj_t *tag;  // "disabled" badge, hidden unless bound pkg is disabled
        int data_idx = -1;
    };

    // One cached launcher icon: an lv_image_dsc_t over a PSRAM ARGB8888 buffer.
    // LVGL-thread only; std::map nodes are address-stable, so a bound lv_image
    // can keep pointing at the dsc while the cache grows.
    struct IconEntry {
        lv_image_dsc_t dsc{};
        uint8_t *buf = nullptr;
    };

    Filter filter_ = Filter::User;
    bool loading_ = false;
    std::string error_;  // non-empty: show this instead of the list
    std::vector<AppEntry> user_pkgs_, system_pkgs_;
    std::set<std::string> disabled_;
    uint32_t load_gen_ = 0;
    lv_obj_t *list_{nullptr};
    lv_obj_t *user_btn_{nullptr}, *system_btn_{nullptr};

    // Icon cache + in-flight set (LVGL thread only). Bounded: past the cap new
    // rows just keep the glyph (a per-pkg ~12 KB ARGB8888 in PSRAM).
    static constexpr int kIconPx = 56;
    static constexpr size_t kMaxIconCache = 256;
    static constexpr size_t kMaxIconInflight = 4;
    std::map<std::string, IconEntry> icons_;
    std::set<std::string> icon_pending_;

    // Recycler state.
    std::vector<Row> pool_;
    lv_obj_t *extent_{nullptr};  // invisible, height = count*row_h (scroll range)
    lv_obj_t *status_{nullptr};  // spinner / error / empty label
    int first_bound_ = -1;       // pool_[0]'s data index; -1 forces a rebind

    std::shared_ptr<app::TransferJob> job_;  // in-flight APK install

    std::vector<AppEntry> &filtered() {
        return filter_ == Filter::User ? user_pkgs_ : system_pkgs_;
    }
    void set_filter(Filter f);
    void refresh();                   // list via the agent or the pm fallback
    void refresh_via_pm(uint32_t gen);  // the Limited-mode / fallback exec path
    void rebuild();  // render filter state + status + rebind the row pool
    void ensure_pool();              // create the row widgets once (lazy)
    void bind_row(Row &r, int idx);  // bind one pool row to filtered()[idx]
    void update_rows(bool force);    // rebind the pool to the scroll window
    void pump_icons();               // fetch missing icons for the bound rows
    void fetch_icon(const std::string &pkg);

    // ---- APK install flow (LVGL thread; the transfer is app::install_apk) ----
    void pick_apk();                              // push the SD picker
    void confirm_install(const std::string &path);
    void start_install(const std::string &path);
};
