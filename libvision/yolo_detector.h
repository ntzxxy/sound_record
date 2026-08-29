#pragma once

#include <memory>
#include <string>
#include <vector>

#include "vision_types.h"

namespace vision {

struct YoloConfig {
    std::string model_path = "models/vision/yolo11n.onnx";
    int input_size = 640;
    float confidence_threshold = 0.35F;
    float nms_iou_threshold = 0.45F;
    int device_id = 0;
    bool use_cuda = true;
};

// ONNX Runtime implementation of the COCO-trained YOLO11 detector.  It takes
// frames from CameraSource and emits compact Detection values, never video data.
class YoloDetector {
  public:
    explicit YoloDetector(YoloConfig config);
    ~YoloDetector();

    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    std::vector<Detection> detect(const Frame& frame, std::int64_t* inference_duration_ms);
    bool uses_cuda() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision
