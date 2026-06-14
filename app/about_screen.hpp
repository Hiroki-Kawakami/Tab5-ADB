#pragma once
#include <string>

#include "screen.hpp"

// About screen — app identity, source/profile/social links (each openable on a
// phone via a QR-code modal), author, and an Acknowledgements entry that will
// push the licenses screen (lands later).
//
// The displayed strings (app name, author, URLs) are editable constants at the
// top of about_screen.cpp.
class AboutScreen : public Screen {
public:
    void build() override;

private:
    // Show a centered modal with a scannable QR code + the URL, so the link can
    // be opened on a phone (the Tab5 has no browser).
    void show_link_qr(const char *title, const char *url);
};
