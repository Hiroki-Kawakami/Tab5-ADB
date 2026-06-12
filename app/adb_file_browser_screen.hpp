#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "adb.hpp"  // adb::Sync, adb::SyncListener, adb::DirEntry, adb::Error
#include "screen.hpp"

// Android file browser. Opens a `sync:` session on the app's adb::Client and
// lists directories into a scrollable list; tapping a folder navigates into it
// (pushing onto directory_stack_), and the nav Back button goes up a level —
// or pops the screen when at the stack root.
//
// Two modes:
//  - browse (path-only ctor): a plain file opens its preview screen.
//  - pick-dir: construct with a PickDir config — files are inert/greyed, and a
//    nav-bar button (`label`, e.g. "Copy Here") pops this screen and calls
//    on_pick(absolute_dir) for the directory being shown, on the LVGL thread
//    with the caller's screen on top again. Back at the root pops without the
//    callback (= cancel).
//
// The screen IS the adb::SyncListener. Sync op completions fire on the Sync
// worker thread, so every UI update is marshalled to the LVGL thread with
// lv_async_call. Each marshalling lambda captures `self = shared_from_this()`
// (keeping the screen alive until it drains on the LVGL thread) and skips the
// update when the base Screen::exited() flag is set, so updates queued before
// teardown never touch freed widgets. A generation counter bumped on every
// navigation drops stale list completions (fast taps / superseded loads).
class ADBFileBrowserScreen : public Screen, public adb::SyncListener {
public:
    struct PickDir {
        std::string label;  // the confirm button's text
        std::function<void(const std::string &dir)> on_pick;
    };

    ADBFileBrowserScreen(std::string path);
    ADBFileBrowserScreen(std::string path, PickDir pick);
    ~ADBFileBrowserScreen() override;

    void build() override;
    void onExit() override;  // close() the session before destruction

    // adb::SyncListener — fires on the Sync worker thread.
    void on_sync_close(adb::Sync *s, adb::Error err) override;

private:
    struct Directory {
        std::string path;
        std::vector<adb::DirEntry> entries;
        bool loading;
        std::string error;  // non-empty: show this instead of entries
    };

    std::shared_ptr<adb::Sync> sync_;
    PickDir pick_dir_{};
    bool picking_dir_ = false;
    std::vector<Directory> directory_stack_;
    uint32_t nav_gen_ = 0;  // bumped per navigation; stale completions are dropped
    lv_obj_t *title_label_{nullptr};
    lv_obj_t *list_{nullptr};

    Directory &current_directory() { return directory_stack_.back(); }
    void open(const std::string &path);  // LVGL thread: descend into `path`
    void back();                          // LVGL thread: up a level / pop screen
    void load();                          // LVGL thread: list current_directory()
    void rebuild();                       // LVGL thread: render current_directory()
};
