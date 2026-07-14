#ifndef AUDIO_PLAYER_CLASS_H
#define AUDIO_PLAYER_CLASS_H

#include <cstdint>

class AudioPlayer {
public:
    AudioPlayer();
    explicit AudioPlayer(int sample_rate);
    ~AudioPlayer();

    bool initialize();
    bool initialize(int sample_rate);
    void play(const int16_t *audioData, int audio_len, float speed = 1.0f);
    void play_chunk(const int16_t *audioData, int audio_len, bool drain);
    void stop();
    void cleanup();

private:
    bool initialized_ = false;
    int sample_rate_ = 44100;
};

#endif
