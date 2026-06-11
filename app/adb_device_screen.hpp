#pragma once
#include <memory>
#include <string>

#include "screen.hpp"
#include "screencap_preview.hpp"

// Post-connection home for a device. A tappable summary header (device name,
// model / Android version / storage line, battery + network status icons;
// tap -> ADBDeviceInfoScreen) sits above a column of tool buttons. The header
// renders immediately from the CNXN banner, then a chained `exec` fills the
// live fields; while the screen shows, a 10 s timer re-fetches them (battery
// and signal are live data).
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

    // summary header widgets + state
    lv_obj_t *name_label_{nullptr};
    lv_obj_t *sub_label_{nullptr};
    lv_obj_t *batt_icon_{nullptr};
    lv_obj_t *batt_pct_{nullptr};
    lv_obj_t *wifi_icon_{nullptr};
    lv_obj_t *cell_icon_{nullptr};
    lv_timer_t *summary_timer_{nullptr};
    bool summary_inflight_{false};
    std::string model_;  // banner model, the pre-exec fallback

    void createHeader();
    void refreshSummary();
    void createPreviewContainer();
    void createToolsContainer();
};
