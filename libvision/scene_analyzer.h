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

enum class SceneAnalysisKind {
    kAutomaticScene,
    kVisualQuestion,
    kVisualFollowUp,
};

const char* scene_analysis_kind_name(SceneAnalysisKind kind);

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
    SceneAnalysisKind kind = SceneAnalysisKind::kAutomaticScene;
    std::uint64_t source_frame_sequence = 0;
    std::int64_t captured_at_unix_ms = 0;
    std::int64_t latency_ms = 0;
    std::string user_question;
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
    // Low-priority environmental description. It is dropped while a visual
    // conversation task is pending or running.
    bool submit(const Frame& frame);
    // Starts a new visual conversation using the supplied frame.
    bool ask_about_frame(const Frame& frame, const std::string& question);
    // Makes a newly selected camera frame the authoritative visual context and
    // clears prior visual dialogue. This does not enqueue an analysis by itself.
    bool select_visual_snapshot(const Frame& frame);
    // Reuses the snapshot selected by ask_about_frame(), or the latest
    // automatically analyzed snapshot if no visual conversation is active.
    bool ask_follow_up(const std::string& question);
    bool has_active_snapshot() const;
    void clear_visual_session();
    SceneAnalysisResult latest_result() const;
    bool is_busy() const;
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision
