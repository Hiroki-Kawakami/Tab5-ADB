#include "agent_audio.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "agent_client.hpp"  // app::agent_client
#include "bsp.h"             // bsp_audio_*
#include "esp_heap_caps.h"

namespace {
// ~340 ms jitter buffer at 48 kHz / stereo / 16-bit (192 KB/s). Drop-oldest past
// this, so a stalled consumer never backs the reader thread up (§6.3).
constexpr size_t kRingCap = 64 * 1024;
// Drain granularity handed to bsp_audio_write (it blocks on the I2S DMA). 2 KB =
// 512 stereo frames ≈ 10.6 ms; small enough to keep on the stack.
constexpr size_t kChunk = 2048;
// Tab5 playback volume for the mirror (0..100). The BSP curve is linear-in-dB over
// a -40 dB span (vol=100 -> 0 dB unity, the loudest without digital gain > 1), so
// default to 100: the mirror should play the captured stream at full level. A
// settings/overlay control can lower it later. Beyond unity the loudness is bounded
// by the source level (the phone's media volume scales the REMOTE_SUBMIX capture —
// the overlay Vol+/- keys raise it) and the speaker hardware.
constexpr int kVolume = 100;
}  // namespace

std::shared_ptr<AgentAudio> AgentAudio::create() {
    auto a = std::shared_ptr<AgentAudio>(new AgentAudio());
    a->ring_ = static_cast<uint8_t*>(heap_caps_malloc(kRingCap, MALLOC_CAP_SPIRAM));
    if (!a->ring_) return nullptr;
    a->ring_cap_ = kRingCap;
    a->done_ = xSemaphoreCreateBinary();
    return a;
}

AgentAudio::AgentAudio() = default;

AgentAudio::~AgentAudio() {
    stop();
    if (done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(done_));
    if (ring_) heap_caps_free(ring_);
}

void AgentAudio::start() {
    auto link = app::agent_client().link();
    if (!link) return;  // not Ready — the caller gates on ensure_connected
    stop_.store(false);
    stopped_.store(false);
    started_.store(false);
    { std::lock_guard<std::mutex> lk(ring_mtx_); ring_head_ = ring_tail_ = ring_count_ = 0; }

    if (!task_) {
        TaskHandle_t t = nullptr;
        // Core 1, priority 4 — above the preview decoder, below the adb reader /
        // LVGL: an audio underrun is more audible than a dropped video frame.
        xTaskCreatePinnedToCore(&AgentAudio::audio_trampoline, "agent_audio",
                                8192, this, 4, &t, 1);
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
    rate_.store(info.sample_rate ? info.sample_rate : 48000);
    channels_.store(info.channels ? info.channels : 2);
    started_.store(true);  // unblocks the task's bsp_audio_open
}

void AgentAudio::on_audio_data(agent_link::Link*, const uint8_t* pcm, size_t len) {
    ring_write(pcm, len);  // copy only — never block the reader thread
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

void AgentAudio::audio_trampoline(void* arg) { static_cast<AgentAudio*>(arg)->audio_loop(); }

void AgentAudio::audio_loop() {
    // Wait for the format (on_audio_started) before opening the sink.
    while (!started_.load() && !stop_.load()) vTaskDelay(pdMS_TO_TICKS(5));

    bool opened = false;
    if (!stop_.load()) {
        // Stereo PCM; the BSP DSP downmixes to the (mono-wired) speaker and keeps
        // stereo on the headphone, so we just hand it the source format.
        if (bsp_audio_open(rate_.load(), 16, channels_.load()) == ESP_OK) {
            bsp_audio_set_volume(kVolume);
            opened = true;
        }
        // No PCM path (caps lack CAP_PCM) -> drain the ring silently.
    }

    uint8_t buf[kChunk];
    while (!stop_.load()) {
        size_t n = ring_read(buf, sizeof(buf));
        if (n == 0) {  // underrun: wait for more PCM (the last frame holds)
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (opened) bsp_audio_write(buf, n);  // blocks on I2S DMA = real-time pacing
    }

    if (opened) bsp_audio_close();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
    vTaskDelete(nullptr);
}
