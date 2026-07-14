#include "tts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sherpa-onnx/csrc/offline-tts.h"

static std::unique_ptr<sherpa_onnx::OfflineTts> g_tts;
static int32_t g_sample_rate = 0;
static thread_local tts_callback_t g_user_callback = nullptr;

static int32_t bridge_callback(const float *samples, int32_t n, float progress) {
    if (!g_user_callback || !samples || n <= 0) return 1;
    std::vector<int16_t> pcm(n);
    for (int32_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }
    g_user_callback(pcm.data(), n, progress);
    return 1;
}

static bool file_exists(const std::string& path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

int tts_init(const char *model_dir) {
    std::string base = model_dir;
    sherpa_onnx::OfflineTtsConfig config;
    config.model.num_threads = 1;
    config.model.provider    = "cpu";
    config.max_num_sentences = 1;  // 按句/chunk 回调，便于边合成边播放

    // 自动检测模型类型：tts.json → Supertonic，否则 VITS/Matcha
    if (file_exists(base + "/tts.json")) {
        config.model.supertonic.duration_predictor = base + "/duration_predictor.int8.onnx";
        config.model.supertonic.text_encoder       = base + "/text_encoder.int8.onnx";
        config.model.supertonic.vector_estimator   = base + "/vector_estimator.int8.onnx";
        config.model.supertonic.vocoder            = base + "/vocoder.int8.onnx";
        config.model.supertonic.tts_json           = base + "/tts.json";
        config.model.supertonic.unicode_indexer    = base + "/unicode_indexer.bin";
        config.model.supertonic.voice_style        = base + "/voice.bin";
        fprintf(stderr, "[TTS] 检测到 Supertonic 模型\n");
    } else {
        // 优先用 INT8 模型（52MB，3x 快），否则用 FP32（163MB）
        std::string model  = base + "/model.int8.onnx";
        if (!file_exists(model)) model = base + "/model.onnx";
        std::string tokens = base + "/tokens.txt";
        if (!file_exists(model) || !file_exists(tokens)) {
            fprintf(stderr, "[TTS] 未找到 model.onnx/tokens.txt 或 tts.json\n");
            return -1;
        }
        config.model.vits.model  = model;
        config.model.vits.tokens = tokens;

        // 可选：中文 lexicon 和 dict
        if (file_exists(base + "/lexicon.txt"))
            config.model.vits.lexicon = base + "/lexicon.txt";
        if (file_exists(base + "/dict"))
            config.model.vits.dict_dir = base + "/dict";

        // 可选：FST 规则文件（数字/日期/电话号码等正则）
        std::string fsts;
        for (const char *f : {"date.fst", "number.fst", "phone.fst", "new_heteronym.fst"}) {
            if (file_exists(base + "/" + f))
                fsts += (fsts.empty() ? "" : ",") + base + "/" + f;
        }
        if (!fsts.empty()) config.rule_fsts = fsts;

        fprintf(stderr, "[TTS] 检测到 VITS/Matcha 模型\n");
    }

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
    try {
        sherpa_onnx::GenerationConfig gen_cfg;
        gen_cfg.extra["lang"] = "zh";
        g_tts->Generate(std::string(text), gen_cfg, bridge_callback);
    } catch (const std::exception &e) {
        fprintf(stderr, "[TTS] 合成失败: %s\n", e.what());
        g_user_callback = nullptr;
        return -1;
    }
    g_user_callback = nullptr;
    return 0;
}

int tts_sample_rate(void) { return g_sample_rate; }
void tts_destroy(void) { g_tts.reset(); g_sample_rate = 0; }
