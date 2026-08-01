#include "opus_decoder.hpp"

#include "esp_opus_dec.h"

std::unique_ptr<OpusPacketDecoder> OpusPacketDecoder::create(uint32_t sample_rate,
                                                             uint8_t channels) {
    if (sample_rate != 48000 || channels != 2) return nullptr;
    auto decoder = std::unique_ptr<OpusPacketDecoder>(new OpusPacketDecoder());
    decoder->sample_rate_ = sample_rate;
    decoder->channels_ = channels;

    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = sample_rate;
    cfg.channel = channels;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    cfg.self_delimited = false;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &decoder->handle_) != ESP_AUDIO_ERR_OK) {
        return nullptr;
    }
    return decoder;
}

OpusPacketDecoder::~OpusPacketDecoder() {
    if (handle_) esp_opus_dec_close(handle_);
}

bool OpusPacketDecoder::decode(const uint8_t* packet, size_t packet_len,
                               uint8_t* pcm, size_t pcm_cap, size_t* pcm_len) {
    if (!handle_ || !packet || !packet_len || !pcm || !pcm_len) return false;
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(packet);
    raw.len = packet_len;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = pcm;
    frame.len = pcm_cap;
    esp_audio_dec_info_t info = {};
    if (esp_opus_dec_decode(handle_, &raw, &frame, &info) != ESP_AUDIO_ERR_OK
            || raw.consumed != packet_len) {
        return false;
    }
    *pcm_len = frame.decoded_size;
    return *pcm_len > 0 && *pcm_len <= pcm_cap;
}

bool OpusPacketDecoder::reset() {
    return handle_ && esp_opus_dec_reset(handle_) == ESP_AUDIO_ERR_OK;
}
