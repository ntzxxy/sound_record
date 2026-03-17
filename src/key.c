#include <stdio.h>      // 用于 printf 报错或调试
#include <fcntl.h>      // 用于 open
#include <unistd.h>     // 用于 read, close
#include <linux/input.h> // 用于 struct input_event 和按键宏定义
#include "key.h"
#include "audio.h"
#include <pthread.h>
#include "net.h"        

static int fd = -1;

int Key_Init(const char *path)
{ 
    if(0 > (fd = open(path,O_RDONLY)))
    {
        perror("key open error");
        return -1;
    }
    return fd;
}

int Key_Read()
{
    struct input_event in_ev = {0};
     if (sizeof(struct input_event) != 
            read(fd, &in_ev, sizeof(struct input_event))) { 
            perror("read error"); 
            return(-1); 
        }
        if (in_ev.type == EV_KEY) {
        return in_ev.value; // 直接返回状态
        }
        return -2;

}

void* key_monitor_thread(void* arg)
{
    int is_recording = 0;
    while(1)
    {
        int key_value = Key_Read();
        if(key_value == 1)
        {
            audio_start_recording();
            is_recording = 1;
        }
        else if(key_value == 0)
        {
            audio_stop_recording();
            if(is_recording == 1)
            {
                while(!g_file_ready) {
                    usleep(1000); // 每次只等 1ms，极速响应
                }
                printf("[APP] 检测到录音收尾完成，开始上传: %s\n", g_filename);
                send_file_to_server(g_filename, "172.25.6.200", 8080);
                g_file_ready = 0; // 上传完重置，等待下次录音
                is_recording = 0;
            }
        }
    }
}

int Key_Thread(const char *path)
{
    int k_fd = -1;
    k_fd = Key_Init(path);
    if(k_fd < 0)
    {
        perror("init error");
        return -1;
    }
    pthread_t tid;
    pthread_create(&tid,NULL,key_monitor_thread,NULL);
}