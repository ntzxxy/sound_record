#include "chat_agent.h"
#include "llm.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

// ===================================================================
// 模块内部状态
// ===================================================================

// 默认 System Prompt（当 agent_init system_prompt 参数为 NULL 时使用）
static const char DEFAULT_SYSTEM[] =
    "Role: 你是部署在智能硬件上的语音助手。\n"
    "Constraints:\n"
    "1. 回答必须极其简短、口语化，严禁列出分点（1、2、3），严禁大段落。\n"
    "2. 每次回答严格限制在 2-3 句话以内（不超过 80 字），适合人耳聆听。\n"
    "3. 保持记忆，根据多轮对话直接给出干脆的建议。\n"
    "4. 用户输入可能含语音识别口误或语气词，请结合上下文自动纠错，忽略无关词汇。";

static std::string g_system_prompt;
static bool g_first_turn = true;
static bool g_is_r1_model = false;

// 滑动窗口
static std::deque<std::pair<std::string, std::string>> g_history;
static const int MAX_HISTORY_PAIRS = 5;

// callback 桥接（全局变量）
// 原因：llama.cpp 的 llm_callback_t 签名只有 (text, is_final)，没有 void* userdata，
// 无法通过参数传递上下文。因此在 agent_chat 调用前设置全局变量来桥接。
// 约束：agent_chat 调用是单线程同步的，当前安全。多线程时需改为 thread_local。
static std::string g_captured;
static agent_callback_t g_user_cb = nullptr;

// ===================================================================
// 内部辅助
// ===================================================================

// 过滤 ASR 识别中的语气词和口癖
static std::string cleanup_input(const std::string& raw) {
    std::string s = raw;
    for (const char* w : {"哎呀", "哎呦", "呃", "嗯", "那个", "就是说", "然后"}) {
        while (s.find(w) == 0) s = s.substr(strlen(w));
    }
    return s;
}

// 检测模型是否拒绝回答
static bool is_refusal(const std::string& response) {
    return response.find("我还没有学会") != std::string::npos ||
           response.find("无法提供")    != std::string::npos ||
           response.find("暂时无法")    != std::string::npos;
}

// 内部 callback：累积回复 + 拒绝检测 + 透传用户 callback
static void internal_cb(const char* text, int is_final) {
    if (text && text[0]) g_captured += text;
    if (g_user_cb) g_user_cb(text, is_final);
}

// ===================================================================
// 对外接口
// ===================================================================

int agent_init(const char *model_path, const char *system_prompt) {
    if (llm_init(model_path) != 0) {
        return -1;
    }
    g_system_prompt = (system_prompt && system_prompt[0]) ? system_prompt : DEFAULT_SYSTEM;
    g_first_turn = true;
    g_history.clear();

    std::string path(model_path);
    for (auto& c : path) c = tolower(c);
    g_is_r1_model = (path.find("deepseek") != std::string::npos ||
                     path.find("-r1")    != std::string::npos);
    fprintf(stderr, "[Agent] 模型: %s | 窗口: %d 对 | prompt: %d 字\n",
            g_is_r1_model ? "R1推理型" : "通用对话型",
            MAX_HISTORY_PAIRS,
            (int)g_system_prompt.size());
    return 0;
}

void agent_destroy(void) {
    llm_destroy();
}

void agent_reset(void) {
    llm_reset_context();
    g_first_turn = true;
    g_history.clear();
}

int agent_chat(const char *raw_user_message, agent_callback_t callback) {
    // 1. 输入清洗（去语气词）
    std::string cleaned = cleanup_input(raw_user_message ? raw_user_message : "");

    // 2. 滑动窗口：满时重建上下文（清除 KV Cache → 重喂最近历史）
    if ((int)g_history.size() >= MAX_HISTORY_PAIRS) {
        fprintf(stderr, "[Agent] 窗口满(%d对)，重建上下文\n", MAX_HISTORY_PAIRS);
        size_t keep_from = g_history.size() > 2 ? g_history.size() - 2 : 0;

        // 构建 ChatML 格式的历史对话字符串
        // <|im_start|>system\n...<|im_end|>\n<|im_start|>user\n...<|im_end|>\n...
        std::string rebuilt;
        rebuilt += "<|im_start|>system\n";
        rebuilt += g_system_prompt;
        rebuilt += "<|im_end|>\n";

        for (size_t i = keep_from; i < g_history.size(); i++) {
            rebuilt += "<|im_start|>user\n";
            rebuilt += g_history[i].first;
            rebuilt += "<|im_end|>\n<|im_start|>assistant\n";
            rebuilt += g_history[i].second;
            rebuilt += "<|im_end|>\n";
        }

        g_history.erase(g_history.begin(), g_history.begin() + keep_from);

        // 清空 KV Cache → 把重建的历史重新喂入模型
        llm_reset_context();
        int append_ret = llm_append_text(rebuilt.c_str());
        fprintf(stderr, "[Agent] 上下文重建: %d chars → append %s, 历史剩余 %d 对\n",
                (int)rebuilt.size(), append_ret == 0 ? "OK" : "FAIL",
                (int)g_history.size());
        g_first_turn = false;  // 历史已加载，下轮不重新加 system prompt
    }

    // 3. 格式化 prompt
    char buf[4096];
    int len;
    if (g_first_turn) {
        len = llm_format_prompt(g_system_prompt.c_str(), cleaned.c_str(),
                                buf, sizeof(buf));
        g_first_turn = false;
    } else {
        len = llm_format_prompt(nullptr, cleaned.c_str(), buf, sizeof(buf));
    }
    if (len < 0 || len >= (int)sizeof(buf)) return -1;

    // 4. R1 思考跳过
    if (g_is_r1_model) {
        static const char SKIP_THINK[] = {'\x3c', '/', 't', 'h', 'i', 'n', 'k', '\x3e', '\0'};
        if (len + 9 < (int)sizeof(buf)) memcpy(buf + len, SKIP_THINK, 9);
    }

    // 5. 推理
    g_captured.clear();
    g_user_cb = callback;
    int ret = llm_chat(buf, internal_cb);

    // 6. 后处理
    if (ret == 1) {
        fprintf(stderr, "[Agent] 回复截断，重置上下文\n");
        agent_reset();
        return 0;
    }

    if (is_refusal(g_captured)) {
        fprintf(stderr, "[Agent] 检测到拒绝回答，重置上下文避免传染\n");
        agent_reset();
        return 0;
    }

    // 7. 记录历史
    g_history.push_back({cleaned, g_captured});
    return 0;
}
