#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Scan for nearby WiFi access points and return results as JSON.
 * No input parameters required.
 * Output: JSON array of up to 10 APs sorted by RSSI:
 *   [{"ssid":"Name","rssi":-42,"channel":6,"auth":"WPA2"}, ...]
 */
esp_err_t tool_wifi_scan_execute(const char *input_json, char *output, size_t output_size);
