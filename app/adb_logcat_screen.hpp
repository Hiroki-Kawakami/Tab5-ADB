#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adb.hpp"  // adb::Shell, adb::ShellListener
#include "screen.hpp"

// Live `logcat` viewer over a streaming `shell:logcat -v threadtime` session,
// with pause/resume, a minimum-level filter, a case-insensitive substring
// filter, and save-to-SD.
//
// Lines live in a PSRAM ring (LogRing): a 2 MB byte pool + a 16 K-entry line
// descriptor ring, oldest-evicted-first. Every line gets a monotonic seq, so
// the filtered view (a deque of seqs) and scroll anchoring survive eviction.
// The ring is owned by the LVGL thread only: on_shell_data (reader thread)
// buffers raw bytes FIFO under out_mtx_ — the ADBShellScreen pattern — and one
// coalesced lv_async_call splits/parses/appends on the LVGL thread.
//
// The list is recycled AppManagerScreen-style (fixed row pool + invisible
// extent + rebind on scroll), and rendering is throttled: appends only mark
// dirty_, a 100 ms lv_timer does the extent/scroll/rebind work, so a logcat
// burst never invalidates per-line. While the view is at the bottom it follows
// the tail; scrolling up detaches (a floating button jumps back to live).
//
// Pause closes the logcat shell (stops the USB traffic too); resume reopens
// with `-T '<last timestamp>'` so the paused span is backfilled (lines in the
// same millisecond may duplicate). Save snapshots the whole ring into a PSRAM
// buffer on the LVGL thread, then a one-shot FreeRTOS task writes it to
// /sd/logcat_NNN.txt (the job owns the buffer, InstallJob-style).
class ADBLogcatScreen : public Screen, public adb::ShellListener {
public:
    ADBLogcatScreen();
    ~ADBLogcatScreen() override;

    void build() override;
    void onExit() override;  // stop the stream + render timer

    // adb::ShellListener — both fire on the reader thread.
    void on_shell_data(adb::Shell *sh, const uint8_t *data, size_t len) override;
    void on_shell_close(adb::Shell *sh, adb::Error err) override;

private:
    // PSRAM line ring: NUL-terminated line texts packed into a byte pool plus
    // a descriptor ring. A line never splits across the pool end (the tail is
    // abandoned on wrap), so a descriptor's text is always one contiguous,
    // directly-displayable string. LVGL thread only.
    class LogRing {
    public:
        struct Line {
            uint32_t seq;
            uint32_t off;   // into the pool; text is NUL-terminated
            uint16_t len;   // text length, terminator excluded
            uint8_t level;  // 0=V 1=D 2=I 3=W 4=E 5=F
        };
        ~LogRing();
        bool init();  // allocate the PSRAM pool + descriptor ring
        // Append one line (truncated to the per-line cap), evicting oldest
        // lines as needed. Returns how many lines were evicted.
        size_t append(const char *text, size_t len, uint8_t level);
        void clear();
        size_t count() const { return count_; }
        uint32_t head_seq() const;  // seq of the oldest line (next_seq if empty)
        const Line &at(size_t idx) const;  // 0 = oldest .. count()-1 = newest
        const char *text(const Line &l) const { return pool_ + l.off; }
        size_t text_bytes() const { return bytes_; }  // sum of stored lengths

    private:
        void evict();
        char *pool_ = nullptr;
        Line *lines_ = nullptr;
        size_t head_ = 0, count_ = 0;
        uint32_t w_ = 0;  // pool write offset
        uint32_t next_seq_ = 0;
        size_t bytes_ = 0;
    };

    // One recycled list row; data_idx is the bound index into filtered_
    // (-1 = hidden), read by the row's click handler.
    struct Row {
        lv_obj_t *btn;
        lv_obj_t *label;
        int data_idx = -1;
    };

    // One SD save in flight. The writer task's closure holds the shared_ptr
    // (never the screen), so the task outliving the screen is safe; the dtor
    // frees the snapshot buffer whenever the last ref drops.
    struct SaveJob {
        ~SaveJob();
        char *buf = nullptr;
        size_t len = 0;
        std::string path;
    };

    // ---- streaming (LVGL thread) ----
    void start_stream();  // open shell:logcat (resumes from last_ts_)
    void pause_stream();
    void toggle_pause();
    void update_pause_icon();
    void flush_stream();  // drain pending_out_: split lines, parse, append
    void push_ring(const char *line, size_t len, uint8_t level);
    void append_line(const char *line, size_t len);
    void append_marker(const char *text);  // synthetic in-stream notice

    // ---- view (LVGL thread) ----
    void ensure_pool();              // create the row widgets once
    void bind_row(Row &r, int idx);  // bind one pool row to filtered_[idx]
    void update_rows(bool force);    // rebind the pool to the scroll window
    void update_view();              // status + extent + scroll + rows
    bool line_matches(const LogRing::Line &l) const;
    void rebuild_filtered();  // full ring re-scan (filter changed)
    void set_min_level(int level);
    void apply_text_filter();
    void show_keyboard();
    void hide_keyboard();
    void clear_logs();
    void save_to_sd();

    std::shared_ptr<adb::Shell> shell_;
    LogRing ring_;
    std::deque<uint32_t> filtered_;  // seqs passing the filter, oldest first
    int min_level_ = 0;              // show level >= this
    std::string filter_lc_;          // lowercased substring filter ("" = off)
    int last_level_ = 2;             // continuation lines inherit (I default)
    std::string last_ts_;            // "MM-DD hh:mm:ss.mmm" of the last parsed line
    bool paused_ = false;
    int expected_closes_ = 0;  // deliberate close()s whose on_shell_close to ignore
    bool follow_tail_ = true;
    size_t pending_evicted_ = 0;  // filtered rows evicted since the last render
    bool dirty_ = false;          // appended since the last render tick
    bool programmatic_scroll_ = false;
    std::string carry_;  // partial line carried between flushes

    lv_obj_t *pause_icon_{nullptr};
    lv_obj_t *level_btns_[5] = {};
    lv_obj_t *filter_ta_{nullptr};
    lv_obj_t *kb_{nullptr};
    lv_obj_t *list_{nullptr};
    lv_obj_t *extent_{nullptr};  // invisible, height = count*row_h (scroll range)
    lv_obj_t *status_{nullptr};  // "No logs." / "No matching logs."
    lv_obj_t *jump_btn_{nullptr};
    lv_timer_t *render_timer_{nullptr};
    std::vector<Row> pool_;
    int first_bound_ = -1;  // pool_[0]'s data index; -1 forces a rebind

    lv_obj_t *save_card_{nullptr};  // saving dialog (also the in-flight guard)

    // Reader-thread FIFO (see ADBShellScreen): bytes append under out_mtx_,
    // one coalesced flush drains on the LVGL thread. Past the cap incoming
    // bytes are dropped and the flush injects an "[output dropped]" marker.
    std::mutex out_mtx_;
    std::string pending_out_;
    bool flush_scheduled_ = false;
    bool overflow_ = false;
    bool close_marker_ = false;
};
