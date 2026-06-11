#include "term_view.hpp"

#include <cstring>

#include "resources/resources.h"

namespace {

// Light-mode theme: white terminal on the FileManager-style light screens.
const lv_color_t kDefaultBg = lv_color_hex(0xffffff);
const lv_color_t kDefaultFg = lv_color_hex(0x202428);

// xterm 256-color palette tuned for a white background: the 16 base colors are
// Tango-ish but darkened where the standard values would vanish on white
// (7/15 "white" become grays, bright yellow is dimmed).
const uint32_t kBase16[16] = {
    0x000000, 0xcc0000, 0x4e9a06, 0xc4a000, 0x3465a4, 0x75507b, 0x06989a,
    0x999999, 0x555753, 0xef2929, 0x73d216, 0xedd400, 0x729fcf, 0xad7fa8,
    0x2cc7c7, 0xb0b0b0,
};

lv_color_t palette(uint8_t idx) {
    if (idx < 16) return lv_color_hex(kBase16[idx]);
    if (idx < 232) {  // 6x6x6 cube; component levels 0,95,135,175,215,255
        int v = idx - 16;
        auto level = [](int n) { return n ? 55 + n * 40 : 0; };
        return lv_color_make(level(v / 36), level((v / 6) % 6), level(v % 6));
    }
    int g = 8 + (idx - 232) * 10;  // grayscale ramp
    return lv_color_make(g, g, g);
}

struct ResolvedCell {
    char ch;
    lv_color_t fg, bg;
    bool opaque_bg;  // bg differs from the widget background -> needs a fill
    bool underline;
};

ResolvedCell resolve(const term::Cell &c) {
    ResolvedCell r;
    r.ch = static_cast<char>(c.ch);
    bool fg_def = c.attr & term::ATTR_FG_DEFAULT;
    bool bg_def = c.attr & term::ATTR_BG_DEFAULT;
    uint8_t fg_idx = c.fg;
    if ((c.attr & term::ATTR_BOLD) && !fg_def && fg_idx < 8)
        fg_idx += 8;  // bold -> bright (no bold face in the font)
    r.fg = fg_def ? kDefaultFg : palette(fg_idx);
    r.bg = bg_def ? kDefaultBg : palette(c.bg);
    r.opaque_bg = !bg_def;
    if (c.attr & term::ATTR_REVERSE) {
        lv_color_t t = r.fg;
        r.fg = r.bg;
        r.bg = t;
        r.opaque_bg = true;
    }
    if (c.attr & term::ATTR_DIM) r.fg = lv_color_mix(r.bg, r.fg, LV_OPA_50);
    r.underline = c.attr & term::ATTR_UNDERLINE;
    return r;
}

bool color_eq(lv_color_t a, lv_color_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr uint32_t kBlinkPeriodMs = 530;

}  // namespace

int TermView::cell_w() { return lv_font_get_glyph_width(R.font.hack_16, 'M', 'M'); }
int TermView::cell_h() { return lv_font_get_line_height(R.font.hack_16); }

TermView::TermView(lv_obj_t *parent, term::TermEmu *emu, int x, int y, int w,
                   int h)
    : emu_(emu) {
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_pos(obj_, x, y);
    lv_obj_set_size(obj_, w, h);
    lv_obj_set_style_bg_color(obj_, kDefaultBg, 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj_, draw_main_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(obj_, pressing_cb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(obj_, released_cb, LV_EVENT_RELEASED, this);
    blink_timer_ = lv_timer_create(blink_timer_cb, kBlinkPeriodMs, this);
    prev_sb_used_ = emu_->scrollback_used();
}

TermView::~TermView() {
    if (blink_timer_) lv_timer_delete(blink_timer_);
    if (obj_) lv_obj_delete(obj_);
}

int TermView::grid_x0() const {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    return coords.x1 + (lv_area_get_width(&coords) - emu_->cols() * cell_w()) / 2;
}

int TermView::grid_y0() const {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    return coords.y1 + (lv_area_get_height(&coords) - emu_->rows() * cell_h()) / 2;
}

lv_area_t TermView::row_area(int vrow) const {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    lv_area_t a;
    a.x1 = coords.x1;
    a.x2 = coords.x2;
    a.y1 = grid_y0() + vrow * cell_h();
    a.y2 = a.y1 + cell_h() - 1;
    return a;
}

void TermView::invalidate_cell(int vrow, int col) {
    lv_area_t a;
    a.x1 = grid_x0() + col * cell_w();
    a.x2 = a.x1 + cell_w() - 1;
    a.y1 = grid_y0() + vrow * cell_h();
    a.y2 = a.y1 + cell_h() - 1;
    lv_obj_invalidate_area(obj_, &a);
}

void TermView::invalidate_rows(uint64_t mask) {
    if (!mask) return;
    // Coalesce into one rect spanning the dirty range — rows are full-width,
    // so a span invalidation beats per-row rects when several are dirty.
    int lo = __builtin_ctzll(mask);
    int hi = 63 - __builtin_clzll(mask);
    if (lo >= emu_->rows()) return;
    if (hi >= emu_->rows()) hi = emu_->rows() - 1;
    lv_area_t a = row_area(lo);
    a.y2 = row_area(hi).y2;
    lv_obj_invalidate_area(obj_, &a);
}

void TermView::refresh() {
    uint64_t dirty = emu_->take_dirty();
    int sb = emu_->scrollback_used();
    int delta = sb - prev_sb_used_;
    prev_sb_used_ = sb;

    if (emu_->alt_screen()) {
        if (view_offset_ != 0) {
            view_offset_ = 0;
            lv_obj_invalidate(obj_);
        }
    } else if (view_offset_ > 0 && delta > 0) {
        // Scrolled back while output arrives: keep the viewport pinned to the
        // content by growing the offset with the scrollback. If the ring is
        // full the oldest lines are gone and the content really moved.
        int grown = view_offset_ + delta;
        if (grown > sb) {
            view_offset_ = sb;
            lv_obj_invalidate(obj_);
        } else {
            view_offset_ = grown;
        }
    }

    if (dirty) {
        uint64_t vmask =
            view_offset_ >= 64 ? 0 : dirty << view_offset_;  // view row = r+off
        invalidate_rows(vmask);
    }

    // Cursor: invalidate the previously drawn cell and the new one on any
    // change; fresh output also restarts the blink phase (solid while active).
    bool drawn = view_offset_ == 0 && emu_->cursor_visible();
    int row = emu_->cursor_row(), col = emu_->cursor_col();
    if (drawn != last_cur_drawn_ || row != last_cur_row_ ||
        col != last_cur_col_) {
        if (last_cur_drawn_) invalidate_cell(last_cur_row_, last_cur_col_);
        if (drawn) invalidate_cell(row, col);
        last_cur_drawn_ = drawn;
        last_cur_row_ = row;
        last_cur_col_ = col;
    }
    if (!blink_on_) {
        blink_on_ = true;
        if (drawn) invalidate_cell(row, col);
    }
    lv_timer_reset(blink_timer_);
}

void TermView::snap_to_live() { set_view_offset(0); }

void TermView::set_view_offset(int offset) {
    if (emu_->alt_screen()) offset = 0;
    if (offset < 0) offset = 0;
    if (offset > emu_->scrollback_used()) offset = emu_->scrollback_used();
    if (offset == view_offset_) return;
    view_offset_ = offset;
    lv_obj_invalidate(obj_);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void TermView::draw_main_cb(lv_event_t *e) {
    auto *self = static_cast<TermView *>(lv_event_get_user_data(e));
    self->draw(lv_event_get_layer(e));
}

void TermView::draw(lv_layer_t *layer) {
    const int cw = cell_w(), ch = cell_h();
    const int rows = emu_->rows();
    const int gx0 = grid_x0(), gy0 = grid_y0();

    // Only rows intersecting the clip area get draw tasks.
    int first = (layer->_clip_area.y1 - gy0) / ch;
    int last = (layer->_clip_area.y2 - gy0) / ch;
    if (first < 0) first = 0;
    if (last >= rows) last = rows - 1;

    for (int vrow = first; vrow <= last; ++vrow)
        draw_row(layer, vrow, emu_->view_row(vrow, view_offset_));

    // Block cursor: inverted cell drawn over the grid.
    if (view_offset_ == 0 && emu_->cursor_visible() && blink_on_) {
        int row = emu_->cursor_row(), col = emu_->cursor_col();
        if (row >= first && row <= last) {
            const term::Cell &c = emu_->view_row(row, 0)[col];
            ResolvedCell rc = resolve(c);
            lv_area_t a;
            a.x1 = gx0 + col * cw;
            a.x2 = a.x1 + cw - 1;
            a.y1 = gy0 + row * ch;
            a.y2 = a.y1 + ch - 1;
            lv_draw_fill_dsc_t fill;
            lv_draw_fill_dsc_init(&fill);
            fill.color = rc.opaque_bg ? rc.fg : kDefaultFg;
            lv_draw_fill(layer, &fill, &a);
            if (rc.ch != ' ') {
                char buf[2] = {rc.ch, 0};
                lv_draw_label_dsc_t dsc;
                lv_draw_label_dsc_init(&dsc);
                dsc.text = buf;
                dsc.text_local = 1;
                dsc.color = rc.opaque_bg ? rc.bg : kDefaultBg;
                dsc.font = R.font.hack_16;
                lv_draw_label(layer, &dsc, &a);
            }
        }
    }
}

void TermView::draw_row(lv_layer_t *layer, int vrow, const term::Cell *cells) {
    const int cw = cell_w(), ch = cell_h();
    const int cols = emu_->cols();
    const int gx0 = grid_x0();
    const int y1 = grid_y0() + vrow * ch;

    // Pass 1: background fills for runs whose bg differs from the widget bg.
    int c = 0;
    while (c < cols) {
        ResolvedCell rc = resolve(cells[c]);
        if (!rc.opaque_bg || color_eq(rc.bg, kDefaultBg)) {
            ++c;
            continue;
        }
        int start = c;
        while (c < cols) {
            ResolvedCell next = resolve(cells[c]);
            if (!next.opaque_bg || !color_eq(next.bg, rc.bg)) break;
            ++c;
        }
        lv_area_t a;
        a.x1 = gx0 + start * cw;
        a.x2 = gx0 + c * cw - 1;
        a.y1 = y1;
        a.y2 = y1 + ch - 1;
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.color = rc.bg;
        lv_draw_fill(layer, &fill, &a);
    }

    // Pass 2: text runs grouped by (fg, underline). Spaces ride along inside a
    // run; all-space runs without underline emit nothing.
    char buf[513];  // cols_ is clamped to 512 in TermEmu
    c = 0;
    while (c < cols) {
        ResolvedCell rc = resolve(cells[c]);
        int start = c;
        int n = 0;
        bool any_glyph = false;
        while (c < cols && n < (int)sizeof(buf) - 1) {
            ResolvedCell next = resolve(cells[c]);
            if (!color_eq(next.fg, rc.fg) || next.underline != rc.underline)
                break;
            buf[n++] = next.ch;
            any_glyph = any_glyph || next.ch != ' ';
            ++c;
        }
        if (!any_glyph && !rc.underline) continue;
        buf[n] = 0;
        lv_area_t a;
        a.x1 = gx0 + start * cw;
        a.x2 = gx0 + c * cw - 1;
        a.y1 = y1;
        a.y2 = y1 + ch - 1;
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.text = buf;
        dsc.text_local = 1;  // LVGL renders draw tasks later: copy the buffer
        dsc.color = rc.fg;
        dsc.font = R.font.hack_16;
        if (rc.underline) dsc.decor = LV_TEXT_DECOR_UNDERLINE;
        lv_draw_label(layer, &dsc, &a);
    }
}

// ---------------------------------------------------------------------------
// Input: swipe to scroll back, blink timer
// ---------------------------------------------------------------------------

void TermView::pressing_cb(lv_event_t *e) {
    auto *self = static_cast<TermView *>(lv_event_get_user_data(e));
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    self->drag_acc_px_ += vect.y;
    int lines = self->drag_acc_px_ / cell_h();
    if (lines != 0) {
        self->drag_acc_px_ -= lines * cell_h();
        // Dragging down (positive y) reveals older content.
        self->set_view_offset(self->view_offset_ + lines);
    }
}

void TermView::released_cb(lv_event_t *e) {
    auto *self = static_cast<TermView *>(lv_event_get_user_data(e));
    self->drag_acc_px_ = 0;
}

void TermView::blink_timer_cb(lv_timer_t *t) {
    auto *self = static_cast<TermView *>(lv_timer_get_user_data(t));
    self->blink_on_ = !self->blink_on_;
    if (self->view_offset_ == 0 && self->emu_->cursor_visible())
        self->invalidate_cell(self->emu_->cursor_row(), self->emu_->cursor_col());
}
