#include "session_mgr.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

/* Max bytes for a single JSONL line. Briefing responses can reach ~4KB;
 * allow 6KB so even large messages aren't silently truncated by fgets. */
#define SESS_LINE_BUF  6144

static void session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s/tg_%s.jsonl", MIMI_SPIFFS_SESSION_DIR, chat_id);
}

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s", MIMI_SPIFFS_SESSION_DIR);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No history yet */
        snprintf(buf, size, "[]");
        return ESP_OK;
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[MIMI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char *line = malloc(SESS_LINE_BUF);
    if (!line) { fclose(f); snprintf(buf, size, "[]"); return ESP_OK; }

    while (fgets(line, SESS_LINE_BUF, f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    free(line);
    fclose(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

cJSON *session_get_history_cjson(const char *chat_id, int max_msgs)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return cJSON_CreateArray();
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *ring[MIMI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char *line = malloc(SESS_LINE_BUF);
    if (!line) { fclose(f); return cJSON_CreateArray(); }

    while (fgets(line, SESS_LINE_BUF, f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        if (count >= max_msgs) {
            cJSON_Delete(ring[write_idx]);
        }
        ring[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    free(line);
    fclose(f);

    /* Build result array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = ring[idx];
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
            cJSON_AddItemToArray(arr, entry);
        }
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        cJSON_Delete(ring[(cleanup_start + i) % max_msgs]);
    }

    return arr;
}

esp_err_t session_trim(const char *chat_id, int max_msgs)
{
    if (max_msgs <= 0) max_msgs = MIMI_SESSION_MAX_MSGS;

    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;  /* nothing to trim */

    /* First pass (512-byte chunks): count '\n' occurrences = complete lines.
     * Track start-of-line file offsets in a ring of size max_msgs+1.
     * After the pass, ring[(ridx+1) % (max_msgs+1)] is the offset of the
     * first line we want to keep. */
    long ring[MIMI_SESSION_MAX_MSGS + 1];
    memset(ring, 0, sizeof(ring));
    int  ridx     = 0;
    int  count    = 0;
    long pos      = 0;
    bool at_start = true;

    char scratch[512];
    size_t n;
    while ((n = fread(scratch, 1, sizeof(scratch), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (at_start) {
                ring[ridx] = pos;
                ridx = (ridx + 1) % (max_msgs + 1);
                at_start = false;
            }
            if (scratch[i] == '\n') {
                count++;
                at_start = true;
            }
            pos++;
        }
    }
    long file_size = pos;
    fclose(f);

    if (count <= max_msgs) return ESP_OK;  /* already within budget */

    /* Offset of the first line to keep */
    long keep_from = ring[(ridx + 1) % (max_msgs + 1)];
    long keep_size = file_size - keep_from;
    if (keep_size <= 0) return ESP_OK;

    /* Read the content we want to retain */
    char *buf = malloc((size_t)keep_size + 1);
    if (!buf) {
        ESP_LOGW(TAG, "session_trim: OOM (need %ld bytes)", keep_size);
        return ESP_ERR_NO_MEM;
    }

    f = fopen(path, "r");
    if (!f) { free(buf); return ESP_FAIL; }
    fseek(f, keep_from, SEEK_SET);
    size_t rd = fread(buf, 1, (size_t)keep_size, f);
    fclose(f);

    /* Rewrite the file with only the kept content */
    remove(path);
    f = fopen(path, "w");
    if (!f) { free(buf); return ESP_FAIL; }
    fwrite(buf, 1, rd, f);
    fclose(f);
    free(buf);

    ESP_LOGI(TAG, "Session %s trimmed: %d → %d lines (%ld bytes kept)",
             chat_id, count, max_msgs, (long)rd);
    return ESP_OK;
}

esp_err_t session_clear(const char *chat_id)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_SESSION_DIR);
    if (!dir) {
        /* SPIFFS is flat, so list all files matching pattern */
        dir = opendir(MIMI_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
