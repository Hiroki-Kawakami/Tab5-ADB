// Host unit test for term_emu: feeds escape streams and asserts the resulting
// grid / cursor / responses. Run via test/run.sh (no phone, no GUI).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "term_emu.hpp"

using term::Cell;
using term::TermEmu;

static int g_fail = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_fail;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                   \
    do {                                                                     \
        std::string a_ = (a), b_ = (b);                                      \
        if (a_ != b_) {                                                      \
            ++g_fail;                                                        \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__,      \
                    __LINE__, a_.c_str(), b_.c_str());                       \
        }                                                                    \
    } while (0)

// Visible row text, right-trimmed.
static std::string row_str(const TermEmu &t, int row, int offset = 0) {
    const Cell *r = t.view_row(row, offset);
    std::string s;
    for (int c = 0; c < t.cols(); ++c) s += static_cast<char>(r[c].ch);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static TermEmu::Config small_cfg(int cols = 10, int rows = 5, int sb = 20) {
    TermEmu::Config cfg;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.scrollback = sb;
    return cfg;
}

static void test_print_and_controls() {
    TermEmu t(small_cfg());
    t.feed("hello");
    CHECK_EQ_STR(row_str(t, 0), "hello");
    CHECK(t.cursor_row() == 0 && t.cursor_col() == 5);
    t.feed("\r\nworld");
    CHECK_EQ_STR(row_str(t, 1), "world");
    t.feed("\rX");
    CHECK_EQ_STR(row_str(t, 1), "Xorld");
    t.feed("\x08\x08Y");  // BS BS then overwrite
    CHECK_EQ_STR(row_str(t, 1), "Yorld");
    // HT: fixed 8-column stops (col 2 -> 8)
    t.feed("\rZ\tT");
    CHECK_EQ_STR(row_str(t, 1), "Zorld   T");
}

static void test_deferred_wrap() {
    TermEmu t(small_cfg());
    t.feed("0123456789");  // exactly one row
    CHECK(t.cursor_row() == 0 && t.cursor_col() == 9);  // wrap pending, not taken
    t.feed("a");
    CHECK(t.cursor_row() == 1 && t.cursor_col() == 1);
    CHECK_EQ_STR(row_str(t, 0), "0123456789");
    CHECK_EQ_STR(row_str(t, 1), "a");
    // CR at wrap-pending cancels the wrap (vim status-line behaviour)
    t.feed("\x1b[1;1H");
    t.feed("0123456789\rB");
    CHECK_EQ_STR(row_str(t, 0), "B123456789");
    CHECK(t.cursor_row() == 0);
    // DECAWM off: stick at last column, overwrite
    TermEmu u(small_cfg());
    u.feed("\x1b[?7l0123456789XY");
    CHECK_EQ_STR(row_str(u, 0), "012345678Y");
    CHECK(u.cursor_row() == 0);
}

static void test_cursor_moves() {
    TermEmu t(small_cfg());
    t.feed("\x1b[3;4H");  // CUP row 3 col 4 (1-based)
    CHECK(t.cursor_row() == 2 && t.cursor_col() == 3);
    t.feed("\x1b[A\x1b[2C");
    CHECK(t.cursor_row() == 1 && t.cursor_col() == 5);
    t.feed("\x1b[10D");  // clamp at 0
    CHECK(t.cursor_col() == 0);
    t.feed("\x1b[7G");  // CHA
    CHECK(t.cursor_col() == 6);
    t.feed("\x1b[2d");  // VPA
    CHECK(t.cursor_row() == 1 && t.cursor_col() == 6);
    t.feed("\x1b[99;99H");  // clamp to grid
    CHECK(t.cursor_row() == 4 && t.cursor_col() == 9);
    t.feed("\x1b[H");
    CHECK(t.cursor_row() == 0 && t.cursor_col() == 0);
}

static void test_erase() {
    TermEmu t(small_cfg());
    t.feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    t.feed("\x1b[2;5H\x1b[K");  // EL 0: erase to end
    CHECK_EQ_STR(row_str(t, 1), "bbbb");
    t.feed("\x1b[1K");  // EL 1: erase to cursor (inclusive)
    CHECK_EQ_STR(row_str(t, 1), "");
    t.feed("\x1b[1;1H\x1b[J");  // ED 0 from home: clears everything
    CHECK_EQ_STR(row_str(t, 0), "");
    CHECK_EQ_STR(row_str(t, 2), "");
}

static void test_insert_delete() {
    TermEmu t(small_cfg());
    t.feed("abcdefghij");
    t.feed("\x1b[1;3H\x1b[2@");  // ICH 2 at col 3: "ij" pushed off the end
    CHECK_EQ_STR(row_str(t, 0), "ab  cdefgh");
    t.feed("\x1b[2P");  // DCH 2: the blanks go away again
    CHECK_EQ_STR(row_str(t, 0), "abcdefgh");
    t.feed("\x1b[3X");  // ECH 3 erases "cde" in place
    CHECK_EQ_STR(row_str(t, 0), "ab   fgh");
    // IL / DL
    TermEmu u(small_cfg());
    u.feed("one\r\ntwo\r\nthree");
    u.feed("\x1b[2;1H\x1b[L");  // insert line above "two"
    CHECK_EQ_STR(row_str(u, 1), "");
    CHECK_EQ_STR(row_str(u, 2), "two");
    CHECK_EQ_STR(row_str(u, 3), "three");
    u.feed("\x1b[M");  // delete it again
    CHECK_EQ_STR(row_str(u, 1), "two");
    CHECK_EQ_STR(row_str(u, 2), "three");
}

static void test_scroll_region() {
    TermEmu t(small_cfg(10, 5));
    t.feed("r1\r\nr2\r\nr3\r\nr4\r\nr5");
    t.feed("\x1b[2;4r");  // region rows 2..4; cursor homes
    CHECK(t.cursor_row() == 0 && t.cursor_col() == 0);
    t.feed("\x1b[4;1H\n");  // LF at region bottom: scroll region only
    CHECK_EQ_STR(row_str(t, 0), "r1");
    CHECK_EQ_STR(row_str(t, 1), "r3");
    CHECK_EQ_STR(row_str(t, 2), "r4");
    CHECK_EQ_STR(row_str(t, 3), "");
    CHECK_EQ_STR(row_str(t, 4), "r5");
    t.feed("\x1b[2;1H\x1bM");  // RI at region top: scroll down
    CHECK_EQ_STR(row_str(t, 1), "");
    CHECK_EQ_STR(row_str(t, 2), "r3");
    t.feed("\x1b[r");  // reset region
    // region scroll must NOT have pushed to scrollback
    CHECK(t.scrollback_used() == 0);
}

static void test_scrollback() {
    TermEmu t(small_cfg(10, 3, 5));
    t.feed("L1\r\nL2\r\nL3\r\nL4\r\nL5");  // scrolls twice
    CHECK_EQ_STR(row_str(t, 0), "L3");
    CHECK(t.scrollback_used() == 2);
    CHECK_EQ_STR(row_str(t, 0, 1), "L2");  // scrolled back one line
    CHECK_EQ_STR(row_str(t, 0, 2), "L1");
    CHECK_EQ_STR(row_str(t, 2, 2), "L3");
    // overflow the 5-line ring: oldest lines drop
    t.feed("\r\nL6\r\nL7\r\nL8\r\nL9");
    CHECK(t.scrollback_used() == 5);
    CHECK_EQ_STR(row_str(t, 0, 5), "L2");
    // ED 3 clears scrollback
    t.feed("\x1b[3J");
    CHECK(t.scrollback_used() == 0);
}

static void test_sgr() {
    TermEmu t(small_cfg());
    t.feed("\x1b[31mr\x1b[1;32mg\x1b[0;48;5;200mp\x1b[4;7mu\x1b[mz");
    const Cell *r = t.view_row(0, 0);
    CHECK(r[0].fg == 1 && !(r[0].attr & term::ATTR_FG_DEFAULT));
    CHECK(r[1].fg == 2 && (r[1].attr & term::ATTR_BOLD));
    CHECK(r[2].bg == 200 && !(r[2].attr & term::ATTR_BG_DEFAULT));
    CHECK((r[2].attr & term::ATTR_FG_DEFAULT));     // 0 reset default fg
    CHECK(!(r[2].attr & term::ATTR_BOLD));          // 0 cleared bold
    CHECK((r[3].attr & term::ATTR_UNDERLINE) && (r[3].attr & term::ATTR_REVERSE));
    CHECK((r[4].attr & term::ATTR_FG_DEFAULT) && (r[4].attr & term::ATTR_BG_DEFAULT));
    // bright fg shorthand + truecolor approximation
    t.feed("\r\n\x1b[95mB\x1b[38;2;255;0;0mT");
    const Cell *r2 = t.view_row(1, 0);
    CHECK(r2[0].fg == 13);
    CHECK(r2[1].fg == 196);  // pure red -> cube 16+36*5
    // BCE: erase with colored bg keeps it
    t.feed("\x1b[44m\x1b[2;5H\x1b[K");
    CHECK(r2[6].bg == 4 && !(r2[6].attr & term::ATTR_BG_DEFAULT));
    CHECK(r2[6].ch == ' ');
}

static void test_modes_and_responses() {
    TermEmu t(small_cfg());
    std::string out;
    t.set_responder([&](const uint8_t *d, size_t n) {
        out.append(reinterpret_cast<const char *>(d), n);
    });
    CHECK(!t.app_cursor_keys());
    t.feed("\x1b[?1h");
    CHECK(t.app_cursor_keys());
    t.feed("\x1b[?1l");
    CHECK(!t.app_cursor_keys());
    CHECK(t.cursor_visible());
    t.feed("\x1b[?25l");
    CHECK(!t.cursor_visible());
    t.feed("\x1b[?25h\x1b[2;3H\x1b[6n");
    CHECK_EQ_STR(out, "\x1b[2;3R");
    out.clear();
    t.feed("\x1b[c");
    CHECK_EQ_STR(out, "\x1b[?6c");
}

static void test_alt_screen() {
    TermEmu t(small_cfg());
    t.feed("main\x1b[?1049h");
    CHECK(t.alt_screen());
    CHECK_EQ_STR(row_str(t, 0), "");  // alt starts cleared
    t.feed("\x1b[HALT");  // cursor does NOT home on switch (xterm); apps CUP
    CHECK_EQ_STR(row_str(t, 0), "ALT");
    t.feed("\x1b[?1049l");
    CHECK(!t.alt_screen());
    CHECK_EQ_STR(row_str(t, 0), "main");
    CHECK(t.cursor_col() == 4);  // cursor restored
}

static void test_osc_and_charset() {
    TermEmu t(small_cfg());
    t.feed("\x1b]0;window title\x07ok");  // OSC + BEL
    CHECK_EQ_STR(row_str(t, 0), "ok");
    t.feed("\r\x1b]2;t\x1b\\ko");  // OSC + ST
    CHECK_EQ_STR(row_str(t, 0), "ko");
    // DEC graphics: q -> '-', x -> '|', corners -> '+'
    t.feed("\r\n\x1b(0lqqk\x1b(Bza");
    CHECK_EQ_STR(row_str(t, 1), "+--+za");
}

static void test_utf8() {
    TermEmu t(small_cfg());
    t.feed("a\xc3\xa9" "b");  // é: 1 placeholder cell
    CHECK_EQ_STR(row_str(t, 0), "a?b");
    CHECK(t.cursor_col() == 3);
    t.feed("\r\n\xe6\x97\xa5x");  // 日: wide -> 2 cells
    CHECK_EQ_STR(row_str(t, 1), "? x");
    CHECK(t.cursor_col() == 3);
    t.feed("\r\n\xff y");  // invalid lead byte -> one placeholder
    CHECK_EQ_STR(row_str(t, 2), "? y");
}

static void test_rep_and_ris() {
    TermEmu t(small_cfg());
    t.feed("=\x1b[4b");
    CHECK_EQ_STR(row_str(t, 0), "=====");
    t.feed("\x1b" "c");  // RIS
    CHECK_EQ_STR(row_str(t, 0), "");
    CHECK(t.cursor_row() == 0 && t.cursor_col() == 0);
    CHECK(t.scrollback_used() == 0);
}

// Same stream fed whole vs in adversarially small chunks must converge to the
// same grid — catches any FSM state that doesn't survive a chunk boundary.
static void test_split_fuzz() {
    static const char *stream =
        "plain text\r\n"
        "\x1b[2;10r\x1b[31;1mred bold\x1b[0m\r\n"
        "\x1b]0;ti\x1b tle\x07"
        "\x1b[3;5H\x1b[2J\x1b[38;5;123mX\x1b[48;2;1;2;3mY\r\n"
        "tab\there\r\n"
        "\xc3\xa9\xe6\x97\xa5 wide\r\n"
        "\x1b(0qqx\x1b(B\r\n"
        "\x1b[?1049hALT\x1b[5;1Hbottom\x1b[?1049l"
        "\x1b[2A\x1b[3C\x1b[Kend\x1b[?25l\x1b[?7l"
        "0123456789012345678901234567890123456789012345678901234567890123456789012\r\n"
        "\x1b[?7h\x1b[4h ins\x1b[4l\x1b[10;1Hscroll\nscroll\nscroll\n";
    size_t len = strlen(stream);

    TermEmu whole(small_cfg(72, 12, 50));
    whole.feed(reinterpret_cast<const uint8_t *>(stream), len);

    uint32_t lcg = 12345;
    for (int trial = 0; trial < 50; ++trial) {
        TermEmu chunked(small_cfg(72, 12, 50));
        size_t pos = 0;
        while (pos < len) {
            lcg = lcg * 1664525u + 1013904223u;
            size_t n = 1 + (lcg >> 24) % 7;
            if (n > len - pos) n = len - pos;
            chunked.feed(reinterpret_cast<const uint8_t *>(stream) + pos, n);
            pos += n;
        }
        bool same = chunked.cursor_row() == whole.cursor_row() &&
                    chunked.cursor_col() == whole.cursor_col() &&
                    chunked.scrollback_used() == whole.scrollback_used() &&
                    chunked.alt_screen() == whole.alt_screen() &&
                    chunked.cursor_visible() == whole.cursor_visible();
        for (int r = 0; same && r < whole.rows(); ++r) {
            const Cell *a = whole.view_row(r, 0);
            const Cell *b = chunked.view_row(r, 0);
            same = memcmp(a, b, sizeof(Cell) * whole.cols()) == 0;
        }
        if (!same) {
            ++g_fail;
            fprintf(stderr, "FAIL split_fuzz trial %d diverged\n", trial);
            return;
        }
    }
}

int main() {
    test_print_and_controls();
    test_deferred_wrap();
    test_cursor_moves();
    test_erase();
    test_insert_delete();
    test_scroll_region();
    test_scrollback();
    test_sgr();
    test_modes_and_responses();
    test_alt_screen();
    test_osc_and_charset();
    test_utf8();
    test_rep_and_ris();
    test_split_fuzz();
    if (g_fail) {
        fprintf(stderr, "%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
