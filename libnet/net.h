#ifndef __NET_H__
#define __NET_H__
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int send_file_to_server(const char *filename, const char *ip, int port);

// 旧版单向 MIC PCM 帧头。保留用于兼容旧接口，新全双工会话使用 AiFrameHeader_t。
typedef struct {
    uint32_t seq;
    uint32_t timestamp;
    uint32_t payload_size;
} __attribute__((packed)) StreamHeader_t;

#define AI_FRAME_MAGIC 0x31495641u  /* 'AIV1' little-endian numeric tag */
#define AI_FRAME_VERSION 1
#define AI_AUDIO_FORMAT_S16_LE 1

typedef enum {
    AI_FRAME_HELLO      = 1,
    AI_FRAME_HEARTBEAT  = 2,

    AI_FRAME_MIC_START  = 10,
    AI_FRAME_MIC_PCM    = 11,
    AI_FRAME_MIC_END    = 12,

    AI_FRAME_TTS_START  = 20,
    AI_FRAME_TTS_PCM    = 21,
    AI_FRAME_TTS_END    = 22,
    AI_FRAME_TTS_CANCEL = 23,

    AI_FRAME_ERROR      = 100,
} AiFrameType_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t timestamp;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;
    uint32_t payload_size;
} __attribute__((packed)) AiFrameHeader_t;

#ifdef STREAMING_MODE
int stream_open(const char *ip, int port);
int stream_send_frame(int sockfd, uint32_t seq, const uint8_t *payload, uint32_t size);
int stream_send_ai_frame(int sockfd, uint16_t type, uint32_t seq,
                         uint32_t sample_rate, uint16_t channels,
                         uint16_t format,
                         const uint8_t *payload, uint32_t size);
void stream_close(int sockfd);
#endif

#ifdef __cplusplus
}
#endif

#endif
