#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace vision {

// Frame is deliberately the only image-bearing type.  Downstream modules must
// publish compact VisionEvent values instead of passing video through the
// existing audio TCP protocol.
struct Frame {
    cv::Mat bgr_image;
    std::uint64_t sequence = 0;
    std::int64_t captured_at_unix_ms = 0;
};

struct Detection {
    std::string label;
    float confidence = 0.0F;
    cv::Rect bounding_box;
};

enum class VisionEventKind {
    kObjectsDetected,
    kSceneDescribed,
    kSnapshotSaved,
    kCameraError,
};

// Stable boundary for the future YOLO/VLM and conversation adapters.  YOLO
// fills detections; VLM fills scene_summary; neither component owns the camera.
struct VisionEvent {
    VisionEventKind kind = VisionEventKind::kCameraError;
    std::uint64_t source_frame_sequence = 0;
    std::int64_t occurred_at_unix_ms = 0;
    std::int64_t inference_duration_ms = 0;
    std::vector<Detection> detections;
    std::string scene_summary;
    std::string snapshot_path;
    std::string message;
};

}  // namespace vision
