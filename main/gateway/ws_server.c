#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "tools/tool_web_search.h"
#include "heartbeat/heartbeat.h"
#include "ota/ota_manager.h"
#include "cron/cron_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ws";

#define WS_NVS_NAMESPACE "ws_config"
#define WS_NVS_KEY_VERBOSE "verbose_logs"
static bool s_verbose_logs = false;

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[MIMI_WS_MAX_CLIENTS];

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    /* Reject duplicate fds (browser reconnect before server cleans up) */
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGW(TAG, "Duplicate fd=%d, replacing client slot", fd);
            return &s_clients[i];
        }
    }
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

/* ── WebSocket handler (moved to /ws) ────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        /* Connection closed or error — free the slot immediately */
        remove_client(httpd_req_to_sockfd(req));
        return ret;
    }
    if (ws_pkt.len == 0) {
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            remove_client(httpd_req_to_sockfd(req));
        }
        return ESP_OK;
    }

    /* H1: Reject oversized frames before allocating — prevents SRAM exhaustion */
    if (ws_pkt.len > 8 * 1024) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes), rejecting fd=%d",
                 (unsigned)ws_pkt.len, httpd_req_to_sockfd(req));
        return ESP_ERR_INVALID_ARG;
    }

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);

    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0
        && content && cJSON_IsString(content)) {

        const char *chat_id = client ? client->chat_id : "ws_unknown";
        cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid)) {
            chat_id = cid->valuestring;
            if (client) {
                strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
            }
        }

        ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── HTTP console handler (GET /) ─────────────────────────────────────────── */

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t console_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

/* ── File name → SPIFFS path mapping ────────────────────────────────────── */

static const char *name_to_path(const char *name)
{
    if (name && strcmp(name, "soul")      == 0) return MIMI_SOUL_FILE;
    if (name && strcmp(name, "user")      == 0) return MIMI_USER_FILE;
    if (name && strcmp(name, "memory")   == 0) return MIMI_MEMORY_FILE;
    if (name && strcmp(name, "heartbeat") == 0) return MIMI_HEARTBEAT_FILE;
    if (name && strcmp(name, "services")  == 0) return "/spiffs/config/SERVICES.md";
    return NULL;
}

/* ── GET /api/file?name=soul|user|memory ─────────────────────────────────── */

static esp_err_t file_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char name[16]  = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }

    const char *path = name_to_path(name);
    if (!path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown file name");
        return ESP_OK;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Return empty string if file not yet created */
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /api/file?name=soul|user|memory ────────────────────────────────── */

static esp_err_t file_post_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char name[16]  = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }

    const char *path = name_to_path(name);
    if (!path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown file name");
        return ESP_OK;
    }

    size_t max_body = 8 * 1024;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;

    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, body_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body received");
        return ESP_OK;
    }
    body[received] = '\0';

    /* L2: check SPIFFS free space before writing */
    {
        size_t spiffs_total = 0, spiffs_used = 0;
        esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);
        if (spiffs_total > 0 && spiffs_used + (size_t)received > spiffs_total) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Insufficient SPIFFS space");
            return ESP_OK;
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    fputs(body, f);
    fclose(f);
    free(body);

    ESP_LOGI(TAG, "Saved %s (%d bytes)", path, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /api/reboot ────────────────────────────────────────────────────── */

static void reboot_timer_cb(void *arg)
{
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    esp_timer_handle_t t;
    esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name     = "reboot",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 500 * 1000); /* 500 ms */
    }
    return ESP_OK;
}

/* ── POST /api/heartbeat ─────────────────────────────────────────────────── */

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    bool triggered = heartbeat_trigger();
    httpd_resp_sendstr(req, triggered ? "{\"ok\":true,\"triggered\":true}"
                                      : "{\"ok\":true,\"triggered\":false}");
    return ESP_OK;
}

/* ── Skill helpers ───────────────────────────────────────────────────────── */

static bool skill_name_valid(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 48) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static void skill_name_to_path(const char *name, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s%s.md", MIMI_SKILLS_PREFIX, name);
}

/* GET /api/skills -> {"skills":["weather","daily-briefing",...]} */
static esp_err_t skills_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    cJSON *arr = cJSON_CreateArray();

    if (dir) {
        struct dirent *ent;
        const char *prefix = "skills/";
        const size_t prefix_len = 7;
        while ((ent = readdir(dir)) != NULL) {
            const char *fname = ent->d_name;
            if (strncmp(fname, prefix, prefix_len) != 0) continue;
            size_t len = strlen(fname);
            if (len < prefix_len + 4) continue;
            if (strcmp(fname + len - 3, ".md") != 0) continue;
            size_t nlen = len - prefix_len - 3;
            char skill_name[64];
            if (nlen >= sizeof(skill_name)) nlen = sizeof(skill_name) - 1;
            memcpy(skill_name, fname + prefix_len, nlen);
            skill_name[nlen] = '\0';
            cJSON_AddItemToArray(arr, cJSON_CreateString(skill_name));
        }
        closedir(dir);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return ESP_OK;
}

/* GET /api/skill?name=<name> -> plain text */
static esp_err_t skill_get_handler(httpd_req_t *req)
{
    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    char path[80];
    skill_name_to_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/skill?name=<name> -> save skill */
static esp_err_t skill_post_handler(httpd_req_t *req)
{
    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    size_t max_body = 8 * 1024;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;
    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    int received = httpd_req_recv(req, body, body_len);
    if (received <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_OK; }
    body[received] = '\0';

    char path[80];
    skill_name_to_path(name, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed"); return ESP_OK; }
    fputs(body, f);
    fclose(f);
    free(body);

    ESP_LOGI(TAG, "Saved skill: %s (%d bytes)", path, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* DELETE /api/skill?name=<name> -> delete skill file */
static esp_err_t skill_delete_handler(httpd_req_t *req)
{
    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    char path[80];
    skill_name_to_path(name, path, sizeof(path));
    int ret = unlink(path);
    ESP_LOGI(TAG, "Delete skill %s: %s", path, ret == 0 ? "ok" : "failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not found\"}");
    return ESP_OK;
}

/* Map esp_reset_reason_t to a short human-readable string */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "ext_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

/* GET /api/sysinfo -> heap, SPIFFS stats, token usage, uptime, reset reason */
static esp_err_t sysinfo_handler(httpd_req_t *req)
{
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);

    uint32_t tok_in = 0, tok_out = 0, llm_cost_mc = 0;
    llm_get_session_stats(&tok_in, &tok_out, &llm_cost_mc);

    uint32_t search_calls = 0, search_cost_mc = 0;
    tool_web_search_get_stats(&search_calls, &search_cost_mc);

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const char *reset_str = reset_reason_str(esp_reset_reason());

    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"heap_free\":%u,\"heap_min\":%u,\"spiffs_total\":%u,\"spiffs_used\":%u"
             ",\"tokens_in\":%u,\"tokens_out\":%u,\"cost_millicents\":%u"
             ",\"search_calls\":%u,\"search_cost_millicents\":%u"
             ",\"uptime_s\":%lu,\"reset_reason\":\"%s\"}",
             (unsigned)heap_free, (unsigned)heap_min,
             (unsigned)spiffs_total, (unsigned)spiffs_used,
             (unsigned)tok_in, (unsigned)tok_out, (unsigned)llm_cost_mc,
             (unsigned)search_calls, (unsigned)search_cost_mc,
             uptime_s, reset_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /api/config -> current config (masked) ──────────────────────────── */

static void mask_key(const char *key, char *out, size_t out_size)
{
    size_t len = strlen(key);
    if (len == 0) {
        out[0] = '\0';
    } else if (len <= 4) {
        snprintf(out, out_size, "****");
    } else {
        snprintf(out, out_size, "****%s", key + len - 4);
    }
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char masked_api[16];
    char masked_search[16];
    const char *ak = llm_get_api_key();
    mask_key(ak ? ak : "", masked_api, sizeof(masked_api));

    const char *sk = tool_web_search_get_key();
    mask_key(sk ? sk : "", masked_search, sizeof(masked_search));

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "provider",   llm_get_provider());
    cJSON_AddStringToObject(j, "model",      llm_get_model());
    cJSON_AddStringToObject(j, "api_key",    masked_api);
    cJSON_AddStringToObject(j, "search_key", masked_search);
    cJSON_AddBoolToObject(j, "verbose_logs", s_verbose_logs);

    char *json_str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json_str ? json_str : "{}");
    free(json_str);
    return ESP_OK;
}

/* ── POST /api/config -> update config fields ────────────────────────────── */

static esp_err_t config_post_handler(httpd_req_t *req)
{
    size_t max_body = 2048;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;

    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, body_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body received");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *provider   = cJSON_GetObjectItem(root, "provider");
    cJSON *model      = cJSON_GetObjectItem(root, "model");
    cJSON *api_key    = cJSON_GetObjectItem(root, "api_key");
    cJSON *search_key = cJSON_GetObjectItem(root, "search_key");

    /* M4: cap all string fields to 128 chars before writing to NVS */
#define CONFIG_FIELD_MAX 128
    if (provider && cJSON_IsString(provider) && provider->valuestring[0]
        && strnlen(provider->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_provider(provider->valuestring);
    }
    if (model && cJSON_IsString(model) && model->valuestring[0]
        && strnlen(model->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_model(model->valuestring);
    }
    if (api_key && cJSON_IsString(api_key) && api_key->valuestring[0]
        && strncmp(api_key->valuestring, "****", 4) != 0
        && strnlen(api_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_api_key(api_key->valuestring);
    }
    if (search_key && cJSON_IsString(search_key) && search_key->valuestring[0]
        && strncmp(search_key->valuestring, "****", 4) != 0
        && strnlen(search_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        tool_web_search_set_key(search_key->valuestring);
    }
#undef CONFIG_FIELD_MAX

    cJSON *verbose = cJSON_GetObjectItem(root, "verbose_logs");
    if (verbose != NULL) {
        s_verbose_logs = cJSON_IsTrue(verbose);
        nvs_handle_t nvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, WS_NVS_KEY_VERBOSE, s_verbose_logs ? 1 : 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Verbose logs set to: %s", s_verbose_logs ? "on" : "off");
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config updated via web");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /api/crons -> list all cron jobs ───────────────────────────────── */

static esp_err_t crons_get_handler(httpd_req_t *req)
{
    const cron_job_t *jobs;
    int count;
    cron_list_jobs(&jobs, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "now_epoch", (double)time(NULL));
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        const cron_job_t *j = &jobs[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id",      j->id);
        cJSON_AddStringToObject(item, "name",    j->name);
        cJSON_AddBoolToObject(item,   "enabled", j->enabled);
        cJSON_AddStringToObject(item, "kind",    j->kind == CRON_KIND_EVERY ? "every" : "at");
        if (j->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", j->interval_s);
        } else {
            cJSON_AddNumberToObject(item, "at_epoch",   (double)j->at_epoch);
        }
        cJSON_AddStringToObject(item, "message", j->message);
        cJSON_AddStringToObject(item, "channel", j->channel);
        cJSON_AddNumberToObject(item, "next_run", (double)j->next_run);
        cJSON_AddNumberToObject(item, "last_run", (double)j->last_run);
        cJSON_AddBoolToObject(item,   "delete_after_run", j->delete_after_run);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json_str ? json_str : "{\"jobs\":[]}");
    free(json_str);
    return ESP_OK;
}

/* ── DELETE /api/cron?id=<id> -> remove a cron job ──────────────────────── */

static esp_err_t cron_delete_handler(httpd_req_t *req)
{
    char query[32] = {0}, id[12] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = cron_remove_job(id);
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}"
                                          : "{\"ok\":false,\"error\":\"not_found\"}");
    return ESP_OK;
}

/* ── OTA handler ────────────────────────────────────────────────────────── */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char body[300] = {0};
    int  rcv = httpd_req_recv(req, body, sizeof(body) - 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (rcv <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no body\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *jurl = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(jurl) || !jurl->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing url\"}");
        return ESP_OK;
    }

    esp_err_t err = ota_start_async(jurl->valuestring);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"updating\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_already_running\"}");
    } else if (err == ESP_ERR_NO_MEM) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"low_heap\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"task_failed\"}");
    }
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    {
        nvs_handle_t nvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(nvs, WS_NVS_KEY_VERBOSE, &v) == ESP_OK) s_verbose_logs = (v != 0);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Verbose logs: %s", s_verbose_logs ? "on" : "off");
    }

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = MIMI_WS_PORT;
    config.ctrl_port         = MIMI_WS_PORT + 1;
    config.max_open_sockets  = 4; /* lwIP max_sockets(8) minus 3 internal = 5 max; use 4 to be safe */
    config.stack_size        = 8192;                     /* SPIFFS I/O needs headroom */
    config.max_uri_handlers  = 17;
    /* Give WS clients more room to tolerate gaps when the device is mid-LLM
     * or mid-tool HTTP call and the lwIP stack is temporarily saturated.
     * Default is 5 s; 30 s covers even the slowest LLM responses. */
    config.send_wait_timeout = 30;
    config.recv_wait_timeout = 30;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* WebSocket on /ws (moved from /) */
    httpd_uri_t ws_uri = {
        .uri                    = "/ws",
        .method                 = HTTP_GET,
        .handler                = ws_handler,
        .is_websocket           = true,
        .handle_ws_control_frames = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* Web console HTML page */
    httpd_uri_t console_uri = {
        .uri    = "/",
        .method = HTTP_GET,
        .handler = console_get_handler,
    };
    httpd_register_uri_handler(s_server, &console_uri);

    /* File read */
    httpd_uri_t file_get_uri = {
        .uri    = "/api/file",
        .method = HTTP_GET,
        .handler = file_get_handler,
    };
    httpd_register_uri_handler(s_server, &file_get_uri);

    /* File write */
    httpd_uri_t file_post_uri = {
        .uri    = "/api/file",
        .method = HTTP_POST,
        .handler = file_post_handler,
    };
    httpd_register_uri_handler(s_server, &file_post_uri);

    /* Reboot */
    httpd_uri_t reboot_uri = {
        .uri    = "/api/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };
    httpd_register_uri_handler(s_server, &reboot_uri);

    /* Heartbeat trigger */
    httpd_uri_t heartbeat_uri = {
        .uri    = "/api/heartbeat",
        .method = HTTP_POST,
        .handler = heartbeat_handler,
    };
    httpd_register_uri_handler(s_server, &heartbeat_uri);

    /* Skills list */
    httpd_uri_t skills_list_uri = {
        .uri    = "/api/skills",
        .method = HTTP_GET,
        .handler = skills_list_handler,
    };
    httpd_register_uri_handler(s_server, &skills_list_uri);

    /* Skill get */
    httpd_uri_t skill_get_uri = {
        .uri    = "/api/skill",
        .method = HTTP_GET,
        .handler = skill_get_handler,
    };
    httpd_register_uri_handler(s_server, &skill_get_uri);

    /* Skill save */
    httpd_uri_t skill_post_uri = {
        .uri    = "/api/skill",
        .method = HTTP_POST,
        .handler = skill_post_handler,
    };
    httpd_register_uri_handler(s_server, &skill_post_uri);

    /* Skill delete */
    httpd_uri_t skill_del_uri = {
        .uri    = "/api/skill",
        .method = HTTP_DELETE,
        .handler = skill_delete_handler,
    };
    httpd_register_uri_handler(s_server, &skill_del_uri);

    /* System info */
    httpd_uri_t sysinfo_uri = {
        .uri    = "/api/sysinfo",
        .method = HTTP_GET,
        .handler = sysinfo_handler,
    };
    httpd_register_uri_handler(s_server, &sysinfo_uri);

    /* Config read */
    httpd_uri_t config_get_uri = {
        .uri    = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(s_server, &config_get_uri);

    /* Config write */
    httpd_uri_t config_post_uri = {
        .uri    = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    httpd_register_uri_handler(s_server, &config_post_uri);

    /* Cron list */
    httpd_uri_t crons_get_uri = {
        .uri    = "/api/crons",
        .method = HTTP_GET,
        .handler = crons_get_handler,
    };
    httpd_register_uri_handler(s_server, &crons_get_uri);

    /* Cron delete */
    httpd_uri_t cron_del_uri = {
        .uri    = "/api/cron",
        .method = HTTP_DELETE,
        .handler = cron_delete_handler,
    };
    httpd_register_uri_handler(s_server, &cron_del_uri);

    /* OTA firmware update */
    httpd_uri_t ota_uri = {
        .uri    = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    httpd_register_uri_handler(s_server, &ota_uri);

    ESP_LOGI(TAG, "Server started on port %d (WS: /ws, Console: /)", MIMI_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client) {
        ESP_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, client->fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Send to %s failed: %s", chat_id, esp_err_to_name(ret));
        remove_client(client->fd);
    }
    return ret;
}

esp_err_t ws_server_broadcast_monitor(const char *event, const char *msg_text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",  "monitor");
    cJSON_AddStringToObject(j, "event", event    ? event    : "");
    cJSON_AddStringToObject(j, "msg",   msg_text ? msg_text : "");

    char *json_str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };

    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active) {
            esp_err_t ret = httpd_ws_send_frame_async(s_server, s_clients[i].fd, &pkt);
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "Monitor send to fd=%d failed, removing", s_clients[i].fd);
                s_clients[i].active = false;
            }
        }
    }

    free(json_str);
    return ESP_OK;
}

bool ws_server_get_verbose_logs(void) { return s_verbose_logs; }

esp_err_t ws_server_broadcast_monitor_verbose(const char *event, const char *msg)
{
    if (!s_verbose_logs) return ESP_OK;
    return ws_server_broadcast_monitor(event, msg);
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Server stopped");
    }
    return ESP_OK;
}
