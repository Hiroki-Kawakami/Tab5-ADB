#include "screen_manager.hpp"

#include <utility>

void ScreenManager::retire(std::shared_ptr<Screen> screen) {
    // Defer the leaving screen's destruction out of the current LVGL call stack.
    // pop()/load()/top() typically run inside the very event that triggered the
    // navigation (a Back button's CLICKED handler lives in the screen being
    // destroyed). Freeing root_ there would delete the event's own target +
    // ancestor chain mid-dispatch and leave the indev pointing at freed objects.
    // Releasing on the next lv_timer_handler pass — after the replacement screen
    // is already the active one — frees it safely on the LVGL thread.
    lv_async_call([screen = std::move(screen)]() mutable { screen.reset(); });
}

void ScreenManager::load(std::shared_ptr<Screen> screen) {
    // Tear the whole stack down, then start fresh. Keep the leaving screens alive
    // until the replacement is loaded so the active screen is never deleted while
    // it is still the active one (see retire()).
    std::vector<std::shared_ptr<Screen>> leaving;
    if (!stack_.empty()) {
        stack_.back()->onDisappear();
        while (!stack_.empty()) {
            stack_.back()->exited_ = true;
            stack_.back()->onExit();
            leaving.push_back(std::move(stack_.back()));
            stack_.pop_back();
        }
    }
    push(std::move(screen));  // builds + loads the new screen as the active one
    for (auto &s : leaving) retire(std::move(s));
}

void ScreenManager::push(std::shared_ptr<Screen> screen) {
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
    stack_.back()->exited_ = true;
    stack_.back()->onExit();
    std::shared_ptr<Screen> leaving = std::move(stack_.back());
    stack_.pop_back();

    Screen *next = stack_.back().get();
    switch_theme(next->theme());
    next->onAppear();
    lv_screen_load(next->root_);  // activate the replacement before freeing `leaving`

    retire(std::move(leaving));
}

void ScreenManager::top() {
    if (stack_.size() < 2) return;
    stack_.back()->onDisappear();
    std::vector<std::shared_ptr<Screen>> leaving;
    while (stack_.size() > 1) {
        stack_.back()->exited_ = true;
        stack_.back()->onExit();
        leaving.push_back(std::move(stack_.back()));
        stack_.pop_back();
    }

    Screen *next = stack_.back().get();
    switch_theme(next->theme());
    next->onAppear();
    lv_screen_load(next->root_);  // activate the replacement before freeing `leaving`

    for (auto &s : leaving) retire(std::move(s));
}

void ScreenManager::switch_theme(lv_theme_t *theme) {
    if (theme == current_theme_) return;
    lv_display_set_theme(lv_display_get_default(), theme);
    current_theme_ = theme;
}

ScreenManager screen_manager;
