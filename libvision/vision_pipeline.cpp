#include "vision_pipeline.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace vision {
namespace {

std::int64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

}  // namespace

class VisionPipeline::Impl {
  public:
    void worker_loop() {
        const auto minimum_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / config.inference_frames_per_second));
        auto next_inference_at = std::chrono::steady_clock::now();

        while (true) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex);
                frame_available.wait(lock, [this] { return stopping || pending_frame.has_value(); });
                if (stopping) {
                    return;
                }
                while (!stopping && std::chrono::steady_clock::now() < next_inference_at) {
                    frame_available.wait_until(lock, next_inference_at);
                }
                if (stopping) {
                    return;
                }
                frame = std::move(*pending_frame);
                pending_frame.reset();
            }

            VisionEvent event;
            event.source_frame_sequence = frame.sequence;
            try {
                event.kind = VisionEventKind::kObjectsDetected;
                event.detections = detector->detect(frame, &event.inference_duration_ms);
            } catch (const std::exception& exception) {
                event.kind = VisionEventKind::kCameraError;
                event.message = exception.what();
            }
            event.occurred_at_unix_ms = now_unix_ms();

            {
                std::lock_guard<std::mutex> lock(mutex);
                latest = std::move(event);
                ++pipeline_stats.processed_frames;
                pipeline_stats.last_inference_duration_ms = latest->inference_duration_ms;
                pipeline_stats.total_inference_duration_ms +=
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, latest->inference_duration_ms));
            }
            next_inference_at = std::chrono::steady_clock::now() + minimum_interval;
        }
    }

    VisionPipelineConfig config;
    std::unique_ptr<YoloDetector> detector;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable frame_available;
    bool running = false;
    bool stopping = false;
    std::optional<Frame> pending_frame;
    std::optional<VisionEvent> latest;
    VisionPipelineStats pipeline_stats;
};

VisionPipeline::VisionPipeline() : impl_(std::make_unique<Impl>()) {}

VisionPipeline::~VisionPipeline() {
    stop();
}

bool VisionPipeline::start(const VisionPipelineConfig& config, std::string* error_message) {
    stop();
    if (config.inference_frames_per_second <= 0.0) {
        if (error_message != nullptr) {
            *error_message = "YOLO 推理帧率必须大于 0。";
        }
        return false;
    }
    try {
        impl_->config = config;
        impl_->detector = std::make_unique<YoloDetector>(config.yolo);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->stopping = false;
            impl_->running = true;
            impl_->latest.reset();
            impl_->pending_frame.reset();
            impl_->pipeline_stats = {};
        }
        impl_->worker = std::thread(&Impl::worker_loop, impl_.get());
        return true;
    } catch (const std::exception& exception) {
        impl_->detector.reset();
        if (error_message != nullptr) {
            *error_message = "无法启动 YOLO 推理：" + std::string(exception.what());
        }
        return false;
    }
}

void VisionPipeline::submit(const Frame& frame) {
    if (frame.bgr_image.empty()) {
        return;
    }
    Frame snapshot = frame;
    snapshot.bgr_image = frame.bgr_image.clone();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        ++impl_->pipeline_stats.submitted_frames;
        if (impl_->pending_frame.has_value()) {
            ++impl_->pipeline_stats.dropped_frames;
        }
        impl_->pending_frame = std::move(snapshot);
    }
    impl_->frame_available.notify_one();
}

std::optional<VisionEvent> VisionPipeline::latest_event() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->latest;
}

VisionPipelineStats VisionPipeline::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pipeline_stats;
}

void VisionPipeline::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        impl_->stopping = true;
        impl_->running = false;
    }
    impl_->frame_available.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->detector.reset();
    impl_->pending_frame.reset();
}

}  // namespace vision
