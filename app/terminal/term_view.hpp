#pragma once
#include "lvgl.h"
#include "term_emu.hpp"

// TermView — LVGL widget rendering a term::TermEmu cell grid. A plain lv_obj
// with an LV_EVENT_DRAW_MAIN handler: per visible row it draws background runs
// (lv_draw_fill) and text runs grouped by color/attrs (lv_draw_label), so no
// textarea/label relayout cost and full per-cell fg/bg control. Light-mode
// 256-color palette; bold maps to the bright variant, dim blends toward the
// background.
//
// Everything runs on the LVGL thread. The owner feeds the emulator, then calls
// refresh() to turn the emulator's dirty rows into partial invalidations (the
// draw handler only walks rows intersecting the clip area). The widget owns
// the cursor blink timer and the swipe-to-scrollback gesture; key input should
// call snap_to_live() so the view jumps back to the live screen.
class TermView {
public:
    // Geometry: the widget is w x h at (x, y) in parent; the grid is centered
    // (cell = 10x17px for hack_16, so emu should be (w/10) x (h/17)).
    TermView(lv_obj_t *parent, term::TermEmu *emu, int x, int y, int w, int h);
    ~TermView();

    lv_obj_t *obj() const { return obj_; }

    // Cell metrics of the terminal font (the screen sizes the TermEmu grid
    // from these): LVGL's per-glyph rounded advance and the line height.
    static int cell_w();
    static int cell_h();

    // Drain emu->take_dirty() into invalidations; track cursor moves and
    // scrollback growth (keeps the viewport pinned while scrolled back).
    void refresh();

    void snap_to_live();  // reset scrollback offset (call on key input)
    bool scrolled_back() const { return view_offset_ > 0; }

private:
    static void draw_main_cb(lv_event_t *e);
    static void pressing_cb(lv_event_t *e);
    static void released_cb(lv_event_t *e);
    static void blink_timer_cb(lv_timer_t *t);

    void draw(lv_layer_t *layer);
    void draw_row(lv_layer_t *layer, int vrow, const term::Cell *cells);
    void invalidate_rows(uint64_t mask);  // view-row bit mask
    void invalidate_cell(int vrow, int col);
    void set_view_offset(int offset);
    lv_area_t row_area(int vrow) const;   // absolute coords
    int grid_x0() const;                  // absolute coords of cell (0,0)
    int grid_y0() const;

    term::TermEmu *emu_;
    lv_obj_t *obj_ = nullptr;
    lv_timer_t *blink_timer_ = nullptr;

    int view_offset_ = 0;     // scrollback lines above the live screen
    int prev_sb_used_ = 0;
    int drag_acc_px_ = 0;     // accumulated swipe delta not yet a whole line
    bool blink_on_ = true;
    // last drawn cursor state, to invalidate the old cell when it moves
    int last_cur_row_ = 0, last_cur_col_ = 0;
    bool last_cur_drawn_ = false;
};
