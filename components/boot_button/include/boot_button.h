#pragma once

#include <stdbool.h>

// Long press on BOOT (GPIO0, pinhole), polled and debounced. A press already
// down when the task starts is ignored until released. on_press(true) when a
// press starts, on_press(false) when it ends without firing; on_fire() once
// per hold after hold_ms. Shorter holds do nothing.
void boot_button_start(int hold_ms, void (*on_press)(bool pressed), void (*on_fire)(void));
