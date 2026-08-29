#include "camera_source.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <utility>

#include <opencv2/videoio.hpp>

namespace vision {
namespace {

bool is_device_index(const std::string& device) {
    return !device.empty() &&
           std::all_of(device.begin(), device.end(), [](unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

int fourcc_from_format(const std::string& pixel_format) {
    if (pixel_format == "MJPG") {
        return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    }
    if (pixel_format == "YUYV") {
        return cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V');
    }
    return 0;
}

std::int64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

}  // namespace

class CameraSource::Impl {
  public:
    cv::VideoCapture capture;
    std::uint64_t sequence = 0;
};

CameraSource::~CameraSource() {
    close();
}

bool CameraSource::open(const CameraConfig& config, std::string* error_message) {
    close();
    impl_ = new Impl();

    bool opened = false;
    if (is_device_index(config.device)) {
        opened = impl_->capture.open(std::stoi(config.device), cv::CAP_V4L2);
        if (!opened) {
            opened = impl_->capture.open(std::stoi(config.device));
        }
    } else {
        opened = impl_->capture.open(config.device, cv::CAP_V4L2);
        if (!opened) {
            opened = impl_->capture.open(config.device);
        }
    }

    if (!opened) {
        if (error_message != nullptr) {
            *error_message = "无法打开摄像头 " + config.device +
                             "。请确认它已通过 usbipd 连接到 WSL，并出现在 /dev/video*。";
        }
        close();
        return false;
    }

    const int requested_fourcc = fourcc_from_format(config.pixel_format);
    if (requested_fourcc == 0) {
        if (error_message != nullptr) {
            *error_message = "不支持的像素格式 " + config.pixel_format + "，可使用 MJPG 或 YUYV。";
        }
        close();
        return false;
    }
    impl_->capture.set(cv::CAP_PROP_FOURCC, requested_fourcc);
    impl_->capture.set(cv::CAP_PROP_FRAME_WIDTH, config.width);
    impl_->capture.set(cv::CAP_PROP_FRAME_HEIGHT, config.height);
    impl_->capture.set(cv::CAP_PROP_FPS, config.frames_per_second);

    const int actual_fourcc = static_cast<int>(impl_->capture.get(cv::CAP_PROP_FOURCC));
    if (actual_fourcc != requested_fourcc) {
        std::cerr << "警告：摄像头未接受请求的 " << config.pixel_format
                  << " 格式，当前格式可能与请求不同。\n";
    }
    return true;
}

bool CameraSource::read(Frame* frame, std::string* error_message) {
    if (frame == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Frame 输出参数不能为空。";
        }
        return false;
    }
    if (!is_open()) {
        if (error_message != nullptr) {
            *error_message = "摄像头尚未打开。";
        }
        return false;
    }

    cv::Mat image;
    if (!impl_->capture.read(image) || image.empty()) {
        if (error_message != nullptr) {
            *error_message = "未能从摄像头读取有效画面。";
        }
        return false;
    }

    frame->bgr_image = std::move(image);
    frame->sequence = ++impl_->sequence;
    frame->captured_at_unix_ms = now_unix_ms();
    return true;
}

void CameraSource::close() {
    if (impl_ != nullptr) {
        impl_->capture.release();
        delete impl_;
        impl_ = nullptr;
    }
}

bool CameraSource::is_open() const {
    return impl_ != nullptr && impl_->capture.isOpened();
}

}  // namespace vision
