#include "tool_gpio.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "cJSON.h"

static const char *TAG = "tool_gpio";

/* ── Pin safety ─────────────────────────────────────────────────
 * Valid GPIO range on ESP32-C6: 0–22
 * Blocked:
 *   Pin 8  — WS2812 RGB LED on most devkit boards
 *   Pin 9  — BOOT button (strapping, input-only)
 *   18-21  — SPI flash interface (internal module wiring)
 */
#define GPIO_PIN_MAX  22

static bool gpio_pin_blocked(int pin)
{
    if (pin < 0 || pin > GPIO_PIN_MAX) return true;
    if (pin == 8)  return true;  /* WS2812 LED */
    if (pin == 9)  return true;  /* BOOT button */
    if (pin >= 18 && pin <= 21) return true;  /* SPI flash */
    return false;
}

static int parse_pin(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return -1;
    }
    cJSON *j_pin = cJSON_GetObjectItem(input, "pin");
    if (!j_pin || !cJSON_IsNumber(j_pin)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'pin' (integer) is required");
        return -1;
    }
    int pin = j_pin->valueint;
    cJSON_Delete(input);

    if (gpio_pin_blocked(pin)) {
        snprintf(output, output_size,
                 "Error: pin %d is not available (blocked or out of range 0-%d, excluding 8,9,18-21)",
                 pin, GPIO_PIN_MAX);
        return -1;
    }
    return pin;
}

/* ── gpio_read ──────────────────────────────────────────────── */

esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size)
{
    int pin = parse_pin(input_json, output, output_size);
    if (pin < 0) return ESP_ERR_INVALID_ARG;

    int level = gpio_get_level((gpio_num_t)pin);
    ESP_LOGI(TAG, "gpio_read pin=%d level=%d", pin, level);

    snprintf(output, output_size,
             "{\"pin\":%d,\"state\":\"%s\",\"raw\":%d}",
             pin, level ? "HIGH" : "LOW", level);
    return ESP_OK;
}

/* ── gpio_write ─────────────────────────────────────────────── */

esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_pin   = cJSON_GetObjectItem(input, "pin");
    cJSON *j_state = cJSON_GetObjectItem(input, "state");

    if (!j_pin || !cJSON_IsNumber(j_pin)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'pin' (integer) is required");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = j_pin->valueint;
    if (gpio_pin_blocked(pin)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: pin %d is not available (blocked or out of range 0-%d, excluding 8,9,18-21)",
                 pin, GPIO_PIN_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    int level = -1;
    if (j_state) {
        if (cJSON_IsNumber(j_state)) {
            level = j_state->valueint ? 1 : 0;
        } else if (cJSON_IsString(j_state)) {
            if (strcasecmp(j_state->valuestring, "HIGH") == 0 ||
                strcmp(j_state->valuestring, "1") == 0) {
                level = 1;
            } else if (strcasecmp(j_state->valuestring, "LOW") == 0 ||
                       strcmp(j_state->valuestring, "0") == 0) {
                level = 0;
            }
        }
    }
    cJSON_Delete(input);

    if (level < 0) {
        snprintf(output, output_size,
                 "Error: 'state' must be \"HIGH\", \"LOW\", 1, or 0");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gpio_set_level((gpio_num_t)pin, (uint32_t)level);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: gpio_set_level failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "gpio_write pin=%d level=%d", pin, level);
    snprintf(output, output_size, "{\"ok\":true,\"pin\":%d,\"state\":\"%s\"}",
             pin, level ? "HIGH" : "LOW");
    return ESP_OK;
}

/* ── gpio_mode ──────────────────────────────────────────────── */

esp_err_t tool_gpio_mode_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_pin  = cJSON_GetObjectItem(input, "pin");
    cJSON *j_mode = cJSON_GetObjectItem(input, "mode");

    if (!j_pin || !cJSON_IsNumber(j_pin)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'pin' (integer) is required");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = j_pin->valueint;
    if (gpio_pin_blocked(pin)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: pin %d is not available (blocked or out of range 0-%d, excluding 8,9,18-21)",
                 pin, GPIO_PIN_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    const char *mode_str = (j_mode && cJSON_IsString(j_mode))
                           ? j_mode->valuestring : "input";
    cJSON_Delete(input);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (strcasecmp(mode_str, "output") == 0) {
        cfg.mode      = GPIO_MODE_OUTPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    } else if (strcasecmp(mode_str, "input_pullup") == 0) {
        cfg.mode      = GPIO_MODE_INPUT;
        cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    } else if (strcasecmp(mode_str, "input_pulldown") == 0) {
        cfg.mode      = GPIO_MODE_INPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else {
        /* default: floating input */
        cfg.mode      = GPIO_MODE_INPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "gpio_mode pin=%d mode=%s", pin, mode_str);
    snprintf(output, output_size,
             "{\"ok\":true,\"pin\":%d,\"mode\":\"%s\"}", pin, mode_str);
    return ESP_OK;
}
