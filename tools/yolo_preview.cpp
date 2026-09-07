#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
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
#ifdef VISION_HAS_LOCAL_ASR
#include "microphone_asr.h"
#endif

namespace {

struct PreviewOptions {
    vision::CameraConfig camera;
    vision::VisionPipelineConfig pipeline;
    bool show_window = true;
    int frame_limit = 0;
    int window_width = 1280;
    int window_height = 820;
    int scene_interval_seconds = 20;
    bool enable_vlm = false;
    bool enable_voice = false;
    bool vlm_use_gpu = false;
    std::string vlm_model_path = "models/gemma-4-E4B-it-Q4_0.gguf";
    std::string mmproj_path = "models/gemma-4-E4B-it/mmproj-gemma-4-E4B-it-Q8_0.gguf";
    std::string asr_model_path =
        "models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16";
    std::string microphone_device = "plughw:1,0";
};

struct DisplayVlmStatus {
    std::string state = "disabled";
    std::int64_t latency_ms = 0;
    std::string summary;
    std::string message;
};

struct DisplayChatStatus {
    bool voice_enabled = false;
    bool text_editing = false;
    std::string state = "disabled";
    std::string partial_text;
    std::string draft_text;
    std::string question;
    std::string answer;
    std::string message;
};

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name
              << " [--device /dev/video0|0] [--width 1920] [--height 1080] [--camera-fps 15]"
                 " [--inference-fps 5] [--model PATH] [--conf 0.35] [--no-cuda]"
                 " [--window-width 1280] [--window-height 820] [--scene-interval 20]"
                 " [--enable-vlm] [--vlm-model PATH] [--mmproj PATH] [--vlm-gpu]"
                 " [--enable-voice] [--asr-model DIR] [--mic-device plughw:1,0]"
                 " [--frames N] [--headless]\n"
                 "窗口模式：空格切换到当前画面并立即分析；T 编辑文字问题，V 从 Windows 剪贴板粘贴，Enter 发送；"
                 "启用 --enable-voice 后，按 R 开始/结束一轮语音提问；"
                 "q 或 Esc 退出。检测线程只处理最新帧，避免延迟累积。\n";
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
        if (argument == "--enable-voice") {
            options->enable_voice = true;
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
        if (argument == "--asr-model" && read_argument(argc, argv, &index, &value)) {
            options->asr_model_path = value;
            continue;
        }
        if (argument == "--mic-device" && read_argument(argc, argv, &index, &value)) {
            options->microphone_device = value;
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
                              const DisplayVlmStatus& vlm,
                              const DisplayChatStatus& chat) {
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
         << vlm.latency_ms << "},\n"
         << "  \"chat\": {\"voice_enabled\": " << (chat.voice_enabled ? "true" : "false")
         << ", \"state\": \"" << escape_json(chat.state) << "\"}";

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

std::vector<std::string> wrap_utf8_lines(const std::string& text, std::size_t max_characters) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t characters = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\n') {
            lines.push_back(current);
            current.clear();
            characters = 0;
            ++index;
            continue;
        }
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        std::size_t width = 1;
        if ((byte & 0xF0U) == 0xF0U) width = 4;
        else if ((byte & 0xE0U) == 0xE0U) width = 3;
        else if ((byte & 0xC0U) == 0xC0U) width = 2;
        if (index + width > text.size()) break;
        if (characters >= max_characters) {
            lines.push_back(current);
            current.clear();
            characters = 0;
        }
        current.append(text, index, width);
        index += width;
        ++characters;
    }
    if (!current.empty() || lines.empty()) lines.push_back(current);
    return lines;
}

std::string trim_whitespace(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first >= last ? std::string{} : std::string(first, last);
}

void remove_last_utf8_codepoint(std::string* text) {
    if (text == nullptr || text->empty()) return;
    std::size_t position = text->size() - 1;
    while (position > 0 && (static_cast<unsigned char>((*text)[position]) & 0xC0U) == 0x80U) {
        --position;
    }
    text->erase(position);
}

std::optional<std::string> read_windows_clipboard(std::string* error_message) {
    // The command is static: clipboard text is read from PowerShell output and is never interpolated into a shell.
    constexpr const char* kClipboardCommand =
        "powershell.exe -NoProfile -Command \"[Console]::OutputEncoding = "
        "[System.Text.UTF8Encoding]::new(); Get-Clipboard -Raw\"";
    FILE* stream = popen(kClipboardCommand, "r");
    if (stream == nullptr) {
        if (error_message != nullptr) *error_message = "无法调用 powershell.exe 读取 Windows 剪贴板";
        return std::nullopt;
    }
    std::string clipboard;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), stream) != nullptr) clipboard += buffer;
    const int result = pclose(stream);
    clipboard = trim_whitespace(clipboard);
    if (result != 0 || clipboard.empty()) {
        if (error_message != nullptr) *error_message = "Windows 剪贴板为空，或无法读取文本";
        return std::nullopt;
    }
    return clipboard;
}

int draw_wrapped_text(cv::Mat* image,
                      const std::string& prefix,
                      const std::string& text,
                      const cv::Point& start,
                      int font_height,
                      int line_spacing,
                      const cv::Scalar& color,
                      std::size_t line_width,
                      int max_lines = 4) {
    const std::vector<std::string> lines = wrap_utf8_lines(prefix + text, line_width);
    const int visible = std::min<int>(static_cast<int>(lines.size()), max_lines);
    for (int index = 0; index < visible; ++index) {
        draw_text_lines(image, lines[index], {start.x, start.y + index * line_spacing}, font_height,
                        line_spacing, color);
    }
    return start.y + visible * line_spacing;
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
                             const DisplayVlmStatus& vlm,
                             const DisplayChatStatus& chat) {
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

    const std::string status_json =
        build_status_json(frame, stats, event, options, next_scene_scan_seconds, vlm, chat);
    draw_text_lines(&canvas, status_json, {panel_rect.x + 16, panel_rect.y + 70}, 16, 21,
                    cv::Scalar(226, 226, 226));

    int section_y = panel_rect.y + 304;
    cv::line(canvas, {panel_rect.x + 14, section_y - 14},
             {panel_rect.x + panel_rect.width - 14, section_y - 14}, cv::Scalar(72, 78, 88), 1);
    cv::putText(canvas, "IMAGE CONTENT", {panel_rect.x + 16, section_y}, cv::FONT_HERSHEY_SIMPLEX, 0.49,
                cv::Scalar(89, 220, 150), 1, cv::LINE_AA);
    const std::string image_description = vlm.summary.empty() ? vlm.message : vlm.summary;
    section_y = draw_wrapped_text(&canvas, "", image_description, {panel_rect.x + 16, section_y + 24},
                                  15, 20,
                                  vlm.state == "failed" ? cv::Scalar(92, 92, 255) : cv::Scalar(220, 220, 220),
                                  34, 2);

    section_y += 10;
    cv::line(canvas, {panel_rect.x + 14, section_y - 14},
             {panel_rect.x + panel_rect.width - 14, section_y - 14}, cv::Scalar(72, 78, 88), 1);
    cv::putText(canvas, "TEXT VISUAL DIALOGUE", {panel_rect.x + 16, section_y},
                cv::FONT_HERSHEY_SIMPLEX, 0.49, cv::Scalar(109, 204, 255), 1, cv::LINE_AA);
    const cv::Scalar input_border = chat.text_editing ? cv::Scalar(89, 220, 150) : cv::Scalar(72, 78, 88);
    cv::rectangle(canvas, {panel_rect.x + 14, section_y + 8, panel_rect.width - 28, 42}, input_border, 1);
    const std::string input_text = chat.draft_text.empty()
                                       ? "T edit | V paste Chinese | Enter send"
                                       : chat.draft_text + (chat.text_editing ? "_" : "");
    draw_wrapped_text(&canvas, "", input_text, {panel_rect.x + 20, section_y + 34}, 15, 18,
                      chat.draft_text.empty() ? cv::Scalar(150, 150, 150) : cv::Scalar(230, 230, 230), 54, 1);
    section_y = draw_wrapped_text(&canvas, "状态：", chat.message, {panel_rect.x + 16, section_y + 72}, 15,
                                  20, cv::Scalar(247, 205, 83), 34, 2);
    if (!chat.partial_text.empty() && chat.state == "listening") {
        section_y = draw_wrapped_text(&canvas, "识别中：", chat.partial_text,
                                      {panel_rect.x + 16, section_y + 4}, 15, 20,
                                      cv::Scalar(205, 205, 205), 32, 2);
    }
    if (!chat.question.empty()) {
        section_y = draw_wrapped_text(&canvas, "我：", chat.question, {panel_rect.x + 16, section_y + 4},
                                      15, 20, cv::Scalar(205, 205, 205), 34, 2);
    }
    if (!chat.answer.empty()) {
        draw_wrapped_text(&canvas, "回答：", chat.answer, {panel_rect.x + 16, section_y + 4}, 15, 20,
                          cv::Scalar(205, 235, 205), 32, 4);
    }
    return canvas;
}

}  // namespace

int main(int argc, char* argv[]) {
    PreviewOptions options;
    if (!parse_arguments(argc, argv, &options)) {
        return EXIT_FAILURE;
    }
#ifndef VISION_HAS_LOCAL_ASR
    if (options.enable_voice) {
        std::cerr << "当前构建未启用本地 ASR。请重新执行 CMake 配置并传入 "
                     "-DBUILD_VISION_VOICE_TEST=ON。\n";
        return EXIT_FAILURE;
    }
#endif
#ifndef VISION_HAS_VLM
    if (options.enable_voice) {
        std::cerr << "语音问图依赖 VLM。请重新执行 CMake 配置并传入 -DBUILD_VLM=ON。\n";
        return EXIT_FAILURE;
    }
#endif

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

    DisplayChatStatus chat_status;
    chat_status.voice_enabled = options.enable_voice;
    chat_status.message = "按空格锁定当前画面；按 T 输入英文或按 V 粘贴中文问题";
#ifdef VISION_HAS_LOCAL_ASR
    vision_tools::MicrophoneAsr microphone;
    if (options.enable_voice) {
        vision_tools::MicrophoneAsrConfig asr_config;
        asr_config.device = options.microphone_device;
        asr_config.model_directory = options.asr_model_path;
        if (!microphone.start(asr_config, &error_message)) {
#ifdef VISION_HAS_VLM
            scene_analyzer.stop();
#endif
            pipeline.stop();
            std::cerr << "无法启动本地语音输入：" << error_message << '\n';
            return EXIT_FAILURE;
        }
    }
#endif
    std::cout << "YOLO 实时预览已启动：摄像头 " << options.camera.frames_per_second << " FPS，推理上限 "
              << options.pipeline.inference_frames_per_second << " FPS，后端 "
              << (options.pipeline.yolo.use_cuda ? "CUDA" : "CPU") << "。\n";
    if (options.enable_vlm) {
        std::cout << "VLM 正在后台加载 Gemma 与 mmproj；加载完成后每 "
                  << options.scene_interval_seconds << " 秒分析一帧，空格可切换到当前画面并立即分析。\n";
    }
    if (options.enable_voice) {
        std::cout << "语音问图已启用：按 R 开始说话，再按 R 提交；首次问题使用当前画面，"
                     "未切换场景时后续问题沿用同一视觉快照。\n";
    }
    if (options.show_window && options.enable_vlm) {
        std::cout << "文字问图：按空格锁定视觉上下文；按 T 编辑英文，按 V 从 Windows 剪贴板粘贴中文，"
                     "按 Enter 发送。YOLO 检测结果不会再自动切换上下文。\n";
    }
    if (options.show_window) {
        cv::namedWindow("AI Assistant Vision Workbench", cv::WINDOW_NORMAL);
        cv::resizeWindow("AI Assistant Vision Workbench", options.window_width, options.window_height);
    }

    const auto event_ttl = std::chrono::milliseconds(1250);
    const auto preview_started_at = std::chrono::steady_clock::now();
    auto next_scene_analysis_at = preview_started_at + std::chrono::seconds(options.scene_interval_seconds);
    std::optional<std::string> pending_visual_question;
    std::string latest_scene_description;
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
        const std::optional<vision::VisionEvent> event = pipeline.latest_event();

        const auto now = std::chrono::steady_clock::now();
#ifdef VISION_HAS_VLM
        // YOLO is deliberately not used to decide visual context: per-frame detections are noisy.
        // Only an explicit Space key press replaces the snapshot used for the conversation.
        if (options.enable_vlm && pending_visual_question.has_value()) {
            const bool accepted = scene_analyzer.has_active_snapshot()
                                      ? scene_analyzer.ask_follow_up(*pending_visual_question)
                                      : scene_analyzer.ask_about_frame(frame, *pending_visual_question);
            if (accepted) {
                pending_visual_question.reset();
                chat_status.state = "vlm_processing";
                chat_status.message = "视觉模型正在根据已锁定的画面回答";
            }
        } else if (options.enable_vlm && now >= next_scene_analysis_at) {
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
            if (result.kind == vision::SceneAnalysisKind::kAutomaticScene && !result.summary.empty()) {
                latest_scene_description = result.summary;
            }
            vlm_status.summary = latest_scene_description;
            vlm_status.message = result.message;
            if (result.kind != vision::SceneAnalysisKind::kAutomaticScene) {
                chat_status.question = result.user_question;
                if (result.state == vision::SceneAnalysisState::kCompleted) {
                    chat_status.state = "answered";
                    chat_status.answer = result.summary;
                    chat_status.message = "视觉回答完成（可继续文字追问；空格可切换画面）";
                } else if (result.state == vision::SceneAnalysisState::kFailed) {
                    chat_status.state = "failed";
                    chat_status.answer.clear();
                    chat_status.message = "视觉回答失败：" + result.message;
                }
            }
        }
#endif

#ifdef VISION_HAS_LOCAL_ASR
        if (options.enable_voice && !pending_visual_question.has_value()) {
            const vision_tools::MicrophoneAsrStatus microphone_status = microphone.status();
            if (chat_status.state != "vlm_processing" && chat_status.state != "answered" &&
                chat_status.state != "failed") {
                chat_status.state = microphone_status.state;
                chat_status.partial_text = microphone_status.partial_text;
                chat_status.message = microphone_status.message;
            } else if (microphone_status.recording) {
                chat_status.state = microphone_status.state;
                chat_status.partial_text = microphone_status.partial_text;
                chat_status.message = microphone_status.message;
            }
        }
#endif

        if (!options.show_window) {
            continue;
        }
        cv::Mat display = frame.bgr_image.clone();
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
                                                       next_scene_scan_seconds, vlm_status, chat_status);
        cv::imshow("AI Assistant Vision Workbench", workbench);
        int key = cv::waitKey(1);

        if (chat_status.text_editing) {
            if (key == 13 || key == 10) {
                chat_status.draft_text = trim_whitespace(chat_status.draft_text);
                if (chat_status.draft_text.empty()) {
                    chat_status.message = "请输入问题后再发送";
                } else if (!options.enable_vlm) {
                    chat_status.message = "请使用 --enable-vlm 启动视觉模型";
                } else {
                    pending_visual_question = chat_status.draft_text;
                    chat_status.question = chat_status.draft_text;
                    chat_status.answer.clear();
                    chat_status.partial_text.clear();
                    chat_status.draft_text.clear();
                    chat_status.text_editing = false;
                    chat_status.state = "waiting_for_vlm";
                    chat_status.message = "文字问题已提交，等待视觉模型空闲";
                    std::cout << "[Text] 收到视觉问题：" << chat_status.question << '\n';
                }
                key = -1;
            } else if (key == 27) {
                chat_status.text_editing = false;
                chat_status.draft_text.clear();
                chat_status.message = "已取消文字输入";
                key = -1;
            } else if (key == 8 || key == 127) {
                remove_last_utf8_codepoint(&chat_status.draft_text);
                key = -1;
            } else if (key == 'v' || key == 'V' || key == 22) {
                std::string clipboard_error;
                const std::optional<std::string> clipboard = read_windows_clipboard(&clipboard_error);
                if (clipboard.has_value()) {
                    chat_status.draft_text = *clipboard;
                    chat_status.message = "已从 Windows 剪贴板粘贴文字，按 Enter 发送";
                } else {
                    chat_status.message = clipboard_error;
                }
                key = -1;
            } else if (key >= 32 && key <= 126) {
                chat_status.draft_text.push_back(static_cast<char>(key));
                key = -1;
            }
        } else if (key == 't' || key == 'T') {
            chat_status.text_editing = true;
            chat_status.state = "editing_text";
            chat_status.message = "输入英文，或按 V 从 Windows 剪贴板粘贴中文；Enter 发送";
            key = -1;
        } else if (key == 'v' || key == 'V' || key == 22) {
            std::string clipboard_error;
            const std::optional<std::string> clipboard = read_windows_clipboard(&clipboard_error);
            if (clipboard.has_value()) {
                chat_status.text_editing = true;
                chat_status.draft_text = *clipboard;
                chat_status.state = "editing_text";
                chat_status.message = "已从 Windows 剪贴板粘贴文字，按 Enter 发送";
            } else {
                chat_status.message = clipboard_error;
            }
            key = -1;
        }
 #ifdef VISION_HAS_VLM
        if (key == ' ' && options.enable_vlm) {
            if (!scene_analyzer.select_visual_snapshot(frame)) {
                std::cout << "无法切换视觉上下文：VLM 尚未就绪。\n";
            } else {
                latest_scene_description.clear();
                chat_status.question.clear();
                chat_status.answer.clear();
                chat_status.partial_text.clear();
                chat_status.draft_text.clear();
                chat_status.text_editing = false;
                chat_status.state = "scene_selected";
                chat_status.message = "已锁定当前画面；YOLO 变化不会重置上下文，请输入文字提问";
                if (!scene_analyzer.submit(frame)) {
                    std::cout << "[Vision] 已切换到当前画面；VLM 正忙，自动描述将在空闲后更新。\n";
                } else {
                    std::cout << "[Vision] 已切换到当前画面并提交新的场景分析，frame="
                              << frame.sequence << '\n';
                }
            }
        }
 #endif
#ifdef VISION_HAS_LOCAL_ASR
        if ((key == 'r' || key == 'R') && options.enable_voice) {
            const vision_tools::MicrophoneAsrStatus microphone_status = microphone.status();
            if (microphone_status.recording) {
                const std::optional<std::string> question = microphone.end_recording();
                if (question.has_value()) {
                    pending_visual_question = *question;
                    chat_status.question = *question;
                    chat_status.answer.clear();
                    chat_status.partial_text.clear();
                    chat_status.state = "waiting_for_vlm";
                    chat_status.message = "已识别语音，等待视觉模型空闲";
                    std::cout << "[Voice] 识别到问题：" << *question << '\n';
                }
            } else {
                if (!microphone.begin_recording(&error_message)) {
                    chat_status.state = "failed";
                    chat_status.message = error_message;
                    std::cerr << "无法开始录音：" << error_message << '\n';
                }
            }
        }
#endif
        if (key == 'q' || key == 'Q' || key == 27) {
            break;
        }
    }

#ifdef VISION_HAS_LOCAL_ASR
    microphone.stop();
#endif
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
