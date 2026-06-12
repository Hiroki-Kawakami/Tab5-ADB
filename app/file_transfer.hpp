#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lvgl.h"

// File transfer jobs between the Android device and the Tab5 SD card, plus the
// APK install flow built on the same machinery. Each job owns its own
// adb::Sync session (a pull abort closes the session — see docs/sync.md — so
// jobs never borrow a browser's), a progress modal with Cancel on the
// initiating screen's root, and the overwrite-confirm step. Screen-agnostic on
// purpose: any caller with a path (a preview screen today, a context menu
// later) starts a transfer the same way.
namespace app {

// A running transfer. The caller keeps the shared_ptr and calls abort() from
// onExit() — the modal dies with the screen, and the job notices (it watches
// its card's LV_EVENT_DELETE) and finishes quietly.
class TransferJob {
public:
    virtual ~TransferJob() = default;
    virtual void abort() = 0;
};

// All three run on the LVGL thread and report completion there too:
// on_done(true) only when the transfer (and for install, `pm install`)
// succeeded; declined overwrite / cancel / failure all report false. Result
// dialogs are the job's own (the caller typically just refreshes).

// Android -> Tab5 SD card: `remote_path`'s content lands in `local_dir` under
// the same leaf name (written as "<name>.part", renamed on success). `size` is
// the listing's st_size (progress denominator).
std::shared_ptr<TransferJob> pull_to_sd(lv_obj_t* parent, std::string remote_path,
                                        uint32_t size, std::string local_dir,
                                        std::function<void(bool)> on_done);

// Tab5 SD card -> Android: `local_path`'s content lands in `remote_dir` under
// the same leaf name (perm 0644, mtime preserved).
std::shared_ptr<TransferJob> push_to_android(lv_obj_t* parent, std::string local_path,
                                             std::string remote_dir,
                                             std::function<void(bool)> on_done);

// Tab5 SD card -> `pm install -r`: push to /data/local/tmp, install, rm the
// temp. Confirmation is the caller's (this starts pushing at once).
std::shared_ptr<TransferJob> install_apk(lv_obj_t* parent, std::string local_path,
                                         std::function<void(bool)> on_done);

}  // namespace app
