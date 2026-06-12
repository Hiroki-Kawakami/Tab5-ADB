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

}  // namespace app
