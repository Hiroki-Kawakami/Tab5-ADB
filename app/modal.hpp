#pragma once
#include <functional>
#include "lvgl.h"

// Minimal modal dialogs: a full-screen scrim that absorbs input + a centered
// card. All LVGL-thread only. The scrim/card live on `parent` (a screen's
// root_), so a screen teardown deletes any open dialog with it.
namespace app {

// Scrim + empty card (flex column). Compose custom dialogs on the returned
// card; close with modal_close(card).
lv_obj_t *modal_open(lv_obj_t *parent);
void modal_close(lv_obj_t *card);  // deletes the whole dialog (scrim + card)

// Cancel / OK confirmation. on_ok runs after the dialog closes; destructive
// styles the OK button red. The Cancel button just closes.
void modal_confirm(lv_obj_t *parent, const char *title, const char *text,
                   const char *ok_text, bool destructive,
                   std::function<void()> on_ok);

// Single-OK message.
void modal_message(lv_obj_t *parent, const char *title, const char *text);

}  // namespace app
