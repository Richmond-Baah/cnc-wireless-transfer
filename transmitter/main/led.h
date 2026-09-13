#ifndef LED_H
#define LED_H

#include <stdint.h>

typedef enum {
  LED_STATE_IDLE,
  LED_STATE_UPLOADING,
  LED_STATE_SENDING,
  LED_STATE_SUCCESS,
  LED_STATE_ERROR
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);

#endif // LED_H
