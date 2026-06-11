// Grid model + dispatch surface: everything the parser (vt_parser.cpp) calls.
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "term_emu.hpp"

namespace term {

namespace {

template <typename T>
T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// DEC special graphics (ESC ( 0) 0x60..0x7e, approximated in ASCII — the font
// is ASCII-only, so box-drawing degrades to +-| instead of garbage letters.
constexpr char kDecGraphics[31] = {
    '+',  // ` diamond
    '#',  // a checkerboard
    ' ', ' ', ' ', ' ',                // b..e control pictures
    '\'',                              // f degree
    '#',                               // g plus/minus
    ' ', ' ',                          // h..i
    '+', '+', '+', '+', '+',           // j..n corners + cross
    '-', '-', '-', '-', '_',           // o..s horizontal scan lines
    '+', '+', '+', '+',                // t..w tees
    '|',                               // x vertical bar
    '<', '>',                          // y..z less/greater-equal
    '*', '!', '#',                     // { pi, | not-equal, } pound
    '.',                               // ~ centered dot
};

// Coarse double-width ranges (wcwidth == 2): enough to keep the cursor column
// in sync with the device for CJK output; precision is not the goal.
bool is_wide_cp(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
           (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
           (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
           (cp >= 0x20000 && cp <= 0x3fffd);
}

Cell *alloc_lines(int lines, int cols) {
    return static_cast<Cell *>(heap_caps_malloc(
        static_cast<size_t>(lines) * cols * sizeof(Cell), MALLOC_CAP_SPIRAM));
}

}  // namespace

TermEmu::TermEmu(const Config &cfg)
    : cols_(clamp(cfg.cols, 2, 512)),
      rows_(clamp(cfg.rows, 1, 64)),  // dirty_ is a 64-bit row mask
      sb_lines_(cfg.scrollback < 0 ? 0 : cfg.scrollback) {
    cur_attr_ = ATTR_FG_DEFAULT | ATTR_BG_DEFAULT;
    scroll_bottom_ = rows_ - 1;
    main_slab_ = alloc_lines(rows_, cols_);
    alt_slab_ = alloc_lines(rows_, cols_);
    sb_slab_ = sb_lines_ ? alloc_lines(sb_lines_, cols_) : nullptr;
    main_rows_ = new Cell *[rows_];
    alt_rows_ = new Cell *[rows_];
    for (int r = 0; r < rows_; ++r) {
        main_rows_[r] = main_slab_ + static_cast<size_t>(r) * cols_;
        alt_rows_[r] = alt_slab_ + static_cast<size_t>(r) * cols_;
    }
    screen_ = main_rows_;
    for (int r = 0; r < rows_; ++r) {
        clear_row(main_rows_[r], 0, cols_ - 1);
        clear_row(alt_rows_[r], 0, cols_ - 1);
    }
    mark_all_dirty();
}

TermEmu::~TermEmu() {
    heap_caps_free(main_slab_);
    heap_caps_free(alt_slab_);
    heap_caps_free(sb_slab_);
    delete[] main_rows_;
    delete[] alt_rows_;
}

void TermEmu::reset() {
    for (int r = 0; r < rows_; ++r) {
        clear_row(main_rows_[r], 0, cols_ - 1);
        clear_row(alt_rows_[r], 0, cols_ - 1);
    }
    screen_ = main_rows_;
    alt_active_ = false;
    sb_used_ = 0;
    sb_head_ = 0;
    cur_row_ = cur_col_ = 0;
    wrap_pending_ = false;
    cur_fg_ = cur_bg_ = 0;
    cur_attr_ = ATTR_FG_DEFAULT | ATTR_BG_DEFAULT;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    cursor_visible_ = true;
    mode_appcursor_ = false;
    mode_autowrap_ = true;
    mode_insert_ = false;
    charset_dec_ = false;
    last_printed_ = ' ';
    saved_ = SavedCursor{};
    alt_saved_ = SavedCursor{};
    utf8_left_ = 0;
    parser_to_ground();
    mark_all_dirty();
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

const Cell *TermEmu::view_row(int row, int view_offset) const {
    row = clamp(row, 0, rows_ - 1);
    if (alt_active_) return screen_[row];
    int offset = clamp(view_offset, 0, sb_used_);
    int abs = row - offset;  // relative to the live screen's top row
    if (abs >= 0) return screen_[abs];
    // Scrollback line, counted back from the newest pushed line (abs == -1).
    int slot = sb_head_ + abs;  // sb_head_ - distance
    while (slot < 0) slot += sb_lines_;
    return sb_slab_ + static_cast<size_t>(slot) * cols_;
}

uint64_t TermEmu::take_dirty() {
    uint64_t d = dirty_;
    dirty_ = 0;
    return d;
}

void TermEmu::mark_all_dirty() {
    dirty_ = (rows_ >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << rows_) - 1);
}

int TermEmu::cursor_col() const { return clamp(cur_col_, 0, cols_ - 1); }

// ---------------------------------------------------------------------------
// Grid primitives
// ---------------------------------------------------------------------------

Cell TermEmu::blank_cell() const {
    // Erased cells keep the current bg (BCE) so full-screen apps that erase
    // with an SGR background get it, but text attributes don't smear.
    uint8_t attr = (cur_attr_ & ATTR_BG_DEFAULT) | ATTR_FG_DEFAULT;
    return Cell{' ', 0, cur_bg_, attr};
}

void TermEmu::clear_row(Cell *row, int from, int to) {
    Cell b = blank_cell();
    for (int c = from; c <= to; ++c) row[c] = b;
}

void TermEmu::clear_screen_rows(int from, int to) {
    for (int r = from; r <= to; ++r) {
        clear_row(screen_[r], 0, cols_ - 1);
        mark_dirty(r);
    }
}

void TermEmu::push_scrollback(const Cell *row) {
    if (!sb_slab_) return;
    memcpy(sb_slab_ + static_cast<size_t>(sb_head_) * cols_, row,
           static_cast<size_t>(cols_) * sizeof(Cell));
    sb_head_ = (sb_head_ + 1) % sb_lines_;
    if (sb_used_ < sb_lines_) ++sb_used_;
}

void TermEmu::scroll_up(int n) {
    int height = scroll_bottom_ - scroll_top_ + 1;
    n = clamp(n, 1, height);
    for (int i = 0; i < n; ++i) {
        Cell *top = screen_[scroll_top_];
        if (!alt_active_ && scroll_top_ == 0) push_scrollback(top);
        for (int r = scroll_top_; r < scroll_bottom_; ++r)
            screen_[r] = screen_[r + 1];
        screen_[scroll_bottom_] = top;
        clear_row(top, 0, cols_ - 1);
    }
    for (int r = scroll_top_; r <= scroll_bottom_; ++r) mark_dirty(r);
}

void TermEmu::scroll_down(int n) {
    int height = scroll_bottom_ - scroll_top_ + 1;
    n = clamp(n, 1, height);
    for (int i = 0; i < n; ++i) {
        Cell *bottom = screen_[scroll_bottom_];
        for (int r = scroll_bottom_; r > scroll_top_; --r)
            screen_[r] = screen_[r - 1];
        screen_[scroll_top_] = bottom;
        clear_row(bottom, 0, cols_ - 1);
    }
    for (int r = scroll_top_; r <= scroll_bottom_; ++r) mark_dirty(r);
}

void TermEmu::move_cursor(int row, int col) {
    cur_row_ = clamp(row, 0, rows_ - 1);
    cur_col_ = clamp(col, 0, cols_ - 1);
    wrap_pending_ = false;
}

void TermEmu::line_feed() {
    wrap_pending_ = false;
    if (cur_row_ == scroll_bottom_) scroll_up(1);
    else if (cur_row_ < rows_ - 1) ++cur_row_;
}

void TermEmu::reverse_line_feed() {
    wrap_pending_ = false;
    if (cur_row_ == scroll_top_) scroll_down(1);
    else if (cur_row_ > 0) --cur_row_;
}

void TermEmu::carriage_return() {
    cur_col_ = 0;
    wrap_pending_ = false;
}

void TermEmu::put_char(uint8_t ch) {
    if (wrap_pending_) {  // DECAWM deferred wrap
        carriage_return();
        line_feed();
    }
    Cell *row = screen_[cur_row_];
    if (mode_insert_ && cur_col_ < cols_ - 1)
        memmove(row + cur_col_ + 1, row + cur_col_,
                static_cast<size_t>(cols_ - 1 - cur_col_) * sizeof(Cell));
    row[cur_col_] = Cell{ch, cur_fg_, cur_bg_, cur_attr_};
    mark_dirty(cur_row_);
    if (cur_col_ == cols_ - 1) {
        if (mode_autowrap_) wrap_pending_ = true;
    } else {
        ++cur_col_;
    }
    last_printed_ = ch;
}

void TermEmu::print_cp(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f) return;
    if (cp <= 0x7e) {
        uint8_t ch = static_cast<uint8_t>(cp);
        if (charset_dec_ && cp >= 0x60) ch = kDecGraphics[cp - 0x60];
        put_char(ch);
        return;
    }
    // Non-ASCII: placeholder cell(s), matching the device's wcwidth count.
    put_char('?');
    if (is_wide_cp(cp)) put_char(' ');
}

// ---------------------------------------------------------------------------
// Dispatch: C0 / ESC / CSI
// ---------------------------------------------------------------------------

void TermEmu::execute_c0(uint8_t c) {
    switch (c) {
        case 0x08:  // BS
            if (cur_col_ > 0) --cur_col_;
            wrap_pending_ = false;
            break;
        case 0x09: {  // HT: fixed 8-column tab stops
            int next = (cur_col_ / 8 + 1) * 8;
            cur_col_ = clamp(next, 0, cols_ - 1);
            break;
        }
        case 0x0a:  // LF
        case 0x0b:  // VT
        case 0x0c:  // FF
            line_feed();
            break;
        case 0x0d:  // CR
            carriage_return();
            break;
        default:  // BEL, ENQ, NUL, ...
            break;
    }
}

void TermEmu::esc_dispatch(uint8_t intermediate, uint8_t final) {
    if (intermediate == '(') {  // G0 charset
        if (final == '0') charset_dec_ = true;
        else if (final == 'B') charset_dec_ = false;
        return;
    }
    if (intermediate != 0) return;  // ')' G1 etc.: ignored
    switch (final) {
        case '7':  // DECSC
            saved_ = SavedCursor{cur_row_, cur_col_, cur_fg_, cur_bg_,
                                 cur_attr_, charset_dec_};
            break;
        case '8':  // DECRC
            move_cursor(saved_.row, saved_.col);
            cur_fg_ = saved_.fg;
            cur_bg_ = saved_.bg;
            cur_attr_ = saved_.attr;
            charset_dec_ = saved_.charset_dec;
            break;
        case 'D': line_feed(); break;             // IND
        case 'E':                                  // NEL
            carriage_return();
            line_feed();
            break;
        case 'M': reverse_line_feed(); break;     // RI
        case 'c': reset(); break;                 // RIS
        default: break;  // ESC = / ESC > keypad modes etc.: ignored
    }
}

void TermEmu::csi_dispatch(uint8_t priv, const int *params, int nparams,
                           uint8_t intermediate, uint8_t final) {
    if (intermediate != 0) return;  // e.g. DECSCUSR (CSI Sp q): ignored
    auto p = [&](int i, int def) { return i < nparams && params[i] ? params[i] : def; };

    if (priv == '?') {
        if (final == 'h' || final == 'l') {
            for (int i = 0; i < (nparams ? nparams : 1); ++i)
                set_mode(p(i, 0), true, final == 'h');
        } else if (final == 'c') {
            respond("\x1b[?6c");  // DA with private marker (rare): VT102
        }
        return;
    }
    if (priv == '>') {
        if (final == 'c') respond("\x1b[>0;0;0c");  // secondary DA
        return;
    }
    if (priv != 0) return;

    switch (final) {
        case 'A': move_cursor(clamp(cur_row_ - p(0, 1),
                                    cur_row_ >= scroll_top_ ? scroll_top_ : 0,
                                    rows_ - 1), cur_col_); break;  // CUU
        case 'B':
        case 'e':  // VPR
            move_cursor(clamp(cur_row_ + p(0, 1), 0,
                              cur_row_ <= scroll_bottom_ ? scroll_bottom_
                                                         : rows_ - 1),
                        cur_col_);
            break;  // CUD
        case 'C':
        case 'a':  // HPR
            move_cursor(cur_row_, cur_col_ + p(0, 1));
            break;  // CUF
        case 'D': move_cursor(cur_row_, cur_col_ - p(0, 1)); break;  // CUB
        case 'E': move_cursor(cur_row_ + p(0, 1), 0); break;         // CNL
        case 'F': move_cursor(cur_row_ - p(0, 1), 0); break;         // CPL
        case 'G':
        case '`':  // HPA
            move_cursor(cur_row_, p(0, 1) - 1);
            break;  // CHA
        case 'H':
        case 'f':  // HVP
            move_cursor(p(0, 1) - 1, p(1, 1) - 1);
            break;  // CUP
        case 'd': move_cursor(p(0, 1) - 1, cur_col_); break;  // VPA

        case 'J': {  // ED
            int mode = (nparams >= 1) ? params[0] : 0;
            wrap_pending_ = false;
            if (mode == 0) {
                clear_row(screen_[cur_row_], cur_col_, cols_ - 1);
                mark_dirty(cur_row_);
                if (cur_row_ < rows_ - 1)
                    clear_screen_rows(cur_row_ + 1, rows_ - 1);
            } else if (mode == 1) {
                if (cur_row_ > 0) clear_screen_rows(0, cur_row_ - 1);
                clear_row(screen_[cur_row_], 0, cur_col_);
                mark_dirty(cur_row_);
            } else if (mode == 2) {
                clear_screen_rows(0, rows_ - 1);
            } else if (mode == 3) {
                sb_used_ = 0;
                sb_head_ = 0;
            }
            break;
        }
        case 'K': {  // EL
            int mode = (nparams >= 1) ? params[0] : 0;
            wrap_pending_ = false;
            if (mode == 0) clear_row(screen_[cur_row_], cur_col_, cols_ - 1);
            else if (mode == 1) clear_row(screen_[cur_row_], 0, cur_col_);
            else if (mode == 2) clear_row(screen_[cur_row_], 0, cols_ - 1);
            mark_dirty(cur_row_);
            break;
        }

        case '@': {  // ICH
            int n = clamp(p(0, 1), 1, cols_ - cur_col_);
            Cell *row = screen_[cur_row_];
            memmove(row + cur_col_ + n, row + cur_col_,
                    static_cast<size_t>(cols_ - cur_col_ - n) * sizeof(Cell));
            clear_row(row, cur_col_, cur_col_ + n - 1);
            mark_dirty(cur_row_);
            break;
        }
        case 'P': {  // DCH
            int n = clamp(p(0, 1), 1, cols_ - cur_col_);
            Cell *row = screen_[cur_row_];
            memmove(row + cur_col_, row + cur_col_ + n,
                    static_cast<size_t>(cols_ - cur_col_ - n) * sizeof(Cell));
            clear_row(row, cols_ - n, cols_ - 1);
            mark_dirty(cur_row_);
            break;
        }
        case 'X': {  // ECH
            int n = clamp(p(0, 1), 1, cols_ - cur_col_);
            clear_row(screen_[cur_row_], cur_col_, cur_col_ + n - 1);
            mark_dirty(cur_row_);
            break;
        }

        case 'L':  // IL
        case 'M':  // DL
            if (cur_row_ >= scroll_top_ && cur_row_ <= scroll_bottom_) {
                int save_top = scroll_top_;
                scroll_top_ = cur_row_;  // rotate within [cursor, bottom]
                if (final == 'L') scroll_down(p(0, 1));
                else scroll_up(p(0, 1));
                scroll_top_ = save_top;
                cur_col_ = 0;
                wrap_pending_ = false;
            }
            break;
        case 'S': scroll_up(p(0, 1)); break;    // SU
        case 'T': scroll_down(p(0, 1)); break;  // SD

        case 'b': {  // REP: repeat the last printed character
            int n = clamp(p(0, 1), 1, cols_ * rows_);
            for (int i = 0; i < n; ++i) put_char(last_printed_);
            break;
        }

        case 'm': do_sgr(params, nparams); break;

        case 'r': {  // DECSTBM
            int top = p(0, 1) - 1;
            int bottom = p(1, rows_) - 1;
            top = clamp(top, 0, rows_ - 1);
            bottom = clamp(bottom, 0, rows_ - 1);
            if (top < bottom) {
                scroll_top_ = top;
                scroll_bottom_ = bottom;
                move_cursor(0, 0);
            }
            break;
        }
        case 's':  // SCOSC (shares the DECSC slot — the common shortcut)
            saved_ = SavedCursor{cur_row_, cur_col_, cur_fg_, cur_bg_,
                                 cur_attr_, charset_dec_};
            break;
        case 'u':  // SCORC
            move_cursor(saved_.row, saved_.col);
            break;

        case 'h':
        case 'l':
            for (int i = 0; i < nparams; ++i)
                set_mode(params[i], false, final == 'h');
            break;

        case 'n':  // DSR
            if (p(0, 0) == 6) {
                char buf[32];  // sized for worst-case INT_MAX coordinates
                snprintf(buf, sizeof(buf), "\x1b[%d;%dR", cur_row_ + 1,
                         cursor_col() + 1);
                respond(buf);
            } else if (p(0, 0) == 5) {
                respond("\x1b[0n");
            }
            break;
        case 'c':  // DA
            respond("\x1b[?6c");
            break;

        default:
            break;  // ignored final
    }
}

void TermEmu::do_sgr(const int *params, int nparams) {
    if (nparams == 0) {  // CSI m == CSI 0 m
        static const int zero = 0;
        params = &zero;
        nparams = 1;
    }
    for (int i = 0; i < nparams; ++i) {
        int v = params[i];
        switch (v) {
            case 0:
                cur_attr_ = ATTR_FG_DEFAULT | ATTR_BG_DEFAULT;
                cur_fg_ = cur_bg_ = 0;
                break;
            case 1: cur_attr_ |= ATTR_BOLD; break;
            case 2: cur_attr_ |= ATTR_DIM; break;
            case 4: cur_attr_ |= ATTR_UNDERLINE; break;
            case 7: cur_attr_ |= ATTR_REVERSE; break;
            case 22: cur_attr_ &= ~(ATTR_BOLD | ATTR_DIM); break;
            case 24: cur_attr_ &= ~ATTR_UNDERLINE; break;
            case 27: cur_attr_ &= ~ATTR_REVERSE; break;
            case 39: cur_attr_ |= ATTR_FG_DEFAULT; break;
            case 49: cur_attr_ |= ATTR_BG_DEFAULT; break;
            case 38:
            case 48: {
                uint8_t idx = 0;
                if (i + 2 < nparams && params[i + 1] == 5) {  // 38;5;N
                    idx = static_cast<uint8_t>(clamp(params[i + 2], 0, 255));
                    i += 2;
                } else if (i + 4 < nparams && params[i + 1] == 2) {  // 38;2;R;G;B
                    int r = clamp(params[i + 2], 0, 255);
                    int g = clamp(params[i + 3], 0, 255);
                    int b = clamp(params[i + 4], 0, 255);
                    idx = static_cast<uint8_t>(16 + 36 * (r * 5 / 255) +
                                               6 * (g * 5 / 255) + (b * 5 / 255));
                    i += 4;
                } else {
                    break;  // malformed: skip
                }
                if (v == 38) {
                    cur_fg_ = idx;
                    cur_attr_ &= ~ATTR_FG_DEFAULT;
                } else {
                    cur_bg_ = idx;
                    cur_attr_ &= ~ATTR_BG_DEFAULT;
                }
                break;
            }
            default:
                if (v >= 30 && v <= 37) {
                    cur_fg_ = static_cast<uint8_t>(v - 30);
                    cur_attr_ &= ~ATTR_FG_DEFAULT;
                } else if (v >= 90 && v <= 97) {
                    cur_fg_ = static_cast<uint8_t>(v - 90 + 8);
                    cur_attr_ &= ~ATTR_FG_DEFAULT;
                } else if (v >= 40 && v <= 47) {
                    cur_bg_ = static_cast<uint8_t>(v - 40);
                    cur_attr_ &= ~ATTR_BG_DEFAULT;
                } else if (v >= 100 && v <= 107) {
                    cur_bg_ = static_cast<uint8_t>(v - 100 + 8);
                    cur_attr_ &= ~ATTR_BG_DEFAULT;
                }
                break;  // italic/blink/strike/...: ignored
        }
    }
}

void TermEmu::set_mode(int mode, bool priv, bool on) {
    if (priv) {
        switch (mode) {
            case 1: mode_appcursor_ = on; break;  // DECCKM
            case 7:                                // DECAWM
                mode_autowrap_ = on;
                if (!on) wrap_pending_ = false;
                break;
            case 25: cursor_visible_ = on; break;
            case 47:
            case 1047:
                if (on) enter_alt_screen(false);
                else leave_alt_screen(false);
                break;
            case 1049:
                if (on) enter_alt_screen(true);
                else leave_alt_screen(true);
                break;
            default: break;  // mouse / bracketed paste / focus / ...: ignored
        }
    } else if (mode == 4) {
        mode_insert_ = on;  // IRM
    }
}

void TermEmu::enter_alt_screen(bool save_cursor) {
    if (alt_active_) return;
    if (save_cursor)
        alt_saved_ = SavedCursor{cur_row_, cur_col_, cur_fg_, cur_bg_,
                                 cur_attr_, charset_dec_};
    alt_active_ = true;
    screen_ = alt_rows_;
    clear_screen_rows(0, rows_ - 1);
    wrap_pending_ = false;
    mark_all_dirty();
}

void TermEmu::leave_alt_screen(bool restore_cursor) {
    if (!alt_active_) return;
    alt_active_ = false;
    screen_ = main_rows_;
    if (restore_cursor) {
        move_cursor(alt_saved_.row, alt_saved_.col);
        cur_fg_ = alt_saved_.fg;
        cur_bg_ = alt_saved_.bg;
        cur_attr_ = alt_saved_.attr;
        charset_dec_ = alt_saved_.charset_dec;
    }
    wrap_pending_ = false;
    mark_all_dirty();
}

void TermEmu::respond(const char *s) {
    if (responder_) responder_(reinterpret_cast<const uint8_t *>(s), strlen(s));
}

}  // namespace term
