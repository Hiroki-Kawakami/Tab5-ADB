#pragma once
#include <string>
#include <vector>
#include "adb.hpp"
#include "screen.hpp"

class ADBFileManagerScreen : public Screen {
public:
    void build() override;
    void onExit() override;

private:
    std::vector<std::string> external_storages_{};
    lv_obj_t *list_{nullptr};

    void refresh();
};
