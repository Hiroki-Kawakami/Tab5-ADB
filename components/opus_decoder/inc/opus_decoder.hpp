#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Target-neutral decoder for one raw Opus packet at a time. The component
// selects Espressif's decoder on device and libopus in the simulator.
class OpusPacketDecoder {
public:
    static std::unique_ptr<OpusPacketDecoder> create(uint32_t sample_rate,
                                                     uint8_t channels);
    ~OpusPacketDecoder();

    bool decode(const uint8_t* packet, size_t packet_len,
                uint8_t* pcm, size_t pcm_cap, size_t* pcm_len);
    bool reset();

private:
    OpusPacketDecoder() = default;
    void* handle_ = nullptr;
    uint32_t sample_rate_ = 0;
    uint8_t channels_ = 0;
};
