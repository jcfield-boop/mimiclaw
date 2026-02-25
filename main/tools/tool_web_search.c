#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "gateway/ws_server.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static bool is_tavily_key(const char *key) {
    return key && strncmp(key, "tvly-", 5) == 0;
}

static char s_search_key[128] = {0};

/* Cost tracking: Brave Pro = $3/1000 queries = 300 millicents/call */
#define BRAVE_COST_PER_CALL_MILLICENTS 300

static uint32_t s_total_searches        = 0;
static uint32_t s_search_cost_millicents = 0;

#define SEARCH_BUF_SIZE      (20 * 1024)  /* Web search with summary=1 ~12-15KB */
#define SUMMARIZER_BUF_SIZE  (12 * 1024)  /* Summarizer response ~3-8KB */
#define SUMMARIZER_KEY_MAX   256
#define SEARCH_RESULT_COUNT  5
/* Brave Answers plan: $4.00/1000 queries = 400 millicents (web + summarizer = 2 calls) */
#define BRAVE_ANSWERS_COST_MILLICENTS 400

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap - 1) {  /* -1 to always reserve space for null terminator */
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (key configured)");
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

const char *tool_web_search_get_key(void) { return s_search_key; }

void tool_web_search_get_stats(uint32_t *calls, uint32_t *cost_millicents)
{
    if (calls)          *calls          = s_total_searches;
    if (cost_millicents) *cost_millicents = s_search_cost_millicents;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Format web results (fallback when no summary) ────────────── */

static void format_results(cJSON *root, char *output, size_t output_size)
{
    /* Try web results first, fall back to news */
    cJSON *category = cJSON_GetObjectItem(root, "web");
    cJSON *results = category ? cJSON_GetObjectItem(category, "results") : NULL;
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        category = cJSON_GetObjectItem(root, "news");
        results = category ? cJSON_GetObjectItem(category, "results") : NULL;
    }
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }
    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;
        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url   = cJSON_GetObjectItem(item, "url");
        cJSON *desc  = cJSON_GetObjectItem(item, "description");
        off += snprintf(output + off, output_size - off, "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url   && cJSON_IsString(url))   ? url->valuestring   : "",
            (desc  && cJSON_IsString(desc))  ? desc->valuestring  : "");
        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Format Brave Summarizer response ─────────────────────────── */

static void format_summary(cJSON *root, char *output, size_t output_size)
{
    cJSON *summary_arr = cJSON_GetObjectItem(root, "summary");
    if (!summary_arr || !cJSON_IsArray(summary_arr) || cJSON_GetArraySize(summary_arr) == 0) {
        snprintf(output, output_size, "No summary available.");
        return;
    }
    size_t off = 0;
    cJSON *token;
    cJSON_ArrayForEach(token, summary_arr) {
        cJSON *text = cJSON_GetObjectItem(token, "text");
        if (text && cJSON_IsString(text)) {
            size_t tlen = strlen(text->valuestring);
            size_t space = output_size - 1 - off;
            if (tlen > space) tlen = space;
            memcpy(output + off, text->valuestring, tlen);
            off += tlen;
        }
    }
    output[off] = '\0';
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t search_direct(const char *url, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        char emsg[64];
        snprintf(emsg, sizeof(emsg), "Search HTTP %d", status);
        ESP_LOGE(TAG, "%s", emsg);
        ws_server_broadcast_monitor("error", emsg);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t search_via_proxy(const char *path, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.search.brave.com\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip headers */
    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Fetch Brave Summarizer (step 2) ──────────────────────────── */

static esp_err_t get_summary(const char *key, char *output, size_t output_size)
{
    char encoded_key[384];
    url_encode(key, encoded_key, sizeof(encoded_key));

    char path[512];
    snprintf(path, sizeof(path), "/res/v1/summarizer/search?key=%s&entity_info=1", encoded_key);

    search_buf_t sb = {0};
    sb.data = calloc(1, SUMMARIZER_BUF_SIZE);
    if (!sb.data) return ESP_ERR_NO_MEM;
    sb.cap = SUMMARIZER_BUF_SIZE;

    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = search_via_proxy(path, &sb);
    } else {
        char url[560];
        snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
        err = search_direct(url, &sb);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Summarizer HTTP failed: %s", esp_err_to_name(err));
        free(sb.data);
        return err;
    }

    ESP_LOGI(TAG, "Summarizer resp: %d bytes", (int)sb.len);
    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);
    if (!root) return ESP_FAIL;

    format_summary(root, output, output_size);
    cJSON_Delete(root);
    return (output[0] != '\0') ? ESP_OK : ESP_FAIL;
}

/* ── Tavily search ────────────────────────────────────────────── */

static esp_err_t search_tavily(const char *query, const char *api_key, char **out)
{
    char *buf = malloc(MIMI_TAVILY_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "api_key", api_key);
    cJSON_AddStringToObject(body, "query", query);
    cJSON_AddNumberToObject(body, "max_results", 5);
    cJSON_AddBoolToObject(body, "include_answer", true);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) { free(buf); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url = "https://api.tavily.com/search",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    int body_len = (int)strlen(body_str);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, body_len);
    int total = 0;
    if (err == ESP_OK) {
        /* Manually write POST body (set_post_field only works with perform()) */
        int written = esp_http_client_write(client, body_str, body_len);
        if (written < 0) {
            ws_server_broadcast_monitor("error", "Tavily: write failed");
            err = ESP_FAIL;
        } else {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status != 200) {
                char emsg[64];
                snprintf(emsg, sizeof(emsg), "Tavily HTTP %d", status);
                ws_server_broadcast_monitor("error", emsg);
                err = ESP_FAIL;
            } else {
                int rd;
                while ((rd = esp_http_client_read(client, buf + total,
                                                   MIMI_TAVILY_BUF_SIZE - total - 1)) > 0) {
                    total += rd;
                }
                buf[total] = '\0';
            }
        }
    }
    esp_http_client_cleanup(client);
    free(body_str);

    if (err != ESP_OK || total == 0) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_ParseWithLength(buf, total);
    free(buf);
    if (!root) return ESP_FAIL;

    cJSON *answer  = cJSON_GetObjectItem(root, "answer");
    cJSON *results = cJSON_GetObjectItem(root, "results");

    char *output = malloc(4096);
    if (!output) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    int pos = 0;

    if (answer && cJSON_IsString(answer) && strlen(answer->valuestring) > 10) {
        pos += snprintf(output + pos, 4096 - pos, "%s\n\n", answer->valuestring);
    }
    if (results && cJSON_IsArray(results)) {
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n && i < 5 && pos < 3800; i++) {
            cJSON *item  = cJSON_GetArrayItem(results, i);
            cJSON *title = cJSON_GetObjectItem(item, "title");
            cJSON *url   = cJSON_GetObjectItem(item, "url");
            cJSON *snip  = cJSON_GetObjectItem(item, "content");
            if (title && url) {
                pos += snprintf(output + pos, 4096 - pos, "[%d] %s\n%s\n",
                                i + 1,
                                cJSON_IsString(title) ? title->valuestring : "",
                                cJSON_IsString(url)   ? url->valuestring   : "");
            }
            if (snip && cJSON_IsString(snip) && pos < 3600) {
                pos += snprintf(output + pos, 4096 - pos, "%.200s\n\n", snip->valuestring);
            }
        }
    }
    cJSON_Delete(root);

    if (pos == 0) { free(output); return ESP_FAIL; }
    *out = output;
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size,
                 "Search not available: Brave Search API key not configured. "
                 "Set it via Settings tab at http://<device-ip> or `set_search_key <key>` in CLI.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);

    /* Route to Tavily if key prefix matches */
    if (is_tavily_key(s_search_key)) {
        ws_server_broadcast_monitor_verbose("search", "Using Tavily");
        char *result = NULL;
        esp_err_t terr = search_tavily(query->valuestring, s_search_key, &result);
        cJSON_Delete(input);
        if (terr == ESP_OK && result) {
            snprintf(output, output_size, "%s", result);
            free(result);
            s_total_searches++;
        } else {
            free(result);
            snprintf(output, output_size, "Error: Tavily search failed");
        }
        return terr;
    }

    /* Build URL */
    char encoded_query[256];
    url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
    cJSON_Delete(input);

    /* Step 1: web search with summary=1 to get summarizer key */
    char path[384];
    snprintf(path, sizeof(path),
             "/res/v1/web/search?q=%s&count=%d&extra_snippets=false&summary=1",
             encoded_query, SEARCH_RESULT_COUNT);

    /* Allocate response buffer from internal SRAM (ESP32-C6 has no PSRAM) */
    search_buf_t sb = {0};
    sb.data = calloc(1, SEARCH_BUF_SIZE);
    if (!sb.data) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    /* Make HTTP request */
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = search_via_proxy(path, &sb);
    } else {
        char url[512];
        snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
        err = search_direct(url, &sb);
    }

    if (err != ESP_OK) {
        free(sb.data);
        snprintf(output, output_size, "Error: Search request failed");
        return err;
    }

    /* Parse and format results */
    int resp_len = (int)sb.len;
    {
        char vlog[160];
        snprintf(vlog, sizeof(vlog), "Search resp: %d bytes, starts: %.100s", resp_len, sb.data);
        for (char *p = vlog; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        ws_server_broadcast_monitor_verbose("search", vlog);
    }
    char dbg_prefix[80];
    snprintf(dbg_prefix, sizeof(dbg_prefix), "%.70s", sb.data ? sb.data : "");
    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed: %d bytes received, starts: %s", resp_len, dbg_prefix);
        snprintf(output, output_size, "Error: Failed to parse search results (%d bytes)", resp_len);
        return ESP_FAIL;
    }

    /* Step 2: extract summarizer key and fetch summary */
    char summarizer_key[SUMMARIZER_KEY_MAX] = {0};
    cJSON *summarizer = cJSON_GetObjectItem(root, "summarizer");
    if (summarizer) {
        cJSON *sk = cJSON_GetObjectItem(summarizer, "key");
        if (sk && cJSON_IsString(sk)) {
            strncpy(summarizer_key, sk->valuestring, sizeof(summarizer_key) - 1);
        }
    }

    {
        char klog[80];
        snprintf(klog, sizeof(klog), "Summarizer key: %s", summarizer_key[0] ? "found" : "absent");
        ws_server_broadcast_monitor_verbose("search", klog);
    }
    bool used_summary = false;
    if (summarizer_key[0]) {
        /* Free web search tree before allocating summarizer buffer */
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Fetching Brave summary...");
        esp_err_t sum_err = get_summary(summarizer_key, output, output_size);
        if (sum_err == ESP_OK && output[0]) {
            used_summary = true;
        } else {
            ESP_LOGW(TAG, "Summarizer failed (%s)", esp_err_to_name(sum_err));
            snprintf(output, output_size, "Search summary unavailable, please try again.");
        }
    } else {
        /* No summarizer key — fall back to formatted web results */
        ESP_LOGW(TAG, "No summarizer key in response (query may not be eligible)");
        format_results(root, output, output_size);
        cJSON_Delete(root);
    }

    s_total_searches++;
    s_search_cost_millicents += used_summary
        ? BRAVE_ANSWERS_COST_MILLICENTS   /* counts as one "answer" query */
        : BRAVE_COST_PER_CALL_MILLICENTS;
    ESP_LOGI(TAG, "Search done (%s), %d bytes (calls: %u)",
             used_summary ? "summary" : "web", (int)strlen(output), (unsigned)s_total_searches);
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}
