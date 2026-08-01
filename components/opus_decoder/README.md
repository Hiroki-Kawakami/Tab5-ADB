# opus_decoder

Target-neutral raw Opus packet decoder used by mirror audio. The public API is
`inc/opus_decoder.hpp`; callers provide one 48 kHz stereo 20 ms packet and
receive interleaved signed 16-bit PCM.

The component owns the platform split:

- ESP32-P4 uses the managed `espressif/esp_audio_codec` component.
- The host simulator uses system libopus, linked by `simulator/CMakeLists.txt`.

Keep codec-specific headers and target checks inside this component. App code
should depend only on `OpusPacketDecoder`.
