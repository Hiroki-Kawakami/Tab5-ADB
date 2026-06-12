#pragma once
#include <cstdint>

// App-wide user settings, persisted in NVS (the nvs_flash C API on both targets,
// per the idf_compat rule). A settings screen lands later and writes the same
// keys; for now these accessors back the features that need a setting, with a
// sensible default when the key is unset.
namespace app {

// Where mirror audio is played (protocol.md §6.1). The default (zero / unset key)
// is Tab5Only.
enum class AudioOutputMode : uint8_t {
    Tab5Only = 0,   // capture the device's audio to the Tab5; the phone is muted
    PhoneOnly = 1,  // no transfer — the phone plays its own audio (Tab5 silent)
};

// Read the persisted audio output mode (default Tab5Only). Cheap; callable from
// the LVGL thread.
AudioOutputMode audio_output_mode();

// Persist the audio output mode (the settings screen / future UI).
void set_audio_output_mode(AudioOutputMode mode);

// Display backlight brightness, 0..100. Default 80. Applied live via
// bsp_display_set_brightness() and re-applied at boot from this stored value.
int  display_brightness();
void set_display_brightness(int level);

// Panel color depth. The framebuffer pixel format is fixed for the boot (the BSP
// allocates it at bsp_init), so changing this needs a restart to take effect; the
// stored value is read in adb_app() to pick the boot pixel format. Default Color24
// (the zero / unset key, matching the boot default RGB888).
enum class ColorDepth : uint8_t {
    Color24 = 0,  // RGB888 (24-bit) — default
    Color16 = 1,  // RGB565 (16-bit)
};
ColorDepth display_color_depth();
void set_display_color_depth(ColorDepth depth);

}  // namespace app
