#pragma once
#include <functional>
#include "lvgl.h"

lv_result_t lv_async_call(std::function<void()>);
lv_event_dsc_t *lv_obj_add_event_fn(lv_obj_t*, lv_event_code_t, std::function<void(lv_event_t*)>);
