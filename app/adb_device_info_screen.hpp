#pragma once
#include "screen.hpp"

struct DeviceInfoData;  // parsed snapshot, defined in the .cpp

// Device detail, pushed from the ADBDeviceScreen summary header. One chained
// `exec` round trip fills everything: featured cards (SoC / Memory / Storage /
// Battery / Network / System), a Performance Metrics button (-> ADBMetricsScreen),
// and a key-value list of the miscellaneous fields. Items that fail to parse on
// this device render as "-" or are skipped — never an error screen.
class ADBDeviceInfoScreen : public Screen {
public:
    void build() override;

private:
    lv_obj_t *content_{nullptr};
    lv_obj_t *info_box_{nullptr};  // spinner, then cards + metrics + misc

    void load_info();
    void rebuild(const DeviceInfoData &d);
};
