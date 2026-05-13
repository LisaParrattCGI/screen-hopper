#ifndef _STATUS_LED_H_
#define _STATUS_LED_H_

void status_led_init();
void status_led_set_red(bool on);
void status_led_flash_green();
void status_led_task();

#endif
