#pragma once
#include <memory>
#include <vector>
#include "screen.hpp"

class ScreenManager {
public:
    void load(std::unique_ptr<Screen>);
    void push(std::unique_ptr<Screen>);
    void pop();
    void top();
    Screen *current_screen();

private:
    lv_theme_t *current_theme_ = nullptr;
    std::vector<std::unique_ptr<Screen>> stack_ = {};

    void switch_theme(lv_theme_t *theme);
};

extern ScreenManager screen_manager;
