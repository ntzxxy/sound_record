#include "tts_model.h"
#include "tts.h"

#include <cstdio>

// tts_speak 的 C callback 桥接：同步调用，用全局指针捕获结果
static thread_local std::vector<int16_t> *g_capture_ptr = nullptr;
static void capture_cb(const int16_t *s, int n, float) {
    if (g_capture_ptr) g_capture_ptr->insert(g_capture_ptr->end(), s, s + n);
}

TTSModel::TTSModel(const std::string &model_path) {
    load_model(model_path);
}

TTSModel::~TTSModel() {
    tts_destroy();
}

bool TTSModel::load_model(const std::string &model_path) {
    if (tts_init(model_path.c_str()) != 0) return false;
    return true;
}

int16_t *TTSModel::infer(const std::string &text, int32_t &audio_len) {
    buffer_.clear();
    g_capture_ptr = &buffer_;
    tts_speak(text.c_str(), capture_cb);
    g_capture_ptr = nullptr;
    audio_len = static_cast<int32_t>(buffer_.size());
    return audio_len > 0 ? buffer_.data() : nullptr;
}

void TTSModel::free_data(int16_t * /*data*/) {
    // buffer_ 由 vector 管理，无需手动释放
}
