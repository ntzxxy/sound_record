#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cockpit {

struct PcmFormat {
    int sample_rate{16000};
    int channels{1};
};

// The runtime owns concrete adapters for these ports.  Keeping the boundary
// small makes the audio pipeline testable without a microphone or any model.
class AsrPort {
public:
    virtual ~AsrPort() = default;
    virtual void reset() = 0;
    virtual bool acceptPcm(const int16_t* samples, std::size_t sample_count,
                           PcmFormat format) = 0;
    virtual std::string finalText() = 0;
};

class ConversationPort {
public:
    virtual ~ConversationPort() = default;
    virtual void submitVoiceText(std::string text) = 0;
};

class TtsPort {
public:
    virtual ~TtsPort() = default;
    virtual void interrupt() = 0;
};

// Single-process, bounded asynchronous path for ALSA capture events.  It is
// deliberately an in-process queue rather than IPC/ZMQ: all AI modules run on
// one Jetson and audio must not be serialized or sent over a socket.
class LocalVoicePipeline {
public:
    struct Config {
        std::size_t max_pending_events{64};
    };

    LocalVoicePipeline(AsrPort& asr, ConversationPort& conversation, TtsPort& tts);
    LocalVoicePipeline(AsrPort& asr, ConversationPort& conversation,
                       TtsPort& tts, Config config);
    ~LocalVoicePipeline();

    LocalVoicePipeline(const LocalVoicePipeline&) = delete;
    LocalVoicePipeline& operator=(const LocalVoicePipeline&) = delete;

    bool start();
    void stop();

    void onCaptureStarted(PcmFormat format);
    // Returns false only when a PCM chunk is dropped due to bounded-queue
    // backpressure. Start/end events are never discarded.
    bool onPcm(const int16_t* samples, std::size_t sample_count, PcmFormat format);
    void onCaptureEnded();

    bool waitForIdle(std::chrono::milliseconds timeout);
    std::size_t droppedPcmChunks() const;

private:
    enum class EventType { CaptureStarted, Pcm, CaptureEnded, Stop };

    struct Event {
        EventType type{EventType::Pcm};
        PcmFormat format;
        std::vector<int16_t> samples;
    };

    void workerLoop();
    void enqueueCritical(Event event);
    bool enqueuePcm(Event event);
    void markIdleIfNeeded();

    AsrPort& asr_;
    ConversationPort& conversation_;
    TtsPort& tts_;
    Config config_;

    mutable std::mutex mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable idle_cv_;
    std::deque<Event> events_;
    std::thread worker_;
    bool running_{false};
    bool processing_{false};
    std::size_t dropped_pcm_chunks_{0};
};

}  // namespace cockpit
