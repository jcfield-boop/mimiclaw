#include "tool_device_temp.h"

#include <stdio.h>
#include "esp_log.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "tool_temp";

esp_err_t tool_device_temp_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    temperature_sensor_handle_t handle = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

    if (temperature_sensor_install(&cfg, &handle) != ESP_OK) {
        snprintf(output, output_size, "Error: temperature sensor init failed");
        return ESP_FAIL;
    }

    if (temperature_sensor_enable(handle) != ESP_OK) {
        temperature_sensor_uninstall(handle);
        snprintf(output, output_size, "Error: temperature sensor enable failed");
        return ESP_FAIL;
    }

    float celsius = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(handle, &celsius);

    temperature_sensor_disable(handle);
    temperature_sensor_uninstall(handle);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: temperature read failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Chip temperature: %.1f°C", celsius);
    snprintf(output, output_size, "Device temperature: %.1f°C (ESP32-C6 internal sensor)", celsius);
    return ESP_OK;
}
