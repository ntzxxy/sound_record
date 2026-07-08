#include "audio_player_class.h"
#include "audio_player.h"

AudioPlayer::AudioPlayer()  { initialize(); }
AudioPlayer::~AudioPlayer() { cleanup(); }

bool AudioPlayer::initialize() {
    if (initialized_) return true;
    int ret = audio_player_init("default", 44100, 1);
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

void AudioPlayer::stop() {
    audio_player_stop();
}

void AudioPlayer::cleanup() {
    if (initialized_) {
        audio_player_destroy();
        initialized_ = false;
    }
}
