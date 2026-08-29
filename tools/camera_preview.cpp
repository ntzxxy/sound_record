#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_source.h"

namespace {

struct PreviewOptions {
    vision::CameraConfig camera;
    bool show_window = true;
    int frame_limit = 0;
    std::filesystem::path save_directory = "vision_records/captures";
};

void print_usage(const char* program_name) {
    std::cout << "用法: " << program_name
              << " [--device /dev/video0|0] [--width 640] [--height 480] [--fps 15]"
                 " [--pixel-format mjpg|yuyv] [--frames N] [--headless] [--save-dir PATH]\n"
                 "窗口模式：按 q 或 Esc 退出，按 s 将当前画面保存为 JPEG。\n";
}

bool parse_positive(const std::string& text, int* value) {
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

bool read_argument(int argc, char* argv[], int* index, std::string* value) {
    if (*index + 1 >= argc) {
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool parse_arguments(int argc, char* argv[], PreviewOptions* options) {
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
        if (argument == "--device" && read_argument(argc, argv, &index, &value)) {
            options->camera.device = value;
            continue;
        }
        if (argument == "--save-dir" && read_argument(argc, argv, &index, &value)) {
            options->save_directory = value;
            continue;
        }
        if (argument == "--pixel-format" && read_argument(argc, argv, &index, &value)) {
            if (value == "mjpg") {
                options->camera.pixel_format = "MJPG";
                continue;
            }
            if (value == "yuyv") {
                options->camera.pixel_format = "YUYV";
                continue;
            }
        }

        int* target = nullptr;
        if (argument == "--width") {
            target = &options->camera.width;
        } else if (argument == "--height") {
            target = &options->camera.height;
        } else if (argument == "--fps") {
            target = &options->camera.frames_per_second;
        } else if (argument == "--frames") {
            target = &options->frame_limit;
        }
        if (target != nullptr && read_argument(argc, argv, &index, &value) &&
            parse_positive(value, target)) {
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

bool save_snapshot(const vision::Frame& frame, const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "无法创建截图目录: " << error.message() << '\n';
        return false;
    }
    const std::filesystem::path path = directory /
                                       ("capture-" + std::to_string(frame.captured_at_unix_ms) + ".jpg");
    if (!cv::imwrite(path.string(), frame.bgr_image)) {
        std::cerr << "截图保存失败: " << path << '\n';
        return false;
    }
    std::cout << "截图已保存: " << path << '\n';
    return true;
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

    std::cout << "摄像头已打开: " << options.camera.device << "，目标分辨率 "
              << options.camera.width << 'x' << options.camera.height << "，格式 "
              << options.camera.pixel_format << "，按 q 退出。\n";
    if (options.show_window) {
        cv::namedWindow("AI Assistant Camera", cv::WINDOW_AUTOSIZE);
    }

    int captured_frames = 0;
    while (options.frame_limit == 0 || captured_frames < options.frame_limit) {
        vision::Frame frame;
        if (!camera.read(&frame, &error_message)) {
            std::cerr << error_message << '\n';
            return EXIT_FAILURE;
        }
        ++captured_frames;

        if (!options.show_window) {
            continue;
        }

        cv::Mat display = frame.bgr_image.clone();
        const std::string status = "frame " + std::to_string(frame.sequence) +
                                   " | " + std::to_string(display.cols) + "x" +
                                   std::to_string(display.rows);
        cv::putText(display, status, {12, 28}, cv::FONT_HERSHEY_SIMPLEX, 0.65,
                    {0, 255, 0}, 2, cv::LINE_AA);
        cv::imshow("AI Assistant Camera", display);

        const int key = cv::waitKey(1);
        if (key == 'q' || key == 'Q' || key == 27) {
            break;
        }
        if (key == 's' || key == 'S') {
            save_snapshot(frame, options.save_directory);
        }
    }

    std::cout << "摄像头预览已结束，共读取 " << captured_frames << " 帧。\n";
    return EXIT_SUCCESS;
}
