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

// BOOT long-press feedback, shown over the state pattern. DONE clears itself.
typedef enum { LED_BUTTON_OFF, LED_BUTTON_HELD, LED_BUTTON_DONE } led_button_t;
void led_set_button(led_button_t mode);

#endif // RECEIVER_LED_H
