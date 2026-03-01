#include "tool_wifi_scan.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "tool_wifi_scan";

#define WIFI_SCAN_MAX_APS  10

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "Open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_ENTERPRISE:      return "Enterprise";
        default:                        return "Unknown";
    }
}

esp_err_t tool_wifi_scan_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    /* Perform a blocking active scan on all channels */
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: wifi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Retrieve results — stack allocation (~1.6 KB for 10 records) */
    wifi_ap_record_t records[WIFI_SCAN_MAX_APS];
    uint16_t ap_count = WIFI_SCAN_MAX_APS;

    err = esp_wifi_scan_get_ap_records(&ap_count, records);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: get scan results failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "wifi_scan: found %u APs", (unsigned)ap_count);

    /* Build JSON output */
    int pos = 0;
    int remaining = (int)output_size;

    pos += snprintf(output + pos, remaining, "[");
    remaining = (int)output_size - pos;

    for (uint16_t i = 0; i < ap_count && remaining > 2; i++) {
        /* Sanitize SSID: replace non-printable chars */
        char ssid[33] = {0};
        memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        for (int j = 0; j < 32; j++) {
            if (ssid[j] != '\0' && (ssid[j] < 0x20 || ssid[j] > 0x7e)) {
                ssid[j] = '?';
            }
        }

        int n = snprintf(output + pos, remaining,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"auth\":\"%s\"}",
                         i > 0 ? "," : "",
                         ssid,
                         records[i].rssi,
                         records[i].primary,
                         auth_mode_str(records[i].authmode));
        if (n <= 0 || n >= remaining) break;
        pos += n;
        remaining -= n;
    }

    if (remaining > 1) {
        output[pos++] = ']';
        output[pos]   = '\0';
    } else {
        /* truncated — close the array safely */
        output[output_size - 2] = ']';
        output[output_size - 1] = '\0';
    }

    return ESP_OK;
}
