#pragma once
#include <memory>
#include <vector>
#include "screen.hpp"

class ScreenManager {
public:
    void load(std::shared_ptr<Screen>);
    void push(std::shared_ptr<Screen>);
    void pop();
    void top();
    Screen *current_screen();

private:
    lv_theme_t *current_theme_ = nullptr;
    std::vector<std::shared_ptr<Screen>> stack_ = {};

    void switch_theme(lv_theme_t *theme);
    // Defer a leaving screen's destruction off the current LVGL event/indev call
    // stack so freeing root_ never deletes the active screen mid-dispatch.
    void retire(std::shared_ptr<Screen> screen);
};

extern ScreenManager screen_manager;
