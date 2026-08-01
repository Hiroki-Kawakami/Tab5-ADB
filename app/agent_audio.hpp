#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "agent_link.hpp"  // agent_link::AudioListener

// Plays the tab5adb-agent AUDIO stream (protocol.md §6) on the Tab5 speaker /
// headphone — the audio analogue of AgentPreview. A feature that rides the
// established agent link and is created/destroyed with the mirror screen; used
// only in the Tab5Only audio mode (PhoneOnly streams no audio, so no AgentAudio).
//
// Threading: on_audio_data fires on the adb reader thread and ONLY copies the codec
// unit into PSRAM (drop-oldest on overflow) — it never blocks, so the
// per-A_WRTE flow control that gates the video stream is never stalled by audio.
// A private FreeRTOS audio task drains the ring into bsp_audio_write (which blocks
// on the I2S DMA = the natural real-time pacing). The task owns every bsp_audio_*
// call, so open / write / close all happen on one thread.
class AgentAudio : public agent_link::AudioListener,
                   public std::enable_shared_from_this<AgentAudio> {
public:
    // Allocate the ring + sync primitives. Returns nullptr if the ring can't be
    // allocated. Create on the LVGL thread.
    static std::shared_ptr<AgentAudio> create();
    ~AgentAudio() override;

    // Register on the agent link (set_audio_listener) and start the audio task.
    // Call BEFORE the mirror screen's start_mirror() (which must include AUDIO in
    // streams) so on_audio_started is delivered. The agent must be Ready.
    void start();
    // Clear the audio listener (the link STAYS open — the AgentClient contract),
    // stop the task, close bsp_audio. Idempotent.
    void stop();

    // agent_link::AudioListener — fire on the adb reader thread.
    void on_audio_started(agent_link::Link* link, const agent_link::AudioInfo& info) override;
    void on_audio_data(agent_link::Link* link, const uint8_t* data, size_t len) override;

private:
    AgentAudio();

    static void audio_trampoline(void* arg);
    void audio_loop();  // audio task: open -> drain ring -> bsp_audio_write -> close

    // PSRAM byte ring (SPSC: the reader thread writes, the audio task reads),
    // guarded by a short-held mutex. Overflow drops the OLDEST audio (a glitch)
    // rather than blocking the reader.
    size_t ring_write(const uint8_t* data, size_t len);  // reader thread
    size_t ring_read(uint8_t* out, size_t want);         // audio task

    // Opus keeps packet boundaries: five 20 ms packets form the 100 ms startup /
    // recovery buffer, while the queue can absorb up to one second of bursts.
    static constexpr size_t kOpusPacketCap = 1536;
    static constexpr size_t kOpusQueueCap = 50;
    static constexpr size_t kOpusPrebufferPackets = 5;
    bool opus_write(const uint8_t* data, size_t len);  // reader thread
    bool opus_read(uint8_t* out, size_t cap, size_t* len);  // audio task
    size_t opus_count();

    uint8_t* ring_ = nullptr;
    size_t ring_cap_ = 0;
    size_t ring_head_ = 0;   // write offset
    size_t ring_tail_ = 0;   // read offset
    size_t ring_count_ = 0;  // bytes currently buffered
    std::mutex ring_mtx_;
    uint8_t* opus_packets_ = nullptr;  // kOpusQueueCap fixed-size PSRAM slots
    uint16_t* opus_lengths_ = nullptr;
    size_t opus_head_ = 0;
    size_t opus_tail_ = 0;
    size_t opus_count_ = 0;
    bool opus_reset_ = false;

    // Stream format from on_audio_started (reader thread -> audio task). started_
    // gates the task's bsp_audio_open.
    std::atomic<bool> started_{false};
    std::atomic<uint32_t> rate_{48000};
    std::atomic<uint8_t> channels_{2};
    std::atomic<uint8_t> codec_{agent_link::kAudioCodecPcmS16le};
    std::atomic<uint32_t> generation_{0};

    void* task_ = nullptr;  // TaskHandle_t
    void* done_ = nullptr;  // binary sem: the audio task exited
    std::atomic<bool> stop_{false};
    std::atomic<bool> stopped_{false};  // stop() idempotency
};
