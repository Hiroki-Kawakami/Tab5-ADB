#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "lvgl.h"
#include "lucide_font.h"

struct Resources {
    struct {
        const lv_image_dsc_t *hard_drive_40px;
    } icon;
    struct {
        const lv_font_t *lucide_40;
        const lv_font_t *hack_16;  // monospace terminal font (ASCII, 10x17px cells)
    } font;
};
extern const struct Resources R;

#ifdef __cplusplus
} /*extern "C"*/
#endif
