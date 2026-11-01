#include "boot_button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_GPIO GPIO_NUM_0
#define POLL_MS 20
#define DEBOUNCE_SAMPLES 3 // level must hold 60 ms to count

static int s_hold_ms;
static void (*s_on_press)(bool);
static void (*s_on_fire)(void);

static void boot_button_task(void *arg) {
  (void)arg;
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << BOOT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);
  bool pressed = gpio_get_level(BOOT_GPIO) == 0; // down at start: wait for release
  bool armed = !pressed;
  bool fired = false;
  int same = 0;
  int held_ms = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    bool low = gpio_get_level(BOOT_GPIO) == 0;
    if (low != pressed) {
      if (++same < DEBOUNCE_SAMPLES) continue;
      pressed = low;
      same = 0;
      if (pressed && armed) {
        held_ms = 0;
        fired = false;
        if (s_on_press) s_on_press(true);
      } else if (!pressed) {
        if (armed && !fired && s_on_press) s_on_press(false);
        armed = true;
      }
      continue;
    }
    same = 0;
    if (pressed && armed && !fired) {
      held_ms += POLL_MS;
      if (held_ms >= s_hold_ms) {
        fired = true;
        if (s_on_fire) s_on_fire();
      }
    }
  }
}

void boot_button_start(int hold_ms, void (*on_press)(bool pressed), void (*on_fire)(void)) {
  s_hold_ms = hold_ms;
  s_on_press = on_press;
  s_on_fire = on_fire;
  xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 2, NULL);
}
