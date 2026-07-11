#ifndef TTS_MODEL_H
#define TTS_MODEL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using TTSPcmCallback = std::function<int(const int16_t *samples, int n, float progress)>;

class TTSModel {
public:
    explicit TTSModel(const std::string &model_path);
    ~TTSModel();

    bool load_model(const std::string &model_path);
    int16_t *infer(const std::string &text, int32_t &audio_len);
    bool infer_stream(const std::string &text, TTSPcmCallback callback);
    void free_data(int16_t *data);

private:
    std::vector<int16_t> buffer_;   // 推理结果缓冲区
    int sample_rate_ = 0;
};

#endif // TTS_MODEL_H
