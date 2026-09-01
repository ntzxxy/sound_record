#include "microphone_asr.h"

#include <alsa/asoundlib.h>
#ifdef VISION_HAS_PULSE_AUDIO
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "asr.h"

namespace vision_tools {

class MicrophoneAsr::Impl {
  public:
    ~Impl() { stop(); }

    bool start(const MicrophoneAsrConfig& requested, std::string* error_message) {
        if (running.load()) {
            return true;
        }
        config = requested;
        if (asr_init(config.model_directory.c_str()) != 0) {
            set_error("无法加载 ASR 模型：" + config.model_directory);
            if (error_message != nullptr) *error_message = status().message;
            return false;
        }

#ifdef VISION_HAS_PULSE_AUDIO
        // WSLg publishes the Windows microphone through PulseAudio. Prefer it
        // when PULSE_SERVER is present; Jetson normally has no such variable
        // and will continue to use its configured ALSA capture device.
        std::string pulse_error;
        if (std::getenv("PULSE_SERVER") != nullptr && open_pulse(&pulse_error)) {
            // Pulse is ready.
        } else {
            std::string alsa_error;
            if (!open_alsa(&alsa_error) && !open_pulse(&pulse_error)) {
                const std::string message = alsa_error + "；Pulse 回退也失败：" + pulse_error;
                asr_destroy();
                set_error(message);
                if (error_message != nullptr) *error_message = message;
                return false;
            }
        }
#else
        std::string alsa_error;
        if (!open_alsa(&alsa_error)) {
            asr_destroy();
            set_error(alsa_error);
            if (error_message != nullptr) *error_message = alsa_error;
            return false;
        }
#endif

        {
            std::lock_guard<std::mutex> lock(status_mutex);
            current = {};
            current.state = "ready";
            current.message = backend == Backend::kAlsa
                                  ? "麦克风已通过 ALSA 连接；按 R 开始说话，再按 R 提交"
                                  : "麦克风已通过 Pulse 连接；按 R 开始说话，再按 R 提交";
        }
        running = true;
        worker = std::thread(&Impl::capture_loop, this);
        return true;
    }

    void stop() {
        recording = false;
        const bool was_running = running.exchange(false);
        if (was_running && worker.joinable()) {
            worker.join();
        }
        if (pcm != nullptr) {
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
#ifdef VISION_HAS_PULSE_AUDIO
        if (pulse != nullptr) {
            pa_simple_free(pulse);
            pulse = nullptr;
        }
#endif
        backend = Backend::kNone;
        asr_destroy();
    }

    bool begin_recording(std::string* error_message) {
        if (!running.load()) {
            if (error_message != nullptr) *error_message = "语音输入尚未初始化";
            return false;
        }
        if (recording.exchange(true)) {
            if (error_message != nullptr) *error_message = "正在录音";
            return false;
        }
        {
            std::lock_guard<std::mutex> asr_lock(asr_mutex);
            asr_reset();
        }
        std::lock_guard<std::mutex> lock(status_mutex);
        current.recording = true;
        current.state = "listening";
        current.partial_text.clear();
        current.message = "正在听，请说出与当前画面相关的问题；再次按 R 提交";
        return true;
    }

    std::optional<std::string> end_recording() {
        if (!recording.exchange(false)) {
            return std::nullopt;
        }

        std::string final_text;
        {
            std::lock_guard<std::mutex> asr_lock(asr_mutex);
            final_text = asr_get_result();
            asr_reset();
        }
        std::lock_guard<std::mutex> lock(status_mutex);
        current.recording = false;
        current.partial_text = final_text;
        if (final_text.empty()) {
            current.state = "ready";
            current.message = "没有识别到有效语音，请按 R 重试";
            return std::nullopt;
        }
        current.state = "submitted";
        current.message = "语音已识别，等待视觉模型回答";
        return final_text;
    }

    MicrophoneAsrStatus status() const {
        std::lock_guard<std::mutex> lock(status_mutex);
        return current;
    }

  private:
    enum class Backend { kNone, kAlsa, kPulse };

    bool open_alsa(std::string* error_message) {
        const int open_result = snd_pcm_open(&pcm, config.device.c_str(), SND_PCM_STREAM_CAPTURE,
                                             SND_PCM_NONBLOCK);
        if (open_result < 0) {
            *error_message = "无法打开 ALSA 麦克风 " + config.device + "：" + snd_strerror(open_result);
            return false;
        }
        const int params_result = snd_pcm_set_params(
            pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 500000);
        if (params_result < 0) {
            *error_message = "无法配置 16 kHz 单声道 ALSA 麦克风 " + config.device + "：" +
                             snd_strerror(params_result);
            snd_pcm_close(pcm);
            pcm = nullptr;
            return false;
        }
        backend = Backend::kAlsa;
        return true;
    }

#ifdef VISION_HAS_PULSE_AUDIO
    bool open_pulse(std::string* error_message) {
        pa_sample_spec sample_spec{};
        sample_spec.format = PA_SAMPLE_S16LE;
        sample_spec.rate = 16000;
        sample_spec.channels = 1;
        int pulse_error = PA_OK;
        pulse = pa_simple_new(nullptr, "vision-workbench", PA_STREAM_RECORD, nullptr,
                              "visual-question", &sample_spec, nullptr, nullptr, &pulse_error);
        if (pulse == nullptr) {
            *error_message = pa_strerror(pulse_error);
            return false;
        }
        backend = Backend::kPulse;
        return true;
    }
#endif

    void set_error(std::string message) {
        std::lock_guard<std::mutex> lock(status_mutex);
        current.recording = false;
        current.state = "failed";
        current.message = std::move(message);
    }

    void capture_loop() {
        std::array<int16_t, 640> samples{};  // 40 ms at 16 kHz
        while (running.load()) {
            if (!recording.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int frames = 0;
            if (backend == Backend::kAlsa) {
                const snd_pcm_sframes_t read_frames = snd_pcm_readi(pcm, samples.data(), samples.size());
                if (read_frames == -EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (read_frames < 0) {
                    const int recover_result = snd_pcm_recover(pcm, static_cast<int>(read_frames), 0);
                    if (recover_result < 0) {
                        set_error(std::string("ALSA 麦克风读取失败：") + snd_strerror(recover_result));
                        recording = false;
                    }
                    continue;
                }
                frames = static_cast<int>(read_frames);
            }
#ifdef VISION_HAS_PULSE_AUDIO
            else if (backend == Backend::kPulse) {
                int pulse_error = PA_OK;
                if (pa_simple_read(pulse, samples.data(), samples.size() * sizeof(int16_t), &pulse_error) < 0) {
                    set_error(std::string("Pulse 麦克风读取失败：") + pa_strerror(pulse_error));
                    recording = false;
                    continue;
                }
                frames = static_cast<int>(samples.size());
            }
#endif
            if (frames == 0) continue;
            if (!recording.load()) continue;

            std::string partial;
            {
                std::lock_guard<std::mutex> asr_lock(asr_mutex);
                if (asr_process_frame(samples.data(), static_cast<int>(frames)) < 0) {
                    set_error("ASR 处理音频失败");
                    recording = false;
                    continue;
                }
                partial = asr_get_result();
            }
            std::lock_guard<std::mutex> lock(status_mutex);
            current.partial_text = std::move(partial);
        }
    }

  public:
    MicrophoneAsrConfig config;
    snd_pcm_t* pcm = nullptr;
#ifdef VISION_HAS_PULSE_AUDIO
    pa_simple* pulse = nullptr;
#endif
    Backend backend = Backend::kNone;
    std::atomic<bool> running{false};
    std::atomic<bool> recording{false};
    std::thread worker;
    mutable std::mutex status_mutex;
    std::mutex asr_mutex;
    MicrophoneAsrStatus current;
};

MicrophoneAsr::MicrophoneAsr() : impl_(new Impl()) {}
MicrophoneAsr::~MicrophoneAsr() { delete impl_; }

bool MicrophoneAsr::start(const MicrophoneAsrConfig& config, std::string* error_message) {
    return impl_->start(config, error_message);
}

void MicrophoneAsr::stop() { impl_->stop(); }

bool MicrophoneAsr::begin_recording(std::string* error_message) {
    return impl_->begin_recording(error_message);
}

std::optional<std::string> MicrophoneAsr::end_recording() { return impl_->end_recording(); }

MicrophoneAsrStatus MicrophoneAsr::status() const { return impl_->status(); }

}  // namespace vision_tools
