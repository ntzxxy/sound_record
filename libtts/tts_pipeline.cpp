#include "tts_pipeline.h"
#include "tts.h"
#include "tts_model.h"
#include "tts_queue.h"
#include "audio_player_class.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/resource.h>
#include <thread>
#include <chrono>


static DoubleMessageQueue           g_queue;
static std::unique_ptr<TTSModel>    g_model;
static std::unique_ptr<AudioPlayer> g_player;
static std::atomic<bool>            g_busy{false};
static tts_output_start_t           g_output_start = nullptr;
static tts_output_pcm_t             g_output_pcm = nullptr;
static tts_output_end_t             g_output_end = nullptr;
static tts_output_cancel_t          g_output_cancel = nullptr;
static std::atomic<bool>            g_output_started{false};

static std::thread                  g_synth_thread;
static std::thread                  g_play_thread;
static std::atomic<uint64_t>        g_generation{0};

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_count_chars(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size();) {
        i += utf8_char_len(static_cast<unsigned char>(s[i]));
        count++;
    }
    return count;
}

void tts_pipeline_set_output(tts_output_start_t start_cb,
                             tts_output_pcm_t pcm_cb,
                             tts_output_end_t end_cb,
                             tts_output_cancel_t cancel_cb) {
    g_output_start = start_cb;
    g_output_pcm = pcm_cb;
    g_output_end = end_cb;
    g_output_cancel = cancel_cb;
}


static void tts_synthesis_loop() {
    // 压榨边缘端算力：赋予合成线程极高的 CPU 优先级，防止大模型推理时抢不到算力卡顿
    setpriority(PRIO_PROCESS, 0, -20);
    bool turn_has_audio = false;

    while (true) {
        TextMessage msg = g_queue.pop_text();

        if (msg.is_stop) {
            break;
        }

        uint64_t generation = g_generation.load();
        if (!msg.text.empty()) {
            long long synth_begin_ms = now_ms();
            long long first_pcm_ms = 0;
            int64_t total_samples = 0;
            int text_chars = utf8_count_chars(msg.text);
            std::cout << "[TTS-Pipeline] synth begin t=" << synth_begin_ms
                      << " final=" << (msg.is_final ? 1 : 0)
                      << " text=" << msg.text << std::endl;
            g_model->infer_stream(msg.text, [generation, &turn_has_audio, &first_pcm_ms, &total_samples, synth_begin_ms, text_chars, first_pcm = true](const int16_t *samples, int n, float) mutable {
                if (generation != g_generation.load()) return 0;
                if (!samples || n <= 0) return 1;
                if (first_pcm) {
                    first_pcm_ms = now_ms();
                    std::cout << "[TTS-Pipeline] first pcm t=" << first_pcm_ms
                              << " samples=" << n << std::endl;
                    std::cout << "[METRIC] tts_first_pcm_ms=" << (first_pcm_ms - synth_begin_ms)
                              << " text_chars=" << text_chars
                              << " samples_first=" << n << std::endl;
                    first_pcm = false;
                }
                total_samples += n;
                auto audio_buf = std::make_unique<int16_t[]>(n);
                std::memcpy(audio_buf.get(), samples, n * sizeof(int16_t));
                g_queue.push_audio(std::move(audio_buf), n, false);
                turn_has_audio = true;
                return 1;
            });
            long long synth_ms = now_ms() - synth_begin_ms;
            int sr = tts_sample_rate();
            long long audio_ms = sr > 0 ? total_samples * 1000LL / sr : 0;
            double rtf = audio_ms > 0 ? static_cast<double>(synth_ms) / audio_ms : 0.0;
            std::cout << "[METRIC] tts synth_ms=" << synth_ms
                      << " audio_ms=" << audio_ms
                      << " rtf=" << rtf
                      << " text_chars=" << text_chars
                      << " final=" << (msg.is_final ? 1 : 0) << std::endl;
        }

        if (generation != g_generation.load()) {
            continue;
        }

        if (msg.is_final) {
            int sr = tts_sample_rate();
            int silence_samples = sr > 0 ? sr / 5 : 0; // 200ms，防止最后尾音被截断
            if (turn_has_audio && silence_samples > 0) {
                auto silence = std::make_unique<int16_t[]>(silence_samples);
                std::memset(silence.get(), 0, silence_samples * sizeof(int16_t));
                g_queue.push_audio(std::move(silence), silence_samples, true);
            } else {
                auto empty_buf = std::make_unique<int16_t[]>(0);
                g_queue.push_audio(std::move(empty_buf), 0, true);
            }
            turn_has_audio = false;
        }
    }
}


static void tts_playback_loop() {
    setpriority(PRIO_PROCESS, 0, -20);

    while (true) {
        AudioMessage msg = g_queue.pop_audio();

        if (msg.data == nullptr) {
            break;
        }

        if (msg.length > 0) {
            if (g_output_pcm) {
                if (!g_output_started.exchange(true) && g_output_start) {
                    g_output_start(tts_sample_rate(), 1);
                }
                g_output_pcm(msg.data.get(), msg.length, msg.is_last ? 1 : 0);
            } else {
                g_player->play_chunk(msg.data.get(), msg.length, msg.is_last);
            }
        } else if (msg.is_last) {
            if (!g_output_pcm) g_player->play_chunk(nullptr, 0, true);
        }

        if (msg.is_last) {
            if (g_output_started.exchange(false) && g_output_end) {
                g_output_end();
            }
            g_busy = false;  // 播完，解除 ASR 门控
            std::cout << "[TTS-Pipeline] 播放完毕，声卡空闲。" << std::endl;
        }
    }
}

int tts_pipeline_init(const char *tts_model_path, const char * /*save_dir*/) {
    if (!tts_model_path) return -1;

    try {
        g_model = std::make_unique<TTSModel>(tts_model_path);
        g_player = std::make_unique<AudioPlayer>(tts_sample_rate());

        long long warmup_begin_ms = now_ms();
        std::cout << "[TTS] warmup begin text=你好" << std::endl;
        bool warmup_ok = g_model->infer_stream("你好", [](const int16_t *, int, float) {
            return 1;
        });
        std::cout << "[TTS] warmup " << (warmup_ok ? "done" : "failed") << " cost_ms="
                  << (now_ms() - warmup_begin_ms) << std::endl;

        // 这两行执行完，两个 loop 函数就会在完全独立的 CPU 核心后台各自埋头死循环
        g_synth_thread = std::thread(tts_synthesis_loop);
        g_play_thread  = std::thread(tts_playback_loop);

        std::cout << "[OK] TTS管线双线程初始化成功。" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[Error] TTS管线初始化失败: " << e.what() << std::endl;
        return -1;
    }
}

void tts_pipeline_push(const char *text, int is_final) {
    if (!text)
        return;
    if (text[0] == '\0' && !is_final)
        return;
    g_busy = true;
    g_queue.push_text(std::string(text), is_final != 0);
}

int tts_pipeline_is_busy(void) {
    return g_busy ? 1 : 0;
}

void tts_pipeline_interrupt(void) {
    std::cout << "[System] 收到强制打断信号，正在清空整个管线..." << std::endl;

    g_generation.fetch_add(1);
    g_queue.clear();
    if (g_player) g_player->stop();
    if (g_output_started.exchange(false) && g_output_cancel) g_output_cancel();
    g_busy = false;
}

void tts_pipeline_destroy(void) {
    std::cout << "[System] 准备关闭TTS模块" << std::endl;

    // 1. 🌟 调用你刚才学完的 stop()。它内部会把 stop_ 置为 true，并且执行 notify_all() 大喇叭广播！
    g_queue.stop();

    // 2. 主线程死等在门口。两个在小黑屋里被 notify_all 惊醒的工人排队出锁，看到 stop_ 后自我解散
    if (g_synth_thread.joinable()) g_synth_thread.join();
    if (g_play_thread.joinable())  g_play_thread.join();

    // 3. 干净地释放所有的硬件和模型资源
    g_player.reset();
    g_model.reset();

    std::cout << "[System] TTS管线关闭。" << std::endl;
}
