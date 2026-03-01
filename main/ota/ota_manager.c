#include "ota_manager.h"
#include "gateway/ws_server.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

/* Minimum free internal heap before we will start OTA.
 * TLS handshake peaks at ~50 KB; 80 KB leaves a safe margin. */
#define OTA_MIN_FREE_HEAP   (80 * 1024)

/* HTTP receive buffer for OTA download — kept small on C6 (no PSRAM). */
#define OTA_HTTP_BUF_SIZE   4096

/* Guard flag: prevents two concurrent OTA attempts. */
static volatile bool s_ota_running = false;

/* ── Core update logic (blocking) ─────────────────────────────── */

esp_err_t ota_update_from_url(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    uint32_t free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_heap < OTA_MIN_FREE_HEAP) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA aborted: heap too low (%lu B, need %d B)",
                 (unsigned long)free_heap, OTA_MIN_FREE_HEAP);
        ESP_LOGW(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        return ESP_ERR_NO_MEM;
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "OTA start: %.90s", url);
        ESP_LOGI(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
    }

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = 120000,
        .buffer_size       = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx    = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Accept HTTP redirects — GitHub Releases redirects to S3 */
        .max_redirection_count = 3,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config       = &http_cfg,
        .http_client_init_cb = NULL,
    };

    /* H4: Use the advanced OTA API so we can validate the image header
     * before committing any bytes to flash.  If the download is corrupt
     * (wrong magic, wrong chip) we abort without touching the running image.
     * NOTE: Bootloader rollback requires OTA partition slots which the
     * current partitions_c6.csv does not include (factory-only layout).
     * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is therefore left disabled. */
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA begin failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        return ret;
    }

    /* Inspect the image descriptor before writing a single byte */
    esp_app_desc_t new_desc;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_desc);
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA image invalid (bad header): %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        esp_https_ota_abort(ota_handle);
        return ret;
    }
    {
        const esp_app_desc_t *running = esp_app_get_description();
        char msg[128];
        snprintf(msg, sizeof(msg), "OTA image OK: v%s → v%s",
                 running->version, new_desc.version);
        ESP_LOGI(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
    }

    /* Stream and write the firmware */
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    }
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA write failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        esp_https_ota_abort(ota_handle);
        return ret;
    }

    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        ws_server_broadcast_monitor("ota", "OTA complete — rebooting...");
        ESP_LOGI(TAG, "OTA successful, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));  /* give WS a chance to flush */
        esp_restart();
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA finish failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
    }

    return ret;
}

/* ── Async task wrapper ──────────────────────────────────────────── */

static char s_ota_url[256];

static void ota_task(void *arg)
{
    (void)arg;
    ota_update_from_url(s_ota_url);
    s_ota_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_start_async(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (s_ota_running) {
        ws_server_broadcast_monitor("ota", "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    /* 10 KB stack: OTA does blocking HTTP inside the task but keeps its own
     * TLS context; the main TLS peak is ~50 KB on the heap, not the stack. */
    s_ota_running = true;
    BaseType_t ret = xTaskCreate(ota_task, "ota_update", 10 * 1024, NULL, 5, NULL);
    if (ret != pdPASS) {
        s_ota_running = false;
        ws_server_broadcast_monitor("ota", "OTA task create failed (OOM)");
        return ESP_ERR_NO_MEM;
    }

    ws_server_broadcast_monitor("ota", "OTA task started");
    return ESP_OK;
}
