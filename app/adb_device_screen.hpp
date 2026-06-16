#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "agent_link.hpp"  // agent_link::Link, agent_link::MediaListener, MediaState
#include "agent_preview.hpp"
#include "media_session.hpp"
#include "screen.hpp"
#include "screencap_preview.hpp"

// Post-connection home for a device. A tappable summary header (device name,
// model / Android version / storage line, battery + network status icons;
// tap -> ADBDeviceInfoScreen) sits above a live screen preview + a column of
// tool buttons. The header renders immediately from the CNXN banner, then a
// chained `exec` fills the live fields; while the screen shows, a 10 s timer
// re-fetches them (battery and signal are live data).
//
// The preview implementation follows the agent mode the connect flow settled
// (app::AgentClient::mode()): Normal = AgentPreview (the mirror stream, light +
// real frame rate), Limited = ScreencapPreview (plain `exec:screencap`).
// Tapping the preview opens the full mirroring screen in Normal mode; Limited
// mode explains why it can't instead.
class ADBDeviceScreen : public Screen, public agent_link::MediaListener {
public:
    ~ADBDeviceScreen() override;
    void build() override;
    void onAppear() override;
    void onDisappear() override;

    // agent_link::MediaListener — fires on the adb reader thread (Normal mode).
    void on_media_update(agent_link::Link *link,
                         const agent_link::MediaState &state) override;

private:
    std::shared_ptr<ScreencapPreview> preview_;
    std::shared_ptr<AgentPreview> agent_preview_;
    bool visible_{false};  // between onAppear and onDisappear (LVGL thread)

    lv_obj_t *header_{nullptr};
    lv_obj_t *control_container_{nullptr};
    lv_obj_t *preview_container_{nullptr};
    lv_obj_t *preview_image_{nullptr};
    lv_obj_t *tools_container_{nullptr};

    // summary header widgets + state
    lv_obj_t *name_label_{nullptr};
    lv_obj_t *sub_label_{nullptr};
    lv_obj_t *batt_icon_{nullptr};
    lv_obj_t *batt_pct_{nullptr};
    lv_obj_t *wifi_icon_{nullptr};
    lv_obj_t *cell_icon_{nullptr};
    lv_timer_t *summary_timer_{nullptr};
    bool summary_inflight_{false};
    std::string model_;  // banner model, the pre-exec fallback

    // Now-playing media card (under the preview/tools row). Always shown; the
    // transport controls render disabled when nothing is playing.
    lv_obj_t *media_card_{nullptr};
    lv_obj_t *media_icon_{nullptr};
    lv_obj_t *media_text_box_{nullptr};
    lv_obj_t *media_title_{nullptr};
    lv_obj_t *media_artist_{nullptr};
    lv_obj_t *media_play_icon_{nullptr};  // play/pause glyph (reflects state)
    lv_obj_t *media_prev_btn_{nullptr};
    lv_obj_t *media_next_btn_{nullptr};
    lv_obj_t *media_play_btn_{nullptr};
    bool media_inflight_{false};
    bool media_playing_{false};  // last known play state (for the optimistic flip)
    lv_timer_t *media_poll_timer_{nullptr};  // one-shot re-fetch after a control tap

    // Agent-driven media (Normal mode): the MEDIA channel pushes state in real time
    // and the agent renders the album art (ARGB8888) + title/artist (so the Tab5
    // needs no CJK fonts). These widgets/buffers overlay the Limited-mode glyph +
    // labels above; only one set shows per mode.
    bool agent_media_{false};            // the MEDIA channel drives the card
    uint32_t media_token_{0};            // content_token we have art/text rendered for
    bool media_render_inflight_{false};
    bool media_art_loaded_{false};       // the last render for this token included art
    bool media_art_tried_{false};        // a with-art fetch was attempted for this token
    agent_link::MediaState last_media_{};  // newest pushed state (LVGL thread)
    lv_obj_t *media_art_{nullptr};        // album art (lv_image; hidden when no art)
    lv_obj_t *media_title_img_{nullptr};  // agent-rendered title (ARGB8888)
    lv_obj_t *media_artist_img_{nullptr}; // agent-rendered artist (ARGB8888)
    lv_image_dsc_t art_dsc_{};
    lv_image_dsc_t title_dsc_{};
    lv_image_dsc_t artist_dsc_{};

    void createHeader();
    void refreshSummary();
    void createMediaCard();
    void applyMedia(const app::mediainfo::NowPlaying &np);  // LVGL thread (Limited)
    void applyMediaState(const agent_link::MediaState &st); // LVGL thread (Normal)
    void maybeFetchRender();                                // LVGL thread (Normal)
    void fetchMediaRender(bool has_art);                    // LVGL thread (Normal)
    void freeMediaBitmaps();                                // LVGL thread
    void refreshMedia();  // media-only fetch, for a snappy post-control update (Limited)
    void dispatchMedia(const char *key);  // transport control (agent or exec)
    void createPreviewContainer();
    void createNavBar();  // Back/Home/Recents/Power row under the preview
    void createToolsContainer();
    void startPreview();   // pick + start the mode's preview (LVGL thread)
    void stopPreview();
    void onPreviewTapped();
    void openPowerMenu();  // power off / reboot / sleep actions on the device
};
