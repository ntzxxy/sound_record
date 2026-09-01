#pragma once

#include <string>

#include "vision_types.h"

namespace vision {

struct CameraConfig {
    // Linux V4L2 default.  "0" is also accepted as an OpenCV device index.
    std::string device = "/dev/video0";
    // Keep the original 1080p frame for on-demand VLM/OCR snapshots. YOLO
    // letterboxes this frame to its independent 640x640 model input.
    int width = 1920;
    int height = 1080;
    int frames_per_second = 15;
    // USB/IP 下未压缩 YUYV 容易造成传输超时；优先使用摄像头普遍支持的 MJPEG。
    std::string pixel_format = "MJPG";
};

// Owns only camera acquisition.  It has no dependency on YOLO, VLM, LLM,
// networking, Qt, ASR, or TTS, so the same interface can later be backed by
// V4L2, GStreamer, a video file, or a Jetson CSI camera pipeline.
class CameraSource {
  public:
    CameraSource() = default;
    ~CameraSource();

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    bool open(const CameraConfig& config, std::string* error_message);
    bool read(Frame* frame, std::string* error_message);
    void close();
    bool is_open() const;

  private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace vision
