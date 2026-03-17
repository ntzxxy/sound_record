#include "audio.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "led.h"
#include "key.h"


int main() {
    // 1. 初始化
    if (audio_init() < 0) {
        printf("音频初始化失败！\n");
        return -1;
    }
    pthread_t led_tid;
    pthread_create(&led_tid, NULL, led_blink_thread, NULL);
    int k_fd = Key_Thread("/dev/input/event2");
    while(1)
    {
        
    }

    return 0;
}
