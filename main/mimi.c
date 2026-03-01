#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include <time.h>
#include <sys/time.h>

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "buttons/button_driver.h"
#include "imu/imu_manager.h"
#include "skills/skill_loader.h"
#include "led/led_status.h"

static const char *TAG = "mimi";

/* Set system clock to compile time if SNTP fails.
 * This ensures TLS cert validation passes since build time
 * is always within any cert's validity window. */
static void set_time_from_build(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    struct tm t = {0};
    strptime(desc->date, "%b %d %Y", &t);
    strptime(desc->time, "%H:%M:%S", &t);
    t.tm_isdst = -1;
    time_t build_time = mktime(&t);
    if (build_time > 0) {
        struct timeval tv = { .tv_sec = build_time };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "Using build timestamp as clock: %s %s", desc->date, desc->time);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Write default content only if the file does not already exist.
 * This preserves user edits across firmware updates. */
static void write_if_missing(const char *path, const char *content)
{
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return; /* already exists — keep user's version */
    }
    f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create default: %s", path);
        return;
    }
    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Created default: %s", path);
}

/* Bootstrap personality and config files so they survive firmware updates.
 * Only writes on first boot (or after a full SPIFFS erase). */
static void bootstrap_defaults(void)
{
    write_if_missing(MIMI_SOUL_FILE,
        "I am C6PO, a personal AI assistant running on an ESP32-C6 microcontroller.\n"
        "\n"
        "Personality:\n"
        "- Helpful and friendly\n"
        "- Concise and to the point\n"
        "- Curious and eager to learn\n"
        "\n"
        "Values:\n"
        "- Accuracy over speed\n"
        "- User privacy and safety\n"
        "- Transparency in actions\n");

    write_if_missing(MIMI_USER_FILE,
        "# User Profile\n"
        "\n"
        "- Name: (not set)\n"
        "- Language: English\n"
        "- Timezone: (not set)\n");

    write_if_missing(MIMI_HEARTBEAT_FILE,
        "# Heartbeat Tasks\n"
        "\n"
        "- [ ] Get the current time and write a one-line system status entry to today's daily note.\n");
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            esp_err_t send_err = telegram_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
                char err_mon[64];
                snprintf(err_mon, sizeof(err_mon), "[tg->%s] dispatch failed", msg.chat_id);
                ws_server_broadcast_monitor("error", err_mon);
            } else {
                ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                char ok_mon[64];
                snprintf(ok_mon, sizeof(ok_mon), "[tg->%s] dispatched", msg.chat_id);
                ws_server_broadcast_monitor("done", ok_mon);
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
            /* Broadcast system responses to the live log so they're visible */
            char mon[200];
            snprintf(mon, sizeof(mon), "[%.20s] %.160s", msg.chat_id, msg.content);
            for (char *p = mon; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("sys", mon);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  C6PO - ESP32-C6 AI Assistant");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Min free heap:    %d bytes",
             (int)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Largest free blk: %d bytes",
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Free DMA heap:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_DMA));
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM free:       %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    ESP_LOGI(TAG, "PSRAM: Not available (ESP32-C6)");
#endif

    /* Input */
    button_Init();
#if !defined(CONFIG_IDF_TARGET_ESP32C6)
    imu_manager_init();
    imu_manager_set_shake_callback(NULL);
#endif

    /* LED: init first so it glows during the rest of boot */
    led_status_init();
    led_set_state(LED_BOOT);

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());
    bootstrap_defaults(); /* create config files if missing (preserves user edits) */

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    led_set_state(LED_WIFI_CONNECTING);
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
            led_set_state(LED_IDLE);

            /* Sync system clock via SNTP before any TLS connections.
             * Multiple servers tried; if UDP/123 is blocked, fall back
             * to the firmware build timestamp so TLS certs validate. */
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            esp_sntp_setservername(2, "time.cloudflare.com");
            esp_sntp_init();
            int sntp_retries = 0;
            while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && sntp_retries++ < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                ESP_LOGI(TAG, "Time synced via SNTP");
            } else {
                ESP_LOGW(TAG, "SNTP unavailable — using build timestamp fallback");
                set_time_from_build();
            }

            /* Outbound dispatch task should start first to avoid dropping early replies. */
            ESP_ERROR_CHECK((xTaskCreate(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            /* Start network-dependent services */
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(telegram_bot_start());
            cron_service_start();
            heartbeat_start();
            ESP_ERROR_CHECK(ws_server_start());

            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "C6PO ready. Type 'help' for CLI commands.");
}
