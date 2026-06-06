#include "screen_manager.hpp"

void ScreenManager::load(std::unique_ptr<Screen> screen) {
    if (!stack_.empty()) {
        stack_.back()->onDisappear();
        while (!stack_.empty()) {
            stack_.back()->onExit();
            stack_.pop_back();
        }
    }
    push(std::move(screen));
}

void ScreenManager::push(std::unique_ptr<Screen> screen) {
    if (!stack_.empty()) stack_.back()->onDisappear();
    switch_theme(screen->theme());
    if (!screen->root_) {
        screen->root_ = lv_obj_create(NULL);
        screen->build();
    }
    stack_.push_back(std::move(screen));
    stack_.back()->onEnter();
    stack_.back()->onAppear();
    lv_screen_load(stack_.back()->root_);
}

void ScreenManager::pop() {
    if (stack_.size() < 2) return;
    stack_.back()->onDisappear();
    stack_.back()->onExit();
    auto screen = std::move(stack_.back());
    stack_.pop_back();

    switch_theme(stack_.back()->theme());
    stack_.back()->onAppear();
    lv_screen_load(stack_.back()->root_);
}

void ScreenManager::top() {
    if (stack_.size() < 2) return;
    if (stack_.size() > 1) {
        stack_.back()->onDisappear();
        while (stack_.size() > 1) {
            stack_.back()->onExit();
            stack_.pop_back();
        }
    }

    switch_theme(stack_.back()->theme());
    stack_.back()->onAppear();
    lv_screen_load(stack_.back()->root_);
}

void ScreenManager::switch_theme(lv_theme_t *theme) {
    if (theme == current_theme_) return;
    lv_display_set_theme(lv_display_get_default(), theme);
    current_theme_ = theme;
}

ScreenManager screen_manager;
