#pragma once

#include <optional>
#include <string>

namespace vision_tools {

struct MicrophoneAsrConfig {
    std::string device = "plughw:1,0";
    std::string model_directory =
        "models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16";
};

struct MicrophoneAsrStatus {
    bool recording = false;
    std::string state = "disabled";
    std::string partial_text;
    std::string message;
};

// Small local microphone-to-ASR adapter for the desktop/WSL vision workbench.
// It deliberately owns no network or TTS resources: its only responsibility is
// producing a completed textual visual question after a push-to-talk turn.
class MicrophoneAsr {
  public:
    MicrophoneAsr();
    ~MicrophoneAsr();

    MicrophoneAsr(const MicrophoneAsr&) = delete;
    MicrophoneAsr& operator=(const MicrophoneAsr&) = delete;

    bool start(const MicrophoneAsrConfig& config, std::string* error_message);
    void stop();

    bool begin_recording(std::string* error_message);
    std::optional<std::string> end_recording();
    MicrophoneAsrStatus status() const;

  private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace vision_tools
