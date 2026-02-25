#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Read the ESP32-C6 internal chip temperature.
 * No input parameters required.
 * Output: "Device temperature: XX.X°C (ESP32-C6 internal sensor)"
 */
esp_err_t tool_device_temp_execute(const char *input_json, char *output, size_t output_size);
