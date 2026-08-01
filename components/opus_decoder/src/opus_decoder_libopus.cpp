#include "opus_decoder.hpp"

#include <opus/opus.h>

std::unique_ptr<OpusPacketDecoder> OpusPacketDecoder::create(uint32_t sample_rate,
                                                             uint8_t channels) {
    if (sample_rate != 48000 || channels != 2) return nullptr;
    auto decoder = std::unique_ptr<OpusPacketDecoder>(new OpusPacketDecoder());
    decoder->sample_rate_ = sample_rate;
    decoder->channels_ = channels;

    int error = OPUS_OK;
    decoder->handle_ = opus_decoder_create(sample_rate, channels, &error);
    if (!decoder->handle_ || error != OPUS_OK) return nullptr;
    return decoder;
}

OpusPacketDecoder::~OpusPacketDecoder() {
    if (handle_) opus_decoder_destroy(static_cast<OpusDecoder*>(handle_));
}

bool OpusPacketDecoder::decode(const uint8_t* packet, size_t packet_len,
                               uint8_t* pcm, size_t pcm_cap, size_t* pcm_len) {
    if (!handle_ || !packet || !packet_len || !pcm || !pcm_len) return false;
    constexpr int kFrameSamples = 48000 * 20 / 1000;
    if (pcm_cap < static_cast<size_t>(kFrameSamples * channels_ * 2)) return false;
    int samples = opus_decode(static_cast<OpusDecoder*>(handle_), packet,
                              static_cast<opus_int32>(packet_len),
                              reinterpret_cast<opus_int16*>(pcm), kFrameSamples, 0);
    if (samples < 0) return false;
    *pcm_len = static_cast<size_t>(samples) * channels_ * 2;
    return *pcm_len > 0 && *pcm_len <= pcm_cap;
}

bool OpusPacketDecoder::reset() {
    return handle_
            && opus_decoder_ctl(static_cast<OpusDecoder*>(handle_), OPUS_RESET_STATE) == OPUS_OK;
}
