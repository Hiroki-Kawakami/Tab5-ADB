#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "adb.hpp"  // adb::Sync, adb::SyncListener
#include "screen.hpp"

// Installed-app manager. Lists the device's packages via one `pm list
// packages` exec round trip (user/system/disabled sections in a single shell
// command), with a User/System filter toggle. Tapping a row opens the app
// detail screen; the nav bar's Install button picks a .apk off the Tab5 SD
// card (SDFileBrowserScreen pick mode) and installs it: Sync::push to
// /data/local/tmp with a progress dialog, then `pm install -r`.
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
class ADBAppManagerScreen : public Screen, public adb::SyncListener {
public:
    void build() override;
    void onAppear() override;  // re-list after returning from detail/install
    void onExit() override;    // abort + tear down an in-flight install

    // adb::SyncListener (the push completion is the authoritative install
    // signal, so session close needs no extra handling).
    void on_sync_close(adb::Sync *s, adb::Error err) override {}

private:
    enum class Filter { User, System };

    // One recycled list row. The widgets are created once (ensure_pool) and
    // rebound while scrolling; data_idx is the bound index into filtered()
    // (-1 = hidden), read by the row's click handler.
    struct Row {
        lv_obj_t *btn;
        lv_obj_t *icon;
        lv_obj_t *name;
        lv_obj_t *tag;  // "disabled" badge, hidden unless bound pkg is disabled
        int data_idx = -1;
    };

    // One APK install in flight. The Sync push source holds the shared_ptr
    // (never the screen), so the worker thread outliving the screen is safe;
    // the dtor releases the OS resources whenever the last ref drops.
    struct InstallJob {
        ~InstallJob();
        int fd = -1;
        size_t total = 0;
        uint8_t *buf = nullptr;  // 16 KB cache-aligned read buffer
        size_t buf_len = 0, buf_off = 0;
        std::atomic<size_t> sent{0};
        std::atomic<bool> abort{false};
        std::shared_ptr<adb::Sync> sync;
    };

    Filter filter_ = Filter::User;
    bool loading_ = false;
    std::string error_;  // non-empty: show this instead of the list
    std::vector<std::string> user_pkgs_, system_pkgs_;
    std::set<std::string> disabled_;
    uint32_t load_gen_ = 0;
    lv_obj_t *list_{nullptr};
    lv_obj_t *user_btn_{nullptr}, *system_btn_{nullptr};

    // Recycler state.
    std::vector<Row> pool_;
    lv_obj_t *extent_{nullptr};  // invisible, height = count*row_h (scroll range)
    lv_obj_t *status_{nullptr};  // spinner / error / empty label
    int first_bound_ = -1;       // pool_[0]'s data index; -1 forces a rebind

    std::shared_ptr<InstallJob> job_;
    lv_obj_t *progress_card_{nullptr};
    lv_obj_t *progress_bar_{nullptr};
    lv_obj_t *progress_label_{nullptr};
    lv_timer_t *progress_timer_{nullptr};

    std::vector<std::string> &filtered() {
        return filter_ == Filter::User ? user_pkgs_ : system_pkgs_;
    }
    void set_filter(Filter f);
    void refresh();  // run pm list via exec, then rebuild
    void rebuild();  // render filter state + status + rebind the row pool
    void ensure_pool();              // create the row widgets once (lazy)
    void bind_row(Row &r, int idx);  // bind one pool row to filtered()[idx]
    void update_rows(bool force);    // rebind the pool to the scroll window

    // ---- APK install flow (all LVGL thread unless noted) ----
    void pick_apk();                              // push the SD picker
    void confirm_install(const std::string &path);
    void start_install(const std::string &path);  // open file + push
    void run_pm_install();                        // after the push landed
    void update_progress();                       // lv_timer: render job_->sent
    void close_progress();                        // dialog + timer + job_
};
