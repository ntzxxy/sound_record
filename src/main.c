#include "audio.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include "led.h"
#include "key.h"

static volatile sig_atomic_t g_quit = 0;

static void sig_handler(int sig) {
    g_quit = 1;
}


int main(int argc, char *argv[]) {
    // 用法: ./out [usb|wm8960] [--upload] [--server IP]
    //       ./out usb                        → 默认连 10.137.46.138:8080
    //       ./out usb --server 192.168.1.100 → 指定 IP
    //       ./out wm8960
    const AudioConfig *cfg = &AUDIO_CFG_USB;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "wm8960") == 0) {
            cfg = &AUDIO_CFG_WM8960;
        } else if (strcmp(argv[i], "usb") == 0) {
            cfg = &AUDIO_CFG_USB;
        } else if (strcmp(argv[i], "--upload") == 0) {
            g_enable_upload = 1;
        } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            g_server_ip = argv[++i];
        }
    }

    // 流传输模式下自动启用上传
#ifdef STREAMING_MODE
    g_enable_upload = 1;
#endif
    if (!g_server_ip) {
        fprintf(stderr, "[APP] 未指定服务器 IP，请通过 run.sh 启动或加 --server 参数\n");
        return -1;
    }
    printf("[APP] 录音模式: %s, 服务器: %s:%d\n",
           cfg == &AUDIO_CFG_USB ? "USB" : "WM8960",
           g_server_ip, g_server_port);
    printf("[APP] protocol: AIV1 full-duplex, build: %s %s\n", __DATE__, __TIME__);

    // 注册信号处理
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 1. 初始化
    if (audio_init(cfg) < 0) {
        printf("音频初始化失败！\n");
        return -1;
    }
    pthread_t led_tid;
    int ret = pthread_create(&led_tid, NULL, led_blink_thread, NULL);
    if(ret != 0){
        fprintf(stderr, "create led thread failed: %s\n", strerror(ret));
        return -1;
    }
    ret = pthread_detach(led_tid);
    if (ret != 0) {
        fprintf(stderr, "detach led thread failed: %s\n", strerror(ret));
        return -1;
    }
    int k_fd = Key_Thread("/dev/input/by-path/platform-gpio_keys@0-event");

    printf("Ready. Press Ctrl+C to exit.\n");

    while (!g_quit) {
        pause();
    }

    printf("\nShutting down...\n");
    audio_cleanup();
    return 0;
}
