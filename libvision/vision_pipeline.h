#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "yolo_detector.h"

namespace vision {

struct VisionPipelineConfig {
    YoloConfig yolo;
    double inference_frames_per_second = 5.0;
};

struct VisionPipelineStats {
    std::uint64_t submitted_frames = 0;
    std::uint64_t processed_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::int64_t last_inference_duration_ms = 0;
    std::uint64_t total_inference_duration_ms = 0;
};

// A bounded latest-frame pipeline.  It deliberately drops stale frames rather
// than building a queue, so spoken answers and preview boxes describe now.
class VisionPipeline {
  public:
    VisionPipeline();
    ~VisionPipeline();

    VisionPipeline(const VisionPipeline&) = delete;
    VisionPipeline& operator=(const VisionPipeline&) = delete;

    bool start(const VisionPipelineConfig& config, std::string* error_message);
    void submit(const Frame& frame);
    std::optional<VisionEvent> latest_event() const;
    VisionPipelineStats stats() const;
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision
