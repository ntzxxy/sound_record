#include "audio.h"
#include <stdio.h>
#include <unistd.h>

int main() {
    // 1. 初始化
    if (audio_init() < 0) {
        printf("音频初始化失败！\n");
        return -1;
    }

    // 2. 模拟按键按下 (value=1)
    printf("--- 模拟按键按下：开始录音 5 秒 ---\n");
    audio_start_recording("test.pcm"); 
    
    // 3. 模拟长按过程中 (value=2)
    sleep(5); 

    // 4. 模拟按键松开 (value=0)
    printf("--- 模拟按键松开：停止录音 ---\n");
    audio_stop_recording();

    printf("测试完成，请检查 test.pcm 文件。\n");
    return 0;
}