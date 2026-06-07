#pragma once
#include "screen.hpp"

// Post-connection home for a device. A tappable summary header (model / status,
// tap -> detail) sits above a grid of tool buttons (Mirror / Capture / Files /
// Shell / Apps / Logcat / Power / Settings), each opening its own tool screen.
// This is a UI draft: only Shell is wired to a real screen; the rest open a
// placeholder. Device fields come from the CNXN banner (no live ADB calls yet).
class ADBDeviceScreen : public Screen {
public:
    void build() override;

private:
    lv_obj_t *header_{nullptr};
    lv_obj_t *control_container_{nullptr};
    lv_obj_t *preview_container_{nullptr};
    lv_obj_t *preview_image_{nullptr};
    lv_obj_t *tools_container_{nullptr};

    void createHeader();
    void createPreviewContainer();
    void createToolsContainer();
};
