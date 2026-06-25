#ifndef ASR_H
#define ASR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 ASR 引擎（加载模型）
 * @param model_dir  模型目录路径，内含 encoder.onnx / decoder.onnx / joiner.onnx / tokens.txt
 * @return 0 成功，-1 失败
 */
int asr_init(const char *model_dir);

/**
 * 喂入一段 int16 PCM 音频数据（16000 Hz 单声道）
 * @param pcm         int16 PCM 数据指针
 * @param num_samples 采样点数
 * @return 实际消费的采样点数，<0 表示错误
 */
int asr_process_frame(const int16_t *pcm, int num_samples);

/**
 * 获取当前识别结果
 * @return 识别文本（可能为空字符串），指针有效期至下一次 asr_* 调用
 */
const char *asr_get_result(void);

/**
 * 检测是否到达句子终点
 * @return 1 表示检测到端点，0 表示未到
 */
int asr_is_endpoint(void);

/**
 * 重置识别流（端点后调用，准备下一句）
 */
void asr_reset(void);

/**
 * 销毁 ASR 引擎，释放资源
 */
void asr_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* ASR_H */
