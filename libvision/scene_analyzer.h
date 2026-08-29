#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vision_types.h"

namespace vision {

enum class SceneAnalysisState {
    kDisabled,
    kLoading,
    kWaiting,
    kCaptured,
    kAnalyzing,
    kCompleted,
    kFailed,
    kStopped,
};

const char* scene_analysis_state_name(SceneAnalysisState state);

struct SceneAnalyzerConfig {
    std::string model_path = "models/gemma-4-E4B-it-Q4_0.gguf";
    std::string projector_path = "models/gemma-4-E4B-it/mmproj-gemma-4-E4B-it-Q8_0.gguf";
    bool use_gpu = true;
    int context_tokens = 4096;
    int threads = 8;
    int max_generation_tokens = 96;
};

struct SceneAnalysisResult {
    SceneAnalysisState state = SceneAnalysisState::kDisabled;
    std::uint64_t source_frame_sequence = 0;
    std::int64_t captured_at_unix_ms = 0;
    std::int64_t latency_ms = 0;
    std::string summary;
    std::string message;
};

// Owns one persistent VLM instance and accepts at most one pending frame.
// A slow analysis never blocks video capture or causes an unbounded queue.
class SceneAnalyzer {
  public:
    SceneAnalyzer();
    ~SceneAnalyzer();

    SceneAnalyzer(const SceneAnalyzer&) = delete;
    SceneAnalyzer& operator=(const SceneAnalyzer&) = delete;

    bool start(const SceneAnalyzerConfig& config, std::string* error_message);
    bool submit(const Frame& frame);
    SceneAnalysisResult latest_result() const;
    bool is_busy() const;
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision
