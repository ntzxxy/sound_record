#ifndef __NET_H__
#define __NET_H__
#include <stdint.h>
/**
 * @brief 将本地文件通过 TCP 发送到服务器
 * * @param filename 要发送的文件路径 (如 "rec_001.wav")
 * @param ip       服务器的 IP 地址 (如 "172.25.6.200")
 * @param port     服务器监听的端口 (如 8080)
 * @return int     成功返回 0，失败返回 -1
 */

int send_file_to_server(const char *filename, const char *ip, int port);

#ifdef STREAMING_MODE
// 帧大小 = rate * 0.1s * channels * 2，由 audio 层根据设备配置动态计算

// 严格对齐的帧头协议 (12 字节)
typedef struct {
    uint32_t seq;          // 帧序号，从 0 开始递增
    uint32_t timestamp;    // 时间戳（暂可用无符号数或递增时间）
    uint32_t payload_size; // 后面跟的原始 PCM 字节数
} __attribute__((packed)) StreamHeader_t;

/**
 * @brief 打开流式 TCP 连接（长连接开始）
 * @return 成功返回 sockfd，失败返回 -1
 */
int stream_open(const char *ip, int port);

/**
 * @brief 发送一帧音频数据（带报头）
 * @return 成功返回 0，失败返回 -1
 */
int stream_send_frame(int sockfd, uint32_t seq, const uint8_t *payload, uint32_t size);

/**
 * @brief 关闭流式连接
 */
void stream_close(int sockfd);
#endif

#endif