#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <string.h>
#include "key.h"
#include "audio.h"
#include <pthread.h>
#include "net.h"

// 服务器配置（可通过 main 覆盖）
int g_enable_upload = 0;
const char *g_server_ip = "192.168.0.7";
int g_server_port = 8080;

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

    while(1)
    {
        int key_value = Key_Read();
        if(key_value == 1)
        {
            audio_start_recording();
            audio_reset_file_ready();
        }
        else if(key_value == 0)
        {
            audio_stop_recording();
            audio_wait_file_ready();

            if (g_enable_upload) {
                printf("[APP] 录音完成，开始上传: %s -> %s:%d\n",
                       g_filename, g_server_ip, g_server_port);
                send_file_to_server(g_filename, g_server_ip, g_server_port);
            } else {
                printf("[APP] 录音完成，文件保存至: %s（上传已禁用）\n", g_filename);
            }
        }
    }
    
return NULL;
}

int Key_Thread(const char *path)
{
    int k_fd = -1;
    k_fd = Key_Init(path);
    if(k_fd < 0)
    {
        perror("init error");
        return -1;
    }static int g_file_ready = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_file_cond = PTHREAD_COND_INITIALIZER;
    pthread_t tid;
    int ret = pthread_create(&tid,NULL,key_monitor_thread,NULL);
    if(ret != 0)
    {
        printf("pthread_create failed: %s\n", strerror(ret));
    }
     ret = pthread_detach(tid);
    if (ret != 0) {
        fprintf(stderr, "key_pthread_detach failed\n");
        return -1;
    }

    return 0;
}