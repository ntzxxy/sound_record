#include <alsa/asoundlib.h>
#include <pthread.h>
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "ringbuffer.hpp"
#include "net.h"
#include <vector>
#include <queue>
#include <atomic>
#include <sys/socket.h>
#include <arpa/inet.h>

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

// 引入外界控制流传输地址的全局变量声明（key中定义）
extern "C" {
extern const char *g_server_ip;
extern int g_server_port;
}

static snd_pcm_t *pcm = NULL;
static snd_pcm_uframes_t period_size = 1024;
static unsigned int periods = 16;
static unsigned int channel = 1;
static unsigned int rate = 16000;
static u_int32_t total_pcm_bytes;
volatile int g_record_run = 0; // 核心控制开关
static pthread_t g_record_thread;
static pthread_t g_writer_thread;
static pthread_t g_downlink_thread;
static pthread_t g_playback_thread;
static std::atomic<int> g_stream_fd{-1};
char g_filename[256];
static int g_record_count = 0;
volatile static int g_file_ready = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_file_cond = PTHREAD_COND_INITIALIZER;
static RingBuffer g_rb(256 * 1024);

struct PlaybackMessage {
    std::vector<int16_t> pcm;
    int sample_rate = 44100;
    int channels = 1;
    bool end = false;
    bool cancel = false;
};

static std::queue<PlaybackMessage> g_playback_queue;
static pthread_mutex_t g_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_playback_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_playback_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t *g_playback_pcm = NULL;
static int g_playback_rate = 0;
static int g_playback_channels = 0;
static uint64_t g_playback_pcm_frames = 0;
static uint64_t g_playback_pcm_bytes = 0;

static ssize_t recv_all_local(int fd, uint8_t *buf, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(fd, buf + got, size - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        got += n;
    }
    return got;
}

static void playback_queue_push(PlaybackMessage &&msg) {
    pthread_mutex_lock(&g_playback_mutex);
    g_playback_queue.push(std::move(msg));
    pthread_cond_signal(&g_playback_cond);
    pthread_mutex_unlock(&g_playback_mutex);
}

static void playback_queue_clear_locked() {
    std::queue<PlaybackMessage> empty;
    g_playback_queue.swap(empty);
}

static void playback_stop_device() {
    pthread_mutex_lock(&g_playback_device_mutex);
    if (g_playback_pcm) {
        snd_pcm_drop(g_playback_pcm);
        snd_pcm_prepare(g_playback_pcm);
    }
    pthread_mutex_unlock(&g_playback_device_mutex);
}

static void playback_cancel_local() {
    pthread_mutex_lock(&g_playback_mutex);
    playback_queue_clear_locked();
    pthread_mutex_unlock(&g_playback_mutex);
    playback_stop_device();
}

static int playback_init_device(int sample_rate, int channels) {
    pthread_mutex_lock(&g_playback_device_mutex);
    if (g_playback_pcm && g_playback_rate == sample_rate && g_playback_channels == channels) {
        pthread_mutex_unlock(&g_playback_device_mutex);
        return 0;
    }
    if (g_playback_pcm) {
        snd_pcm_drop(g_playback_pcm);
        snd_pcm_close(g_playback_pcm);
        g_playback_pcm = NULL;
    }

    const char *devices[] = {"default", "plughw:0,0"};
    int ret = -1;
    for (const char *dev : devices) {
        ret = snd_pcm_open(&g_playback_pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
        if (ret >= 0) {
            ret = snd_pcm_set_params(g_playback_pcm, SND_PCM_FORMAT_S16_LE,
                                     SND_PCM_ACCESS_RW_INTERLEAVED,
                                     channels, sample_rate, 1, 500000);
            if (ret >= 0) {
                g_playback_rate = sample_rate;
                g_playback_channels = channels;
                fprintf(stderr, "[Playback] ALSA ready device=%s rate=%d channels=%d\n",
                        dev, sample_rate, channels);
                pthread_mutex_unlock(&g_playback_device_mutex);
                return 0;
            }
            snd_pcm_close(g_playback_pcm);
            g_playback_pcm = NULL;
        }
    }
    fprintf(stderr, "[Playback] ALSA init failed: %s\n", snd_strerror(ret));
    pthread_mutex_unlock(&g_playback_device_mutex);
    return -1;
}

static void playback_write_pcm(const int16_t *pcm_data, int samples, int channels) {
    if (!g_playback_pcm || !pcm_data || samples <= 0) return;
    pthread_mutex_lock(&g_playback_device_mutex);
    if (!g_playback_pcm) {
        pthread_mutex_unlock(&g_playback_device_mutex);
        return;
    }
    int frames_total = samples / channels;
    int written = 0;
    while (written < frames_total) {
        int frames = frames_total - written;
        if (frames > 1024) frames = 1024;
        int ret = snd_pcm_writei(g_playback_pcm, pcm_data + written * channels, frames);
        if (ret < 0) {
            ret = snd_pcm_recover(g_playback_pcm, ret, 0);
            if (ret < 0) {
                pthread_mutex_unlock(&g_playback_device_mutex);
                return;
            }
            continue;
        }
        written += ret;
    }
    pthread_mutex_unlock(&g_playback_device_mutex);
}

void* playback_worker(void*) {
    while (1) {
        pthread_mutex_lock(&g_playback_mutex);
        while (g_playback_queue.empty()) {
            pthread_cond_wait(&g_playback_cond, &g_playback_mutex);
        }
        PlaybackMessage msg = std::move(g_playback_queue.front());
        g_playback_queue.pop();
        pthread_mutex_unlock(&g_playback_mutex);

        if (msg.cancel) {
            pthread_mutex_lock(&g_playback_mutex);
            playback_queue_clear_locked();
            pthread_mutex_unlock(&g_playback_mutex);
            playback_stop_device();
            continue;
        }

        if (!msg.pcm.empty()) {
            if (playback_init_device(msg.sample_rate, msg.channels) == 0) {
                playback_write_pcm(msg.pcm.data(), static_cast<int>(msg.pcm.size()), msg.channels);
            }
        }

        if (msg.end && g_playback_pcm) {
            // 不在后台线程里 drain：部分 ALSA 驱动在 underrun/设备状态异常时 drain 可能长期阻塞，
            // 会连带影响下一次按键打断和录音开始。writei 已把数据交给内核缓冲，结束帧只作边界标记。
        }
    }
    return NULL;
}

void* downlink_worker(void*) {
    while (1) {
        int fd = g_stream_fd.load();
        if (fd < 0) {
            usleep(50000);
            continue;
        }

        AiFrameHeader_t header;
        ssize_t n = recv_all_local(fd, reinterpret_cast<uint8_t *>(&header), sizeof(header));
        if (n <= 0) {
            if (g_stream_fd.exchange(-1) == fd) stream_close(fd);
            continue;
        }

        uint32_t magic = ntohl(header.magic);
        uint16_t version = ntohs(header.version);
        uint16_t type = ntohs(header.type);
        uint32_t sample_rate = ntohl(header.sample_rate);
        uint16_t channels = ntohs(header.channels);
        uint32_t payload_size = ntohl(header.payload_size);

        if (magic != AI_FRAME_MAGIC || version != AI_FRAME_VERSION || payload_size > 1024 * 1024) {
            if (g_stream_fd.exchange(-1) == fd) stream_close(fd);
            continue;
        }

        std::vector<uint8_t> payload;
        if (payload_size > 0) {
            payload.resize(payload_size);
            n = recv_all_local(fd, payload.data(), payload_size);
            if (n <= 0) {
                if (g_stream_fd.exchange(-1) == fd) stream_close(fd);
                continue;
            }
        }

        if (type == AI_FRAME_TTS_START) {
            pthread_mutex_lock(&g_playback_mutex);
            playback_queue_clear_locked();
            pthread_mutex_unlock(&g_playback_mutex);
            playback_stop_device();
            g_playback_pcm_frames = 0;
            g_playback_pcm_bytes = 0;
            fprintf(stderr, "[Playback] TTS_START rate=%u channels=%u\n", sample_rate, channels);
        } else if (type == AI_FRAME_TTS_PCM) {
            g_playback_pcm_frames++;
            g_playback_pcm_bytes += payload.size();
            if (g_playback_pcm_frames == 1) {
                fprintf(stderr, "[Playback] first TTS_PCM bytes=%zu\n", payload.size());
            }
            PlaybackMessage msg;
            msg.sample_rate = sample_rate ? static_cast<int>(sample_rate) : 44100;
            msg.channels = channels ? static_cast<int>(channels) : 1;
            msg.pcm.resize(payload.size() / sizeof(int16_t));
            if (!msg.pcm.empty()) {
                memcpy(msg.pcm.data(), payload.data(), msg.pcm.size() * sizeof(int16_t));
                playback_queue_push(std::move(msg));
            }
        } else if (type == AI_FRAME_TTS_END) {
            PlaybackMessage msg;
            msg.sample_rate = sample_rate ? static_cast<int>(sample_rate) : 44100;
            msg.channels = channels ? static_cast<int>(channels) : 1;
            msg.end = true;
            playback_queue_push(std::move(msg));
            fprintf(stderr, "[Playback] TTS_END frames=%llu bytes=%llu\n",
                    (unsigned long long)g_playback_pcm_frames,
                    (unsigned long long)g_playback_pcm_bytes);
        } else if (type == AI_FRAME_TTS_CANCEL) {
            PlaybackMessage msg;
            msg.cancel = true;
            playback_queue_push(std::move(msg));
            fprintf(stderr, "[Playback] TTS_CANCEL\n");
        }
    }
    return NULL;
}

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
#ifdef STREAMING_MODE
    /* ==========================================
     * 流实时传输模式 (STREAMING)
     * ========================================== */
    const uint32_t frame_size = rate * channel / 5; // 100ms PCM: rate * 0.1 * ch * 2
    std::vector<uint8_t> out_buf(frame_size);
    uint32_t frame_seq = 0;

    int conn_fail_cnt = 0;
    bool was_recording = false;
    bool mic_started = false;
    while (1) {
        int stream_fd = g_stream_fd.load();
        if (stream_fd < 0) {
            frame_seq = 0;
            stream_fd = stream_open(g_server_ip, g_server_port);
            if (stream_fd < 0) {
                if (++conn_fail_cnt == 1) {
                    fprintf(stderr, "[Audio-Stream] 等待连接 %s:%d ...\n",
                            g_server_ip, g_server_port);
                }
                usleep(50000);
                continue;
            }
            conn_fail_cnt = 0;
            mic_started = false;
            int old_fd = g_stream_fd.exchange(stream_fd);
            if (old_fd >= 0 && old_fd != stream_fd) stream_close(old_fd);
        }

        if (g_record_run) {
            was_recording = true;
            if (!mic_started) {
                playback_cancel_local();
                if (stream_send_ai_frame(stream_fd, AI_FRAME_MIC_START, frame_seq++,
                                         rate, channel, AI_AUDIO_FORMAT_S16_LE,
                                         NULL, 0) < 0) {
                    fprintf(stderr, "[Audio-Stream] MIC_START 发送失败，重连\n");
                    if (g_stream_fd.exchange(-1) == stream_fd) stream_close(stream_fd);
                    continue;
                }
                mic_started = true;
                g_rb.reset();
                printf("[Audio-Stream] MIC_START\n");
            }

            // 只有当 RingBuffer 里的数据够拼满 100ms 一帧时才读出来发送
            if (g_rb.availableData() >= frame_size) {
                size_t read_bytes = g_rb.read(out_buf.data(), frame_size);
                if (read_bytes == frame_size) {
                    if (stream_send_ai_frame(stream_fd, AI_FRAME_MIC_PCM, frame_seq++,
                                             rate, channel, AI_AUDIO_FORMAT_S16_LE,
                                             out_buf.data(), frame_size) < 0) {
                        fprintf(stderr, "[Audio-Stream] 发送中断，强行关闭流\n");
                        if (g_stream_fd.exchange(-1) == stream_fd) stream_close(stream_fd);
                        mic_started = false;
                    }
                }
            } else {
                usleep(10000);
            }
        } else if (was_recording) {
            // 下降沿：松开按键，扫尾阶段（只执行一次）
            was_recording = false;
            if (stream_fd >= 0) {
                printf("[Audio-Stream] 停止录音，发送剩余数据...\n");
                while (g_rb.availableData() > 0) {
                    size_t data_len = g_rb.availableData();
                    if (data_len > frame_size) data_len = frame_size;
                    size_t read_bytes = g_rb.read(out_buf.data(), data_len);
                    if (read_bytes > 0) {
                        if (stream_send_ai_frame(stream_fd, AI_FRAME_MIC_PCM, frame_seq++,
                                                 rate, channel, AI_AUDIO_FORMAT_S16_LE,
                                                 out_buf.data(), read_bytes) < 0)
                            break;
                    }
                }
                // 等待 record_worker in-flight 数据
                usleep(50000);
                while (g_rb.availableData() > 0) {
                    size_t data_len = g_rb.availableData();
                    if (data_len > frame_size) data_len = frame_size;
                    size_t read_bytes = g_rb.read(out_buf.data(), data_len);
                    if (read_bytes > 0)
                        stream_send_ai_frame(stream_fd, AI_FRAME_MIC_PCM, frame_seq++,
                                             rate, channel, AI_AUDIO_FORMAT_S16_LE,
                                             out_buf.data(), read_bytes);
                }
                if (mic_started) {
                    if (stream_send_ai_frame(stream_fd, AI_FRAME_MIC_END, frame_seq++,
                                             rate, channel, AI_AUDIO_FORMAT_S16_LE,
                                             NULL, 0) < 0) {
                        fprintf(stderr, "[Audio-Stream] MIC_END 发送失败，重连\n");
                        if (g_stream_fd.exchange(-1) == stream_fd) stream_close(stream_fd);
                    } else {
                        printf("[Audio-Stream] MIC_END，连接保持\n");
                    }
                }
                mic_started = false;
                g_rb.reset();
            } else {
                fprintf(stderr, "[Audio-Stream] 录音结束，未建立连接，无数据发送\n");
                g_rb.reset();
            }
            audio_set_file_ready();
        }
        usleep(10000);
    }

#else
/* ==========================================
     * 原有本地 WAV 文件落盘模式
     * ========================================== */
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
#endif
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

     ret = pthread_create(&g_downlink_thread, NULL, downlink_worker, NULL);
    if (ret != 0) {
        fprintf(stderr, "downlink_pthread_create failed\n");
        return -1;
    }
    ret = pthread_detach(g_downlink_thread);
    if (ret != 0) {
        fprintf(stderr, "downlink_pthread_detach failed\n");
        return -1;
    }

    ret = pthread_create(&g_playback_thread, NULL, playback_worker, NULL);
    if (ret != 0) {
        fprintf(stderr, "playback_pthread_create failed\n");
        return -1;
    }
    ret = pthread_detach(g_playback_thread);
    if (ret != 0) {
        fprintf(stderr, "playback_pthread_detach failed\n");
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
    int fd = g_stream_fd.exchange(-1);
    if (fd >= 0) stream_close(fd);
    if (g_playback_pcm) {
        pthread_mutex_lock(&g_playback_device_mutex);
        snd_pcm_drop(g_playback_pcm);
        snd_pcm_close(g_playback_pcm);
        g_playback_pcm = NULL;
        pthread_mutex_unlock(&g_playback_device_mutex);
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
