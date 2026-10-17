#include "state_machine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led.h"


static const char *TAG = "STATE_MACHINE";
static app_state_t current_state = STATE_IDLE;
static SemaphoreHandle_t state_mutex;

void state_machine_init(void) {
  state_mutex = xSemaphoreCreateMutex();
  configASSERT(state_mutex);
  current_state = STATE_IDLE;
  ESP_LOGI(TAG, "State machine initialized dynamically: IDLE");
}

app_state_t state_machine_get(void) {
  app_state_t state;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  state = current_state;
  xSemaphoreGive(state_mutex);
  return state;
}

const char *state_to_string(app_state_t state) {
  switch (state) {
  case STATE_IDLE:
    return "IDLE";
  case STATE_RECEIVING:
    return "RECEIVING";
  case STATE_FILE_READY:
    return "FILE_READY";
  case STATE_USB_ACTIVE:
    return "USB_ACTIVE";
  case STATE_ERROR:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

bool state_machine_transition(app_state_t new_state) {
  xSemaphoreTake(state_mutex, portMAX_DELAY);

  // Enforcement of valid transitions
  bool valid = false;
  if (new_state == STATE_RECEIVING) {
      valid = true; // Always allow starting a new transfer from any state
  } else if (new_state == STATE_ERROR) {
      valid = true; // Errors (e.g. unmountable SD at boot) can occur in any state
  } else {
      switch (current_state) {
      case STATE_IDLE:
        if (new_state == STATE_USB_ACTIVE)
          valid = true;
        break;
      case STATE_RECEIVING:
        if (new_state == STATE_FILE_READY || new_state == STATE_ERROR || new_state == STATE_IDLE)
          valid = true;
        break;
      case STATE_FILE_READY:
        if (new_state == STATE_USB_ACTIVE || new_state == STATE_IDLE)
          valid = true;
        break;
      case STATE_USB_ACTIVE:
        if (new_state == STATE_IDLE || new_state == STATE_FILE_READY)
          valid = true;
        break;
      case STATE_ERROR:
        if (new_state == STATE_IDLE)
          valid = true;
        break;
      }
  }

  if (valid) {
    ESP_LOGI(TAG, "Transition %s -> %s", state_to_string(current_state),
             state_to_string(new_state));
    current_state = new_state;

    // Tie LED directly to state
    switch (current_state) {
    case STATE_IDLE:
      led_set_state(RECEIVER_LED_IDLE);
      break;
    case STATE_RECEIVING:
      led_set_state(RECEIVER_LED_RECEIVING);
      break;
    case STATE_FILE_READY:
      led_set_state(RECEIVER_LED_FILE_READY);
      break;
    case STATE_USB_ACTIVE:
      led_set_state(RECEIVER_LED_USB_ACTIVE);
      break;
    case STATE_ERROR:
      led_set_state(RECEIVER_LED_ERROR);
      break;
    }

  } else {
    ESP_LOGW(TAG, "Invalid Transition Attempt: %s -> %s",
             state_to_string(current_state), state_to_string(new_state));
  }

  xSemaphoreGive(state_mutex);
  return valid;
}
