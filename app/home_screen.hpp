#pragma once
#include "screen.hpp"

class HomeScreen : public Screen {
public:
    void build() override;

private:
    lv_obj_t *connect_btn_ = nullptr;
    lv_obj_t *status_label_ = nullptr;

    void build_hero();
    void build_body();
    void start_connect();
};
