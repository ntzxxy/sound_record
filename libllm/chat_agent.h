#ifndef CHAT_AGENT_H
#define CHAT_AGENT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 流式输出回调（与 llm_callback_t 签名一致）
 * @param text     本次输出的文本片段（可能为空）
 * @param is_final 1 表示推理完成，0 表示还有后续
 */
typedef void (*agent_callback_t)(const char *text, int is_final);

/**
 * 初始化对话代理（加载 LLM 模型并设置系统提示词）
 * @param model_path    GGUF 模型文件路径
 * @param system_prompt 系统提示词（可为 NULL，使用默认值）
 * @return 0 成功，-1 失败
 */
int agent_init(const char *model_path, const char *system_prompt);

/**
 * 发起对话（流式输出，追加到对话上下文中）
 * 首轮自动附带 system prompt，后续轮次只追加用户消息。
 * @param user_message  用户输入文本
 * @param callback      每生成一段文字时回调
 * @return 0 成功，-1 失败（未初始化或推理出错）
 */
int agent_chat(const char *user_message, agent_callback_t callback);

/**
 * 发起带本轮运行时上下文的对话（流式输出，兼容原历史管理）
 * runtime_context 只注入本轮 prompt，不作为用户原始消息写入长期历史。
 * @param user_message     用户输入文本
 * @param runtime_context  本轮相关系统记忆/上下文（可为 NULL 或空）
 * @param callback         每生成一段文字时回调
 * @return 0 成功，-1 失败
 */
int agent_chat_with_context(const char *user_message,
                            const char *runtime_context,
                            agent_callback_t callback);

/**
 * 重置对话上下文，开始新一轮对话（下次 agent_chat 将重新附加 system prompt）
 */
void agent_reset(void);

/**
 * 销毁对话代理，释放 LLM 资源
 */
void agent_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_AGENT_H */
