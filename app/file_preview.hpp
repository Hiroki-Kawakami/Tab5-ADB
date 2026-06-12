#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lvgl.hpp"

class Screen;

namespace app {

// One file as the browsers see it — either on the Tab5 SD card (POSIX path
// under /sd) or on the Android device (sync/shell path). size/mtime come from
// the originating listing (0 = unknown; the SD side stats locally instead).
struct FileRef {
    enum class Where { SD, Android };
    Where where;
    std::string path;  // absolute
    uint32_t size = 0;
    uint32_t mtime = 0;  // epoch seconds

    std::string name() const;  // leaf name
};

// Per-type preview screen for `ref`: an extension-keyed registry picks the
// screen (e.g. ".apk" on the SD card -> APK info + install); anything
// unmatched gets the generic info screen. Never returns null. The browsers
// push the result on file tap; preview is an entry point, not the owner of
// the file actions — those live in file_transfer, so a future context menu
// (long-press) can drive the same actions from a FileRef without a preview.
std::shared_ptr<Screen> make_file_preview(const FileRef &ref);

// "1.2 GB" / "34.5 MB" / "120 KB" / "999 B" — shared by previews and dialogs.
std::string format_size(uint64_t bytes);

// Building blocks shared by the preview screens (the generic one and the
// per-type ones like the APK preview), so they all look alike:
// nav bar + Back ("title"), a flex content column...
void preview_chrome(Screen *screen, const char *title, lv_obj_t **content_out);
// ...an icon + file-name header card...
void preview_header(lv_obj_t *content, const char *icon_glyph, const std::string &name);
// ...grey-key/value rows for an info card...
void preview_info_row(lv_obj_t *box, const char *key, const std::string &value);
// ...and app-detail-style action rows (enabled=false: greyed, no handler).
void preview_action(lv_obj_t *box, const char *icon_glyph, const char *text,
                    bool enabled, std::function<void(lv_event_t *)> cb);

}  // namespace app
