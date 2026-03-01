#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Read the current logic level of a GPIO pin.
 * Input: {"pin": int}
 * Output: {"pin":N,"state":"HIGH"|"LOW","raw":0|1}
 */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/**
 * Write a logic level to a GPIO pin (must be configured as output first).
 * Input: {"pin": int, "state": "HIGH"|"LOW"|1|0}
 * Output: {"ok":true}
 */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/**
 * Configure the mode of a GPIO pin.
 * Input: {"pin": int, "mode": "input"|"output"|"input_pullup"|"input_pulldown"}
 * Output: {"ok":true}
 */
esp_err_t tool_gpio_mode_execute(const char *input_json, char *output, size_t output_size);
