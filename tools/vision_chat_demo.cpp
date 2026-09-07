#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "camera_source.h"
#include "scene_analyzer.h"

namespace {

bool wait_for_state(vision::SceneAnalyzer* analyzer,
                    vision::SceneAnalysisState expected,
                    std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const vision::SceneAnalysisResult result = analyzer->latest_result();
        if (result.state == expected) {
            return true;
        }
        if (result.state == vision::SceneAnalysisState::kFailed) {
            std::cerr << "VLM failed: " << result.message << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "Timed out while waiting for " << vision::scene_analysis_state_name(expected) << '\n';
    return false;
}

bool print_completed_result(vision::SceneAnalyzer* analyzer) {
    if (!wait_for_state(analyzer, vision::SceneAnalysisState::kCompleted, std::chrono::seconds(120))) {
        return false;
    }
    const vision::SceneAnalysisResult result = analyzer->latest_result();
    std::cout << "kind: " << vision::scene_analysis_kind_name(result.kind) << '\n'
              << "question: " << result.user_question << '\n'
              << "answer: " << result.summary << '\n'
              << "latency_ms: " << result.latency_ms << "\n\n";
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string device = "/dev/video0";
    if (argc == 3 && std::string(argv[1]) == "--device") {
        device = argv[2];
    } else if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " [--device /dev/video0]\n";
        return EXIT_FAILURE;
    }

    vision::CameraConfig camera_config;
    camera_config.device = device;
    vision::CameraSource camera;
    std::string error;
    if (!camera.open(camera_config, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    vision::Frame frame;
    if (!camera.read(&frame, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    vision::SceneAnalyzer analyzer;
    vision::SceneAnalyzerConfig config;
    config.use_gpu = false;
    // Keep this regression test quick; the assertions validate frame/session
    // selection rather than answer length. The workbench keeps its normal limit.
    config.max_generation_tokens = 16;
    if (!analyzer.start(config, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    if (!wait_for_state(&analyzer, vision::SceneAnalysisState::kWaiting, std::chrono::seconds(90))) {
        return EXIT_FAILURE;
    }

    const std::string first_question = "请问这是什么设备？画面中有哪些明显物体？";
    std::cout << "First question: " << first_question << '\n';
    if (!analyzer.ask_about_frame(frame, first_question) || !print_completed_result(&analyzer)) {
        return EXIT_FAILURE;
    }

    // This is the regression check for a user selecting a new camera image:
    // history must be cleared and the next question must use that frame, not
    // the earlier active snapshot.
    vision::Frame replacement_frame = frame;
    replacement_frame.sequence += 1000000;
    replacement_frame.bgr_image = frame.bgr_image.clone();
    if (!analyzer.select_visual_snapshot(replacement_frame)) {
        std::cerr << "Unable to select replacement visual snapshot\n";
        return EXIT_FAILURE;
    }
    const std::string replacement_question = "这是新选择的画面。请只根据当前图片说出主要物体。";
    std::cout << "Replacement-frame question: " << replacement_question << '\n';
    if (!analyzer.ask_follow_up(replacement_question) || !print_completed_result(&analyzer)) {
        return EXIT_FAILURE;
    }
    const vision::SceneAnalysisResult replacement_result = analyzer.latest_result();
    if (replacement_result.kind != vision::SceneAnalysisKind::kVisualQuestion ||
        replacement_result.source_frame_sequence != replacement_frame.sequence) {
        std::cerr << "Replacement-frame regression check failed: kind="
                  << vision::scene_analysis_kind_name(replacement_result.kind)
                  << " source_frame=" << replacement_result.source_frame_sequence << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Replacement-frame regression check passed.\n\n";

    analyzer.stop();
    return EXIT_SUCCESS;
}
