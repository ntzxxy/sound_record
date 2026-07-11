#ifndef TTS_PIPELINE_H
#define TTS_PIPELINE_H

#include <string>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tts_output_start_t)(int sample_rate, int channels);
typedef void (*tts_output_pcm_t)(const int16_t *samples, int n, int is_last);
typedef void (*tts_output_end_t)(void);
typedef void (*tts_output_cancel_t)(void);

/**
 * 设置 TTS 输出回调。未设置时默认本地 ALSA 播放。
 */
void tts_pipeline_set_output(tts_output_start_t start_cb,
                             tts_output_pcm_t pcm_cb,
                             tts_output_end_t end_cb,
                             tts_output_cancel_t cancel_cb);

/**
 * 初始化 TTS 管线（加载模型 + 招募合成+播放双线程）
 * @param tts_model_path  TTS 模型路径/目录
 * @param save_dir        WAV 存储目录（保留接口兼容，当前不落盘）
 * @return 0 成功，-1 失败
 */
int tts_pipeline_init(const char *tts_model_path, const char *save_dir);

/**
 * 将文本片段推入 TTS 传送带（非阻塞，立刻返回）
 */
void tts_pipeline_push(const char *text, int is_final);

/**
 * TTS 管线繁忙标志（从收到文本到播放完成的全程）
 * @return 1 繁忙，0 空闲
 */
int tts_pipeline_is_busy(void);

/**
 * 打断当前操作：清空队列 + 停止播放
 */
void tts_pipeline_interrupt(void);

/**
 * 销毁 TTS 管线（广播下班 + join 线程 + 释放资源）
 */
void tts_pipeline_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // TTS_PIPELINE_H
