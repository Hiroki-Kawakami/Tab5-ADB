// The byte-level escape-sequence state machine (a reduced Paul Williams VT500
// parser). This file only turns bytes into dispatch calls — the grid operations
// they trigger live in term_emu.cpp. Chunk boundaries can fall anywhere; all
// collect state lives in the TermEmu members so feed() resumes seamlessly.
#include <cstdint>
#include <cstring>

#include "term_emu.hpp"

namespace term {

namespace {
constexpr uint8_t BEL = 0x07, CAN = 0x18, SUB = 0x1a, ESC = 0x1b, DEL = 0x7f;
}  // namespace

void TermEmu::feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) parse_byte(data[i]);
}

void TermEmu::feed(const char *s) {
    feed(reinterpret_cast<const uint8_t *>(s), strlen(s));
}

void TermEmu::parser_to_ground() {
    pstate_ = PState::Ground;
    nparams_ = 0;
    params_[0] = 0;
    param_seen_ = false;
    csi_ignore_ = false;
    csi_priv_ = 0;
    intermediate_ = 0;
}

void TermEmu::parse_byte(uint8_t b) {
    // --- string-consuming states first (any byte is string data there) ---
    switch (pstate_) {
        case PState::OscString:
        case PState::SosString:
            if (b == BEL) { parser_to_ground(); return; }  // xterm BEL terminator
            if (b == ESC) {
                pstate_ = (pstate_ == PState::OscString) ? PState::OscEsc
                                                         : PState::SosEsc;
                return;
            }
            if (b == CAN || b == SUB) { parser_to_ground(); return; }
            return;  // consume
        case PState::OscEsc:
        case PState::SosEsc:
            if (b == '\\') { parser_to_ground(); return; }  // ST
            // ESC followed by anything else cancels the string and starts a
            // fresh escape sequence: reprocess b in the Esc state.
            parser_to_ground();
            pstate_ = PState::Esc;
            parse_byte(b);
            return;
        default:
            break;
    }

    // --- C0 controls execute immediately in every non-string state ---
    if (b < 0x20) {
        utf8_left_ = 0;  // a control aborts any in-flight UTF-8 sequence
        if (b == ESC) {
            parser_to_ground();
            pstate_ = PState::Esc;
            return;
        }
        if (b == CAN || b == SUB) { parser_to_ground(); return; }
        execute_c0(b);
        return;
    }

    switch (pstate_) {
        case PState::Ground: {
            if (b == DEL) return;
            // UTF-8 decode: non-ASCII prints as a placeholder cell, but the
            // *code point* count must match the device's column arithmetic.
            if (utf8_left_ > 0) {
                if ((b & 0xc0) == 0x80) {
                    utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3f);
                    if (--utf8_left_ == 0) print_cp(utf8_cp_);
                } else {
                    utf8_left_ = 0;
                    print_cp(0xfffd);
                    parse_byte(b);  // reprocess as a fresh byte
                }
                return;
            }
            if (b < 0x80) { print_cp(b); return; }
            if ((b & 0xe0) == 0xc0) { utf8_cp_ = b & 0x1f; utf8_left_ = 1; return; }
            if ((b & 0xf0) == 0xe0) { utf8_cp_ = b & 0x0f; utf8_left_ = 2; return; }
            if ((b & 0xf8) == 0xf0) { utf8_cp_ = b & 0x07; utf8_left_ = 3; return; }
            print_cp(0xfffd);  // stray continuation / invalid lead
            return;
        }

        case PState::Esc:
            if (b >= 0x20 && b <= 0x2f) {  // intermediate (e.g. '(' of ESC ( 0)
                intermediate_ = b;
                pstate_ = PState::EscInt;
                return;
            }
            switch (b) {
                case '[': pstate_ = PState::Csi; return;
                case ']': pstate_ = PState::OscString; return;
                case 'P':            // DCS
                case 'X':            // SOS
                case '^':            // PM
                case '_':            // APC
                    pstate_ = PState::SosString;
                    return;
                default:
                    parser_to_ground();
                    if (b >= 0x30 && b <= 0x7e) esc_dispatch(0, b);
                    return;
            }

        case PState::EscInt:
            if (b >= 0x20 && b <= 0x2f) { intermediate_ = b; return; }
            if (b >= 0x30 && b <= 0x7e) {
                uint8_t inter = intermediate_;
                parser_to_ground();
                esc_dispatch(inter, b);
            } else {
                parser_to_ground();
            }
            return;

        case PState::Csi:
            csi_collect(b);
            return;

        default:  // string states handled above
            return;
    }
}

void TermEmu::csi_collect(uint8_t b) {
    if (b >= '0' && b <= '9') {
        int &p = params_[nparams_ < kMaxParams ? nparams_ : kMaxParams - 1];
        if (p < 100000) p = p * 10 + (b - '0');
        param_seen_ = true;
        return;
    }
    if (b == ';' || b == ':') {  // ':' tolerated as ';' (SGR 38:5:N colon form)
        if (nparams_ < kMaxParams - 1) {
            ++nparams_;
            params_[nparams_] = 0;
        } else {
            csi_ignore_ = true;
        }
        param_seen_ = true;  // a separator implies a (default-0) param exists
        return;
    }
    if (b >= 0x3c && b <= 0x3f) {  // '<' '=' '>' '?' private marker
        if (nparams_ == 0 && !param_seen_ && csi_priv_ == 0) csi_priv_ = b;
        else csi_ignore_ = true;
        return;
    }
    if (b >= 0x20 && b <= 0x2f) { intermediate_ = b; return; }
    if (b >= 0x40 && b <= 0x7e) { csi_done(b); return; }
    // anything else (DEL, stray byte): ignore the sequence
    csi_ignore_ = true;
}

void TermEmu::csi_done(uint8_t final) {
    if (!csi_ignore_) {
        int total = param_seen_ ? nparams_ + 1 : 0;
        csi_dispatch(csi_priv_, params_, total, intermediate_, final);
    }
    parser_to_ground();
}

}  // namespace term
