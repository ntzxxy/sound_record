#include <alsa/asoundlib.h>
#include <pthread.h>
#include "audio.h"
#include <stdio.h> 
#include <stdlib.h> 
#include <errno.h> 
#include <string.h> 
#include <fcntl.h>

#define PCM_CAPTURE_DEV "hw:0,0" 

static snd_pcm_t *pcm = NULL;
static snd_pcm_uframes_t period_size = 1024; 
static unsigned int periods = 16;
static unsigned int channel = 2;              //声道数 
static unsigned int rate = 44100;           // 以后对接 AI 建议改 16000
static u_int32_t total_pcm_bytes;
volatile int g_record_run = 0; // 核心控制开关
static pthread_t g_record_thread;
static char g_filename[256];
static int g_record_count = 0;

// 这是从 pcm_capture.c 抽离出来的纯采样逻辑
void* record_worker(void* arg) {
    unsigned char *buf = malloc(period_size * channel * 2); // 假设 period_size 是 1024
    int fd = -1;

    while (1) {
        if (g_record_run) {
            // 1. 只有刚开始录音时才打开文件
            if (fd < 0) {
                fd = open(g_filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    // 核心异常处理：打日志，并且可以考虑关闭录音开关
                    perror("音频文件打开失败"); 
                    g_record_run = 0;
                    continue; 
                }
            
            if (fd >= 0) {
                lseek(fd, 44, SEEK_SET);
                total_pcm_bytes = 0;
            }
        }
            // 2. 读取音频数据 (这是原来的阻塞调用)
            int ret = snd_pcm_readi(pcm, buf, period_size);
            if (ret > 0 && fd > 0) {
                int bytes_to_write = ret * channel * 2;
                write(fd, buf, bytes_to_write);
                total_pcm_bytes += bytes_to_write;
            } else if (ret == -EPIPE) {
                snd_pcm_prepare(pcm); // 处理 Overrun
            }
        } else {
            // 3. 停止录音时关闭文件
            if (fd >= 0) {
                write_wav_header(fd, total_pcm_bytes, rate, channel);
                close(fd);
                fd = -1;
                printf("[Audio] WAV文件保存完成，大小: %u 字节\n", total_pcm_bytes);
            }
            usleep(10000); // 停止期间进入低功耗休眠
        }
    }
    free(buf);
    return NULL;
}

// 对外接口：启动录音线程
int audio_init(void) {
    // 这里放原来的 snd_pcm_init 代码
    snd_pcm_hw_params_t *hwparams = NULL;
    int ret;

    /* 打开PCM设备 */
    ret = snd_pcm_open(&pcm, PCM_CAPTURE_DEV, SND_PCM_STREAM_CAPTURE, 0);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_open error: %s: %s\n",
                    PCM_CAPTURE_DEV, snd_strerror(ret));
        return -1;
    }

    /* 实例化hwparams对象 */
    snd_pcm_hw_params_malloc(&hwparams);

    /* 获取PCM设备当前硬件配置,对hwparams进行初始化 */
    ret = snd_pcm_hw_params_any(pcm, hwparams);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_any error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /************** 
     设置参数
    ***************/
    /* 设置访问类型: 交错模式 */
    ret = snd_pcm_hw_params_set_access(pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_access error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 设置数据格式: 有符号16位、小端模式 */
    ret = snd_pcm_hw_params_set_format(pcm, hwparams, SND_PCM_FORMAT_S16_LE);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_format error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 设置采样率 */
    ret = snd_pcm_hw_params_set_rate(pcm, hwparams, rate, 0);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 设置声道数: 双声道 */
    ret = snd_pcm_hw_params_set_channels(pcm, hwparams, channel);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_channels error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 设置周期大小: period_size */
    ret = snd_pcm_hw_params_set_period_size(pcm, hwparams, period_size, 0);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_period_size error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 设置周期数（buffer的大小）: periods */
    ret = snd_pcm_hw_params_set_periods(pcm, hwparams, periods, 0);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_periods error: %s\n", snd_strerror(ret));
        goto err2;
    }

    /* 使配置生效 */
    ret = snd_pcm_hw_params(pcm, hwparams);
    snd_pcm_hw_params_free(hwparams);   //释放hwparams对象占用的内存
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params error: %s\n", snd_strerror(ret));
        goto err1;
    }
     pthread_create(&g_record_thread, NULL, record_worker, NULL);
     return 0;

err2:
    snd_pcm_hw_params_free(hwparams);   //释放内存
err1:
    snd_pcm_close(pcm); //关闭pcm设备
    return -1;
   
}

void audio_start_recording(void) {
    snprintf(g_filename, sizeof(g_filename), "rec_%03d.wav", g_record_count++);
    g_record_run = 1; // 拨动开关
}

void audio_stop_recording(void) {
    g_record_run = 0; // 拨动开关
}

/* 这里的参数分别是：文件描述符，PCM数据的大小（字节），采样率，通道数 */
void write_wav_header(int fd, int pcm_data_size, int sample_rate, int channels) {
    RIFF_t riff;
    FMT_t fmt;
    DATA_t data;

    // 1. 填充 RIFF 段
    memcpy(riff.ChunkID, "RIFF", 4);
    riff.ChunkSize = pcm_data_size + 36; // 总大小 - ChunkID和ChunkSize占据的8字节
    memcpy(riff.Format, "WAVE", 4);

    // 2. 填充 FMT 段
    memcpy(fmt.Subchunk1ID, "fmt ", 4);
    fmt.Subchunk1Size = 16;
    fmt.AudioFormat = 1; // PCM格式
    fmt.NumChannels = channels;
    fmt.SampleRate = sample_rate;
    fmt.ByteRate = sample_rate * channels * 16 / 8;
    fmt.BlockAlign = channels * 16 / 8;
    fmt.BitsPerSample = 16;

    // 3. 填充 DATA 段
    memcpy(data.Subchunk2ID, "data", 4);
    data.Subchunk2Size = pcm_data_size;

    // 4. 写入文件开头
    lseek(fd, 0, SEEK_SET); // 回到文件头
    write(fd, &riff, sizeof(RIFF_t));
    write(fd, &fmt, sizeof(FMT_t));
    write(fd, &data, sizeof(DATA_t));
}