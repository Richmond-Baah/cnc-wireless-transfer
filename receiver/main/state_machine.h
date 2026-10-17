#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdbool.h>

typedef enum {
  STATE_IDLE,       // Ready to receive, USB PC can access
  STATE_RECEIVING,  // Receiving file over Wi-Fi, USB stalled
  STATE_FILE_READY, // Verified successfully, USB PC can access
  STATE_USB_ACTIVE, // PC actively reading via USB, reject uploads
  STATE_ERROR,      // Checksum mismatch, write error, etc.
  STATE_CARD_ERROR  // SD card present but its FAT filesystem cannot be mounted
} app_state_t;

void state_machine_init(void);
app_state_t state_machine_get(void);
bool state_machine_transition(app_state_t new_state);
const char *state_to_string(app_state_t state);

#endif // STATE_MACHINE_H
