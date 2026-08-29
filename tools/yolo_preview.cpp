#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/freetype.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_source.h"
#include "vision_pipeline.h"
#ifdef VISION_HAS_VLM
#include "scene_analyzer.h"
#endif

namespace {

struct PreviewOptions {
    vision::CameraConfig camera;
    vision::VisionPipelineConfig pipeline;
    bool show_window = true;
    int frame_limit = 0;
    int window_width = 1280;
    int window_height = 720;
    int scene_interval_seconds = 20;
    bool enable_vlm = false;
    bool vlm_use_gpu = false;
    std::string vlm_model_path = "models/gemma-4-E4B-it-Q4_0.gguf";
    std::string mmproj_path = "models/gemma-4-E4B-it/mmproj-gemma-4-E4B-it-Q8_0.gguf";
};

struct DisplayVlmStatus {
    std::string state = "disabled";
    std::int64_t latency_ms = 0;
    std::string summary;
    std::string message;
};

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name
              << " [--device /dev/video0|0] [--width 640] [--height 480] [--camera-fps 15]"
                 " [--inference-fps 5] [--model PATH] [--conf 0.35] [--no-cuda]"
                 " [--window-width 1280] [--window-height 720] [--scene-interval 20]"
                 " [--enable-vlm] [--vlm-model PATH] [--mmproj PATH] [--vlm-gpu]"
                 " [--frames N] [--headless]\n"
                 "窗口模式：按空格立即分析、q 或 Esc 退出。检测线程只处理最新帧，避免延迟累积。\n";
}

bool read_argument(int argc, char* argv[], int* index, std::string* value) {
    if (*index + 1 >= argc) {
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool parse_positive_int(const std::string& text, int* value) {
    try {
        const int parsed = std::stoi(text);
        if (parsed <= 0) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_positive_double(const std::string& text, double* value) {
    try {
        const double parsed = std::stod(text);
        if (parsed <= 0.0) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_confidence(const std::string& text, float* value) {
    try {
        const float parsed = std::stof(text);
        if (parsed <= 0.0F || parsed > 1.0F) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_arguments(int argc, char* argv[], PreviewOptions* options) {
    options->pipeline.yolo.model_path = "models/vision/yolo11n.onnx";
    options->pipeline.yolo.use_cuda = true;
    options->pipeline.inference_frames_per_second = 5.0;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        std::string value;
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (argument == "--headless") {
            options->show_window = false;
            continue;
        }
        if (argument == "--no-cuda") {
            options->pipeline.yolo.use_cuda = false;
            continue;
        }
        if (argument == "--enable-vlm") {
            options->enable_vlm = true;
            continue;
        }
        if (argument == "--vlm-gpu") {
            options->vlm_use_gpu = true;
            continue;
        }
        if (argument == "--device" && read_argument(argc, argv, &index, &value)) {
            options->camera.device = value;
            continue;
        }
        if (argument == "--model" && read_argument(argc, argv, &index, &value)) {
            options->pipeline.yolo.model_path = value;
            continue;
        }
        if (argument == "--vlm-model" && read_argument(argc, argv, &index, &value)) {
            options->vlm_model_path = value;
            continue;
        }
        if (argument == "--mmproj" && read_argument(argc, argv, &index, &value)) {
            options->mmproj_path = value;
            continue;
        }

        int* integer_target = nullptr;
        double* double_target = nullptr;
        if (argument == "--width") {
            integer_target = &options->camera.width;
        } else if (argument == "--height") {
            integer_target = &options->camera.height;
        } else if (argument == "--camera-fps") {
            integer_target = &options->camera.frames_per_second;
        } else if (argument == "--frames") {
            integer_target = &options->frame_limit;
        } else if (argument == "--window-width") {
            integer_target = &options->window_width;
        } else if (argument == "--window-height") {
            integer_target = &options->window_height;
        } else if (argument == "--scene-interval") {
            integer_target = &options->scene_interval_seconds;
        } else if (argument == "--inference-fps") {
            double_target = &options->pipeline.inference_frames_per_second;
        }
        if (integer_target != nullptr && read_argument(argc, argv, &index, &value) &&
            parse_positive_int(value, integer_target)) {
            continue;
        }
        if (double_target != nullptr && read_argument(argc, argv, &index, &value) &&
            parse_positive_double(value, double_target)) {
            continue;
        }
        if (argument == "--conf" && read_argument(argc, argv, &index, &value) &&
            parse_confidence(value, &options->pipeline.yolo.confidence_threshold)) {
            continue;
        }

        std::cerr << "无效参数: " << argument << '\n';
        print_usage(argv[0]);
        return false;
    }
    if (!options->show_window && options->frame_limit == 0) {
        std::cerr << "无窗口模式必须使用 --frames 指定采集帧数。\n";
        return false;
    }
    return true;
}

cv::Scalar color_for_label(const std::string& label) {
    const std::size_t hash = std::hash<std::string>{}(label);
    return {static_cast<double>(64 + hash % 192), static_cast<double>(64 + (hash >> 8U) % 192),
            static_cast<double>(64 + (hash >> 16U) % 192)};
}

std::string escape_json(const std::string& text) {
    std::ostringstream escaped;
    for (const char character : text) {
        switch (character) {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                escaped << character;
                break;
        }
    }
    return escaped.str();
}

std::string build_status_json(const vision::Frame& frame,
                              const vision::VisionPipelineStats& stats,
                              const std::optional<vision::VisionEvent>& event,
                              const PreviewOptions& options,
                              int next_scene_scan_seconds,
                              const DisplayVlmStatus& vlm) {
    std::ostringstream json;
    json << "{\n"
         << "  \"state\": \"live_detection\",\n"
         << "  \"camera\": {\"frame\": " << frame.sequence << ", \"fps\": "
         << options.camera.frames_per_second << "},\n"
         << "  \"yolo\": {\"backend\": \"" << (options.pipeline.yolo.use_cuda ? "cuda" : "cpu")
         << "\", \"latency_ms\": " << stats.last_inference_duration_ms
         << ", \"processed\": " << stats.processed_frames << ", \"dropped\": "
         << stats.dropped_frames << "},\n"
         << "  \"scene_scan\": {\"next_in_s\": " << next_scene_scan_seconds << "},\n"
         << "  \"vlm\": {\"state\": \"" << escape_json(vlm.state) << "\", \"latency_ms\": "
         << vlm.latency_ms << ", \"summary\": \"" << escape_json(vlm.summary) << "\"}";

    if (event.has_value()) {
        json << ",\n  \"latest_event\": {\n"
             << "    \"kind\": \"objects_detected\",\n"
             << "    \"object_count\": " << event->detections.size() << ",\n"
             << "    \"objects\": [\n";
        const std::size_t visible_detections = std::min<std::size_t>(event->detections.size(), 3);
        for (std::size_t index = 0; index < visible_detections; ++index) {
            const vision::Detection& detection = event->detections[index];
            json << "      {\"label\": \"" << escape_json(detection.label) << "\", \"confidence\": "
                 << std::fixed << std::setprecision(2) << detection.confidence << '}';
            if (index + 1 < visible_detections) {
                json << ',';
            }
            json << '\n';
        }
        json << "    ]\n  }";
    } else {
        json << ",\n  \"latest_event\": null";
    }
    json << "\n}";
    return json.str();
}

cv::Ptr<cv::freetype::FreeType2> status_font() {
    static cv::Ptr<cv::freetype::FreeType2> font = [] {
        const std::vector<std::string> font_paths = {
            "/mnt/c/Windows/Fonts/msyh.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        };
        for (const std::string& font_path : font_paths) {
            if (!std::filesystem::is_regular_file(font_path)) {
                continue;
            }
            try {
                cv::Ptr<cv::freetype::FreeType2> loaded = cv::freetype::createFreeType2();
                loaded->loadFontData(font_path, 0);
                return loaded;
            } catch (const cv::Exception&) {
            }
        }
        return cv::Ptr<cv::freetype::FreeType2>();
    }();
    return font;
}

void draw_text_lines(cv::Mat* image,
                     const std::string& text,
                     const cv::Point& start,
                     int font_height,
                     int line_spacing,
                     const cv::Scalar& color) {
    const cv::Ptr<cv::freetype::FreeType2> font = status_font();
    std::istringstream lines(text);
    std::string line;
    int y = start.y;
    while (std::getline(lines, line)) {
        if (font) {
            font->putText(*image, line, {start.x, y}, font_height, color, -1, cv::LINE_AA, false);
        } else {
            cv::putText(*image, line, {start.x, y}, cv::FONT_HERSHEY_SIMPLEX, 0.43, color, 1, cv::LINE_AA);
        }
        y += line_spacing;
    }
}

void draw_event(cv::Mat* image, const vision::VisionEvent& event) {
    if (event.kind == vision::VisionEventKind::kCameraError) {
        cv::putText(*image, "YOLO error: " + event.message, {12, 58}, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    {0, 0, 255}, 1, cv::LINE_AA);
        return;
    }
    for (const vision::Detection& detection : event.detections) {
        const cv::Scalar color = color_for_label(detection.label);
        cv::rectangle(*image, detection.bounding_box, color, 2);
        std::ostringstream label;
        label << detection.label << ' ' << std::fixed << std::setprecision(2) << detection.confidence;
        int baseline = 0;
        const cv::Size text_size = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX, 0.55, 1,
                                                   &baseline);
        const int label_top = std::max(0, detection.bounding_box.y - text_size.height - 8);
        cv::rectangle(*image,
                      {detection.bounding_box.x, label_top, text_size.width + 8, text_size.height + 8}, color,
                      cv::FILLED);
        cv::putText(*image, label.str(), {detection.bounding_box.x + 4, label_top + text_size.height + 2},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, {0, 0, 0}, 1, cv::LINE_AA);
    }
}

cv::Mat build_workbench_view(const cv::Mat& annotated_frame,
                             const vision::Frame& frame,
                             const vision::VisionPipelineStats& stats,
                             const std::optional<vision::VisionEvent>& event,
                             const PreviewOptions& options,
                             int next_scene_scan_seconds,
                             const DisplayVlmStatus& vlm) {
    constexpr int kPanelWidth = 500;
    constexpr int kMargin = 18;
    constexpr int kHeaderHeight = 54;
    const int canvas_width = std::max(options.window_width, kPanelWidth + 400);
    const int canvas_height = std::max(options.window_height, 480);
    cv::Mat canvas(canvas_height, canvas_width, CV_8UC3, cv::Scalar(24, 25, 29));

    cv::rectangle(canvas, {0, 0, canvas_width, kHeaderHeight}, cv::Scalar(37, 41, 47), cv::FILLED);
    cv::putText(canvas, "VISION WORKBENCH", {kMargin, 34}, cv::FONT_HERSHEY_SIMPLEX, 0.78,
                cv::Scalar(89, 220, 150), 2, cv::LINE_AA);
    std::ostringstream title_status;
    title_status << "YOLO " << (options.pipeline.yolo.use_cuda ? "CUDA" : "CPU") << " | VLM "
                 << vlm.state;
    cv::putText(canvas, title_status.str(), {canvas_width - 300, 33}, cv::FONT_HERSHEY_SIMPLEX, 0.53,
                cv::Scalar(215, 215, 215), 1, cv::LINE_AA);

    const int video_area_width = canvas_width - kPanelWidth - 3 * kMargin;
    const int video_area_height = canvas_height - kHeaderHeight - 2 * kMargin;
    const double scale = std::min(static_cast<double>(video_area_width) / annotated_frame.cols,
                                  static_cast<double>(video_area_height) / annotated_frame.rows);
    const cv::Size video_size(std::max(1, static_cast<int>(annotated_frame.cols * scale)),
                              std::max(1, static_cast<int>(annotated_frame.rows * scale)));
    cv::Mat scaled_video;
    cv::resize(annotated_frame, scaled_video, video_size, 0.0, 0.0, cv::INTER_LINEAR);
    const int video_x = kMargin + (video_area_width - video_size.width) / 2;
    const int video_y = kHeaderHeight + kMargin + (video_area_height - video_size.height) / 2;
    scaled_video.copyTo(canvas({video_x, video_y, video_size.width, video_size.height}));

    const cv::Rect panel_rect(canvas_width - kPanelWidth - kMargin, kHeaderHeight + kMargin,
                              kPanelWidth, canvas_height - kHeaderHeight - 2 * kMargin);
    cv::rectangle(canvas, panel_rect, cv::Scalar(32, 35, 41), cv::FILLED);
    cv::rectangle(canvas, panel_rect, cv::Scalar(72, 78, 88), 1);
    cv::putText(canvas, "LIVE STATUS (JSON)", {panel_rect.x + 16, panel_rect.y + 31},
                cv::FONT_HERSHEY_SIMPLEX, 0.59, cv::Scalar(109, 204, 255), 1, cv::LINE_AA);
    cv::line(canvas, {panel_rect.x + 14, panel_rect.y + 43}, {panel_rect.x + panel_rect.width - 14, panel_rect.y + 43},
             cv::Scalar(72, 78, 88), 1);

    const std::string status_json = build_status_json(frame, stats, event, options, next_scene_scan_seconds, vlm);
    draw_text_lines(&canvas, status_json, {panel_rect.x + 16, panel_rect.y + 70}, 16, 21,
                    cv::Scalar(226, 226, 226));

    const int footer_y = panel_rect.y + panel_rect.height - 52;
    cv::line(canvas, {panel_rect.x + 14, footer_y - 14}, {panel_rect.x + panel_rect.width - 14, footer_y - 14},
             cv::Scalar(72, 78, 88), 1);
    std::ostringstream countdown;
    countdown << "NEXT SCENE SCAN: " << next_scene_scan_seconds << " s";
    cv::putText(canvas, countdown.str(), {panel_rect.x + 16, footer_y}, cv::FONT_HERSHEY_SIMPLEX, 0.52,
                cv::Scalar(247, 205, 83), 1, cv::LINE_AA);
    const std::string vlm_result = vlm.summary.empty() ? vlm.message : vlm.summary;
    draw_text_lines(&canvas, "VLM: " + vlm_result, {panel_rect.x + 16, footer_y + 25}, 15, 20,
                    vlm.state == "failed" ? cv::Scalar(92, 92, 255) : cv::Scalar(205, 205, 205));
    return canvas;
}

}  // namespace

int main(int argc, char* argv[]) {
    PreviewOptions options;
    if (!parse_arguments(argc, argv, &options)) {
        return EXIT_FAILURE;
    }

    vision::CameraSource camera;
    std::string error_message;
    if (!camera.open(options.camera, &error_message)) {
        std::cerr << error_message << '\n';
        return EXIT_FAILURE;
    }

    vision::VisionPipeline pipeline;
    if (!pipeline.start(options.pipeline, &error_message)) {
        std::cerr << error_message << '\n';
        return EXIT_FAILURE;
    }
#ifdef VISION_HAS_VLM
    vision::SceneAnalyzer scene_analyzer;
    if (options.enable_vlm) {
        vision::SceneAnalyzerConfig vlm_config;
        vlm_config.model_path = options.vlm_model_path;
        vlm_config.projector_path = options.mmproj_path;
        vlm_config.use_gpu = options.vlm_use_gpu;
        if (!scene_analyzer.start(vlm_config, &error_message)) {
            std::cerr << "无法启动 VLM：" << error_message << '\n';
            pipeline.stop();
            return EXIT_FAILURE;
        }
    }
#else
    if (options.enable_vlm) {
        std::cerr << "当前构建未启用 BUILD_VLM。请重新执行 CMake 配置并传入 -DBUILD_VLM=ON。\n";
        pipeline.stop();
        return EXIT_FAILURE;
    }
#endif
    std::cout << "YOLO 实时预览已启动：摄像头 " << options.camera.frames_per_second << " FPS，推理上限 "
              << options.pipeline.inference_frames_per_second << " FPS，后端 "
              << (options.pipeline.yolo.use_cuda ? "CUDA" : "CPU") << "。\n";
    if (options.enable_vlm) {
        std::cout << "VLM 正在后台加载 Gemma 与 mmproj；加载完成后每 "
                  << options.scene_interval_seconds << " 秒分析一帧，空格可立即分析。\n";
    }
    if (options.show_window) {
        cv::namedWindow("AI Assistant Vision Workbench", cv::WINDOW_NORMAL);
        cv::resizeWindow("AI Assistant Vision Workbench", options.window_width, options.window_height);
    }

    const auto event_ttl = std::chrono::milliseconds(1250);
    const auto preview_started_at = std::chrono::steady_clock::now();
    auto next_scene_analysis_at = preview_started_at + std::chrono::seconds(options.scene_interval_seconds);
    int captured_frames = 0;
    while (options.frame_limit == 0 || captured_frames < options.frame_limit) {
        vision::Frame frame;
        if (!camera.read(&frame, &error_message)) {
            std::cerr << error_message << '\n';
            pipeline.stop();
            return EXIT_FAILURE;
        }
        ++captured_frames;
        pipeline.submit(frame);

        const auto now = std::chrono::steady_clock::now();
#ifdef VISION_HAS_VLM
        if (options.enable_vlm && now >= next_scene_analysis_at) {
            scene_analyzer.submit(frame);
            do {
                next_scene_analysis_at += std::chrono::seconds(options.scene_interval_seconds);
            } while (next_scene_analysis_at <= now);
        }
#endif

        DisplayVlmStatus vlm_status;
#ifdef VISION_HAS_VLM
        if (options.enable_vlm) {
            const vision::SceneAnalysisResult result = scene_analyzer.latest_result();
            vlm_status.state = vision::scene_analysis_state_name(result.state);
            vlm_status.latency_ms = result.latency_ms;
            vlm_status.summary = result.summary;
            vlm_status.message = result.message;
        }
#endif

        if (!options.show_window) {
            continue;
        }
        cv::Mat display = frame.bgr_image.clone();
        const auto event = pipeline.latest_event();
        if (event.has_value()) {
            const auto event_age = std::chrono::milliseconds(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()
                                                                         .time_since_epoch())
                    .count() -
                event->occurred_at_unix_ms);
            if (event_age < event_ttl) {
                draw_event(&display, *event);
            }
        }
        const vision::VisionPipelineStats stats = pipeline.stats();
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      next_scene_analysis_at - std::chrono::steady_clock::now())
                                      .count();
        const int next_scene_scan_seconds = std::max(0, static_cast<int>((remaining_ms + 999) / 1000));
        const cv::Mat workbench = build_workbench_view(display, frame, stats, event, options,
                                                       next_scene_scan_seconds, vlm_status);
        cv::imshow("AI Assistant Vision Workbench", workbench);
        const int key = cv::waitKey(1);
 #ifdef VISION_HAS_VLM
        if (key == ' ' && options.enable_vlm && !scene_analyzer.submit(frame)) {
            std::cout << "无法立即分析：VLM 尚未就绪或正在处理。\n";
        }
 #endif
        if (key == 'q' || key == 'Q' || key == 27) {
            break;
        }
    }

#ifdef VISION_HAS_VLM
    scene_analyzer.stop();
#endif
    pipeline.stop();
    const vision::VisionPipelineStats stats = pipeline.stats();
    const auto average_inference_ms = stats.processed_frames == 0
                                          ? 0
                                          : stats.total_inference_duration_ms / stats.processed_frames;
    std::cout << "YOLO 预览结束：采集 " << captured_frames << " 帧，推理 " << stats.processed_frames
              << " 帧，平均推理 " << average_inference_ms << " ms，丢弃旧帧 "
              << stats.dropped_frames << " 帧。\n";
    return EXIT_SUCCESS;
}
