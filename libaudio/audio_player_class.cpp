#include "audio_player_class.h"
#include "audio_player.h"

AudioPlayer::AudioPlayer()  { initialize(); }
AudioPlayer::AudioPlayer(int sample_rate) : sample_rate_(sample_rate) { initialize(sample_rate); }
AudioPlayer::~AudioPlayer() { cleanup(); }

bool AudioPlayer::initialize() {
    return initialize(sample_rate_);
}

bool AudioPlayer::initialize(int sample_rate) {
    if (initialized_) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : sample_rate_;
    int ret = audio_player_init("default", sample_rate_, 1);
    initialized_ = (ret == 0);
    return initialized_;
}

void AudioPlayer::play(const int16_t *audioData, int audio_len, float /*speed*/) {
    if (!initialized_) {
        // 尝试初始化（ALSA不可用时 silent fail，WAV fallback 自动生效）
        if (!initialize()) return;
    }
    audio_player_play(audioData, audio_len);
}

void AudioPlayer::play_chunk(const int16_t *audioData, int audio_len, bool drain) {
    if (!initialized_) {
        if (!initialize()) return;
    }
    audio_player_play_chunk(audioData, audio_len, drain ? 1 : 0);
}

void AudioPlayer::stop() {
    audio_player_stop();
}

void AudioPlayer::cleanup() {
    if (initialized_) {
        audio_player_destroy();
        initialized_ = false;
    }
}
