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
        int k_fd = Key_Thread("/dev/input/event2");
    while(1)
    {
        pause();
    }

    return 0;
}
