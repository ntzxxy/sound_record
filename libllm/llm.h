#ifndef LLM_H
#define LLM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 流式输出回调
 * @param text     本次输出的文本片段（可能为空）
 * @param is_final 1 表示推理完成，0 表示还有后续
 */
typedef void (*llm_callback_t)(const char *text, int is_final);

/**
 * 初始化 LLM 引擎（加载模型）
 * @param model_path  GGUF 模型文件路径
 * @return 0 成功，-1 失败
 */
int llm_init(const char *model_path);

/**
 * 发起对话（流式输出，追加到已有上下文中）
 * 不会自动清空 KV Cache——多次调用会累积对话历史。
 * @param prompt    用户输入文本（已格式化好，含 chat template）
 * @param callback  每生成一段文字时回调
 * @return 0 成功，-1 失败
 */
int llm_chat(const char *prompt, llm_callback_t callback);

/**
 * 清空对话上下文（KV Cache），开始新一轮对话
 */
void llm_reset_context(void);

/**
 * 获取当前上下文中的 token 总数（用于滑动窗口管理）
 */
int llm_get_context_size(void);

/**
 * 将文本 tokenize 并 decode 到上下文（仅追加到 KV Cache，不生成 token）。
 * 用于滑动窗口重建历史对话。
 * @param text 要追加的文本
 * @return 0 成功，-1 失败
 */
int llm_append_text(const char *text);

/**
 * 使用模型的 Chat Template 格式化对话 prompt
 * @param system_prompt  系统提示词（可为 NULL）
 * @param user_message   用户消息
 * @param buf            输出缓冲区
 * @param buf_size       缓冲区大小
 * @return 格式化后的字节数，>=buf_size 表示缓冲区不足
 */
int llm_format_prompt(const char *system_prompt, const char *user_message,
                      char *buf, int buf_size);

/**
 * 销毁 LLM 引擎，释放资源
 */
void llm_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_H */
