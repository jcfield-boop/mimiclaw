#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "cJSON.h"

static const char *TAG = "ws";

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
    if (ret != ESP_OK) return ret;
    if (ws_pkt.len == 0) return ESP_OK;

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

static esp_err_t console_get_handler(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/console/index.html", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "Console not found. Ensure index.html is in SPIFFS.");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── File name → SPIFFS path mapping ────────────────────────────────────── */

static const char *name_to_path(const char *name)
{
    if (name && strcmp(name, "soul")   == 0) return MIMI_SOUL_FILE;
    if (name && strcmp(name, "user")   == 0) return MIMI_USER_FILE;
    if (name && strcmp(name, "memory") == 0) return MIMI_MEMORY_FILE;
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

/* GET /api/sysinfo -> heap and SPIFFS stats */
static esp_err_t sysinfo_handler(httpd_req_t *req)
{
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"heap_free\":%u,\"heap_min\":%u,\"spiffs_total\":%u,\"spiffs_used\":%u}",
             (unsigned)heap_free, (unsigned)heap_min,
             (unsigned)spiffs_total, (unsigned)spiffs_used);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = MIMI_WS_PORT;
    config.ctrl_port         = MIMI_WS_PORT + 1;
    config.max_open_sockets  = 4; /* lwIP max_sockets(8) minus 3 internal = 5 max; use 4 to be safe */
    config.stack_size        = 8192;                     /* SPIFFS I/O needs headroom */
    config.max_uri_handlers  = 14;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* WebSocket on /ws (moved from /) */
    httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
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

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Server stopped");
    }
    return ESP_OK;
}
