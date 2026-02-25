#include "tool_system_info.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_app_desc.h"

static const char *TAG = "tool_sysinfo";

esp_err_t tool_system_info_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    /* Heap */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap  = esp_get_minimum_free_heap_size();

    /* SPIFFS */
    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);

    /* Uptime */
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    int hours   = (int)(uptime_s / 3600);
    int minutes = (int)((uptime_s % 3600) / 60);
    int seconds = (int)(uptime_s % 60);

    /* WiFi RSSI + local IP */
    int rssi = 0;
    char ip_str[16] = "0.0.0.0";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    /* Firmware version */
    const esp_app_desc_t *desc = esp_app_get_description();

    char uptime_str[24];
    if (hours > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%dh %dm", hours, minutes);
    } else if (minutes > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%dm %ds", minutes, seconds);
    } else {
        snprintf(uptime_str, sizeof(uptime_str), "%ds", seconds);
    }

    int spiffs_pct = spiffs_total ? (int)(spiffs_used * 100 / spiffs_total) : 0;

    snprintf(output, output_size,
             "Free heap: %lu bytes (min: %lu)\n"
             "SPIFFS: %u / %u bytes used (%d%%)\n"
             "Uptime: %s\n"
             "WiFi: %s  RSSI: %d dBm\n"
             "Firmware: %s (built %s %s)",
             (unsigned long)free_heap,
             (unsigned long)min_heap,
             (unsigned)spiffs_used,
             (unsigned)spiffs_total,
             spiffs_pct,
             uptime_str,
             ip_str,
             rssi,
             desc ? desc->version : "unknown",
             desc ? desc->date    : "",
             desc ? desc->time    : "");

    ESP_LOGI(TAG, "system_info: heap=%lu spiffs=%u/%u uptime=%s rssi=%d",
             (unsigned long)free_heap, (unsigned)spiffs_used, (unsigned)spiffs_total,
             uptime_str, rssi);

    return ESP_OK;
}
