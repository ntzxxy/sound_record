#include "llm.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

#include "llama.h"

// 模块内部状态
static llama_model   *g_model   = nullptr;
static llama_context *g_ctx    = nullptr;
static const llama_vocab *g_vocab  = nullptr;
static llama_sampler *g_smpl   = nullptr;

static int g_n_predict = 256;  // 足够完成一个完整回答
static int g_total_tokens = 0;  // 追踪上下文中的 token 总数

// ======================================================================

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

    // 3. 创建上下文
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx   = 2048;  // 支持多轮对话
    ctx_params.n_batch = 512;

    g_ctx = llama_init_from_model(g_model, ctx_params);
    if (!g_ctx) {
        fprintf(stderr, "[LLM] 上下文创建失败\n");
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

    long long total_begin_ms = now_ms();

    // 1. Tokenize 输入
    std::string prompt_str(prompt);
    int n_prompt = -llama_tokenize(g_vocab, prompt_str.c_str(), prompt_str.size(),
                                   nullptr, 0, true, true);
    if (n_prompt <= 0) return -1;

    std::vector<llama_token> prompt_tokens(n_prompt);
    if (llama_tokenize(g_vocab, prompt_str.c_str(), prompt_str.size(),
                       prompt_tokens.data(), n_prompt, true, true) < 0) {
        return -1;
    }

    // 2. 处理 prompt
    long long prompt_decode_begin_ms = now_ms();
    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), n_prompt);
    if (llama_decode(g_ctx, batch)) {
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

        batch = llama_batch_get_one(&new_token_id, 1);
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
    fprintf(stderr,
            "[METRIC] llm prompt_tokens=%d output_tokens=%d ttft_ms=%lld prompt_decode_ms=%lld decode_ms=%lld tokens_per_s=%.2f truncated=%d\n",
            n_prompt, generated_tokens, ttft_ms, prompt_decode_ms, decode_ms,
            tokens_per_s, eog_reached ? 0 : 1);
    // 返回 1 表示被 token 上限截断（未到 EOS）
    return eog_reached ? 0 : 1;
}

int llm_append_text(const char *text) {
    if (!g_model || !g_ctx || !g_vocab || !text || !text[0]) return -1;

    std::string s(text);
    int n = -llama_tokenize(g_vocab, s.c_str(), s.size(),
                            nullptr, 0, true, true);
    if (n <= 0) return -1;

    std::vector<llama_token> tokens(n);
    if (llama_tokenize(g_vocab, s.c_str(), s.size(),
                       tokens.data(), n, true, true) < 0) {
        return -1;
    }

    llama_batch batch = llama_batch_get_one(tokens.data(), n);
    if (llama_decode(g_ctx, batch)) return -1;
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
    if (!g_model || !buf || buf_size <= 1) return -1;

    std::vector<llama_chat_message> msgs;
    if (system_prompt && system_prompt[0] != '\0') {
        msgs.push_back({"system", system_prompt});
    }
    msgs.push_back({"user", user_message});

    const char *tmpl = llama_model_chat_template(g_model, nullptr);
    int len = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                        true, buf, buf_size);
    if (len < 0) {
        fprintf(stderr, "[LLM] Chat template 格式化失败\n");
        return -1;
    }
    return len;
}

void llm_destroy(void) {
    if (g_smpl) { llama_sampler_free(g_smpl); g_smpl = nullptr; }
    if (g_ctx)  { llama_free(g_ctx);         g_ctx  = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }
}
