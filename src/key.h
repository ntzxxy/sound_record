#ifndef KEY_H_
#define KEY_H_

int Key_Init(const char *path);
int Key_Read();
void* key_monitor_thread(void* arg);
int Key_Thread(const char *path);

#endif 

