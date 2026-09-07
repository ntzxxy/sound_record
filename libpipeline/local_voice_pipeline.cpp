#include "local_voice_pipeline.h"

#include <algorithm>
#include <utility>

namespace cockpit {

LocalVoicePipeline::LocalVoicePipeline(AsrPort& asr, ConversationPort& conversation,
                                       TtsPort& tts)
    : LocalVoicePipeline(asr, conversation, tts, Config{}) {}

LocalVoicePipeline::LocalVoicePipeline(AsrPort& asr, ConversationPort& conversation,
                                       TtsPort& tts, Config config)
    : asr_(asr), conversation_(conversation), tts_(tts), config_(config) {
    config_.max_pending_events = std::max<std::size_t>(config_.max_pending_events, 3);
}

LocalVoicePipeline::~LocalVoicePipeline() {
    stop();
}

bool LocalVoicePipeline::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return true;
    running_ = true;
    worker_ = std::thread(&LocalVoicePipeline::workerLoop, this);
    return true;
}

void LocalVoicePipeline::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        events_.push_back({EventType::Stop, {}, {}});
    }
    queue_cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void LocalVoicePipeline::enqueueCritical(Event event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        // Keep control transitions even under a slow ASR model. Drop the
        // oldest PCM rather than losing a turn boundary.
        while (events_.size() >= config_.max_pending_events) {
            const auto pcm = std::find_if(events_.begin(), events_.end(), [](const Event& queued) {
                return queued.type == EventType::Pcm;
            });
            if (pcm == events_.end()) break;
            events_.erase(pcm);
            ++dropped_pcm_chunks_;
        }
        events_.push_back(std::move(event));
    }
    queue_cv_.notify_one();
}

bool LocalVoicePipeline::enqueuePcm(Event event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || events_.size() >= config_.max_pending_events) {
            ++dropped_pcm_chunks_;
            return false;
        }
        events_.push_back(std::move(event));
    }
    queue_cv_.notify_one();
    return true;
}

void LocalVoicePipeline::onCaptureStarted(PcmFormat format) {
    enqueueCritical({EventType::CaptureStarted, format, {}});
}

bool LocalVoicePipeline::onPcm(const int16_t* samples, std::size_t sample_count,
                               PcmFormat format) {
    if (samples == nullptr || sample_count == 0) return true;
    Event event;
    event.type = EventType::Pcm;
    event.format = format;
    event.samples.assign(samples, samples + sample_count);
    return enqueuePcm(std::move(event));
}

void LocalVoicePipeline::onCaptureEnded() {
    enqueueCritical({EventType::CaptureEnded, {}, {}});
}

bool LocalVoicePipeline::waitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] {
        return events_.empty() && !processing_;
    });
}

std::size_t LocalVoicePipeline::droppedPcmChunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_pcm_chunks_;
}

void LocalVoicePipeline::markIdleIfNeeded() {
    if (events_.empty() && !processing_) idle_cv_.notify_all();
}

void LocalVoicePipeline::workerLoop() {
    bool capture_active = false;
    while (true) {
        Event event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_cv_.wait(lock, [this] { return !events_.empty(); });
            event = std::move(events_.front());
            events_.pop_front();
            processing_ = true;
        }

        if (event.type == EventType::Stop) {
            std::lock_guard<std::mutex> lock(mutex_);
            processing_ = false;
            events_.clear();
            markIdleIfNeeded();
            return;
        }

        switch (event.type) {
            case EventType::CaptureStarted:
                // A new user utterance always wins over residual speech.
                tts_.interrupt();
                asr_.reset();
                capture_active = true;
                break;
            case EventType::Pcm:
                if (capture_active) {
                    asr_.acceptPcm(event.samples.data(), event.samples.size(), event.format);
                }
                break;
            case EventType::CaptureEnded:
                if (capture_active) {
                    capture_active = false;
                    std::string text = asr_.finalText();
                    if (!text.empty()) conversation_.submitVoiceText(std::move(text));
                }
                break;
            case EventType::Stop:
                break;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            processing_ = false;
            markIdleIfNeeded();
        }
    }
}

}  // namespace cockpit
