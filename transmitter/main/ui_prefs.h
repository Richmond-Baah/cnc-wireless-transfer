#ifndef UI_PREFS_H
#define UI_PREFS_H

#include <stdbool.h>
#include <stddef.h>

#define UI_ACCENT_DEFAULT "#C2410C"

// Web UI accent colour, NVS sys_cfg/ui_accent ("#RRGGBB"); default if unset or invalid.
void ui_accent_get(char *out, size_t len);
// Validates ^#[0-9a-fA-F]{6}$ and saves it (upper case). False if invalid or not saved.
bool ui_accent_set(const char *hex);
// Factory reset paths: back to the default.
void ui_accent_reset(void);

#endif // UI_PREFS_H
