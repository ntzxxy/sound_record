#include "llm.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "llama.h"

// 模块内部状态
static llama_model   *g_model   = nullptr;
static llama_context *g_ctx    = nullptr;
static llama_context *g_intent_ctx = nullptr;
static const llama_vocab *g_vocab  = nullptr;
static llama_sampler *g_smpl   = nullptr;
static bool g_is_gemma4 = false;
static std::mutex g_metrics_mutex;
static llm_generation_metrics_t g_last_chat_metrics{};

// 语音场景只需要一句可播报的答复；限制输出长度可减少生成时间和无效续写。
static int g_n_predict = 128;
static int g_total_tokens = 0;  // 追踪上下文中的 token 总数

// Orin NX 16 GB can accommodate an 8K chat KV cache for this Q4 model.  The
// batch size is deliberately smaller than the context: prefill is chunked so
// long history rebuilds never exceed llama.cpp's per-decode batch limit.
static constexpr int kChatContextTokens = 8192;
static constexpr int kChatBatchTokens = 1024;
static constexpr int kIntentContextTokens = 4096;
static constexpr int kIntentBatchTokens = 4096;

// ======================================================================

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int tokenize_text(const std::string& text, std::vector<llama_token>& tokens) {
    int n = -llama_tokenize(g_vocab, text.c_str(), text.size(),
                            nullptr, 0, true, true);
    if (n <= 0) return -1;
    tokens.resize(n);
    if (llama_tokenize(g_vocab, text.c_str(), text.size(),
                       tokens.data(), n, true, true) < 0) {
        return -1;
    }
    return n;
}

// llama_batch_get_one() tracks positions automatically for its single sequence.
// Feeding a prompt in consecutive chunks therefore preserves the same KV cache
// as a single prefill while keeping every llama_decode() call within n_batch.
static int decode_tokens_in_batches(llama_context* context,
                                    std::vector<llama_token>& tokens,
                                    int batch_limit) {
    if (!context || tokens.empty() || batch_limit <= 0) return -1;

    for (size_t offset = 0; offset < tokens.size();) {
        const int count = static_cast<int>(std::min(
            tokens.size() - offset, static_cast<size_t>(batch_limit)));
        llama_batch batch = llama_batch_get_one(tokens.data() + offset, count);
        if (llama_decode(context, batch)) return -1;
        offset += static_cast<size_t>(count);
    }
    return 0;
}

static int format_prompt_internal(const char *system_prompt, const char *user_message,
                                  char *buf, int buf_size) {
    std::vector<llm_chat_message_t> messages;
    if (system_prompt && system_prompt[0] != '\0') {
        messages.push_back({"system", system_prompt});
    }
    messages.push_back({"user", user_message ? user_message : ""});
    return llm_format_messages(messages.data(), static_cast<int>(messages.size()),
                               1, buf, buf_size);
}

// Gemma 4 的官方模板是完整的 Jinja 模板；llama_chat_apply_template 的 C 接口
// 目前仅支持一部分预定义模板。文本对话不涉及工具或多模态内容时，以下格式与
// Gemma 4 官方模板等价，且默认关闭 thinking，适合低延迟语音交互。
static int format_gemma4_text_chat(const std::vector<llama_chat_message>& msgs,
                                   bool add_generation_prompt, char *buf, int buf_size) {
    // tokenize_text() 会按模型元数据自动添加 BOS；这里不能重复写入 <bos>。
    std::string prompt;
    for (const auto& msg : msgs) {
        const char *role = std::strcmp(msg.role, "assistant") == 0 ? "model" : msg.role;
        prompt += "<|turn>";
        prompt += role;
        prompt += '\n';
        prompt += msg.content;
        prompt += "<turn|>\n";
    }
    if (add_generation_prompt) prompt += "<|turn>model\n";

    const int len = static_cast<int>(prompt.size());
    if (buf_size > 0) {
        const int copy_len = std::min(len, buf_size - 1);
        if (copy_len > 0) std::memcpy(buf, prompt.data(), copy_len);
        buf[copy_len] = '\0';
    }
    return len;
}

int llm_format_messages(const llm_chat_message_t *messages, int message_count,
                        int add_generation_prompt, char *buf, int buf_size) {
    if (!g_model || !messages || message_count <= 0 || !buf || buf_size <= 1) return -1;

    std::vector<llama_chat_message> msgs;
    msgs.reserve(message_count);
    for (int i = 0; i < message_count; ++i) {
        if (!messages[i].role || !messages[i].content) return -1;
        msgs.push_back({messages[i].role, messages[i].content});
    }

    int len;
    if (g_is_gemma4) {
        len = format_gemma4_text_chat(msgs, add_generation_prompt != 0, buf, buf_size);
    } else {
        const char *tmpl = llama_model_chat_template(g_model, nullptr);
        len = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                        add_generation_prompt != 0, buf, buf_size);
    }
    if (len < 0) {
        fprintf(stderr, "[LLM] Chat template 格式化失败\n");
        return -1;
    }
    return len;
}

int llm_init(const char *model_path) {
    // 抑制 CUDA graph 等 DEBUG 日志
    ggml_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);

    // 1. 加载 GPU/CPU 后端
    ggml_backend_load_all();

    // 2. 加载模型
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 99; // 全部层卸载到 GPU

    g_model = llama_model_load_from_file(model_path, model_params);
    if (!g_model) {
        fprintf(stderr, "[LLM] 模型加载失败: %s\n", model_path);
        return -1;
    }
    g_vocab = llama_model_get_vocab(g_model);
    char architecture[64] = {};
    g_is_gemma4 = llama_model_meta_val_str(g_model, "general.architecture",
                                            architecture, sizeof(architecture)) >= 0 &&
                  std::strcmp(architecture, "gemma4") == 0;

    // 3. 创建上下文
    llama_context_params ctx_params = llama_context_default_params();
    // Gemma 4 虽支持 128K 上下文，但 KV 缓存随窗口线性占用显存。
    // Orin NX 16 GB 使用 8K 聊天窗口；长 prompt 会按 1024 tokens 分块 prefill。
    ctx_params.n_ctx   = kChatContextTokens;
    ctx_params.n_batch = kChatBatchTokens;

    g_ctx = llama_init_from_model(g_model, ctx_params);
    if (!g_ctx) {
        fprintf(stderr, "[LLM] 上下文创建失败\n");
        llama_model_free(g_model);
        g_model = nullptr;
        return -1;
    }

    llama_context_params intent_params = llama_context_default_params();
    // 意图提示词本身包含 JSON 约束和示例，1536 tokens 会使长用户输入
    // 没有足够空间输出完整 JSON，继而错误回退为 CLARIFY。
    intent_params.n_ctx = kIntentContextTokens;
    intent_params.n_batch = kIntentBatchTokens;
    g_intent_ctx = llama_init_from_model(g_model, intent_params);
    if (!g_intent_ctx) {
        fprintf(stderr, "[LLM] 意图分类上下文创建失败\n");
        llama_free(g_ctx);
        g_ctx = nullptr;
        llama_model_free(g_model);
        g_model = nullptr;
        return -1;
    }

    // 4. 初始化采样器
    // top_k=1 在 llama.cpp 中等价于确定性贪心 → 会无限重复，不可用
    // 使用 top_k=40 给模型足够的候选 token 避免重复循环
    auto sparams = llama_sampler_chain_default_params();
    g_smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(g_smpl, llama_sampler_init_penalties(
        -1,       // penalty_last_n
        1.15f,    // penalty_repeat: 1.15（比 RKLLM 的 1.1 更激进防重复）
        0.0f,     // penalty_freq
        0.0f));   // penalty_present
    llama_sampler_chain_add(g_smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(g_smpl, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(g_smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(g_smpl, llama_sampler_init_dist(time(nullptr)));

    fprintf(stderr, "[LLM] 模型加载成功\n");
    return 0;
}

int llm_chat(const char *prompt, llm_callback_t callback) {
    if (!g_model || !g_ctx || !g_vocab || !g_smpl || !prompt || !callback) {
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_metrics_mutex);
        g_last_chat_metrics = {};
    }

    long long total_begin_ms = now_ms();

    // 1. Tokenize 输入
    std::string prompt_str(prompt);
    std::vector<llama_token> prompt_tokens;
    int n_prompt = tokenize_text(prompt_str, prompt_tokens);
    if (n_prompt <= 0) return -1;
    if (g_total_tokens + n_prompt > kChatContextTokens) {
        fprintf(stderr, "[LLM] 聊天上下文超出 %d tokens，需重建或裁剪历史\n",
                kChatContextTokens);
        return -1;
    }

    // 2. 处理 prompt
    long long prompt_decode_begin_ms = now_ms();
    if (decode_tokens_in_batches(g_ctx, prompt_tokens, kChatBatchTokens)) {
        return -1;
    }
    long long generation_begin_ms = now_ms();
    g_total_tokens += n_prompt;

    // 3. 逐 token 生成
    llama_token new_token_id;
    bool eog_reached = false;
    bool first_piece = true;
    long long first_token_ms = 0;
    int generated_tokens = 0;
    int i;
    for (i = 0; i < g_n_predict; i++) {
        if (g_total_tokens >= kChatContextTokens) break;
        new_token_id = llama_sampler_sample(g_smpl, g_ctx, -1);

        if (llama_vocab_is_eog(g_vocab, new_token_id)) {
            eog_reached = true;
            break;  // 生成结束
        }

        char buf[256];
        int n = llama_token_to_piece(g_vocab, new_token_id,
                                     buf, sizeof(buf), 0, false);
        if (n > 0) {
            std::string text(buf, n);
            if (first_piece) {
                first_token_ms = now_ms();
                first_piece = false;
            }
            callback(text.c_str(), 0);
        }

        llama_batch batch = llama_batch_get_one(&new_token_id, 1);
        if (llama_decode(g_ctx, batch)) {
            break;
        }
        g_total_tokens++;
        generated_tokens++;
    }

    callback("", 1);
    long long end_ms = now_ms();
    long long ttft_ms = first_token_ms > 0 ? first_token_ms - total_begin_ms : -1;
    long long prompt_decode_ms = generation_begin_ms - prompt_decode_begin_ms;
    long long decode_ms = end_ms - generation_begin_ms;
    double tokens_per_s = decode_ms > 0 ? generated_tokens * 1000.0 / decode_ms : 0.0;
    {
        std::lock_guard<std::mutex> lock(g_metrics_mutex);
        g_last_chat_metrics = {
            1,
            n_prompt,
            generated_tokens,
            ttft_ms,
            prompt_decode_ms,
            decode_ms,
            tokens_per_s,
            eog_reached ? 0 : 1,
        };
    }
    fprintf(stderr,
            "[METRIC] llm prompt_tokens=%d output_tokens=%d ttft_ms=%lld prompt_decode_ms=%lld decode_ms=%lld tokens_per_s=%.2f truncated=%d\n",
            n_prompt, generated_tokens, ttft_ms, prompt_decode_ms, decode_ms,
            tokens_per_s, eog_reached ? 0 : 1);
    // 返回 1 表示被 token 上限截断（未到 EOS）
    return eog_reached ? 0 : 1;
}

int llm_get_last_chat_metrics(llm_generation_metrics_t *metrics) {
    if (!metrics) return 0;
    std::lock_guard<std::mutex> lock(g_metrics_mutex);
    *metrics = g_last_chat_metrics;
    return metrics->valid;
}

int llm_generate_once(const char *system_prompt,
                      const char *user_message,
                      const llm_once_params_t *params,
                      char *output,
                      int output_size,
                      int *latency_ms) {
    if (!g_model || !g_intent_ctx || !g_vocab || !user_message || !output || output_size <= 1) {
        return -1;
    }

    const int max_tokens = params && params->max_tokens > 0 ? params->max_tokens : 160;
    const float temperature = params ? params->temperature : 0.0f;
    output[0] = '\0';
    if (latency_ms) *latency_ms = 0;

    long long begin_ms = now_ms();
    llama_memory_clear(llama_get_memory(g_intent_ctx), true);

    char prompt_buf[8192];
    int len = format_prompt_internal(system_prompt, user_message,
                                     prompt_buf, sizeof(prompt_buf));
    if (len < 0 || len >= static_cast<int>(sizeof(prompt_buf))) return -1;

    std::vector<llama_token> prompt_tokens;
    int n_prompt = tokenize_text(std::string(prompt_buf, len), prompt_tokens);
    if (n_prompt <= 0) return -1;
    if (n_prompt > kIntentContextTokens) {
        fprintf(stderr, "[LLM] 意图提示词超出 %d tokens\n", kIntentContextTokens);
        return -1;
    }

    if (decode_tokens_in_batches(g_intent_ctx, prompt_tokens, kIntentBatchTokens)) return -1;

    auto sparams = llama_sampler_chain_default_params();
    llama_sampler *sampler = llama_sampler_chain_init(sparams);
    if (!sampler) return -1;
    if (temperature <= 0.0f) {
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(20));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(1));
    }

    int written = 0;
    bool eog_reached = false;
    for (int i = 0; i < max_tokens; ++i) {
        if (n_prompt + i >= kIntentContextTokens) break;
        llama_token token = llama_sampler_sample(sampler, g_intent_ctx, -1);
        if (llama_vocab_is_eog(g_vocab, token)) {
            eog_reached = true;
            break;
        }

        char piece[256];
        int n = llama_token_to_piece(g_vocab, token, piece, sizeof(piece), 0, false);
        if (n > 0 && written < output_size - 1) {
            int copy_n = std::min(n, output_size - 1 - written);
            std::memcpy(output + written, piece, copy_n);
            written += copy_n;
            output[written] = '\0';
        }

        llama_batch batch = llama_batch_get_one(&token, 1);
        if (llama_decode(g_intent_ctx, batch)) break;
    }

    llama_sampler_free(sampler);
    if (latency_ms) *latency_ms = static_cast<int>(now_ms() - begin_ms);
    fprintf(stderr, "[METRIC] intent prompt_tokens=%d output_chars=%d latency_ms=%d truncated=%d\n",
            n_prompt, written, latency_ms ? *latency_ms : -1, eog_reached ? 0 : 1);
    return eog_reached ? 0 : 1;
}

int llm_append_text(const char *text) {
    if (!g_model || !g_ctx || !g_vocab || !text || !text[0]) return -1;

    std::string s(text);
    std::vector<llama_token> tokens;
    int n = tokenize_text(s, tokens);
    if (n <= 0) return -1;

    if (g_total_tokens + n > kChatContextTokens) {
        fprintf(stderr, "[LLM] 历史重建超出 %d tokens，拒绝写入 KV cache\n",
                kChatContextTokens);
        return -1;
    }
    if (decode_tokens_in_batches(g_ctx, tokens, kChatBatchTokens)) return -1;
    g_total_tokens += n;
    return 0;
}

void llm_reset_context(void) {
    if (g_ctx) {
        llama_memory_clear(llama_get_memory(g_ctx), true);
        g_total_tokens = 0;
    }
}

int llm_get_context_size(void) {
    return g_total_tokens;
}

int llm_format_prompt(const char *system_prompt, const char *user_message,
                      char *buf, int buf_size) {
    return format_prompt_internal(system_prompt, user_message, buf, buf_size);
}

void llm_destroy(void) {
    if (g_smpl) { llama_sampler_free(g_smpl); g_smpl = nullptr; }
    if (g_intent_ctx) { llama_free(g_intent_ctx); g_intent_ctx = nullptr; }
    if (g_ctx)  { llama_free(g_ctx);         g_ctx  = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }
    g_is_gemma4 = false;
}
