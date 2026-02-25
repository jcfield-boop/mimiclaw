#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Return live device health: free heap, SPIFFS usage, uptime, WiFi RSSI,
 * and firmware version. No input parameters required.
 */
esp_err_t tool_system_info_execute(const char *input_json, char *output, size_t output_size);
