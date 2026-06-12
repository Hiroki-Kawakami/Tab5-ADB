#pragma once
#include "screen.hpp"

// App settings screen. The body is intentionally empty for now — settings rows
// land later; this gives the nav chrome and the HomeScreen entry point.
class SettingsScreen : public Screen {
public:
    void build() override;

private:
    lv_obj_t *body_ = nullptr;
};
