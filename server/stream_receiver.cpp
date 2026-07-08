//
// Created by Lenovo on 2026/6/12.
//

#include "stream_receiver.h"
#include "net.h"
#include "asr.h"
#include "chat_agent.h"
#include "tts_pipeline.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <fstream>
#include <ctime>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <climits>

static void save_wav_file(const std::string& filename, const std::vector<uint8_t>& pcm_data, uint32_t sample_rate, uint16_t channels) {
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs.is_open()) return;

    WavHeader_t header;
    std::memcpy(header.ChunkID, "RIFF", 4);
    header.ChunkSize = pcm_data.size() + 36;
    std::memcpy(header.Format, "WAVE", 4);
    std::memcpy(header.Subchunk1ID, "fmt ", 4);
    header.Subchunk1Size = 16;
    header.AudioFormat = 1;
    header.NumChannels = channels;
    header.SampleRate = sample_rate;
    header.ByteRate = sample_rate * channels * 16 / 8;
    header.BlockAlign = channels * 16 / 8;
    header.BitsPerSample = 16;
    std::memcpy(header.Subchunk2ID, "data", 4);
    header.Subchunk2Size = pcm_data.size();

    ofs.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader_t));
    ofs.write(reinterpret_cast<const char*>(pcm_data.data()), pcm_data.size());
    ofs.close();
    std::cout << "[OK] 音频解析落盘成功: " << filename << std::endl;
}

static ssize_t read_all(int fd, uint8_t* buf, size_t size) {
    size_t bytes_read = 0;
    while (bytes_read < size) {
        ssize_t ret = recv(fd, buf + bytes_read, size - bytes_read, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return bytes_read;
        bytes_read += ret;
    }
    return bytes_read;
}

// ===================================================================
// LLM 异步推理线程
// ===================================================================
struct ChatTask { std::string text; bool is_reset; };

static std::queue<ChatTask>    g_task_queue;
static std::mutex              g_queue_mutex;
static std::condition_variable g_queue_cv;
static std::atomic<bool>       g_llm_running{true};
static std::thread             g_llm_thread;

static void llm_worker() {
    while (g_llm_running) {
        ChatTask task;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] { return !g_task_queue.empty() || !g_llm_running; });
            if (!g_llm_running && g_task_queue.empty()) break;
            task = std::move(g_task_queue.front());
            g_task_queue.pop();
        }
        if (task.is_reset) { agent_reset(); continue; }

        // LLM 回调：打印 + 推送到 TTS 管线
        // LLM→TTS：全文生成后一次性推送
        std::cout << "[Agent] " << std::flush;
        static std::string g_full;
        g_full.clear();
        agent_chat(task.text.c_str(), [](const char *text, int is_final) {
            if (text && text[0]) { std::cout << text << std::flush; g_full += text; }
            if (is_final) {
                std::cout << std::endl;
                if (!g_full.empty()) tts_pipeline_push(g_full.c_str());
            }
        });
    }
}

// ===================================================================
// 核心业务
// ===================================================================
int start_stream_server(int port, const std::string& save_dir,
                        const std::string& model_dir,
                        const std::string& llm_model_path,
                        const std::string& tts_model_path) {
    if (mkdir(save_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        perror("mkdir 失败"); return -1;
    }
    char abs_path[PATH_MAX];
    const char* display_path = realpath(save_dir.c_str(), abs_path) ? abs_path : save_dir.c_str();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0 || listen(server_fd, 5) < 0) {
        perror("网络初始化失败"); return -1;
    }
    std::cout << "[*] 服务已启动，监听端口: " << port << std::endl;
    std::cout << "[*] 录音保存路径: " << display_path << std::endl;

    if (asr_init(model_dir.c_str()) != 0) {
        std::cerr << "[ASR] 模型加载失败: " << model_dir << std::endl;
        return -1;
    }
    std::cout << "[ASR] 模型加载成功" << std::endl;

    if (!llm_model_path.empty()) {
        if (agent_init(llm_model_path.c_str(), nullptr) != 0) {
            std::cerr << "[Agent] 模型加载失败: " << llm_model_path << std::endl;
            asr_destroy(); return -1;
        }
        g_llm_thread = std::thread(llm_worker);
        std::cout << "[Agent] 模型加载成功, LLM线程已启动" << std::endl;
    }

    if (!tts_model_path.empty()) {
        std::string tts_save = std::string(display_path) + "/../voice_answers";
        mkdir(tts_save.c_str(), 0755);
        if (tts_pipeline_init(tts_model_path.c_str(), tts_save.c_str()) == 0) {
            std::cout << "[TTS] 管线已启动, WAV保存: " << tts_save << std::endl;
        }
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::cout << "[+] 开发板按键按下，长连接流通道已建立。" << std::endl;

        std::vector<uint8_t> pcm_pool;
        uint32_t expected_seq = 0;
        bool first_frame = true;
        std::string last_text;

        while (true) {
            StreamHeader_t header;
            ssize_t ret = read_all(client_fd, reinterpret_cast<uint8_t*>(&header), sizeof(StreamHeader_t));
            if (ret <= 0) break;

            uint32_t seq = ntohl(header.seq);
            uint32_t payload_size = ntohl(header.payload_size);
            if (first_frame) { expected_seq = seq; first_frame = false; }
            else if (seq != expected_seq)
                std::cerr << "[Warning] 丢包! 期望:" << expected_seq << " 实际:" << seq << std::endl;
            expected_seq = seq + 1;

            if (payload_size > 0) {
                std::vector<uint8_t> payload_buf(payload_size);
                ret = read_all(client_fd, payload_buf.data(), payload_size);
                if (ret < static_cast<ssize_t>(payload_size)) break;
                pcm_pool.insert(pcm_pool.end(), payload_buf.begin(), payload_buf.end());

                int num_samples = static_cast<int>(payload_size) / 2;

                // === 回声防护状态机 ===
                static bool g_was_busy = false;
                // TTS 刚开始 → 清 ASR 回声
                if (!tts_model_path.empty() && tts_pipeline_is_busy() && !g_was_busy) {
                    asr_reset(); last_text.clear(); g_was_busy = true;
                }
                // TTS 刚结束 → 清 ASR 积压回声 + 标记冷却时间
                static auto g_cooldown_until = std::chrono::steady_clock::now();
                if (g_was_busy && !tts_pipeline_is_busy()) {
                    asr_reset(); last_text.clear(); g_was_busy = false;
                    g_cooldown_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
                }

                // 始终喂帧到 ASR（保持热机），冷却期内丢弃端点
                asr_process_frame(reinterpret_cast<const int16_t*>(payload_buf.data()), num_samples);

                const char* text = asr_get_result();
                bool in_cooldown = (std::chrono::steady_clock::now() < g_cooldown_until);
                if (text && text[0] && last_text != text) {
                    last_text = text;
                    if (!tts_model_path.empty() && (tts_pipeline_is_busy() || in_cooldown))
                        {} // 回声/冷却期，不打印
                    else
                        std::cout << "[ASR] " << text << std::endl;
                }

                if (asr_is_endpoint()) {
                    if (!last_text.empty()) {
                        std::cout << "[ASR] 断句完成: " << last_text << std::endl;
                        if (!tts_model_path.empty() && (tts_pipeline_is_busy() || in_cooldown)) {
                            if (in_cooldown) std::cout << "[TTS] 冷却期，忽略此句" << std::endl;
                            else            std::cout << "[TTS] 播放中，忽略此句" << std::endl;
                        } else if (!llm_model_path.empty()) {
                            { std::lock_guard<std::mutex> lk(g_queue_mutex);
                              g_task_queue.push({last_text, false}); }
                            g_queue_cv.notify_one();
                        }
                    }
                    asr_reset();
                    last_text.clear();
                }
            }
        }

        close(client_fd);
        std::cout << "[-] 开发板松开按键，流通道断开。" << std::endl;
        asr_reset();

        if (!llm_model_path.empty()) {
            { std::lock_guard<std::mutex> lk(g_queue_mutex);
              while (!g_task_queue.empty()) g_task_queue.pop();
              g_task_queue.push({"", true}); }
            g_queue_cv.notify_one();
        }
        if (!tts_model_path.empty()) tts_pipeline_interrupt();

        if (!pcm_pool.empty()) {
            char time_buf[64];
            std::time_t t = std::time(nullptr);
            std::strftime(time_buf, sizeof(time_buf), "voice_%Y%m%d_%H%M%S.wav", std::localtime(&t));
            save_wav_file(save_dir + "/" + time_buf, pcm_pool, 16000, 1);
        }
    }

    asr_destroy();
    if (!llm_model_path.empty()) {
        g_llm_running = false; g_queue_cv.notify_one();
        if (g_llm_thread.joinable()) g_llm_thread.join();
        agent_destroy();
    }
    if (!tts_model_path.empty()) tts_pipeline_destroy();
    close(server_fd);
    return 0;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string save_dir = "./voice_records";
    std::string model_dir = "../models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16";
    std::string llm_model_path;
    std::string tts_model_path;

    if (argc > 1) port = std::stoi(argv[1]);
    if (argc > 2) save_dir = argv[2];
    if (argc > 3) model_dir = argv[3];
    if (argc > 4) llm_model_path = argv[4];
    if (argc > 5) tts_model_path = argv[5];

    return start_stream_server(port, save_dir, model_dir, llm_model_path, tts_model_path);
}
