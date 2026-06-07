#pragma once
#include <memory>
#include "lvgl.hpp"

class Screen : public std::enable_shared_from_this<Screen> {
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

    bool exited() const { return exited_; }

private:
    friend class ScreenManager;
    bool exited_ = false;
};
