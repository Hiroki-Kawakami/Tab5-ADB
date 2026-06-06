#pragma once
#include "lvgl.hpp"

class Screen {
public:
    lv_obj_t *root_;

    Screen(): root_{nullptr} {}
    virtual ~Screen() { lv_obj_delete(root_); }

    // Build Screen Components
    virtual lv_theme_t *theme() { return lv_theme_default_get(); }
    virtual void build() = 0;

    // Lifecycle Events
    virtual void onEnter()  {}
    virtual void onExit()   {}
    virtual void onAppear()  {}
    virtual void onDisappear() {}
};
