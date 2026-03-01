#include "tool_ha.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "gateway/ws_server.h"

static const char *TAG = "tool_ha";

/* Max bytes of HA response body returned to the model */
#define HA_BODY_MAX     2048

/* ── SERVICES.md parser ──────────────────────────────────────── */

typedef struct {
    char url[128];    /* e.g. "http://192.168.0.50:8123" */
    char token[256];  /* Long-Lived Access Token */
} ha_creds_t;

static bool parse_ha_creds(ha_creds_t *c)
{
    memset(c, 0, sizeof(*c));

    FILE *f = fopen("/spiffs/config/SERVICES.md", "r");
    if (!f) return false;

    char line[320];
    bool in_section = false;

    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '))
            line[--len] = '\0';

        if (strcmp(line, "## Home Assistant") == 0) { in_section = true;  continue; }
        if (in_section && len > 1 && line[0] == '#') break;
        if (!in_section || len == 0 || line[0] == '#') continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        /* ha_token contains colons — only split on the first one */
        *colon = '\0';
        const char *key = line;
        const char *val = colon + 1;
        while (*val == ' ') val++;

        if      (strcmp(key, "ha_url")   == 0) strncpy(c->url,   val, sizeof(c->url)   - 1);
        else if (strcmp(key, "ha_token") == 0) strncpy(c->token, val, sizeof(c->token) - 1);
    }

    fclose(f);
    return c->url[0] != '\0' && c->token[0] != '\0';
}

/* ── Endpoint blocking ───────────────────────────────────────── */

/* Returns true if the endpoint must be blocked.
 * - /api/config*          blocked (exposes HA secrets)
 * - /api/services/hassio* blocked (supervisor / add-on access)
 * - /api/states exactly   blocked (bulk dump — heap risk)
 * - /api/states/...       ALLOWED (specific entity endpoint)
 */
static bool endpoint_blocked(const char *ep)
{
    static const char * const BLOCKED_PREFIXES[] = {
        "/api/config",
        "/api/services/hassio",
        NULL
    };
    for (int i = 0; BLOCKED_PREFIXES[i]; i++) {
        if (strncmp(ep, BLOCKED_PREFIXES[i],
                    strlen(BLOCKED_PREFIXES[i])) == 0) {
            return true;
        }
    }
    /* Exact /api/states blocked; /api/states/ or longer is fine */
    if (strcmp(ep, "/api/states") == 0) return true;

    return false;
}

/* ── HTTP response accumulator ───────────────────────────────── */

typedef struct {
    char *buf;
    int   len;
    int   cap;
    bool  truncated;
} ha_resp_t;

static esp_err_t ha_http_event_cb(esp_http_client_event_t *evt)
{
    ha_resp_t *r = (ha_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r->buf) {
        int copy = evt->data_len;
        if (r->len + copy > r->cap - 1) {
            copy = r->cap - 1 - r->len;
            r->truncated = true;
        }
        if (copy > 0) {
            memcpy(r->buf + r->len, evt->data, copy);
            r->len += copy;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Helper: write structured JSON error into output ─────────── */

static void ha_err(char *out, size_t out_size, int status,
                   const char *error, const char *reason)
{
    snprintf(out, out_size,
             "{\"ok\":false,\"status\":%d,\"error\":\"%s\","
             "\"reason\":\"%s\",\"bytes\":0}",
             status, error, reason);
}

/* ── Tool entry point ────────────────────────────────────────── */

esp_err_t tool_ha_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        ha_err(output, output_size, 0, "parse_error", "Invalid JSON input");
        return ESP_OK;
    }

    const char *method   = "GET";
    const char *endpoint = NULL;
    const char *body_str = NULL;

    cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *je = cJSON_GetObjectItemCaseSensitive(root, "endpoint");
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "body");

    if (cJSON_IsString(jm) && jm->valuestring[0]) method   = jm->valuestring;
    if (cJSON_IsString(je))                        endpoint = je->valuestring;
    if (cJSON_IsString(jb) && jb->valuestring[0]) body_str = jb->valuestring;

    if (!endpoint || endpoint[0] == '\0') {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "missing_endpoint", "endpoint is required");
        return ESP_OK;
    }

    /* Endpoint must start with /api/ */
    if (strncmp(endpoint, "/api/", 5) != 0) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "invalid_endpoint",
               "endpoint must start with /api/");
        return ESP_OK;
    }

    /* Blocking check */
    if (endpoint_blocked(endpoint)) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "blocked_endpoint",
               "endpoint not permitted for security reasons");
        ESP_LOGW(TAG, "Blocked HA endpoint: %s", endpoint);
        return ESP_OK;
    }

    /* Load credentials */
    ha_creds_t creds;
    if (!parse_ha_creds(&creds)) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "no_credentials",
               "SERVICES.md missing ## Home Assistant section with ha_url and ha_token");
        return ESP_OK;
    }

    /* Build full URL */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", creds.url, endpoint);

    /* Build Authorization header value */
    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", creds.token);

    /* Zero token immediately — it's now in auth_header */
    memset(creds.token, 0, sizeof(creds.token));

    /* Allocate response buffer — freed after JSON output is built */
    char *resp_buf = malloc(HA_BODY_MAX + 1);
    if (!resp_buf) {
        cJSON_Delete(root);
        memset(auth_header, 0, sizeof(auth_header));
        ha_err(output, output_size, 0, "no_mem", "ESP_ERR_NO_MEM");
        return ESP_OK;
    }
    resp_buf[0] = '\0';

    ha_resp_t ha_resp = {
        .buf       = resp_buf,
        .len       = 0,
        .cap       = HA_BODY_MAX,
        .truncated = false,
    };

    /* Detect TLS from URL scheme */
    bool use_tls = (strncmp(url, "https://", 8) == 0);

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = ha_http_event_cb,
        .user_data     = &ha_resp,
        .timeout_ms    = 8000,
    };
    if (use_tls) {
        cfg.crt_bundle_attach           = esp_crt_bundle_attach;
        /* TODO: pin cert — add ha_cert_fingerprint: AA:BB:... to SERVICES.md
         * and validate against esp_http_client_config_t.cert_pem */
        cfg.skip_cert_common_name_check = true;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp_buf);
        cJSON_Delete(root);
        memset(auth_header, 0, sizeof(auth_header));
        ha_err(output, output_size, 0, "client_init_failed",
               "esp_http_client_init returned NULL");
        return ESP_OK;
    }

    /* Set method and body */
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (body_str && body_str[0]) {
            esp_http_client_set_post_field(client, body_str, (int)strlen(body_str));
        }
    }

    /* Set headers */
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type",  "application/json");

    /* Execute */
    esp_err_t err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    memset(auth_header, 0, sizeof(auth_header));

    if (err != ESP_OK) {
        free(resp_buf);
        snprintf(output, output_size,
                 "{\"ok\":false,\"status\":0,\"error\":\"connection_failed\","
                 "\"reason\":\"%s\",\"bytes\":0}",
                 esp_err_to_name(err));
        ESP_LOGW(TAG, "HA request failed: %s %s → %s",
                 method, endpoint, esp_err_to_name(err));
        return ESP_OK;
    }

    bool ok = (http_status >= 200 && http_status < 300);

    /* Build structured JSON response — resp_buf still valid here */
    cJSON *out_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(out_obj,   "ok",        ok);
    cJSON_AddNumberToObject(out_obj, "status",    http_status);
    cJSON_AddStringToObject(out_obj, "body",      ha_resp.buf);
    cJSON_AddBoolToObject(out_obj,   "truncated", ha_resp.truncated);
    cJSON_AddNumberToObject(out_obj, "bytes",     ha_resp.len);

    free(resp_buf);  /* safe now — cJSON copied the string above */

    char *out_str = cJSON_PrintUnformatted(out_obj);
    cJSON_Delete(out_obj);

    if (out_str) {
        snprintf(output, output_size, "%s", out_str);
        free(out_str);
    } else {
        snprintf(output, output_size,
                 "{\"ok\":%s,\"status\":%d,\"body\":\"\","
                 "\"truncated\":false,\"bytes\":0}",
                 ok ? "true" : "false", http_status);
    }

    {
        char mon[72];
        snprintf(mon, sizeof(mon), "%s %s → %d (%d bytes%s)",
                 method, endpoint, http_status, ha_resp.len,
                 ha_resp.truncated ? ", truncated" : "");
        ws_server_broadcast_monitor("ha", mon);
    }
    ESP_LOGI(TAG, "HA %s %s → %d (%d bytes%s)",
             method, endpoint, http_status, ha_resp.len,
             ha_resp.truncated ? " truncated" : "");

    return ESP_OK;
}
