#include "asr.h"
#include "audio.h"
#include "chat_agent.h"
#include "conversation_runtime.h"
#include "key.h"
#include "local_voice_pipeline.h"
#include "tts_pipeline.h"

#include <atomic>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

std::atomic<bool> g_quit{false};
cockpit::LocalVoicePipeline* g_pipeline = nullptr;

class RuntimeAsr final : public cockpit::AsrPort {
public:
    void reset() override { asr_reset(); }

    bool acceptPcm(const int16_t* samples, std::size_t sample_count,
                   cockpit::PcmFormat format) override {
        if (format.sample_rate != 16000 || format.channels != 1 ||
            sample_count > static_cast<std::size_t>(INT32_MAX)) {
            std::cerr << "[CockpitRuntime] ASR expects 16 kHz mono PCM\n";
            return false;
        }
        return asr_process_frame(samples, static_cast<int>(sample_count)) == 0;
    }

    std::string finalText() override { return asr_get_result(); }
};

class RuntimeConversation final : public cockpit::ConversationPort {
public:
    explicit RuntimeConversation(conversation::ConversationRuntime& runtime) : runtime_(runtime) {}

    void submitVoiceText(std::string text) override {
        conversation::ConversationRequest request;
        request.text = std::move(text);
        request.source = conversation::InputSource::Voice;
        request.enable_tts = true;
        runtime_.submit(std::move(request));
    }

private:
    conversation::ConversationRuntime& runtime_;
};

class RuntimeTts final : public cockpit::TtsPort {
public:
    void interrupt() override { tts_pipeline_interrupt(); }
};

void onAudioCapture(audio_capture_event_t event, const int16_t* samples,
                    std::size_t sample_count, unsigned int sample_rate,
                    unsigned int channels, void*) {
    if (!g_pipeline) return;
    const cockpit::PcmFormat format{static_cast<int>(sample_rate), static_cast<int>(channels)};
    switch (event) {
        case AUDIO_CAPTURE_STARTED:
            g_pipeline->onCaptureStarted(format);
            break;
        case AUDIO_CAPTURE_PCM:
            g_pipeline->onPcm(samples, sample_count, format);
            break;
        case AUDIO_CAPTURE_ENDED:
            g_pipeline->onCaptureEnded();
            break;
    }
}

void signalHandler(int) {
    g_quit = true;
}

void printUsage(const char* binary) {
    std::cerr << "Usage: " << binary
              << " <asr_model_dir> <llm_model.gguf> <tts_model_dir> [usb|wm8960]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 5) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const AudioConfig* audio_config = &AUDIO_CFG_USB;
    if (argc == 5 && std::string(argv[4]) == "wm8960") audio_config = &AUDIO_CFG_WM8960;
    if (audio_config->rate != 16000 || audio_config->channels != 1) {
        std::cerr << "[CockpitRuntime] P0 ASR currently requires a 16 kHz mono input.\n";
        return EXIT_FAILURE;
    }

    if (asr_init(argv[1]) != 0 || agent_init(argv[2], nullptr) != 0 ||
        tts_pipeline_init(argv[3], nullptr) != 0) {
        std::cerr << "[CockpitRuntime] model initialization failed\n";
        tts_pipeline_destroy();
        agent_destroy();
        asr_destroy();
        return EXIT_FAILURE;
    }

    conversation::ConversationRuntime runtime("runtime/assistant_memory_v2.tsv",
                                              "runtime/device_fault_events.tsv");
    if (!runtime.initialize() || !runtime.start()) {
        std::cerr << "[CockpitRuntime] conversation runtime initialization failed\n";
        tts_pipeline_destroy();
        agent_destroy();
        asr_destroy();
        return EXIT_FAILURE;
    }

    std::string reply_buffer;
    runtime.setEventCallback([&reply_buffer](const conversation::ConversationEvent& event) {
        if (event.type == conversation::EventType::ReplyDelta && event.enable_tts) {
            reply_buffer += event.text;
        } else if (event.type == conversation::EventType::ReplyFinal && event.enable_tts) {
            tts_pipeline_push(reply_buffer.c_str(), 1);
            reply_buffer.clear();
        }
    });

    RuntimeAsr asr;
    RuntimeConversation conversation(runtime);
    RuntimeTts tts;
    cockpit::LocalVoicePipeline pipeline(asr, conversation, tts);
    pipeline.start();
    g_pipeline = &pipeline;
    audio_set_capture_callback(&onAudioCapture, nullptr);

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    if (audio_init(audio_config) != 0 ||
        Key_Thread("/dev/input/by-path/platform-gpio_keys@0-event") != 0) {
        std::cerr << "[CockpitRuntime] ALSA or key initialization failed\n";
        audio_set_capture_callback(nullptr, nullptr);
        g_pipeline = nullptr;
        pipeline.stop();
        runtime.stop();
        tts_pipeline_destroy();
        agent_destroy();
        asr_destroy();
        return EXIT_FAILURE;
    }

    std::cout << "[CockpitRuntime] local audio pipeline ready; legacy TCP is not used.\n";
    while (!g_quit) pause();

    audio_set_capture_callback(nullptr, nullptr);
    g_pipeline = nullptr;
    pipeline.stop();
    audio_cleanup();
    runtime.stop();
    tts_pipeline_destroy();
    agent_destroy();
    asr_destroy();
    return EXIT_SUCCESS;
}
