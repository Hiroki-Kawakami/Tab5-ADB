#pragma once
#include <functional>
#include <string>
#include <vector>
#include "screen.hpp"

// Tab5 SD card file browser — the local (POSIX dirent) counterpart of
// ADBFileBrowserScreen. Mounts the card on demand when built (already-mounted
// is fine) and scans directories synchronously on the LVGL thread; the card
// stays mounted across screens (no hot-plug detection — Refresh re-mounts
// after a failure).
//
// Two modes:
//  - browse (default ctor): read-only navigation, plain files are inert.
//  - pick: construct with a Pick config — files matching `ext` are tappable;
//    tapping one pops this screen and then calls on_pick(absolute_path) on the
//    LVGL thread, with the caller's screen on top again.
class SDFileBrowserScreen : public Screen {
public:
    struct Pick {
        std::string ext;  // case-insensitive extension filter, e.g. ".apk"
        std::function<void(const std::string &path)> on_pick;
    };

    SDFileBrowserScreen() = default;
    explicit SDFileBrowserScreen(Pick pick);

    void build() override;

private:
    struct Entry {
        std::string name;
        bool dir;
    };

    Pick pick_{};
    bool picking_ = false;
    std::vector<std::string> path_stack_;  // directory names below the SD root
    std::vector<Entry> entries_;
    std::string error_;  // non-empty: show this instead of entries
    lv_obj_t *title_label_{nullptr};
    lv_obj_t *list_{nullptr};

    std::string current_path() const;
    bool pickable(const Entry &e) const;
    void open(const std::string &name);  // descend into a child directory
    void back();                         // up a level / pop screen at the root
    void load();                         // mount if needed + scan current_path()
    void rebuild();                      // render entries_/error_
};
