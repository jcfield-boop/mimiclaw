#pragma once

#include "esp_err.h"

/**
 * Perform OTA firmware update from a URL (blocking).
 * Downloads the firmware binary and applies it. Reboots on success.
 * Safe to call from a serial CLI handler.
 *
 * @param url  HTTPS URL to the firmware .bin file
 * @return ESP_OK on success (device will reboot), error code otherwise
 */
esp_err_t ota_update_from_url(const char *url);

/**
 * Start an OTA update in a background task (non-blocking).
 * Use from web/Telegram handlers that must return promptly.
 * Progress is broadcast to the live log monitor channel.
 *
 * @param url  HTTPS URL to the firmware .bin file
 * @return ESP_OK if the task was started, ESP_ERR_INVALID_STATE if one is already running,
 *         ESP_ERR_NO_MEM if there is insufficient heap to start the task
 */
esp_err_t ota_start_async(const char *url);
