#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化音频播放器（打开 ALSA 设备）
 * @param device    ALSA PCM 设备名（如 "default"），NULL 使用默认
 * @param rate      采样率
 * @param channels  声道数
 * @return 0 成功，-1 失败
 */
int audio_player_init(const char *device, unsigned int rate, unsigned int channels);

/**
 * 播放 PCM 数据（阻塞直到播放完成）
 * @param pcm        int16 PCM 数据
 * @param num_samples 采样点数
 * @return 0 成功，-1 失败
 */
int audio_player_play(const int16_t *pcm, int num_samples);

/**
 * 播放 PCM 分块，可控制是否 drain。流式播放时普通块不 drain，最后一块 drain。
 */
int audio_player_play_chunk(const int16_t *pcm, int num_samples, int drain);

/**
 * 停止当前播放
 */
void audio_player_stop(void);

/**
 * 获取播放器是否正在播放
 * @return 1 正在播放，0 空闲
 */
int audio_player_is_playing(void);

/**
 * 销毁播放器
 */
void audio_player_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
