#ifndef RECEIVER_LED_H
#define RECEIVER_LED_H

typedef enum {
  RECEIVER_LED_IDLE,
  RECEIVER_LED_RECEIVING,
  RECEIVER_LED_FILE_READY,
  RECEIVER_LED_USB_ACTIVE,
  RECEIVER_LED_ERROR,
  RECEIVER_LED_CARD_ERROR
} receiver_led_state_t;

void led_init(void);
void led_set_state(receiver_led_state_t state);

#endif // RECEIVER_LED_H
