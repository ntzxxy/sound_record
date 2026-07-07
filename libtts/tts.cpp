#include "tts.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sherpa-onnx/csrc/offline-tts.h"

static std::unique_ptr<sherpa_onnx::OfflineTts> g_tts;
static int32_t g_sample_rate = 0;

// 桥接：sherpa-onnx float 回调 → 用户 int16 回调
static thread_local tts_callback_t g_user_callback = nullptr;

static int32_t bridge_callback(const float *samples, int32_t n, float progress) {
    if (!g_user_callback || !samples || n <= 0) return 1;

    // float → int16 转换（sherpa-onnx 输出 [-1, 1] 范围的 float）
    std::vector<int16_t> pcm(n);
    for (int32_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }

    g_user_callback(pcm.data(), n, progress);
    return 1;  // 继续生成
}

int tts_init(const char *model_dir) {
    std::string base = model_dir;
    std::string model_path  = base + "/model.onnx";
    std::string tokens_path = base + "/tokens.txt";

    // 检查模型文件是否存在
    FILE *f = fopen(model_path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[TTS] 模型文件不存在: %s\n", model_path.c_str());
        return -1;
    }
    fclose(f);
    f = fopen(tokens_path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[TTS] tokens 文件不存在: %s\n", tokens_path.c_str());
        return -1;
    }
    fclose(f);

    sherpa_onnx::OfflineTtsConfig config;
    config.model.vits.model  = model_path;
    config.model.vits.tokens = tokens_path;
    config.model.num_threads = 1;
    config.model.provider    = "cpu";
    config.model.debug       = false;

    try {
        g_tts = std::make_unique<sherpa_onnx::OfflineTts>(config);
        g_sample_rate = g_tts->SampleRate();
        fprintf(stderr, "[TTS] 模型加载成功, sample_rate=%d\n", g_sample_rate);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[TTS] 模型加载失败: %s\n", e.what());
        return -1;
    }
}

int tts_speak(const char *text, tts_callback_t callback) {
    if (!g_tts || !text || !text[0] || !callback) return -1;

    g_user_callback = callback;
    std::string input(text);

    try {
        g_tts->Generate(input, /*sid=*/0, /*speed=*/1.0, bridge_callback);
    } catch (const std::exception &e) {
        fprintf(stderr, "[TTS] 合成失败: %s\n", e.what());
        g_user_callback = nullptr;
        return -1;
    }

    g_user_callback = nullptr;
    return 0;
}

int tts_sample_rate(void) {
    return g_sample_rate;
}

void tts_destroy(void) {
    g_tts.reset();
    g_sample_rate = 0;
}
