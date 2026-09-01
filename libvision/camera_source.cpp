#include "camera_source.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iostream>
#include <thread>
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
    const int actual_width = static_cast<int>(std::lround(impl_->capture.get(cv::CAP_PROP_FRAME_WIDTH)));
    const int actual_height = static_cast<int>(std::lround(impl_->capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
    const int actual_fps = static_cast<int>(std::lround(impl_->capture.get(cv::CAP_PROP_FPS)));
    std::cout << "摄像头采集配置：请求 " << config.width << 'x' << config.height << '@'
              << config.frames_per_second << " FPS，实际 " << actual_width << 'x' << actual_height << '@'
              << actual_fps << " FPS。\n";
    if (actual_width != config.width || actual_height != config.height) {
        std::cerr << "警告：摄像头未按请求分辨率输出；将使用实际分辨率。\n";
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

    // USB/IP cameras can occasionally deliver an incomplete MJPEG packet. In
    // OpenCV 4.5 this may throw from imdecode(), rather than simply returning
    // an empty frame. A bad video packet must not crash the VLM workbench.
    std::string last_failure;
    for (int attempt = 0; attempt < 3; ++attempt) {
        cv::Mat image;
        try {
            if (impl_->capture.read(image) && !image.empty()) {
                frame->bgr_image = std::move(image);
                frame->sequence = ++impl_->sequence;
                frame->captured_at_unix_ms = now_unix_ms();
                return true;
            }
            last_failure = "摄像头返回了空画面";
        } catch (const cv::Exception& exception) {
            last_failure = std::string("OpenCV 解码摄像头画面失败：") + exception.what();
        }
        if (attempt < 2) {
            std::cerr << "警告：摄像头帧无效，正在重试（" << (attempt + 1) << "/3）。\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    if (error_message != nullptr) {
        *error_message = "连续 3 次未能读取有效摄像头画面：" + last_failure;
    }
    return false;
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
