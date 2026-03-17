#ifndef LED_H_
#define LED_H_

void set_led_brightness(int value);
void* led_blink_thread(void* arg);

#endif 