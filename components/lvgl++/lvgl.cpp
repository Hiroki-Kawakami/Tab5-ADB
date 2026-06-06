#include "lvgl.hpp"

static void async_fn_cb(void *arg) {
    auto *fn = static_cast<std::function<void()>*>(arg);
    (*fn)();
    delete fn;
}
lv_result_t lv_async_call(std::function<void()> fn) {
    auto *fn_ptr = new std::function<void()>(std::move(fn));
    auto result = lv_async_call(async_fn_cb, fn_ptr);
    if (result != LV_RESULT_OK) delete fn_ptr;
    return result;
}

static void event_fn_cb(lv_event_t *e) {
    auto *fn = static_cast<std::function<void(lv_event_t*)>*>(lv_event_get_user_data(e));
    (*fn)(e);
}
lv_event_dsc_t *lv_obj_add_event_fn(lv_obj_t *obj, lv_event_code_t filter, std::function<void(lv_event_t*)> fn) {
    auto *fn_ptr = new std::function<void(lv_event_t*)>(std::move(fn));

    lv_obj_add_event_cb(obj, [](lv_event_t *e) {
        delete static_cast<std::function<void(lv_event_t*)>*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, fn_ptr);

    return lv_obj_add_event_cb(obj, event_fn_cb, filter, fn_ptr);
}
