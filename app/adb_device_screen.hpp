#pragma once
#include <memory>

#include "screen.hpp"
#include "screencap_preview.hpp"

// Post-connection home for a device. A tappable summary header (model / status,
// tap -> detail) sits above a column of tool buttons. Mirroring, Shell, and File
// Manager open real screens; the rest (Apps / Logcat / Power / Disconnect) are
// still placeholders. Device fields come from the CNXN banner (no live ADB calls).
class ADBDeviceScreen : public Screen {
public:
    void build() override;
    void onAppear() override;
    void onDisappear() override;

private:
    std::shared_ptr<ScreencapPreview> preview_;

    lv_obj_t *header_{nullptr};
    lv_obj_t *control_container_{nullptr};
    lv_obj_t *preview_container_{nullptr};
    lv_obj_t *preview_image_{nullptr};
    lv_obj_t *tools_container_{nullptr};

    void createHeader();
    void createPreviewContainer();
    void createToolsContainer();
};
