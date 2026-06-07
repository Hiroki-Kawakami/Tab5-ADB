#pragma once
#include "screen.hpp"

// Shows information about the connected ADB device: fields parsed from the CNXN
// banner (model / name / device) plus a few props fetched live over a shell:
// stream in the background.
class ADBDeviceScreen : public Screen {
public:
    void build() override;

private:
    lv_obj_t *props_label_ = nullptr;
};
