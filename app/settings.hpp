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

// Tab5-side master playback volume, 0..150. 100 = unity (0 dB): below attenuates,
// 100..150 is a digital boost up to +6 dB (the BSP SW-gain path amplifies above
// unity). Default 100. Consumed by the mirror audio path; applied live via
// bsp_audio_set_volume.
int  master_volume();
void set_master_volume(int volume);

// Speaker route policy. Default Auto (speaker on only while no headphone is
// plugged). The BSP's "always on" mode is intentionally not exposed in the UI.
enum class SpeakerMode : uint8_t {
    Auto = 0,  // default — maps to BSP_AUDIO_SPEAKER_MODE_AUTO
    Off  = 1,  // maps to BSP_AUDIO_SPEAKER_MODE_OFF
};
SpeakerMode speaker_mode();
void set_speaker_mode(SpeakerMode mode);

// The audio_dsp fixed-chain equalizer, on/off. Default on.
bool equalizer_enabled();
void set_equalizer_enabled(bool enabled);

// Whether the app uses the tab5adb-agent on the Android device. Normal starts the
// agent and enables the agent-backed features (screen mirroring, the live preview,
// app icons); Limited never starts it and runs adb-only. Default Normal.
// Consumed by AgentClient::ensure_connected, which short-circuits to Limited mode
// (the agent is never brought up) when this is Limited. Mirrors AgentClient::Mode
// but is the user's *choice*, not the runtime outcome.
enum class AndroidMode : uint8_t {
    Normal  = 0,  // default — start the tab5adb-agent
    Limited = 1,  // never start the agent; adb-only features
};
AndroidMode android_mode();
void set_android_mode(AndroidMode mode);

}  // namespace app
