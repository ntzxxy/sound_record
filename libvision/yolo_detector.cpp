#include "yolo_detector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <onnxruntime_cxx_api.h>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace vision {
namespace {

const std::array<const char*, 80> kCocoLabels = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich",
    "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book",
    "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush",
};

struct LetterboxInfo {
    float scale = 1.0F;
    int pad_left = 0;
    int pad_top = 0;
};

cv::Mat letterbox(const cv::Mat& image, int target_size, LetterboxInfo* info) {
    const float scale = std::min(static_cast<float>(target_size) / image.cols,
                                 static_cast<float>(target_size) / image.rows);
    const int resized_width = static_cast<int>(std::round(image.cols * scale));
    const int resized_height = static_cast<int>(std::round(image.rows * scale));
    const int pad_left = (target_size - resized_width) / 2;
    const int pad_top = (target_size - resized_height) / 2;

    cv::Mat resized;
    cv::resize(image, resized, {resized_width, resized_height});
    cv::Mat output(target_size, target_size, image.type(), cv::Scalar(114, 114, 114));
    resized.copyTo(output({pad_left, pad_top, resized_width, resized_height}));

    info->scale = scale;
    info->pad_left = pad_left;
    info->pad_top = pad_top;
    return output;
}

cv::Rect clip_box(float left, float top, float width, float height, const cv::Size& image_size) {
    const int x1 = std::clamp(static_cast<int>(std::floor(left)), 0, image_size.width);
    const int y1 = std::clamp(static_cast<int>(std::floor(top)), 0, image_size.height);
    const int x2 = std::clamp(static_cast<int>(std::ceil(left + width)), 0, image_size.width);
    const int y2 = std::clamp(static_cast<int>(std::ceil(top + height)), 0, image_size.height);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

}  // namespace

class YoloDetector::Impl {
  public:
    explicit Impl(YoloConfig detector_config)
        : config(std::move(detector_config)), environment(ORT_LOGGING_LEVEL_WARNING, "ai_assistant_yolo") {
        if (config.input_size <= 0 || config.confidence_threshold <= 0.0F ||
            config.confidence_threshold > 1.0F || config.nms_iou_threshold <= 0.0F ||
            config.nms_iou_threshold > 1.0F) {
            throw std::invalid_argument("YOLO 配置参数无效。");
        }

        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads(1);
        if (config.use_cuda) {
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = config.device_id;
            options.AppendExecutionProvider_CUDA(cuda_options);
        }
        session = std::make_unique<Ort::Session>(environment, config.model_path.c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;
        const auto input_name = session->GetInputNameAllocated(0, allocator);
        input_names.emplace_back(input_name.get());
        for (std::size_t index = 0; index < session->GetOutputCount(); ++index) {
            const auto output_name = session->GetOutputNameAllocated(index, allocator);
            output_names.emplace_back(output_name.get());
        }
        input_name_views.push_back(input_names.front().c_str());
        for (const std::string& name : output_names) {
            output_name_views.push_back(name.c_str());
        }
    }

    YoloConfig config;
    Ort::Env environment;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<const char*> input_name_views;
    std::vector<const char*> output_name_views;
};

YoloDetector::YoloDetector(YoloConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

YoloDetector::~YoloDetector() = default;

std::vector<Detection> YoloDetector::detect(const Frame& frame, std::int64_t* inference_duration_ms) {
    if (frame.bgr_image.empty()) {
        throw std::invalid_argument("不能对空画面执行 YOLO 推理。");
    }

    LetterboxInfo letterbox_info;
    const cv::Mat prepared = letterbox(frame.bgr_image, impl_->config.input_size, &letterbox_info);
    cv::Mat blob;
    cv::dnn::blobFromImage(prepared, blob, 1.0 / 255.0, cv::Size(impl_->config.input_size,
                                                                 impl_->config.input_size),
                           cv::Scalar(), true, false, CV_32F);
    const std::array<std::int64_t, 4> input_shape = {1, 3, impl_->config.input_size,
                                                     impl_->config.input_size};
    const auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, blob.ptr<float>(), blob.total(), input_shape.data(), input_shape.size());

    const auto started_at = std::chrono::steady_clock::now();
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, impl_->input_name_views.data(),
                                       &input_tensor, 1, impl_->output_name_views.data(),
                                       impl_->output_name_views.size());
    const auto completed_at = std::chrono::steady_clock::now();
    if (inference_duration_ms != nullptr) {
        *inference_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     completed_at - started_at)
                                     .count();
    }

    if (outputs.empty()) {
        throw std::runtime_error("YOLO ONNX 未返回输出张量。");
    }
    const auto output_info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::vector<std::int64_t> shape = output_info.GetShape();
    if (shape.size() != 3 || (shape[1] != 84 && shape[2] != 84)) {
        throw std::runtime_error("不支持的 YOLO ONNX 输出形状。");
    }

    const bool channels_first = shape[1] == 84;
    const std::size_t prediction_count = static_cast<std::size_t>(channels_first ? shape[2] : shape[1]);
    const float* output = outputs.front().GetTensorData<float>();
    std::unordered_map<int, std::vector<cv::Rect>> boxes_by_class;
    std::unordered_map<int, std::vector<float>> scores_by_class;

    const auto value_at = [output, channels_first, prediction_count](int channel, std::size_t prediction) {
        return channels_first ? output[static_cast<std::size_t>(channel) * prediction_count + prediction]
                              : output[prediction * 84U + static_cast<std::size_t>(channel)];
    };
    for (std::size_t prediction = 0; prediction < prediction_count; ++prediction) {
        int label_index = -1;
        float score = 0.0F;
        for (int class_index = 0; class_index < static_cast<int>(kCocoLabels.size()); ++class_index) {
            const float class_score = value_at(4 + class_index, prediction);
            if (class_score > score) {
                score = class_score;
                label_index = class_index;
            }
        }
        if (label_index < 0 || score < impl_->config.confidence_threshold) {
            continue;
        }

        const float center_x = value_at(0, prediction);
        const float center_y = value_at(1, prediction);
        const float width = value_at(2, prediction);
        const float height = value_at(3, prediction);
        const cv::Rect box = clip_box((center_x - width / 2.0F - letterbox_info.pad_left) /
                                          letterbox_info.scale,
                                      (center_y - height / 2.0F - letterbox_info.pad_top) /
                                          letterbox_info.scale,
                                      width / letterbox_info.scale, height / letterbox_info.scale,
                                      frame.bgr_image.size());
        if (box.area() <= 0) {
            continue;
        }
        boxes_by_class[label_index].push_back(box);
        scores_by_class[label_index].push_back(score);
    }

    std::vector<Detection> detections;
    for (const auto& [label_index, boxes] : boxes_by_class) {
        std::vector<int> kept_indices;
        cv::dnn::NMSBoxes(boxes, scores_by_class.at(label_index), impl_->config.confidence_threshold,
                           impl_->config.nms_iou_threshold, kept_indices);
        for (const int index : kept_indices) {
            detections.push_back({kCocoLabels.at(label_index), scores_by_class.at(label_index).at(index),
                                  boxes.at(index)});
        }
    }
    std::sort(detections.begin(), detections.end(), [](const Detection& left, const Detection& right) {
        return left.confidence > right.confidence;
    });
    return detections;
}

bool YoloDetector::uses_cuda() const {
    return impl_->config.use_cuda;
}

}  // namespace vision
