#include "resources.h"

// MARK: icons
extern const lv_image_dsc_t hard_drive_40px;

// MARK: fonts
extern const lv_font_t lucide_40;
extern const lv_font_t hack_16;

const struct Resources R = {
    .icon = {
        .hard_drive_40px = &hard_drive_40px,
    },
    .font = {
        .lucide_40 = &lucide_40,
        .hack_16 = &hack_16,
    },
};
