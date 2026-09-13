#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"


#define LED_STRIP_GPIO 48
#define LED_STRIP_MAX_LEDS 1

static const char *TAG = "LED";
static volatile receiver_led_state_t current_state = RECEIVER_LED_IDLE;
static led_strip_handle_t led_strip;

static void set_color(uint8_t r, uint8_t g, uint8_t b) {
  if (led_strip) {
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
  }
}

static void led_task(void *pvParameters) {
  uint8_t breathe = 0;
  int8_t breathe_dir = 5;

  while (1) {
    switch (current_state) {
    case RECEIVER_LED_IDLE:
      // White pulse (slow breathing)
      set_color(breathe, breathe, breathe);
      breathe += breathe_dir;
      if (breathe >= 100 || breathe <= 0) {
        breathe_dir = -breathe_dir;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      break;

    case RECEIVER_LED_RECEIVING:
      // Blinking yellow (medium speed)
      set_color(128, 128, 0); // Yellow
      vTaskDelay(pdMS_TO_TICKS(250));
      set_color(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(250));
      break;

    case RECEIVER_LED_FILE_READY:
      // Solid green
      set_color(0, 128, 0);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;

    case RECEIVER_LED_USB_ACTIVE:
      // Solid blue
      set_color(0, 0, 128);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;

    case RECEIVER_LED_ERROR:
      // Solid red for 5 seconds, then reset LED state to IDLE.
      // NOTE: The LED task must NOT call state_machine_transition() here.
      // LED is a leaf component driven BY the state machine; driving it
      // back would create a circular dependency and potential re-entrancy.
      // The state machine itself transitions to STATE_IDLE after an error
      // (e.g., after a successful cancel or retry), which will update the
      // LED state via led_set_state(). This side here just clears the
      // visual indication after the timeout.
      set_color(128, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(5000));
      current_state = RECEIVER_LED_IDLE;
      break;
    }
  }
}

void led_init(void) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = LED_STRIP_GPIO,
      .max_leds = LED_STRIP_MAX_LEDS,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = {
          .format = {
              .r_pos = 1,
              .g_pos = 0,
              .b_pos = 2,
              .num_components = 3,
          },
      },
      .flags.invert_out = false,
  };

  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
      .flags.with_dma = false,
  };

  ESP_ERROR_CHECK(
      led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
  led_strip_clear(led_strip);

  xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
  ESP_LOGI(TAG, "WS2812 LED initialized on GPIO %d", LED_STRIP_GPIO);
}

void led_set_state(receiver_led_state_t state) {
  current_state = state;
  ESP_LOGI(TAG, "State changed to %d", state);
}
