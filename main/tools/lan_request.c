#include "lan_request.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "lan_req";

/* ── Event callback: accumulate response into caller buffer ─── */

typedef struct {
    char   *buf;
    size_t  cap;    /* usable bytes (response_buf_size - 1) */
    size_t  len;
    bool    truncated;
} lan_rx_t;

static esp_err_t lan_event_cb(esp_http_client_event_t *evt)
{
    lan_rx_t *rx = (lan_rx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !rx->buf) return ESP_OK;

    size_t copy = (size_t)evt->data_len;
    if (rx->len + copy >= rx->cap) {
        copy = rx->cap - rx->len;
        if (copy > 0) rx->truncated = true;
        else return ESP_OK;  /* no room at all */
    }
    memcpy(rx->buf + rx->len, evt->data, copy);
    rx->len += copy;
    rx->buf[rx->len] = '\0';
    return ESP_OK;
}

/* ── lan_request ─────────────────────────────────────────────── */

esp_err_t lan_request(const char *method,
                      const char *base_url,
                      const char *endpoint,
                      const char *token_header,
                      const char *token_value,
                      const char *body,
                      char       *response_buf,
                      size_t      response_buf_size,
                      lan_result_t *result)
{
    memset(result, 0, sizeof(*result));
    if (response_buf && response_buf_size > 0) response_buf[0] = '\0';

    /* Build full URL */
    char url[320];
    snprintf(url, sizeof(url), "%s%s",
             base_url  ? base_url  : "",
             endpoint  ? endpoint  : "");

    lan_rx_t rx = {
        .buf       = response_buf,
        .cap       = response_buf_size > 0 ? response_buf_size - 1 : 0,
        .len       = 0,
        .truncated = false,
    };

    bool use_tls = (strncmp(url, "https://", 8) == 0);

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = lan_event_cb,
        .user_data     = &rx,
        .timeout_ms    = 8000,
    };
    if (use_tls) {
        cfg.crt_bundle_attach           = esp_crt_bundle_attach;
        /* TODO: per-host cert pinning via SERVICES.md fingerprint field */
        cfg.skip_cert_common_name_check = true;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(result->err_msg, sizeof(result->err_msg), "client_init_failed");
        return ESP_FAIL;
    }

    /* Method + body */
    if (method && strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (body && body[0]) {
            esp_http_client_set_post_field(client, body, (int)strlen(body));
        }
    }

    /* Optional auth header */
    if (token_header && token_value && token_header[0] && token_value[0]) {
        esp_http_client_set_header(client, token_header, token_value);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* Execute */
    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        result->ok          = false;
        result->http_status = 0;
        snprintf(result->err_msg, sizeof(result->err_msg), "%s", esp_err_to_name(err));
        ESP_LOGW(TAG, "HTTP failed: %s → %s", url, esp_err_to_name(err));
        return ESP_OK;   /* caller reads result->err_msg; not a C error */
    }

    result->ok          = (http_status >= 200 && http_status < 300);
    result->http_status = http_status;
    result->bytes       = (int)rx.len;
    result->truncated   = rx.truncated;

    ESP_LOGD(TAG, "%s %s → %d (%d bytes%s)",
             method ? method : "GET", url, http_status, (int)rx.len,
             rx.truncated ? " truncated" : "");
    return ESP_OK;
}

/* ── lan_result_to_json ─────────────────────────────────────── */

void lan_result_to_json(const lan_result_t *result,
                        const char *body_str,
                        char *out, size_t out_size)
{
    if (result->http_status == 0 && !result->ok) {
        snprintf(out, out_size,
                 "{\"ok\":false,\"status\":0,\"error\":\"connection_failed\","
                 "\"reason\":\"%s\",\"bytes\":0}",
                 result->err_msg);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj,   "ok",        result->ok);
    cJSON_AddNumberToObject(obj, "status",    result->http_status);
    cJSON_AddStringToObject(obj, "body",      body_str ? body_str : "");
    cJSON_AddBoolToObject(obj,   "truncated", result->truncated);
    cJSON_AddNumberToObject(obj, "bytes",     result->bytes);

    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (s) {
        snprintf(out, out_size, "%s", s);
        free(s);
    } else {
        snprintf(out, out_size,
                 "{\"ok\":%s,\"status\":%d,\"body\":\"\","
                 "\"truncated\":false,\"bytes\":0}",
                 result->ok ? "true" : "false", result->http_status);
    }
}
