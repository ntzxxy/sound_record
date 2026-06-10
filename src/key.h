#ifndef KEY_H_
#define KEY_H_

#include <stdint.h>

// 上传开关与服务器配置
extern int g_enable_upload;
extern const char *g_server_ip;
extern int g_server_port;

int Key_Init(const char *path);
int Key_Read();
void* key_monitor_thread(void* arg);
int Key_Thread(const char *path);

#endif

