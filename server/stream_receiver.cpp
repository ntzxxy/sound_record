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
#include <cctype>

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

static void write_wav_header(std::fstream& f, uint32_t pcm_bytes, uint32_t sample_rate, uint16_t channels) {
    WavHeader_t header;
    std::memcpy(header.ChunkID, "RIFF", 4);
    header.ChunkSize = pcm_bytes + 36;
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
    header.Subchunk2Size = pcm_bytes;

    f.seekp(0, std::ios::beg);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));
    f.seekp(0, std::ios::end);
    f.flush();
}

struct SessionRecorder {
    std::fstream file;
    std::string filename;
    uint32_t sample_rate = 16000;
    uint16_t channels = 1;
    uint32_t pcm_bytes = 0;
    bool has_segment = false;
    bool active = false;

    bool open(const std::string& save_dir) {
        char time_buf[64];
        std::time_t t = std::time(nullptr);
        std::strftime(time_buf, sizeof(time_buf), "session_%Y%m%d_%H%M%S.wav", std::localtime(&t));
        filename = save_dir + "/" + time_buf;
        file.open(filename, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file.is_open()) return false;
        write_wav_header(file, 0, sample_rate, channels);
        std::cout << "[Recorder] session started: " << filename << std::endl;
        return true;
    }

    void start_segment(uint32_t sr, uint16_t ch) {
        if (!file.is_open()) return;
        sample_rate = sr ? sr : sample_rate;
        channels = ch ? ch : channels;
        if (has_segment) {
            uint32_t silence_samples = sample_rate / 5; // 200ms
            std::vector<int16_t> silence(silence_samples * channels, 0);
            file.write(reinterpret_cast<const char*>(silence.data()), silence.size() * sizeof(int16_t));
            pcm_bytes += static_cast<uint32_t>(silence.size() * sizeof(int16_t));
        }
        has_segment = true;
        active = true;
    }

    void write_pcm(const uint8_t* data, size_t size) {
        if (!file.is_open() || !active || !data || size == 0) return;
        file.write(reinterpret_cast<const char*>(data), size);
        pcm_bytes += static_cast<uint32_t>(size);
    }

    void end_segment() {
        active = false;
        flush_header();
    }

    void flush_header() {
        if (!file.is_open()) return;
        write_wav_header(file, pcm_bytes, sample_rate, channels);
    }

    void close() {
        if (!file.is_open()) return;
        flush_header();
        file.close();
        std::cout << "[Recorder] session saved: " << filename
                  << " bytes=" << pcm_bytes << std::endl;
    }
};

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
static std::atomic<bool>       g_tts_enabled{false};
static std::mutex              g_client_send_mutex;
static std::atomic<int>        g_client_fd{-1};
static std::atomic<uint32_t>   g_tts_seq{0};
static std::atomic<int>        g_tts_sample_rate{44100};
static std::atomic<uint64_t>   g_tts_pcm_frames{0};
static std::atomic<uint64_t>   g_tts_pcm_bytes{0};

static int send_ai_frame_server(int fd, uint16_t type, uint32_t seq,
                                uint32_t sample_rate, uint16_t channels,
                                uint16_t format,
                                const uint8_t *payload, uint32_t size) {
    AiFrameHeader_t header;
    header.magic = htonl(AI_FRAME_MAGIC);
    header.version = htons(AI_FRAME_VERSION);
    header.type = htons(type);
    header.seq = htonl(seq);
    header.timestamp = htonl(seq * 100);
    header.sample_rate = htonl(sample_rate);
    header.channels = htons(channels);
    header.format = htons(format);
    header.payload_size = htonl(size);

    auto send_all = [](int sock, const uint8_t *buf, size_t len) -> int {
        while (len > 0) {
            ssize_t n = send(sock, buf, len, MSG_NOSIGNAL);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            buf += n;
            len -= n;
        }
        return 0;
    };

    std::lock_guard<std::mutex> lock(g_client_send_mutex);
    if (send_all(fd, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) < 0) return -1;
    if (size > 0 && payload) return send_all(fd, payload, size);
    return 0;
}

static void send_tts_start(int sample_rate, int channels) {
    int fd = g_client_fd.load();
    if (fd < 0) return;
    g_tts_seq = 0;
    g_tts_sample_rate = sample_rate;
    g_tts_pcm_frames = 0;
    g_tts_pcm_bytes = 0;
    send_ai_frame_server(fd, AI_FRAME_TTS_START, g_tts_seq++, sample_rate,
                         static_cast<uint16_t>(channels), AI_AUDIO_FORMAT_S16_LE,
                         nullptr, 0);
    std::cout << "[TTS-Downlink] START rate=" << sample_rate
              << " channels=" << channels << std::endl;
}

static void send_tts_pcm(const int16_t *samples, int n, int) {
    int fd = g_client_fd.load();
    if (fd < 0 || !samples || n <= 0) return;
    g_tts_pcm_frames++;
    g_tts_pcm_bytes += static_cast<uint64_t>(n * sizeof(int16_t));
    send_ai_frame_server(fd, AI_FRAME_TTS_PCM, g_tts_seq++, g_tts_sample_rate.load(), 1,
                         AI_AUDIO_FORMAT_S16_LE,
                         reinterpret_cast<const uint8_t *>(samples),
                         static_cast<uint32_t>(n * sizeof(int16_t)));
}

static void send_tts_end(void) {
    int fd = g_client_fd.load();
    if (fd < 0) return;
    send_ai_frame_server(fd, AI_FRAME_TTS_END, g_tts_seq++, g_tts_sample_rate.load(), 1,
                         AI_AUDIO_FORMAT_S16_LE, nullptr, 0);
    std::cout << "[TTS-Downlink] END frames=" << g_tts_pcm_frames.load()
              << " bytes=" << g_tts_pcm_bytes.load() << std::endl;
}

static void send_tts_cancel(void) {
    int fd = g_client_fd.load();
    if (fd < 0) return;
    send_ai_frame_server(fd, AI_FRAME_TTS_CANCEL, g_tts_seq++, g_tts_sample_rate.load(), 1,
                         AI_AUDIO_FORMAT_S16_LE, nullptr, 0);
    std::cout << "[TTS-Downlink] CANCEL" << std::endl;
}

static size_t utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_count_chars(const std::string& s, size_t end) {
    int count = 0;
    for (size_t i = 0; i < end && i < s.size();) {
        i += utf8_char_len(static_cast<unsigned char>(s[i]));
        count++;
    }
    return count;
}

static std::string trim_ascii(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) begin++;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(begin, end - begin);
}

static bool is_strong_punct(const std::string& ch) {
    return ch == "。" || ch == "！" || ch == "？" || ch == "." || ch == "!" || ch == "?";
}

static bool is_weak_punct(const std::string& ch) {
    return ch == "，" || ch == "、" || ch == "；" || ch == ";" || ch == "：" || ch == ":" || ch == ",";
}

static size_t byte_pos_for_chars(const std::string& s, int max_chars) {
    int count = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = utf8_char_len(static_cast<unsigned char>(s[i]));
        i += len;
        count++;
        if (count >= max_chars) return i;
    }
    return s.size();
}

static size_t find_tts_segment_boundary(const std::string& pending, bool final) {
    constexpr int kMinChars = 8;
    constexpr int kWeakChars = 18;
    constexpr int kMaxChars = 28;

    size_t last_weak = 0;
    int chars = 0;
    for (size_t i = 0; i < pending.size();) {
        size_t len = utf8_char_len(static_cast<unsigned char>(pending[i]));
        std::string ch = pending.substr(i, len);
        size_t end = i + len;
        chars++;
        if (is_strong_punct(ch) && chars >= kMinChars) return end;
        if (is_weak_punct(ch) && chars >= kMinChars) {
            last_weak = end;
            if (chars >= kWeakChars) return end;
        }
        i = end;
    }

    if (chars >= kMaxChars) {
        if (last_weak > 0) return last_weak;
        return byte_pos_for_chars(pending, kMaxChars);
    }

    if (final && chars > 0) return pending.size();
    return 0;
}

static void flush_tts_segments(std::string& pending, bool final) {
    if (!g_tts_enabled) return;

    if (final && trim_ascii(pending).empty()) {
        tts_pipeline_push("", 1);
        pending.clear();
        return;
    }

    while (!pending.empty()) {
        size_t boundary = find_tts_segment_boundary(pending, final);
        if (boundary == 0) break;

        std::string segment = trim_ascii(pending.substr(0, boundary));
        pending.erase(0, boundary);
        if (!segment.empty()) {
            bool is_final = final && trim_ascii(pending).empty();
            std::cout << "[TTS-Segment] " << segment << " final=" << (is_final ? 1 : 0) << std::endl;
            tts_pipeline_push(segment.c_str(), is_final ? 1 : 0);
        }
    }
}

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

        // LLM 回调：打印 + 智能分块推送到 TTS 管线
        std::cout << "[Agent] " << std::flush;
        static std::string g_full;
        static std::string g_tts_pending;
        g_full.clear();
        g_tts_pending.clear();
        agent_chat(task.text.c_str(), [](const char *text, int is_final) {
            if (text && text[0]) {
                std::cout << text << std::flush;
                g_full += text;
                g_tts_pending += text;
                flush_tts_segments(g_tts_pending, false);
            }
            if (is_final) {
                std::cout << std::endl;
                flush_tts_segments(g_tts_pending, true);
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
            tts_pipeline_set_output(send_tts_start, send_tts_pcm,
                                    send_tts_end, send_tts_cancel);
            g_tts_enabled = true;
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
        g_client_fd = client_fd;
        std::cout << "[+] 开发板按键按下，长连接流通道已建立。" << std::endl;

        SessionRecorder recorder;
        recorder.open(save_dir);
        uint32_t expected_seq = 0;
        bool first_frame = true;
        std::string last_text;
        bool mic_active = false;
        bool utterance_submitted = false;
        uint32_t asr_cooldown_bytes = 0;

        while (true) {
            AiFrameHeader_t header;
            ssize_t ret = read_all(client_fd, reinterpret_cast<uint8_t*>(&header), sizeof(AiFrameHeader_t));
            if (ret <= 0) break;

            uint32_t magic = ntohl(header.magic);
            uint16_t version = ntohs(header.version);
            uint16_t type = ntohs(header.type);
            uint32_t seq = ntohl(header.seq);
            uint32_t sample_rate = ntohl(header.sample_rate);
            uint16_t channels = ntohs(header.channels);
            uint32_t payload_size = ntohl(header.payload_size);

            if (magic != AI_FRAME_MAGIC || version != AI_FRAME_VERSION) {
                std::cerr << "[Warning] 非法帧头，断开连接" << std::endl;
                break;
            }

            if (first_frame) { expected_seq = seq; first_frame = false; }
            else if (seq != expected_seq)
                std::cerr << "[Warning] 帧序号跳变! 期望:" << expected_seq << " 实际:" << seq << std::endl;
            expected_seq = seq + 1;

            std::vector<uint8_t> payload_buf;
            if (payload_size > 0) {
                payload_buf.resize(payload_size);
                ret = read_all(client_fd, payload_buf.data(), payload_size);
                if (ret < static_cast<ssize_t>(payload_size)) break;
            }

            if (type == AI_FRAME_MIC_START) {
                if (!tts_model_path.empty() && tts_pipeline_is_busy()) {
                    std::cout << "[TTS] 新一轮录音开始，打断当前播报" << std::endl;
                    tts_pipeline_interrupt();
                } else if (!tts_model_path.empty()) {
                    send_tts_cancel();
                }
                mic_active = true;
                utterance_submitted = false;
                last_text.clear();
                asr_reset();
                recorder.start_segment(sample_rate ? sample_rate : 16000,
                                       channels ? channels : 1);
                uint32_t sr = sample_rate ? sample_rate : 16000;
                uint16_t ch = channels ? channels : 1;
                asr_cooldown_bytes = (sr / 5) * ch * sizeof(int16_t); // 200ms，防 TTS 尾音/混响自激
                std::cout << "[MIC] START rate=" << sample_rate
                          << " channels=" << channels << std::endl;
                continue;
            }

            if (type == AI_FRAME_MIC_END) {
                mic_active = false;
                std::cout << "[MIC] END" << std::endl;
                if (!utterance_submitted && !last_text.empty()) {
                    std::cout << "[ASR] 断句完成: " << last_text << std::endl;
                    if (!tts_model_path.empty() && tts_pipeline_is_busy()) {
                        std::cout << "[TTS] 播放中，忽略此句" << std::endl;
                    } else if (!llm_model_path.empty()) {
                        { std::lock_guard<std::mutex> lk(g_queue_mutex);
                          g_task_queue.push({last_text, false}); }
                        g_queue_cv.notify_one();
                        utterance_submitted = true;
                    }
                }
                recorder.end_segment();
                asr_reset();
                last_text.clear();
                continue;
            }

            if (type != AI_FRAME_MIC_PCM) {
                continue;
            }

            if (payload_size > 0) {
                if (mic_active) recorder.write_pcm(payload_buf.data(), payload_buf.size());

                if (asr_cooldown_bytes > 0) {
                    if (payload_size <= asr_cooldown_bytes) {
                        asr_cooldown_bytes -= payload_size;
                        continue;
                    }
                    size_t skip = asr_cooldown_bytes;
                    payload_buf.erase(payload_buf.begin(), payload_buf.begin() + skip);
                    payload_size = static_cast<uint32_t>(payload_buf.size());
                    asr_cooldown_bytes = 0;
                }

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
                        if (mic_active) {
                            // 按键式交互下，业务提交以 MIC_END 为准；录音期间只保留最新识别文本。
                            asr_reset();
                            continue;
                        }
                        if (!tts_model_path.empty() && (tts_pipeline_is_busy() || in_cooldown)) {
                            if (in_cooldown) std::cout << "[TTS] 冷却期，忽略此句" << std::endl;
                            else            std::cout << "[TTS] 播放中，忽略此句" << std::endl;
                        } else if (!llm_model_path.empty()) {
                            { std::lock_guard<std::mutex> lk(g_queue_mutex);
                              g_task_queue.push({last_text, false}); }
                            g_queue_cv.notify_one();
                            utterance_submitted = true;
                        }
                    }
                    asr_reset();
                    last_text.clear();
                }
            }
        }

        close(client_fd);
        if (g_client_fd.load() == client_fd) g_client_fd = -1;
        recorder.close();
        std::cout << "[-] 开发板连接断开。" << std::endl;
        asr_reset();

        if (!llm_model_path.empty()) {
            { std::lock_guard<std::mutex> lk(g_queue_mutex);
              while (!g_task_queue.empty()) g_task_queue.pop();
              g_task_queue.push({"", true}); }
            g_queue_cv.notify_one();
        }
    }

    asr_destroy();
    if (!llm_model_path.empty()) {
        g_llm_running = false; g_queue_cv.notify_one();
        if (g_llm_thread.joinable()) g_llm_thread.join();
        agent_destroy();
    }
    if (!tts_model_path.empty()) {
        g_tts_enabled = false;
        tts_pipeline_destroy();
    }
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
