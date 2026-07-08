#ifndef AUDIO_PLAYER_CLASS_H
#define AUDIO_PLAYER_CLASS_H

#include <cstdint>

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool initialize();
    void play(const int16_t *audioData, int audio_len, float speed = 1.0f);
    void stop();
    void cleanup();

private:
    bool initialized_ = false;
};

#endif
