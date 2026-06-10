#include <alsa/asoundlib.h>
#include <pthread.h>
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "ringbuffer.hpp"
#include <vector>

// 预设配置定义
const AudioConfig AUDIO_CFG_WM8960 = {
    .device   = "hw:0,0",
    .rate     = 44100,
    .channels = 2,
};

const AudioConfig AUDIO_CFG_USB = {
    .device   = "plughw:1,0",
    .rate     = 16000,
    .channels = 1,
    
};

static snd_pcm_t *pcm = NULL;
static snd_pcm_uframes_t period_size = 1024;
static unsigned int periods = 16;
static unsigned int channel = 1;
static unsigned int rate = 16000;
static u_int32_t total_pcm_bytes;
volatile int g_record_run = 0; // 核心控制开关
static pthread_t g_record_thread;
static pthread_t g_writer_thread;
char g_filename[256];
static int g_record_count = 0;
volatile static int g_file_ready = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_file_cond = PTHREAD_COND_INITIALIZER;
static RingBuffer g_rb(128 * 1024);

// 这是从 pcm_capture.c 抽离出来的纯采样逻辑
void* record_worker(void* arg) {
    std::vector<uint8_t> buf(period_size * channel * 2);

    while (1) {
        if (g_record_run) {
            
            int ret = snd_pcm_readi(pcm, buf.data(), period_size);
            if (ret > 0 ) {
                int bytes_to_write = ret * channel * 2;
                size_t written = g_rb.write(buf.data(),bytes_to_write);
                if (written < static_cast<size_t>(bytes_to_write)) {
                fprintf(stderr, "[RB] buffer full, drop %d bytes\n",
                        bytes_to_write - static_cast<int>(written));
            }
            } else if (ret == -EPIPE) {
                snd_pcm_prepare(pcm); // 处理 Overrun
            }
        } else {
            // 3. 停止录音时关闭文件
            usleep(10000); // 停止期间进入低功耗休眠
        }
    }
    return NULL;
}

void* writer_worker(void* arg)
{
    std::vector<uint8_t> out_buf(period_size * channel * 2);
    int fd = -1;

    while (1) {
        if (g_record_run) {
            if (fd < 0) {
                fd = open(g_filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror("音频文件打开失败");
                    usleep(10000);
                    continue;
                }

                lseek(fd, 44, SEEK_SET);
                total_pcm_bytes = 0;
            }

            size_t read_bytes = g_rb.read(out_buf.data(), out_buf.size());
            if (read_bytes > 0) {
                write(fd, out_buf.data(), read_bytes);
                total_pcm_bytes += read_bytes;
            }
        } else {
            if (fd >= 0) {
                while (g_rb.availableData() > 0) {
                    size_t chunk = g_rb.availableData();
                    if (chunk > out_buf.size()) {
                        chunk = out_buf.size();
                    }

                    chunk = g_rb.read(out_buf.data(), chunk);
                    if (chunk > 0) {
                        write(fd, out_buf.data(), chunk);
                        total_pcm_bytes += chunk;
                    }
                }

                write_wav_header(fd, total_pcm_bytes, rate, channel);
                close(fd);
                fd = -1;
                audio_set_file_ready();

                printf("[Audio] WAV文件保存完成，大小: %u 字节\n", total_pcm_bytes);
            }

            usleep(10000);
        }
    }

    return NULL;
}

// 对外接口：启动录音线程
int audio_init(const AudioConfig *cfg) {
    snd_pcm_hw_params_t *hwparams = NULL;
    int ret;
    unsigned int actual_rate;

    if (cfg == NULL) return -1;

    channel = cfg->channels;
    rate    = cfg->rate;

    fprintf(stderr, "[Audio] device=%s, rate=%u, channels=%u\n",
            cfg->device, rate, channel);

    /* 打开PCM设备 */
    ret = snd_pcm_open(&pcm, cfg->device, SND_PCM_STREAM_CAPTURE, 0);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_open error: %s: %s\n",
                    cfg->device, snd_strerror(ret));
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

    /* 设置采样率（near: 允许硬件选择最接近的支持值，plughw 自动转换） */
    actual_rate = rate;
    ret = snd_pcm_hw_params_set_rate_near(pcm, hwparams, &actual_rate, NULL);
    if (0 > ret) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate_near error: %s\n", snd_strerror(ret));
        goto err2;
    }
    if (actual_rate != rate) {
        fprintf(stderr, "[Audio] rate adjusted: requested=%u, actual=%u\n",
                rate, actual_rate);
        rate = actual_rate;
    }

    /* 设置声道数 */
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

    fprintf(stderr, "[Audio] init OK: actual_rate=%u, channels=%u\n",
            rate, channel);

     ret = pthread_create(&g_record_thread, NULL, record_worker, NULL);
     if(ret != 0){
        fprintf(stderr,"record_pthread_create failed\n");
     }
     ret = pthread_detach(g_record_thread);
    if (ret != 0) {
        fprintf(stderr, "record_pthread_detach failed\n");
        return -1;
    }
    ret = pthread_create(&g_writer_thread, NULL, writer_worker, NULL);
    if (ret != 0) {
        fprintf(stderr, "writer_pthread_create failed\n");
        return -1;
    }

    ret = pthread_detach(g_writer_thread);
    if (ret != 0) {
        fprintf(stderr, "writer_pthread_detach failed\n");
        return -1;
    }

     return 0;

err2:
    snd_pcm_hw_params_free(hwparams);   //释放内存
err1:
    snd_pcm_close(pcm); //关闭pcm设备
    return -1;

}

void audio_start_recording(void) {
    snprintf(g_filename, sizeof(g_filename), "/tmp/rec_%03d.wav", g_record_count++);
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

    // 填充 RIFF 段
    memcpy(riff.ChunkID, "RIFF", 4);
    riff.ChunkSize = pcm_data_size + 36; // 总大小 - ChunkID和ChunkSize占据的8字节
    memcpy(riff.Format, "WAVE", 4);

    // 填充 FMT 段
    memcpy(fmt.Subchunk1ID, "fmt ", 4);
    fmt.Subchunk1Size = 16;
    fmt.AudioFormat = 1; // PCM格式
    fmt.NumChannels = channels;
    fmt.SampleRate = sample_rate;
    fmt.ByteRate = sample_rate * channels * 16 / 8;
    fmt.BlockAlign = channels * 16 / 8;
    fmt.BitsPerSample = 16;

    // 填充 DATA 段
    memcpy(data.Subchunk2ID, "data", 4);
    data.Subchunk2Size = pcm_data_size;

    // 写入文件开头
    lseek(fd, 0, SEEK_SET); // 回到文件头
    write(fd, &riff, sizeof(RIFF_t));
    write(fd, &fmt, sizeof(FMT_t));
    write(fd, &data, sizeof(DATA_t));
}

void audio_cleanup(void)
{
    if(pcm)
    {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        pcm = NULL;
    }
}
//等待按键调用
void audio_wait_file_ready(void)
{
    pthread_mutex_lock(&g_file_mutex);

    while (!g_file_ready) {
        pthread_cond_wait(&g_file_cond, &g_file_mutex);
    }

    pthread_mutex_unlock(&g_file_mutex);
}
//录音线程置位
void audio_set_file_ready(void)
{
    pthread_mutex_lock(&g_file_mutex);

    g_file_ready = 1;
    pthread_cond_signal(&g_file_cond);

    pthread_mutex_unlock(&g_file_mutex);
}
//开始新录音
void audio_reset_file_ready(void)
{
    pthread_mutex_lock(&g_file_mutex);

    g_file_ready = 0;

    pthread_mutex_unlock(&g_file_mutex);
}