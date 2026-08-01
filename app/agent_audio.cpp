#include "agent_audio.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "agent_client.hpp"  // app::agent_client
#include "bsp.h"             // bsp_audio_*
#include "esp_heap_caps.h"
#include "settings.hpp"      // app::master_volume
#include "opus_decoder.hpp"

namespace {
// USB PCM ring: ~340 ms at 48 kHz / stereo / 16-bit (192 KB/s). Drop-oldest past
// this, so a stalled consumer never backs the reader thread up (§6.3). Wi-Fi Opus
// uses the packet queue declared in AgentAudio instead.
constexpr size_t kRingCap = 64 * 1024;
// Drain granularity handed to bsp_audio_write (it blocks on the I2S DMA). 2 KB =
// 512 stereo frames ≈ 10.6 ms; small enough to keep on the stack.
constexpr size_t kChunk = 2048;
}  // namespace

std::shared_ptr<AgentAudio> AgentAudio::create() {
    auto a = std::shared_ptr<AgentAudio>(new AgentAudio());
    a->ring_ = static_cast<uint8_t*>(heap_caps_malloc(kRingCap, MALLOC_CAP_SPIRAM));
    if (!a->ring_) return nullptr;
    a->ring_cap_ = kRingCap;
    a->opus_packets_ = static_cast<uint8_t*>(heap_caps_malloc(
        AgentAudio::kOpusQueueCap * AgentAudio::kOpusPacketCap, MALLOC_CAP_SPIRAM));
    a->opus_lengths_ = static_cast<uint16_t*>(heap_caps_malloc(
        AgentAudio::kOpusQueueCap * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!a->opus_packets_ || !a->opus_lengths_) return nullptr;
    a->done_ = xSemaphoreCreateBinary();
    if (!a->done_) return nullptr;
    return a;
}

AgentAudio::AgentAudio() = default;

AgentAudio::~AgentAudio() {
    stop();
    if (done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(done_));
    if (ring_) heap_caps_free(ring_);
    if (opus_packets_) heap_caps_free(opus_packets_);
    if (opus_lengths_) heap_caps_free(opus_lengths_);
}

void AgentAudio::start() {
    auto link = app::agent_client().link();
    if (!link) return;  // not Ready — the caller gates on ensure_connected
    stop_.store(false);
    stopped_.store(false);
    started_.store(false);
    {
        std::lock_guard<std::mutex> lk(ring_mtx_);
        ring_head_ = ring_tail_ = ring_count_ = 0;
        opus_head_ = opus_tail_ = opus_count_ = 0;
        opus_reset_ = false;
    }

    if (!task_) {
        TaskHandle_t t = nullptr;
        // Core 1, priority 4 — above the preview decoder, below the adb reader /
        // LVGL: an audio underrun is more audible than a dropped video frame.
        xTaskCreatePinnedToCore(&AgentAudio::audio_trampoline, "agent_audio",
                                20 * 1024, this, 4, &t, 1);
        task_ = t;
    }
    link->set_audio_listener(weak_from_this());
}

void AgentAudio::stop() {
    if (stopped_.exchange(true)) return;
    // Detach from the link (kept open) so no further on_audio_data arrives.
    if (auto link = app::agent_client().link()) link->set_audio_listener({});
    if (task_) {
        stop_.store(true);
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), portMAX_DELAY);
        task_ = nullptr;
    }
}

void AgentAudio::on_audio_started(agent_link::Link*, const agent_link::AudioInfo& info) {
    {
        std::lock_guard<std::mutex> lk(ring_mtx_);
        ring_head_ = ring_tail_ = ring_count_ = 0;
        opus_head_ = opus_tail_ = opus_count_ = 0;
        opus_reset_ = false;
    }
    rate_.store(info.sample_rate ? info.sample_rate : 48000);
    channels_.store(info.channels ? info.channels : 2);
    codec_.store(info.codec);
    generation_.fetch_add(1);
    started_.store(true);  // unblocks the task's bsp_audio_open
}

void AgentAudio::on_audio_data(agent_link::Link*, const uint8_t* data, size_t len) {
    if (codec_.load() == agent_link::kAudioCodecOpus) opus_write(data, len);
    else ring_write(data, len);  // copy only — never block the reader thread
}

size_t AgentAudio::ring_write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(ring_mtx_);
    if (len > ring_cap_) { data += len - ring_cap_; len = ring_cap_; }  // keep the newest
    // Drop oldest to make room (overflow = a glitch, never a stall).
    if (ring_count_ + len > ring_cap_) {
        size_t drop = ring_count_ + len - ring_cap_;
        ring_tail_ = (ring_tail_ + drop) % ring_cap_;
        ring_count_ -= drop;
    }
    size_t first = std::min(len, ring_cap_ - ring_head_);
    std::memcpy(ring_ + ring_head_, data, first);
    if (len > first) std::memcpy(ring_, data + first, len - first);
    ring_head_ = (ring_head_ + len) % ring_cap_;
    ring_count_ += len;
    return len;
}

size_t AgentAudio::ring_read(uint8_t* out, size_t want) {
    std::lock_guard<std::mutex> lk(ring_mtx_);
    size_t n = std::min(want, ring_count_);
    size_t first = std::min(n, ring_cap_ - ring_tail_);
    std::memcpy(out, ring_ + ring_tail_, first);
    if (n > first) std::memcpy(out + first, ring_, n - first);
    ring_tail_ = (ring_tail_ + n) % ring_cap_;
    ring_count_ -= n;
    return n;
}

bool AgentAudio::opus_write(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > kOpusPacketCap) return false;
    std::lock_guard<std::mutex> lk(ring_mtx_);
    if (opus_count_ == kOpusQueueCap) {
        // A one-second backlog is already stale. Collapse it to the 100 ms target
        // rather than keeping a permanent second of latency, and reset prediction.
        while (opus_count_ >= kOpusPrebufferPackets) {
            opus_tail_ = (opus_tail_ + 1) % kOpusQueueCap;
            opus_count_--;
        }
        opus_reset_ = true;
    }
    std::memcpy(opus_packets_ + opus_head_ * kOpusPacketCap, data, len);
    opus_lengths_[opus_head_] = static_cast<uint16_t>(len);
    opus_head_ = (opus_head_ + 1) % kOpusQueueCap;
    opus_count_++;
    return true;
}

bool AgentAudio::opus_read(uint8_t* out, size_t cap, size_t* len) {
    std::lock_guard<std::mutex> lk(ring_mtx_);
    if (opus_count_ == 0) return false;
    size_t n = opus_lengths_[opus_tail_];
    if (n > cap) return false;
    std::memcpy(out, opus_packets_ + opus_tail_ * kOpusPacketCap, n);
    opus_tail_ = (opus_tail_ + 1) % kOpusQueueCap;
    opus_count_--;
    *len = n;
    return true;
}

size_t AgentAudio::opus_count() {
    std::lock_guard<std::mutex> lk(ring_mtx_);
    return opus_count_;
}

void AgentAudio::audio_trampoline(void* arg) { static_cast<AgentAudio*>(arg)->audio_loop(); }

void AgentAudio::audio_loop() {
    // Wait for the format (on_audio_started) before opening the sink.
    while (!started_.load() && !stop_.load()) vTaskDelay(pdMS_TO_TICKS(5));

    bool opened = false;
    bool prebuffering = false;
    bool played_opus = false;
    uint32_t seen_generation = 0;
    std::unique_ptr<OpusPacketDecoder> opus;
    uint8_t buf[kChunk];
    uint8_t opus_packet[kOpusPacketCap];
    constexpr size_t kOpusPcmBytes = 48000 * 2 * 2 * 20 / 1000;
    uint8_t opus_pcm[kOpusPcmBytes];
    while (!stop_.load()) {
        uint32_t generation = generation_.load();
        if (generation != seen_generation) {
            if (opened) bsp_audio_close();
            opened = false;
            opus.reset();
            seen_generation = generation;
            if (bsp_audio_open(rate_.load(), 16, channels_.load()) == ESP_OK) {
                bsp_audio_set_volume(app::master_volume());
                opened = true;
            }
            if (codec_.load() == agent_link::kAudioCodecOpus) {
                opus = OpusPacketDecoder::create(rate_.load(), channels_.load());
                prebuffering = true;
                played_opus = false;
            }
            continue;
        }

        if (codec_.load() != agent_link::kAudioCodecOpus) {
            size_t n = ring_read(buf, sizeof(buf));
            if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            if (opened) bsp_audio_write(buf, n);
            continue;
        }

        bool reset = false;
        {
            std::lock_guard<std::mutex> lk(ring_mtx_);
            reset = opus_reset_;
            opus_reset_ = false;
        }
        if (reset && opus) {
            opus->reset();
            prebuffering = true;
        }
        if (!opus) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (prebuffering && opus_count() < kOpusPrebufferPackets) {
            if (played_opus && opened) {
                std::memset(opus_pcm, 0, sizeof(opus_pcm));
                bsp_audio_write(opus_pcm, sizeof(opus_pcm));
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }
        prebuffering = false;

        size_t packet_len = 0;
        if (!opus_read(opus_packet, sizeof(opus_packet), &packet_len)) {
            prebuffering = true;
            continue;
        }
        size_t pcm_len = 0;
        if (!opus->decode(opus_packet, packet_len,
                          opus_pcm, sizeof(opus_pcm), &pcm_len)) {
            std::memset(opus_pcm, 0, sizeof(opus_pcm));
            pcm_len = sizeof(opus_pcm);
            opus->reset();
        }
        if (opened) bsp_audio_write(opus_pcm, pcm_len);
        played_opus = true;
    }

    if (opened) bsp_audio_close();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}
