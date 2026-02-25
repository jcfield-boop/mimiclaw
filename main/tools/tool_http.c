#include "tool_http.h"
#include "gateway/ws_server.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_http";

#define HTTP_RESP_BUF_SIZE (4 * 1024)

typedef struct {
    char *data;
    int   len;
    int   cap;
} http_resp_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (r->len + copy > r->cap - 1) copy = r->cap - 1 - r->len;
        if (copy > 0) {
            memcpy(r->data + r->len, evt->data, copy);
            r->len += copy;
            r->data[r->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t tool_http_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_url     = cJSON_GetObjectItem(input, "url");
    cJSON *j_method  = cJSON_GetObjectItem(input, "method");
    cJSON *j_headers = cJSON_GetObjectItem(input, "headers");
    cJSON *j_body    = cJSON_GetObjectItem(input, "body");

    if (!j_url || !cJSON_IsString(j_url)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'url' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *url    = j_url->valuestring;
    const char *method = (j_method && cJSON_IsString(j_method)) ? j_method->valuestring : "GET";
    const char *body   = (j_body   && cJSON_IsString(j_body))   ? j_body->valuestring   : NULL;
    bool is_post = (strcasecmp(method, "POST") == 0);

    http_resp_t resp = {0};
    resp.data = malloc(HTTP_RESP_BUF_SIZE);
    if (!resp.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';
    resp.cap = HTTP_RESP_BUF_SIZE;

    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms       = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler    = http_event_cb,
        .user_data        = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp.data);
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    /* Apply custom headers */
    if (j_headers && cJSON_IsObject(j_headers)) {
        cJSON *h;
        cJSON_ArrayForEach(h, j_headers) {
            if (cJSON_IsString(h)) {
                esp_http_client_set_header(client, h->string, h->valuestring);
            }
        }
    }

    /* Set POST body */
    if (is_post && body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(input);

    if (err != ESP_OK) {
        free(resp.data);
        snprintf(output, output_size, "Error: HTTP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%s %s → HTTP %d (%d bytes)", method, url, status, resp.len);

    char vlog[128];
    snprintf(vlog, sizeof(vlog), "http_request %s → %d", url, status);
    ws_server_broadcast_monitor_verbose("tool", vlog);

    snprintf(output, output_size, "HTTP %d\n%.*s",
             status, (int)(output_size - 10), resp.data);
    free(resp.data);

    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}
