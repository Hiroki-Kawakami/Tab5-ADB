#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

// term_emu — a VT100/xterm-subset terminal emulator: bytes in, cell grid out.
// Pure portable C++ (no LVGL, no adb): the caller feeds PTY output bytes and
// renders the grid however it likes; key handling stays outside (the caller
// asks app_cursor_keys() to encode arrows). Single-threaded by design — feed()
// and all accessors must run on one thread (the app calls everything on the
// LVGL thread).
//
// Supported (see test/test_term.cpp for the executable spec):
//   C0      BS HT(8-col tabs) LF VT FF CR; BEL ignored; CAN/SUB abort escapes
//   ESC     7/8 (DECSC/DECRC), D/E/M (IND/NEL/RI), c (RIS), ( B / ( 0 (DEC
//           line-drawing mapped to ASCII approximations)
//   CSI     CUU CUD CUF CUB CNL CPL CHA CUP VPA, ED(0-3) EL(0-2), ICH DCH ECH,
//           IL DL, SU SD, REP, DECSTBM, SCOSC/SCORC (s/u), SGR, DSR(5/6), DA
//   SGR     reset bold dim reverse underline, 16-color + bright, 38/48;5;N
//           (256-color); default fg/bg are attribute bits, not palette entries
//   modes   DECCKM(?1), DECAWM(?7, deferred wrap), cursor visible(?25),
//           alt screen(?47/?1047/?1049), IRM(4); everything else is ignored
//   strings OSC/DCS/SOS/PM/APC parsed and discarded (BEL / ESC \ terminated)
//   UTF-8   decoded; non-ASCII prints as a '?' placeholder, one cell per code
//           point (two for wide/CJK ranges) so the column count tracks the
//           device's wcwidth-based one
//
// Scrollback: main screen only, fixed line count, PSRAM-backed
// (heap_caps_malloc(MALLOC_CAP_SPIRAM)). The view can be scrolled back with
// view_offset (0 = live bottom); the alt screen has no scrollback.
namespace term {

// Cell attribute bits.
enum : uint8_t {
    ATTR_BOLD       = 1 << 0,
    ATTR_DIM        = 1 << 1,
    ATTR_REVERSE    = 1 << 2,
    ATTR_UNDERLINE  = 1 << 3,
    ATTR_FG_DEFAULT = 1 << 4,  // fg index invalid: use the theme default fg
    ATTR_BG_DEFAULT = 1 << 5,  // bg index invalid: use the theme default bg
};

struct Cell {
    uint8_t ch;    // printable ASCII 0x20..0x7e ('?' placeholder for non-ASCII)
    uint8_t fg;    // xterm 256-color index (unless ATTR_FG_DEFAULT)
    uint8_t bg;    // xterm 256-color index (unless ATTR_BG_DEFAULT)
    uint8_t attr;  // ATTR_* bits
};

class TermEmu {
public:
    struct Config {
        int cols = 72;
        int rows = 42;
        int scrollback = 1000;  // main-screen scrollback lines (0 = none)
    };

    // Bytes the terminal writes back to the host (DSR/DA responses). Invoked
    // from inside feed().
    using Responder = std::function<void(const uint8_t *data, size_t len)>;

    explicit TermEmu(const Config &cfg);
    ~TermEmu();
    TermEmu(const TermEmu &) = delete;
    TermEmu &operator=(const TermEmu &) = delete;

    void set_responder(Responder r) { responder_ = std::move(r); }

    // Parse and apply a chunk of PTY output. Chunks may split anywhere —
    // escape-sequence state carries across calls.
    void feed(const uint8_t *data, size_t len);
    void feed(const char *s);  // convenience (tests)

    // Full reset (RIS): clears both screens, scrollback, modes, attributes.
    void reset();

    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // --- view (what to draw) ---
    // view_offset scrolls back into history: 0 = live screen, max =
    // scrollback_used(). Row 0 is the top of the visible viewport.
    const Cell *view_row(int row, int view_offset) const;
    int scrollback_used() const { return sb_used_; }
    bool alt_screen() const { return alt_active_; }

    // Dirty screen rows accumulated since the last call (bit i = row i; all
    // bits set after a scroll/clear). The caller owns mapping this to redraws.
    uint64_t take_dirty();

    // --- cursor (live-screen coordinates) ---
    int cursor_row() const { return cur_row_; }
    int cursor_col() const;  // clamped to cols-1 when a wrap is pending
    bool cursor_visible() const { return cursor_visible_; }

    // --- input-side modes the keyboard needs ---
    bool app_cursor_keys() const { return mode_appcursor_; }  // DECCKM

private:
    // ---- grid model (term_emu.cpp) ----
    Cell *row_ptr(int row) const { return screen_[row]; }
    Cell blank_cell() const;
    void clear_row(Cell *row, int from, int to);  // [from, to] inclusive
    void clear_screen_rows(int from, int to);
    void scroll_up(int n);    // within the scroll region; pushes to scrollback
    void scroll_down(int n);  // within the scroll region
    void push_scrollback(const Cell *row);
    void put_char(uint8_t ch);          // one resolved ASCII cell at the cursor
    void print_cp(uint32_t cp);         // decoded code point (wraps, wide, DEC)
    void move_cursor(int row, int col);  // clamps; clears wrap-pending
    void line_feed();                    // IND: down or scroll
    void reverse_line_feed();            // RI: up or scroll
    void carriage_return();
    void mark_dirty(int row) { dirty_ |= (uint64_t)1 << row; }
    void mark_all_dirty();

    // ---- dispatch surface the parser calls (term_emu.cpp) ----
    void execute_c0(uint8_t c);
    void esc_dispatch(uint8_t intermediate, uint8_t final);
    void csi_dispatch(uint8_t priv, const int *params, int nparams,
                      uint8_t intermediate, uint8_t final);
    void do_sgr(const int *params, int nparams);
    void set_mode(int mode, bool priv, bool on);
    void enter_alt_screen(bool save_cursor);
    void leave_alt_screen(bool restore_cursor);
    void respond(const char *s);

    // ---- parser (vt_parser.cpp) ----
    enum class PState : uint8_t {
        Ground, Esc, EscInt, Csi, OscString, OscEsc, SosString, SosEsc,
    };
    void parse_byte(uint8_t b);
    void csi_collect(uint8_t b);
    void csi_done(uint8_t final);
    void parser_to_ground();

    // ---- configuration / storage ----
    int cols_, rows_;
    int sb_lines_;        // scrollback capacity (lines)
    Cell *main_slab_ = nullptr;   // rows_ lines
    Cell *alt_slab_ = nullptr;    // rows_ lines
    Cell *sb_slab_ = nullptr;     // sb_lines_ lines (ring)
    Cell **screen_ = nullptr;     // active row pointers (rotated on scroll)
    Cell **main_rows_ = nullptr;  // main-screen row pointers
    Cell **alt_rows_ = nullptr;   // alt-screen row pointers
    int sb_head_ = 0;  // ring slot the next pushed line goes to
    int sb_used_ = 0;

    // ---- terminal state ----
    int cur_row_ = 0, cur_col_ = 0;
    bool wrap_pending_ = false;  // DECAWM deferred wrap
    uint8_t cur_fg_ = 0, cur_bg_ = 0;
    uint8_t cur_attr_;  // current SGR state (FG/BG_DEFAULT initially)
    int scroll_top_ = 0, scroll_bottom_;  // DECSTBM region, inclusive
    bool cursor_visible_ = true;
    bool alt_active_ = false;
    bool mode_appcursor_ = false;  // DECCKM
    bool mode_autowrap_ = true;    // DECAWM
    bool mode_insert_ = false;     // IRM
    bool charset_dec_ = false;     // G0 = DEC special graphics (ESC ( 0)
    uint8_t last_printed_ = ' ';   // for REP
    // DECSC/DECRC + alt-screen save slots
    struct SavedCursor {
        int row = 0, col = 0;
        uint8_t fg = 0, bg = 0;
        uint8_t attr = ATTR_FG_DEFAULT | ATTR_BG_DEFAULT;
        bool charset_dec = false;
    };
    SavedCursor saved_;          // ESC 7/8 + CSI s/u
    SavedCursor alt_saved_;      // cursor saved across ?1049 enter/leave

    uint64_t dirty_ = 0;
    Responder responder_;

    // ---- parser state ----
    PState pstate_ = PState::Ground;
    static constexpr int kMaxParams = 16;
    int params_[kMaxParams];
    int nparams_ = 0;
    bool param_seen_ = false;   // digits seen for the current param
    bool csi_ignore_ = false;   // too many params / bad collect: ignore at final
    uint8_t csi_priv_ = 0;      // '?' '>' '<' '=' marker
    uint8_t intermediate_ = 0;  // single intermediate byte (enough for our set)
    // UTF-8 accumulator
    uint32_t utf8_cp_ = 0;
    int utf8_left_ = 0;
};

}  // namespace term
