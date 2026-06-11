#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adb.hpp"  // adb::Shell, adb::ShellListener
#include "device_info.hpp"
#include "screen.hpp"

// Live CPU / memory metrics, pushed from ADBDeviceInfoScreen. Instead of an
// exec per tick, one streaming `shell:` session runs a device-side loop
// (`while true; do echo @@@; ...; sleep 1; done`), so the device's own sleep is
// the tick and there is no per-sample fork + stream-open round trip. Frames are
// delimited by the @@@ marker; CPU % comes from the delta of two consecutive
// /proc/stat samples (computed here — `top` output is too vendor-dependent).
//
// Threading is the Logcat pattern: on_shell_data (reader thread) appends to a
// capped FIFO and coalesces one lv_async_call; the flush frames + parses on the
// LVGL thread only. The stream is closed on onDisappear (USB traffic stops with
// it) and reopened on onAppear with a fresh /proc/stat baseline.
class ADBMetricsScreen : public Screen, public adb::ShellListener {
public:
    void build() override;
    void onAppear() override;
    void onDisappear() override;

    // adb::ShellListener — both fire on the reader thread.
    void on_shell_data(adb::Shell *sh, const uint8_t *data, size_t len) override;
    void on_shell_close(adb::Shell *sh, adb::Error err) override;

private:
    std::shared_ptr<adb::Shell> shell_;

    // reader-thread -> LVGL FIFO
    std::mutex out_mtx_;
    std::string pending_;
    bool flush_scheduled_ = false;
    bool overflow_ = false;
    int expected_closes_ = 0;  // our own onDisappear close vs a dying stream

    // LVGL-thread state
    std::string acc_;  // bytes up to the next complete @@@-delimited frame
    app::devinfo::CpuTimes prev_cpu_;

    // widgets
    lv_obj_t *cpu_pct_label_{nullptr};
    lv_obj_t *cpu_chart_{nullptr};
    lv_chart_series_t *cpu_ser_{nullptr};
    lv_obj_t *cores_box_{nullptr};
    struct CoreRow {
        lv_obj_t *bar;
        lv_obj_t *pct;
    };
    std::vector<CoreRow> core_rows_;
    lv_obj_t *mem_label_{nullptr};
    lv_obj_t *mem_chart_{nullptr};
    lv_chart_series_t *mem_ser_{nullptr};
    lv_obj_t *load_label_{nullptr};
    lv_obj_t *batt_label_{nullptr};

    void start_stream();
    void stop_stream();
    void flush_stream();                          // LVGL thread
    void apply_frame(const std::string &frame);   // LVGL thread
    void ensure_core_rows(size_t n);
};
