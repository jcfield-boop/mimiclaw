#include "tool_klipper.h"
#include "lan_request.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "gateway/ws_server.h"

static const char *TAG = "tool_klipper";

/* Max bytes of Moonraker response body returned to the model */
#define KLIPPER_BODY_MAX    2048

/* ── SERVICES.md parser ──────────────────────────────────────── */

typedef struct {
    char url[128];     /* e.g. "http://192.168.0.100:7125" */
    char apikey[128];  /* optional — empty if no auth configured */
} klipper_creds_t;

static bool parse_klipper_creds(klipper_creds_t *c)
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

        if (strcmp(line, "## Klipper / Moonraker") == 0) { in_section = true; continue; }
        if (in_section && len > 1 && line[0] == '#') break;
        if (!in_section || len == 0 || line[0] == '#') continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *key = line;
        const char *val = colon + 1;
        while (*val == ' ') val++;

        if      (strcmp(key, "moonraker_url")    == 0) strncpy(c->url,    val, sizeof(c->url)    - 1);
        else if (strcmp(key, "moonraker_apikey") == 0) strncpy(c->apikey, val, sizeof(c->apikey) - 1);
    }

    fclose(f);
    return c->url[0] != '\0';  /* URL required; apikey optional */
}

/* ── Endpoint blocking ───────────────────────────────────────── */

/* Block dangerous Moonraker machine/system endpoints.
 * /machine/... can reboot or shut down the host Raspberry Pi.
 * /server/files/delete is blocked to prevent accidental gcode loss.
 * Everything else (printer status, temps, print control) is allowed.
 */
static bool endpoint_blocked(const char *ep)
{
    static const char * const BLOCKED_PREFIXES[] = {
        "/machine",          /* system power / update manager */
        "/server/files/delete",
        NULL
    };
    for (int i = 0; BLOCKED_PREFIXES[i]; i++) {
        if (strncmp(ep, BLOCKED_PREFIXES[i],
                    strlen(BLOCKED_PREFIXES[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Helper: write structured JSON error ─────────────────────── */

static void klipper_err(char *out, size_t out_size, int status,
                        const char *error, const char *reason)
{
    snprintf(out, out_size,
             "{\"ok\":false,\"status\":%d,\"error\":\"%s\","
             "\"reason\":\"%s\",\"bytes\":0}",
             status, error, reason);
}

/* ── Tool entry point ────────────────────────────────────────── */

esp_err_t tool_klipper_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        klipper_err(output, output_size, 0, "parse_error", "Invalid JSON input");
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
        klipper_err(output, output_size, 0, "missing_endpoint", "endpoint is required");
        return ESP_OK;
    }

    if (endpoint[0] != '/') {
        cJSON_Delete(root);
        klipper_err(output, output_size, 0, "invalid_endpoint",
                    "endpoint must start with /");
        return ESP_OK;
    }

    if (endpoint_blocked(endpoint)) {
        cJSON_Delete(root);
        klipper_err(output, output_size, 0, "blocked_endpoint",
                    "endpoint not permitted for safety reasons");
        ESP_LOGW(TAG, "Blocked Klipper endpoint: %s", endpoint);
        return ESP_OK;
    }

    klipper_creds_t creds;
    if (!parse_klipper_creds(&creds)) {
        cJSON_Delete(root);
        klipper_err(output, output_size, 0, "no_credentials",
                    "SERVICES.md missing ## Klipper / Moonraker section with moonraker_url");
        return ESP_OK;
    }

    /* API key header — optional (NULL if no key configured) */
    const char *auth_header = creds.apikey[0] ? "X-Api-Key" : NULL;
    const char *auth_value  = creds.apikey[0] ? creds.apikey : NULL;

    char *resp_buf = malloc(KLIPPER_BODY_MAX + 1);
    if (!resp_buf) {
        cJSON_Delete(root);
        klipper_err(output, output_size, 0, "no_mem", "ESP_ERR_NO_MEM");
        return ESP_OK;
    }

    lan_result_t result;
    lan_request(method, creds.url, endpoint,
                auth_header, auth_value,
                body_str,
                resp_buf, KLIPPER_BODY_MAX + 1,
                &result);

    cJSON_Delete(root);
    memset(creds.apikey, 0, sizeof(creds.apikey));

    lan_result_to_json(&result, resp_buf, output, output_size);
    free(resp_buf);

    {
        char mon[80];
        snprintf(mon, sizeof(mon), "%s %s → %d (%d bytes%s)",
                 method, endpoint, result.http_status, result.bytes,
                 result.truncated ? ", truncated" : "");
        ws_server_broadcast_monitor("klipper", mon);
    }
    ESP_LOGI(TAG, "Klipper %s %s → %d (%d bytes%s)",
             method, endpoint, result.http_status, result.bytes,
             result.truncated ? " truncated" : "");

    return ESP_OK;
}
