#ifndef __NET_H__
#define __NET_H__
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int send_file_to_server(const char *filename, const char *ip, int port);

// 严格对齐的帧头协议 (12 字节) — 板子端和 PC 端共用
typedef struct {
    uint32_t seq;
    uint32_t timestamp;
    uint32_t payload_size;
} __attribute__((packed)) StreamHeader_t;

#ifdef STREAMING_MODE
int stream_open(const char *ip, int port);
int stream_send_frame(int sockfd, uint32_t seq, const uint8_t *payload, uint32_t size);
void stream_close(int sockfd);
#endif

#ifdef __cplusplus
}
#endif

#endif