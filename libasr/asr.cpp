#include "asr.h"

#include <cstdlib>
#include <memory>
#include <string>

#include "sherpa-onnx/csrc/online-recognizer.h"

static std::unique_ptr<sherpa_onnx::OnlineRecognizer> g_recognizer;
static std::unique_ptr<sherpa_onnx::OnlineStream> g_stream;

int asr_init(const char *model_dir) {
    // TODO: 加载模型，创建 OnlineRecognizer
    std::string base = model_dir;
    // 第1层：Transducer 模型配置
    sherpa_onnx::OnlineTransducerModelConfig transducer(
        base + "/encoder-epoch-99-avg-1.onnx",
        base + "/decoder-epoch-99-avg-1.onnx",
        base + "/joiner-epoch-99-avg-1.onnx"
    );

    sherpa_onnx::OnlineModelConfig model;
    model.transducer = transducer;
    model.tokens = base + "/tokens.txt";
    model.num_threads = 1;
    model.provider_config.provider = "cpu";

    sherpa_onnx::OnlineRecognizerConfig config;
    config.model_config = model;

    g_recognizer = std::make_unique<sherpa_onnx::OnlineRecognizer>(config);
    g_stream = g_recognizer->CreateStream();

    return 0;
}

int asr_process_frame(const int16_t *pcm, int num_samples) {
    // TODO: int16→float32 转换，AcceptWaveform，DecodeStream
    if (!g_recognizer || !g_stream || !pcm || num_samples <= 0) {
        return -1;
    }

    // 1.1 int16_t 转 float32，并归一化到 [-1.0, 1.0]
    std::vector<float> float_samples(num_samples);
    for (int i = 0; i < num_samples; ++i) {
        float_samples[i] = pcm[i] / 32768.0f;
    }

    // 1.2 输入音频数据
    g_stream->AcceptWaveform(16000, float_samples.data(), float_samples.size());

    // 1.3 驱动模型进行解码
    while (g_recognizer->IsReady(g_stream.get())) {
        g_recognizer->DecodeStream(g_stream.get());
    }

    return 0;
}

const char *asr_get_result(void) {
    // GetResult 返回一个临时对象，text 需要缓存下来
    static std::string g_last_text;
    if (g_recognizer && g_stream) {
        auto result = g_recognizer->GetResult(g_stream.get());
        g_last_text = result.text;
    }
    return g_last_text.c_str();
}

int asr_is_endpoint(void) {
    if (g_recognizer && g_stream) {
        return g_recognizer->IsEndpoint(g_stream.get()) ? 1 : 0;
    }
    return 0;
}

void asr_reset(void) {
    if (g_recognizer && g_stream) {
        g_recognizer->Reset(g_stream.get());
    }
}

void asr_destroy(void) {
    g_stream.reset();      // unique_ptr::reset() = delete + 置空
    g_recognizer.reset();
}
