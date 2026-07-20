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
struct ChatTask {
    std::string text;
    bool is_reset;
    uint64_t turn_id;
    long long submit_ms;
};

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
static int utf8_count_chars(const std::string& s, size_t end);
static std::atomic<uint64_t>   g_turn_id{0};
static std::atomic<long long>  g_metric_mic_start_ms{0};
static std::atomic<long long>  g_metric_mic_end_ms{0};
static std::atomic<long long>  g_metric_asr_final_ms{0};
static std::atomic<long long>  g_metric_llm_submit_ms{0};
static std::atomic<long long>  g_metric_tts_first_segment_ms{0};
static std::atomic<bool>       g_metric_llm_first_token_seen{false};
static std::atomic<bool>       g_metric_tts_first_segment_seen{false};
static std::atomic<bool>       g_metric_first_audio_seen{false};

static long long metric_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void metric_reset_turn() {
    g_metric_mic_start_ms = 0;
    g_metric_mic_end_ms = 0;
    g_metric_asr_final_ms = 0;
    g_metric_llm_submit_ms = 0;
    g_metric_tts_first_segment_ms = 0;
    g_metric_llm_first_token_seen = false;
    g_metric_tts_first_segment_seen = false;
    g_metric_first_audio_seen = false;
}

static void metric_mark_asr_final(const std::string& text) {
    long long now = metric_now_ms();
    g_metric_asr_final_ms = now;
    std::cout << "[METRIC] asr_final turn=" << g_turn_id.load()
              << " text_chars=" << utf8_count_chars(text, text.size())
              << " mic_end_to_asr_ms=" << (g_metric_mic_end_ms.load() > 0 ? now - g_metric_mic_end_ms.load() : -1)
              << std::endl;
}

static int send_ai_frame_server(int fd, uint16_t type, uint32_t seq,
                                uint32_t sample_rate, uint16_t channels,
                                uint16_t format,
                                const uint8_t *payload, uint32_t size) {
    AiFrameHeader_t header;
    header.magic = htonl(AI_FRAME_MAGIC);
    header.version = htons(AI_FRAME_VERSION);
    header.type = htons(type);
    header.seq = htonl(seq);
    header.timestamp = htonl(seq * 100);            //时间戳需要是真实时间戳？
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
    long long now = metric_now_ms();
    if (!g_metric_first_audio_seen.exchange(true)) {
        long long mic_end_ms = g_metric_mic_end_ms.load();
        long long asr_final_ms = g_metric_asr_final_ms.load();
        long long llm_submit_ms = g_metric_llm_submit_ms.load();
        long long tts_segment_ms = g_metric_tts_first_segment_ms.load();
        std::cout << "[METRIC] e2e_first_audio_ms=" << (mic_end_ms > 0 ? now - mic_end_ms : -1)
                  << " asr_final_to_audio_ms=" << (asr_final_ms > 0 ? now - asr_final_ms : -1)
                  << " llm_submit_to_audio_ms=" << (llm_submit_ms > 0 ? now - llm_submit_ms : -1)
                  << " tts_segment_to_audio_ms=" << (tts_segment_ms > 0 ? now - tts_segment_ms : -1)
                  << " turn=" << g_turn_id.load() << std::endl;
    }
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
            long long now = metric_now_ms();
            if (!g_metric_tts_first_segment_seen.exchange(true)) {
                g_metric_tts_first_segment_ms = now;
                long long llm_submit_ms = g_metric_llm_submit_ms.load();
                std::cout << "[METRIC] tts_first_segment turn=" << g_turn_id.load()
                          << " llm_submit_to_segment_ms=" << (llm_submit_ms > 0 ? now - llm_submit_ms : -1)
                          << " chars=" << utf8_count_chars(segment, segment.size()) << std::endl;
            }
            bool is_final = final && trim_ascii(pending).empty();
            std::cout << "[TTS-Segment] " << segment << " final=" << (is_final ? 1 : 0) << std::endl;
            tts_pipeline_push(segment.c_str(), is_final ? 1 : 0);
        }
    }
}

static void append_asr_segment(std::string& full_text,
                               std::string& last_segment,
                               const std::string& segment) {
    if (segment.empty() || segment == last_segment) return;
    if (!full_text.empty() && full_text.size() >= segment.size() &&
        full_text.compare(full_text.size() - segment.size(), segment.size(), segment) == 0) {
        last_segment = segment;
        return;
    }
    full_text += segment;
    last_segment = segment;
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
        long long worker_begin_ms = metric_now_ms();
        if (task.turn_id == g_turn_id.load()) {
            g_metric_llm_submit_ms = task.submit_ms;
            std::cout << "[METRIC] llm_queue turn=" << task.turn_id
                      << " wait_ms=" << (worker_begin_ms - task.submit_ms)
                      << " text_chars=" << utf8_count_chars(task.text, task.text.size()) << std::endl;
        }
        std::cout << "[Agent] " << std::flush;
        static std::string g_full;
        static std::string g_tts_pending;
        g_full.clear();
        g_tts_pending.clear();
        agent_chat(task.text.c_str(), [](const char *text, int is_final) {
            if (text && text[0]) {
                long long now = metric_now_ms();
                if (!g_metric_llm_first_token_seen.exchange(true)) {
                    long long submit_ms = g_metric_llm_submit_ms.load();
                    long long asr_final_ms = g_metric_asr_final_ms.load();
                    std::cout << "[METRIC] llm_first_token turn=" << g_turn_id.load()
                              << " llm_submit_to_first_token_ms=" << (submit_ms > 0 ? now - submit_ms : -1)
                              << " asr_final_to_first_token_ms=" << (asr_final_ms > 0 ? now - asr_final_ms : -1)
                              << std::endl;
                }
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
        uint64_t stream_pcm_frames = 0;
        uint64_t stream_pcm_bytes = 0;
        uint64_t stream_seq_gap = 0;
        long long stream_first_pcm_ms = 0;
        long long stream_last_pcm_ms = 0;
        uint64_t turn_pcm_bytes = 0;
        std::string last_text;
        std::string turn_text;
        std::string last_asr_segment;
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
            else if (seq != expected_seq) {
                std::cerr << "[Warning] 帧序号跳变! 期望:" << expected_seq << " 实际:" << seq << std::endl;
                stream_seq_gap++;
            }
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
                    long long interrupt_begin_ms = metric_now_ms();
                    tts_pipeline_interrupt();
                    std::cout << "[METRIC] interrupt turn=" << g_turn_id.load()
                              << " busy=1 interrupt_ms=" << (metric_now_ms() - interrupt_begin_ms) << std::endl;
                } else if (!tts_model_path.empty()) {
                    long long cancel_begin_ms = metric_now_ms();
                    send_tts_cancel();
                    std::cout << "[METRIC] interrupt turn=" << g_turn_id.load()
                              << " busy=0 interrupt_ms=" << (metric_now_ms() - cancel_begin_ms) << std::endl;
                }
                uint64_t new_turn = g_turn_id.fetch_add(1) + 1;
                metric_reset_turn();
                g_metric_mic_start_ms = metric_now_ms();
                turn_pcm_bytes = 0;
                mic_active = true;
                utterance_submitted = false;
                last_text.clear();
                turn_text.clear();
                last_asr_segment.clear();
                asr_reset();
                recorder.start_segment(sample_rate ? sample_rate : 16000,
                                       channels ? channels : 1);
                uint32_t sr = sample_rate ? sample_rate : 16000;
                uint16_t ch = channels ? channels : 1;
                asr_cooldown_bytes = (sr / 5) * ch * sizeof(int16_t); // 200ms，防 TTS 尾音/混响自激
                std::cout << "[MIC] START rate=" << sample_rate
                          << " channels=" << channels << std::endl;
                std::cout << "[METRIC] mic_start turn=" << new_turn
                          << " sample_rate=" << sr
                          << " channels=" << ch << std::endl;
                continue;
            }

            if (type == AI_FRAME_MIC_END) {
                mic_active = false;
                long long mic_end_ms = metric_now_ms();
                g_metric_mic_end_ms = mic_end_ms;
                std::cout << "[MIC] END" << std::endl;
                std::cout << "[METRIC] mic_end turn=" << g_turn_id.load()
                          << " record_ms=" << (g_metric_mic_start_ms.load() > 0 ? mic_end_ms - g_metric_mic_start_ms.load() : -1)
                          << " pcm_bytes=" << turn_pcm_bytes << std::endl;
                append_asr_segment(turn_text, last_asr_segment, last_text);
                if (!utterance_submitted && !turn_text.empty()) {
                    std::cout << "[ASR] 按键提交: " << turn_text << std::endl;
                    metric_mark_asr_final(turn_text);
                    if (!tts_model_path.empty() && tts_pipeline_is_busy()) {
                        std::cout << "[TTS] 播放中，忽略此句" << std::endl;
                    } else if (!llm_model_path.empty()) {
                        long long submit_ms = metric_now_ms();
                        g_metric_llm_submit_ms = submit_ms;
                        { std::lock_guard<std::mutex> lk(g_queue_mutex);
                          g_task_queue.push({turn_text, false, g_turn_id.load(), submit_ms}); }
                        g_queue_cv.notify_one();
                        utterance_submitted = true;
                    }
                }
                recorder.end_segment();
                asr_reset();
                last_text.clear();
                turn_text.clear();
                last_asr_segment.clear();
                continue;
            }

            if (type != AI_FRAME_MIC_PCM) {
                continue;
            }

            if (payload_size > 0) {
                long long frame_now_ms = metric_now_ms();
                stream_pcm_frames++;
                stream_pcm_bytes += payload_size;
                if (stream_first_pcm_ms == 0) stream_first_pcm_ms = frame_now_ms;
                stream_last_pcm_ms = frame_now_ms;

                if (mic_active) {
                    recorder.write_pcm(payload_buf.data(), payload_buf.size());
                    turn_pcm_bytes += payload_buf.size();
                }

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
                            // 按键式交互下，业务提交以 MIC_END 为准；录音期间的 ASR endpoint 只做内部切段。
                            append_asr_segment(turn_text, last_asr_segment, last_text);
                            asr_reset();
                            last_text.clear();
                            continue;
                        }
                        metric_mark_asr_final(last_text);
                        if (!tts_model_path.empty() && (tts_pipeline_is_busy() || in_cooldown)) {
                            if (in_cooldown) std::cout << "[TTS] 冷却期，忽略此句" << std::endl;
                            else            std::cout << "[TTS] 播放中，忽略此句" << std::endl;
                        } else if (!llm_model_path.empty()) {
                            long long submit_ms = metric_now_ms();
                            g_metric_llm_submit_ms = submit_ms;
                            { std::lock_guard<std::mutex> lk(g_queue_mutex);
                              g_task_queue.push({last_text, false, g_turn_id.load(), submit_ms}); }
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
        long long stream_duration_ms = stream_first_pcm_ms > 0 ? stream_last_pcm_ms - stream_first_pcm_ms : 0;
        double avg_frame_interval_ms = stream_pcm_frames > 1
            ? static_cast<double>(stream_duration_ms) / (stream_pcm_frames - 1)
            : 0.0;
        std::cout << "[METRIC] stream frames=" << stream_pcm_frames
                  << " bytes=" << stream_pcm_bytes
                  << " seq_gap=" << stream_seq_gap
                  << " duration_ms=" << stream_duration_ms
                  << " avg_frame_interval_ms=" << avg_frame_interval_ms << std::endl;
        std::cout << "[-] 开发板连接断开。" << std::endl;
        asr_reset();

        if (!llm_model_path.empty()) {
            { std::lock_guard<std::mutex> lk(g_queue_mutex);
              while (!g_task_queue.empty()) g_task_queue.pop();
              g_task_queue.push({"", true, g_turn_id.load(), metric_now_ms()}); }
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
