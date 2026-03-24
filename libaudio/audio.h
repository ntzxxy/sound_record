#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>    
#include <sys/types.h> 

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WAV_RIFF {
    char ChunkID[4];                /* "RIFF" */
    u_int32_t ChunkSize;            /* 总字节数 - 8 */
    char Format[4];                 /* "WAVE" */
} __attribute__ ((packed)) RIFF_t;

typedef struct WAV_FMT {
    char Subchunk1ID[4];            /* "fmt " */
    u_int32_t Subchunk1Size;        /* 16 for PCM */
    u_int16_t AudioFormat;          /* 1 for PCM */
    u_int16_t NumChannels;          /* 1 for mono, 2 for stereo */
    u_int32_t SampleRate;           /* 采样率 */
    u_int32_t ByteRate;             /* 字节率 */
    u_int16_t BlockAlign;           /* 区块对齐 */
    u_int16_t BitsPerSample;        /* 采样深度 */
} __attribute__ ((packed)) FMT_t;

typedef struct WAV_DATA {
    char Subchunk2ID[4];            /* "data" */
    u_int32_t Subchunk2Size;        /* pcm数据大小 */
} __attribute__ ((packed)) DATA_t;

// 初始化和去初始化
int audio_init(void);
void audio_cleanup(void);

// 录音控制接口
void audio_start_recording(void);
void audio_stop_recording(void);
void write_wav_header(int fd, int pcm_data_size, int sample_rate, int channels);
void audio_wait_file_ready(void);
void audio_set_file_ready(void);
void audio_reset_file_ready(void);
extern volatile int g_record_run;
extern char g_filename[256];

#ifdef __cplusplus
}
#endif

#endif
