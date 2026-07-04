//
// Created by Lenovo on 2026/6/12.
//

#ifndef STREAM_RECEIVER_H
#define STREAM_RECEIVER_H

#include <stdint.h>
#include <string>

// 1. 标准 WAV 文件头 (固定 44 字节)
typedef struct {
    char ChunkID[4];         // "RIFF"
    uint32_t ChunkSize;      // 总文件大小 - 8 字节
    char Format[4];          // "WAVE"
    char Subchunk1ID[4];     // "fmt "
    uint32_t Subchunk1Size;  // PCM 格式固定为 16
    uint16_t AudioFormat;    // 1 代表原始 PCM
    uint16_t NumChannels;    // 声道数 (单声道为 1)
    uint32_t SampleRate;     // 采样率 (如 16000)
    uint32_t ByteRate;       // 每秒传输的字节率
    uint16_t BlockAlign;     // 区块对齐大小
    uint16_t BitsPerSample;  // 采样深度 (16位)
    char Subchunk2ID[4];     // "data"
    uint32_t Subchunk2Size;  // 原始 PCM 数据的总大小
} __attribute__ ((packed)) WavHeader_t;

int start_stream_server(int port, const std::string& save_dir,
                       const std::string& asr_model_dir,
                       const std::string& llm_model_path);

#endif //STREAM_RECEIVER_H
