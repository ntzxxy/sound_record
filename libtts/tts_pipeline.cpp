#include "tts_pipeline.h"
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

// ===================================================================
// 全局单例
// ===================================================================
static DoubleMessageQueue           g_queue;
static std::unique_ptr<TTSModel>    g_model;
static std::unique_ptr<AudioPlayer> g_player;
static std::atomic<bool>            g_busy{false};

static std::thread                  g_synth_thread; // 后台唯一的合成工人
static std::thread                  g_play_thread;  // 后台唯一的播放工人

// 用一个原子变量来跟踪大模型是不是已经吐到了这句话的尾巴
static std::atomic<bool>            g_is_last_chunk{false};

// ===================================================================
// 2. 线程一：后台合成工人循环（对应你的 pop_text）
// ===================================================================
static void tts_synthesis_loop() {
    // 压榨边缘端算力：赋予合成线程极高的 CPU 优先级，防止大模型推理时抢不到算力卡顿
    setpriority(PRIO_PROCESS, 0, -20);

    while (true) {
        // 🌟 伸出双手，阻塞死等文字。由于全厂只有它一个工人，notify_one 精准且唯一的叫醒它！
        std::string text = g_queue.pop_text();

        // 关键下班通知：如果收到 stop() 触发的空字符串，代表公司倒闭了，直接打破死循环，线程销毁
        if (text.empty()) {
            break;
        }

        // 全文一次性推送模式：每段文本就是完整回复，标记为最后一句话
        bool is_last = true;

        int32_t audio_len = 0;
        if (!text.empty()) {
            // 调用底层的底层：真正把文字喂给模型进行 ONNX/GGUF 推理
            int16_t* wav_raw = g_model->infer(text, audio_len);

            if (wav_raw && audio_len > 0) {
                // 将裸 PCM 数据无缝装入智能指针，移交所有权
                auto audio_buf = std::make_unique<int16_t[]>(audio_len);
                std::memcpy(audio_buf.get(), wav_raw, audio_len * sizeof(int16_t));

                // 🌟 推入音频传送带，并标记这块音频是不是大模型的最后一句话
                g_queue.push_audio(std::move(audio_buf), audio_len, is_last);

                // 释放底层推理引擎的临时内存
                g_model->free_data(wav_raw);
            }
        } else {
            // 如果是一句空文本，也发一个空包过去，确保 is_last 标记能够透传给播放端
            auto empty_buf = std::make_unique<int16_t[]>(0);
            g_queue.push_audio(std::move(empty_buf), 0, is_last);
        }
    }
}

// ===================================================================
// 3. 线程二：后台播放工人循环（对应你的 pop_audio）
// ===================================================================
static void tts_playback_loop() {
    // 同样赋予声卡写入线程最高优先级，保证音频输出绝对平滑、没有杂音和断续
    setpriority(PRIO_PROCESS, 0, -20);

    while (true) {
        // 🌟 阻塞死等合成完的裸 PCM 音频分片
        AudioMessage msg = g_queue.pop_audio();

        // 关键下班通知：收到全局 stop() 广播发来的空指针，直接下班销毁
        if (msg.data == nullptr) {
            break;
        }

        // 如果长度大于0，真正调用底层 ALSA 的 snd_pcm_writei 驱动声卡发出声音
        if (msg.length > 0) {
            g_player->play(msg.data.get(), msg.length, 1.0f);
        }

        // 🌟 【彻底攻克自激死循环的战术核心】
        // 当这个唯一的播放工人真正把这一轮对话的最后一个切片（is_last）播完、喇叭闭嘴的那一瞬间！
        if (msg.is_last) {
            g_busy = false;  // 播完，解除 ASR 门控
            std::cout << "[TTS-Pipeline] 播放完毕，声卡空闲。" << std::endl;
        }
    }
}

// ===================================================================
// 4. 对外开放的生命周期控制接口
// ===================================================================
int tts_pipeline_init(const char *tts_model_path, const char * /*save_dir*/) {
    if (!tts_model_path) return -1;

    try {
        g_model = std::make_unique<TTSModel>(tts_model_path);
        g_player = std::make_unique<AudioPlayer>();

        // 🌟【招募打工人】真正创建并盘活这两个后台线程！
        // 这两行执行完，两个 loop 函数就会在完全独立的 CPU 核心后台各自埋头死循环，井水不犯河水
        g_synth_thread = std::thread(tts_synthesis_loop);
        g_play_thread  = std::thread(tts_playback_loop);

        std::cout << "[OK] TTS管线双线程初始化成功。" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[Error] TTS管线初始化失败: " << e.what() << std::endl;
        return -1;
    }
}

void tts_pipeline_push(const char *text) {
    if (!text || text[0] == '\0') return;
    g_busy = true;
    g_queue.push_text(std::string(text));
}

int tts_pipeline_is_busy(void) { return g_busy ? 1 : 0; }

void tts_pipeline_interrupt(void) {
    std::cout << "[System] 收到强制打断信号，正在清空整个管线..." << std::endl;

    // 先让底层的播放驱动强行把 DMA 缓冲区里的声音掐断
    // g_player->stop_device();

    // 清空历史遗留的、还没来得及合成的文本，以及还没来得及播放的音频
    // 注意：你可以在 MessageQueue 里面加一个 clear() 函数来配合，或者直接让现有的队列排空
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