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
// The push source streams the file on the Sync worker thread — read() in
// 16 KB chunks into a MALLOC_CAP_CACHE_ALIGNED buffer (the FATFS/SD fast
// path; see bsp_sd.h) — and bumps an atomic byte counter an lv_timer renders,
// so no per-chunk marshalling. exec/push completions fire on adb threads and
// are marshalled to the LVGL thread with lv_async_call (self + exited()
// guards); load_gen_ drops stale list completions.
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
    bool listed_once_ = false;
    lv_obj_t *list_{nullptr};
    lv_obj_t *user_btn_{nullptr}, *system_btn_{nullptr};

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
    void rebuild();  // render the current filter's packages

    // ---- APK install flow (all LVGL thread unless noted) ----
    void pick_apk();                              // push the SD picker
    void confirm_install(const std::string &path);
    void start_install(const std::string &path);  // open file + push
    void run_pm_install();                        // after the push landed
    void update_progress();                       // lv_timer: render job_->sent
    void close_progress();                        // dialog + timer + job_
};
