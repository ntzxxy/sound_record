#include "scene_analyzer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

namespace vision {
namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

std::string strip_control_tokens(std::string text) {
    std::size_t begin = 0;
    while ((begin = text.find('<', begin)) != std::string::npos) {
        const std::size_t end = text.find('>', begin + 1);
        if (end == std::string::npos) {
            break;
        }
        text.erase(begin, end - begin + 1);
    }
    return trim(std::move(text));
}

std::string truncate_utf8(const std::string& text, std::size_t max_characters) {
    std::size_t characters = 0;
    std::size_t index = 0;
    while (index < text.size() && characters < max_characters) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        std::size_t width = 1;
        if ((byte & 0xF0U) == 0xF0U) {
            width = 4;
        } else if ((byte & 0xE0U) == 0xE0U) {
            width = 3;
        } else if ((byte & 0xC0U) == 0xC0U) {
            width = 2;
        }
        if (index + width > text.size()) {
            break;
        }
        index += width;
        ++characters;
    }
    if (index == text.size()) {
        return text;
    }
    return text.substr(0, index) + "...";
}

}  // namespace

const char* scene_analysis_state_name(SceneAnalysisState state) {
    switch (state) {
        case SceneAnalysisState::kDisabled:
            return "disabled";
        case SceneAnalysisState::kLoading:
            return "loading";
        case SceneAnalysisState::kWaiting:
            return "waiting";
        case SceneAnalysisState::kCaptured:
            return "captured";
        case SceneAnalysisState::kAnalyzing:
            return "analyzing";
        case SceneAnalysisState::kCompleted:
            return "completed";
        case SceneAnalysisState::kFailed:
            return "failed";
        case SceneAnalysisState::kStopped:
            return "stopped";
    }
    return "unknown";
}

class SceneAnalyzer::Impl {
  public:
    ~Impl() {
        release_models();
    }

    void publish(SceneAnalysisState state, const Frame* frame, std::string summary, std::string message,
                 std::int64_t latency_ms = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        result.state = state;
        if (frame != nullptr) {
            result.source_frame_sequence = frame->sequence;
            result.captured_at_unix_ms = frame->captured_at_unix_ms;
        }
        result.latency_ms = latency_ms;
        result.summary = std::move(summary);
        result.message = std::move(message);
    }

    void initialize_models() {
        const auto quiet_log = [](enum ggml_log_level, const char*, void*) {};
        ggml_log_set(quiet_log, nullptr);
        llama_log_set(quiet_log, nullptr);
        mtmd_helper_log_set(quiet_log, nullptr);
        ggml_backend_load_all();

        llama_model_params model_params = llama_model_default_params();
        model_params.n_gpu_layers = config.use_gpu ? 99 : 0;
        model = llama_model_load_from_file(config.model_path.c_str(), model_params);
        if (model == nullptr) {
            throw std::runtime_error("unable to load the Gemma model");
        }

        llama_context_params context_params = llama_context_default_params();
        context_params.n_ctx = static_cast<std::uint32_t>(config.context_tokens);
        context_params.n_batch = 512;
        context = llama_init_from_model(model, context_params);
        if (context == nullptr) {
            throw std::runtime_error("unable to create the Gemma context");
        }

        mtmd_context_params projector_params = mtmd_context_params_default();
        projector_params.use_gpu = config.use_gpu;
        projector_params.print_timings = false;
        projector_params.n_threads = config.threads;
        projector_params.warmup = false;
        projector = mtmd_init_from_file(config.projector_path.c_str(), model, projector_params);
        if (projector == nullptr || !mtmd_support_vision(projector)) {
            throw std::runtime_error("unable to load a vision-capable mmproj");
        }
        vocab = llama_model_get_vocab(model);
        if (vocab == nullptr) {
            throw std::runtime_error("the Gemma vocabulary is unavailable");
        }
    }

    void release_models() {
        if (projector != nullptr) {
            mtmd_free(projector);
            projector = nullptr;
        }
        if (context != nullptr) {
            llama_free(context);
            context = nullptr;
        }
        if (model != nullptr) {
            llama_model_free(model);
            model = nullptr;
        }
        vocab = nullptr;
    }

    std::string analyze(const Frame& frame) {
        if (frame.bgr_image.empty()) {
            throw std::runtime_error("captured frame is empty");
        }

        cv::Mat rgb;
        cv::cvtColor(frame.bgr_image, rgb, cv::COLOR_BGR2RGB);
        if (!rgb.isContinuous()) {
            rgb = rgb.clone();
        }
        mtmd::bitmap image(static_cast<std::uint32_t>(rgb.cols), static_cast<std::uint32_t>(rgb.rows),
                           rgb.ptr<unsigned char>());
        if (!image.ptr) {
            throw std::runtime_error("unable to create the image input");
        }

        const std::string prompt =
            "<|turn>system\n"
            "You are a visual assistant. Reply only with one short factual sentence in Chinese. "
            "Do not show reasoning or analysis.\n"
            "<turn|>\n<|turn>user\n" +
            std::string(mtmd_default_marker()) +
            "Describe the main objects and scene in this image.\n<turn|>\n<|turn>model\n";
        const mtmd_input_text input{prompt.c_str(), true, true};
        mtmd::input_chunks chunks(mtmd_input_chunks_init());
        if (!chunks.ptr) {
            throw std::runtime_error("unable to allocate multimodal input chunks");
        }
        mtmd::bitmaps images;
        images.entries.push_back(std::move(image));
        std::vector<const mtmd_bitmap*> bitmaps = images.c_ptr();
        if (mtmd_tokenize(projector, chunks.ptr.get(), &input, bitmaps.data(), bitmaps.size()) != 0) {
            throw std::runtime_error("unable to tokenize the image prompt");
        }

        llama_memory_clear(llama_get_memory(context), true);
        llama_pos n_past = 0;
        if (mtmd_helper_eval_chunks(projector, context, chunks.ptr.get(), n_past, 0, 512, true, &n_past) != 0) {
            throw std::runtime_error("unable to evaluate the multimodal prompt");
        }

        llama_sampler_chain_params sampler_params = llama_sampler_chain_default_params();
        llama_sampler* sampler = llama_sampler_chain_init(sampler_params);
        if (sampler == nullptr) {
            throw std::runtime_error("unable to create the VLM sampler");
        }
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

        std::string response;
        for (int index = 0; index < config.max_generation_tokens; ++index) {
            const llama_token token = llama_sampler_sample(sampler, context, -1);
            if (llama_vocab_is_eog(vocab, token)) {
                break;
            }
            char piece[256];
            const int length = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
            if (length > 0) {
                response.append(piece, static_cast<std::size_t>(length));
            }
            llama_sampler_accept(sampler, token);
            const llama_batch batch = llama_batch_get_one(const_cast<llama_token*>(&token), 1);
            if (llama_decode(context, batch) != 0) {
                llama_sampler_free(sampler);
                throw std::runtime_error("unable to decode VLM output token");
            }
            ++n_past;
        }
        llama_sampler_free(sampler);

        response = truncate_utf8(strip_control_tokens(std::move(response)), 42);
        if (response.empty()) {
            throw std::runtime_error("VLM returned an empty response");
        }
        return response;
    }

    void worker_loop() {
        try {
            initialize_models();
            initialized = true;
            publish(SceneAnalysisState::kWaiting, nullptr, {}, "ready");
        } catch (const std::exception& exception) {
            release_models();
            publish(SceneAnalysisState::kFailed, nullptr, {}, exception.what());
            return;
        }

        while (true) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work_available.wait(lock, [this] { return stopping || pending_frame.has_value(); });
                if (stopping) {
                    return;
                }
                frame = std::move(*pending_frame);
                pending_frame.reset();
                result.state = SceneAnalysisState::kAnalyzing;
            }

            const auto started_at = std::chrono::steady_clock::now();
            try {
                const std::string summary = analyze(frame);
                const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started_at)
                                            .count();
                publish(SceneAnalysisState::kCompleted, &frame, summary, {}, latency_ms);
                std::cout << "[VLM] 场景分析完成（" << latency_ms << " ms）：" << summary << '\n';
            } catch (const std::exception& exception) {
                const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started_at)
                                            .count();
                publish(SceneAnalysisState::kFailed, &frame, {}, exception.what(), latency_ms);
                std::cerr << "[VLM] 场景分析失败：" << exception.what() << '\n';
            }
        }
    }

    SceneAnalyzerConfig config;
    mutable std::mutex mutex;
    std::condition_variable work_available;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    bool initialized = false;
    std::optional<Frame> pending_frame;
    SceneAnalysisResult result;
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    const llama_vocab* vocab = nullptr;
    mtmd_context* projector = nullptr;
};

SceneAnalyzer::SceneAnalyzer() : impl_(std::make_unique<Impl>()) {}

SceneAnalyzer::~SceneAnalyzer() {
    stop();
}

bool SceneAnalyzer::start(const SceneAnalyzerConfig& config, std::string* error_message) {
    stop();
    if (config.context_tokens <= 0 || config.threads <= 0 || config.max_generation_tokens <= 0) {
        if (error_message != nullptr) {
            *error_message = "VLM 参数必须为正数。";
        }
        return false;
    }
    if (!std::filesystem::is_regular_file(config.model_path) ||
        !std::filesystem::is_regular_file(config.projector_path)) {
        if (error_message != nullptr) {
            *error_message = "未找到 Gemma 主模型或视觉投影文件。";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->config = config;
        impl_->stopping = false;
        impl_->running = true;
        impl_->initialized = false;
        impl_->pending_frame.reset();
        impl_->result = {};
        impl_->result.state = SceneAnalysisState::kLoading;
        impl_->result.message = "loading Gemma and mmproj";
    }
    impl_->worker = std::thread(&Impl::worker_loop, impl_.get());
    return true;
}

bool SceneAnalyzer::submit(const Frame& frame) {
    if (frame.bgr_image.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running || !impl_->initialized || impl_->pending_frame.has_value() ||
            impl_->result.state == SceneAnalysisState::kCaptured ||
            impl_->result.state == SceneAnalysisState::kAnalyzing) {
            return false;
        }
        impl_->pending_frame = frame;
        impl_->pending_frame->bgr_image = frame.bgr_image.clone();
        impl_->result.state = SceneAnalysisState::kCaptured;
        impl_->result.source_frame_sequence = frame.sequence;
        impl_->result.captured_at_unix_ms = frame.captured_at_unix_ms;
        impl_->result.latency_ms = 0;
        impl_->result.summary.clear();
        impl_->result.message = "frame captured";
    }
    impl_->work_available.notify_one();
    return true;
}

SceneAnalysisResult SceneAnalyzer::latest_result() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->result;
}

bool SceneAnalyzer::is_busy() const {
    const SceneAnalysisResult current = latest_result();
    return current.state == SceneAnalysisState::kLoading || current.state == SceneAnalysisState::kCaptured ||
           current.state == SceneAnalysisState::kAnalyzing;
}

void SceneAnalyzer::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        impl_->stopping = true;
        impl_->running = false;
    }
    impl_->work_available.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->release_models();
    impl_->publish(SceneAnalysisState::kStopped, nullptr, {}, "stopped");
}

}  // namespace vision
