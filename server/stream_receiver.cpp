//
// Created by Lenovo on 2026/6/12.
//

#include "stream_receiver.h"
#include "net.h"
#include "asr.h"
#include "chat_agent.h"
#include <iostream>
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

// 辅助函数 A：在流结束时，将累积的原始 PCM 数据裹上 44 字节的 WAV 头，变成标准音频文件
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

    ofs.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader_t)); // 先写 44 字节头
    ofs.write(reinterpret_cast<const char*>(pcm_data.data()), pcm_data.size()); // 再写原始 PCM 字节
    ofs.close();
    std::cout << "[OK] 音频解析落盘成功: " << filename << std::endl;
}

// 辅助函数 B：TCP 是流式传输，recv 可能会产生拆包（一次没读够指定大小）。这个函数保证"不读够指定大小不返回"。
static ssize_t read_all(int fd, uint8_t* buf, size_t size) {
    size_t bytes_read = 0;
    while (bytes_read < size) {
        ssize_t ret = recv(fd, buf + bytes_read, size - bytes_read, 0);
        if (ret < 0) {
            if (errno == EINTR) continue; // 信号中断则重试
            return -1;
        }
        if (ret == 0)
            return bytes_read; // 对端正常断开了长连接
        bytes_read += ret;
    }
    return bytes_read;
}

static std::string g_response_buf;  // 累积一轮回复，用于检测拒绝

static void agent_callback(const char* text, int is_final) {
    if (text && text[0] != '\0') {
        std::cout << text << std::flush;
        g_response_buf += text;
    }
    if (is_final) {
        std::cout << std::endl;
        // 检测到模型拒绝回答时，清掉有毒上下文，下轮重新开始
        if (g_response_buf.find("对不起") != std::string::npos ||
            g_response_buf.find("我还没有学会") != std::string::npos) {
            std::cerr << "[Agent] 检测到拒绝回答，重置上下文" << std::endl;
            agent_reset();
        }
        g_response_buf.clear();
    }
}

// 核心业务函数：Socket 循环解包逻辑
int start_stream_server(int port, const std::string& save_dir,
                        const std::string& model_dir,
                        const std::string& llm_model_path) {
    // 自动创建保存目录
    if (mkdir(save_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        perror("mkdir 失败");
        return -1;
    }
    char abs_path[PATH_MAX];
    const char* display_path = realpath(save_dir.c_str(), abs_path) ? abs_path : save_dir.c_str();

    // 1. 标准网络 Socket 初始化
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 端口复用保护

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0 || listen(server_fd, 5) < 0) {
        perror("网络初始化失败");
        return -1;
    }

    std::cout << "[*] 服务已启动，监听端口: " << port << std::endl;
    std::cout << "[*] 录音保存路径: " << display_path << std::endl;

    // 初始化 ASR 引擎
    if (asr_init(model_dir.c_str()) != 0) {
        std::cerr << "[ASR] 模型加载失败，请检查路径: " << model_dir << std::endl;
        return -1;
    }
    std::cout << "[ASR] 模型加载成功" << std::endl;

    // 初始化对话代理（内部加载 LLM 模型）
    if (!llm_model_path.empty()) {
        if (agent_init(llm_model_path.c_str(), nullptr) != 0) {
            std::cerr << "[Agent] 模型加载失败，请检查路径: " << llm_model_path << std::endl;
            asr_destroy();
            return -1;
        }
        std::cout << "[Agent] 模型加载成功" << std::endl;
    } else {
        std::cout << "[Agent] 未指定模型路径，跳过初始化（仅 ASR 模式）" << std::endl;
    }

    // 2. 主循环：等待开发板的长连接建立
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::cout << "[+] 开发板按键按下，长连接流通道已建立。" << std::endl;

        std::vector<uint8_t> pcm_pool; // 用来暂存整条语音的所有原始 PCM 字节
        uint32_t expected_seq = 0;
        bool first_frame = true;

        // 3. 次级循环：在长连接没有断开前，源源不断地进行【帧头 + 负载】的解包流转
        std::string last_text;
        while (true) {
            StreamHeader_t header;

            // 步骤一：先雷打不动地从网络上读 12 字节的"固定帧头"
            ssize_t ret = read_all(client_fd, reinterpret_cast<uint8_t*>(&header), sizeof(StreamHeader_t));
            if (ret <= 0) break; // 板子松开按键，连接关闭，自然跳出循环

            // 步骤二：使用 ntohl 转换字节序 (网络大端转主机小端)，获取里面的帧信息
            uint32_t seq = ntohl(header.seq);
            uint32_t timestamp = ntohl(header.timestamp);
            uint32_t payload_size = ntohl(header.payload_size);

            // 步骤三：质量监控，校验网络丢包情况
            if (first_frame) {
                expected_seq = seq;
                first_frame = false;
            }
            else if (seq != expected_seq) {
                std::cerr << "[Warning] 网络发生丢包! 期望 seq: " << expected_seq << ", 实际收到: " << seq << std::endl;
            }
            expected_seq = seq + 1;

            // 步骤四：根据帧头里指定的 payload_size，从网络上精准读取紧跟在后面的 PCM 数据字节
            if (payload_size > 0) {
                std::vector<uint8_t> payload_buf(payload_size);
                ret = read_all(client_fd, payload_buf.data(), payload_size);
                if (ret < static_cast<ssize_t>(payload_size)) break;

                // 将本帧解出来的原始 PCM 塞入缓存池
                pcm_pool.insert(pcm_pool.end(), payload_buf.begin(), payload_buf.end());

                // === ASR 流式识别：每收到一帧就喂给引擎 ===
                int num_samples = static_cast<int>(payload_size) / 2;
                asr_process_frame(reinterpret_cast<const int16_t*>(payload_buf.data()), num_samples);

                // 只在文字变化时输出，避免刷屏
                const char* text = asr_get_result();
                if (text && text[0] != '\0' && last_text != text) {
                    last_text = text;
                    std::cout << "[ASR] " << text << std::endl;
                }

                // 检测到停顿后自动断句
                if (asr_is_endpoint()) {
                    if (!last_text.empty()) {
                        std::cout << "[ASR] 断句完成: " << last_text << std::endl;
                        if (!llm_model_path.empty()) {
                            std::cout << "[Agent] " << std::flush;
                            agent_chat(last_text.c_str(), agent_callback);
                        }
                    }
                    asr_reset();
                    last_text.clear();
                }
            }
        }

        // 4. 开发板松开按键，流结束，长连接彻底断开
        close(client_fd);
        std::cout << "[-] 开发板松开按键，流通道断开。" << std::endl;

        asr_reset();
        // 一次按键对话结束，重置 LLM 上下文，下次按键开始新一轮
        if (!llm_model_path.empty()) agent_reset();

        // 5. 扫尾落盘：将整段话的音频数据封装为 WAV 文件
        if (!pcm_pool.empty()) {
            char time_buf[64];
            std::time_t t = std::time(nullptr);
            std::strftime(time_buf, sizeof(time_buf), "voice_%Y%m%d_%H%M%S.wav", std::localtime(&t));
            save_wav_file(save_dir + "/" + time_buf, pcm_pool, 16000, 1); // 默认 16000Hz 单声道
        }
    }

    asr_destroy();
    if (!llm_model_path.empty()) agent_destroy();
    close(server_fd);
    return 0;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string save_dir = "./voice_records";
    std::string model_dir = "../models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16";
    std::string llm_model_path;  // 默认空，仅 ASR 模式

    if (argc > 1) port = std::stoi(argv[1]);
    if (argc > 2) save_dir = argv[2];
    if (argc > 3) model_dir = argv[3];
    if (argc > 4) llm_model_path = argv[4];

    return start_stream_server(port, save_dir, model_dir, llm_model_path);
}
