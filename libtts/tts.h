#ifndef TTS_H
#define TTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TTS 音频输出回调（流式，逐段回调 PCM 数据）
 * @param samples  int16 PCM 音频数据
 * @param n        采样点数
 * @param progress 生成进度 0.0~1.0（1.0 表示合成完成）
 */
typedef void (*tts_callback_t)(const int16_t *samples, int n, float progress);

/**
 * 初始化 TTS 引擎（加载 ONNX 模型）
 * @param model_dir 模型目录，需包含 model.onnx 和 tokens.txt
 * @return 0 成功，-1 失败
 */
int tts_init(const char *model_dir);

/**
 * 将文本转为语音（通过回调流式输出 int16 PCM）
 * @param text     输入文本
 * @param callback PCM 数据回调（在调用线程中同步回调）
 * @return 0 成功，-1 失败
 */
int tts_speak(const char *text, tts_callback_t callback);

/**
 * 获取合成音频的采样率
 */
int tts_sample_rate(void);

/**
 * 销毁 TTS 引擎，释放资源
 */
void tts_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* TTS_H */
