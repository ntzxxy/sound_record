#include <stdio.h>      // 用于 printf 报错或调试
#include <fcntl.h>      // 用于 open
#include <unistd.h>     // 用于 read, close
#include <linux/input.h> // 用于 struct input_event 和按键宏定义
#include "led.h"
#include "audio.h"
#include <pthread.h> 



void set_led_brightness(int value) {
    int fd = open("/sys/class/leds/sys-led/brightness", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "%d", value);
        close(fd);
    }
}

void* led_blink_thread(void* arg) {
    while(1)
    {
        if(g_record_run)
        {
            set_led_brightness(1);
            usleep(200000);         // 200ms
            set_led_brightness(0);  // 灭
            usleep(200000);
        }
        else
        {
            set_led_brightness(0);
            usleep(200000);
        }
        
        

    }
return NULL;
}