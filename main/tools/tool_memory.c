#include "tool_memory.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "memory/memory_store.h"
#include "gateway/ws_server.h"
#include "mimi_config.h"

static const char *TAG = "tool_mem";

/* ── Rate limiting ───────────────────────────────────────────── */
#define MEMORY_WRITE_COOLDOWN_US  (60LL * 1000000LL)   /* 60 s */
#define CONTENT_MAX_BYTES          500
#define TODAY_MAX_BYTES            300

static int64_t s_last_write_us         = 0;
static bool    s_written_this_turn     = false;

void memory_tool_reset_turn(void)
{
    s_written_this_turn = false;
}

/* ── Helpers ─────────────────────────────────────────────────── */

static bool is_whitespace_only(const char *s)
{
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

/* Emit a structured JSON error into the output buffer. */
static esp_err_t tool_err(char *out, size_t out_size, const char *error, const char *reason)
{
    snprintf(out, out_size,
             "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}",
             error, reason);
    return ESP_OK;   /* return OK so the LLM gets the message rather than a framework error */
}

static esp_err_t check_rate_limit(char *out, size_t out_size)
{
    int64_t now_us = esp_timer_get_time();

    if (s_written_this_turn) {
        return tool_err(out, out_size, "rate_limited",
                        "Memory already written this conversation turn.");
    }
    if (s_last_write_us > 0 &&
        (now_us - s_last_write_us) < MEMORY_WRITE_COOLDOWN_US) {
        int remaining_s = (int)((MEMORY_WRITE_COOLDOWN_US -
                                 (now_us - s_last_write_us)) / 1000000LL);
        char reason[64];
        snprintf(reason, sizeof(reason),
                 "Cooldown active — %d s remaining.", remaining_s);
        return tool_err(out, out_size, "rate_limited", reason);
    }
    return ESP_FAIL;   /* sentinel: no rate limit hit, proceed */
}

static void mark_written(void)
{
    s_last_write_us     = esp_timer_get_time();
    s_written_this_turn = true;
}

/* ── Tool: memory_write ──────────────────────────────────────── */

esp_err_t tool_memory_write_execute(const char *input_json,
                                     char *output, size_t output_size)
{
    /* Rate limit check */
    esp_err_t rl = check_rate_limit(output, output_size);
    if (rl == ESP_OK) {
        /* rate limit hit — output already set */
        ws_server_broadcast_monitor("memory", "write rate-limited");
        return ESP_OK;
    }

    /* Parse input */
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        return tool_err(output, output_size,
                        "parse_error", "Invalid JSON input.");
    }

    const char *memory_type = NULL;
    const char *content     = NULL;
    double      confidence  = 0.0;

    cJSON *jtype = cJSON_GetObjectItemCaseSensitive(root, "memory_type");
    cJSON *jcont = cJSON_GetObjectItemCaseSensitive(root, "content");
    cJSON *jconf = cJSON_GetObjectItemCaseSensitive(root, "confidence");

    if (cJSON_IsString(jtype)) memory_type = jtype->valuestring;
    if (cJSON_IsString(jcont)) content     = jcont->valuestring;
    if (cJSON_IsNumber(jconf)) confidence  = jconf->valuedouble;

    /* Validate memory_type */
    if (!memory_type ||
        (strcmp(memory_type, "preference")   != 0 &&
         strcmp(memory_type, "fact")         != 0 &&
         strcmp(memory_type, "instruction")  != 0)) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "invalid_type",
                        "memory_type must be preference, fact, or instruction.");
    }

    /* Validate content */
    if (!content || content[0] == '\0' || is_whitespace_only(content)) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "empty_content", "content must not be empty.");
    }
    if (strlen(content) > CONTENT_MAX_BYTES) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "content_too_long", "content exceeds 500 character limit.");
    }

    /* Hard confidence gate */
    if (confidence < 0.5) {
        cJSON_Delete(root);
        char reason[64];
        snprintf(reason, sizeof(reason),
                 "confidence %.2f below minimum 0.5", confidence);
        return tool_err(output, output_size, "low_confidence", reason);
    }

    /* Read current MEMORY.md, append new entry, write back.
     * Buffer is MIMI_MEMORY_MAX_BYTES + 600 headroom for the new entry. */
    #define MEM_BUF_SIZE (MIMI_MEMORY_MAX_BYTES + 600)
    char *mem_buf = malloc(MEM_BUF_SIZE);
    if (!mem_buf) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "write_failed", "ESP_ERR_NO_MEM");
    }

    memory_read_long_term(mem_buf, MEM_BUF_SIZE);

    /* Append the new entry as a bullet point */
    size_t cur_len = strlen(mem_buf);
    char entry[600];
    snprintf(entry, sizeof(entry),
             "\n- [%s] %s", memory_type, content);
    size_t entry_len = strlen(entry);

    if (cur_len + entry_len < MEM_BUF_SIZE - 1) {
        memcpy(mem_buf + cur_len, entry, entry_len + 1);
    }

    esp_err_t err = memory_write_long_term(mem_buf);
    free(mem_buf);
    cJSON_Delete(root);
    #undef MEM_BUF_SIZE

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "memory_write failed: %s", esp_err_to_name(err));
        char reason[48];
        snprintf(reason, sizeof(reason), "%s", esp_err_to_name(err));
        return tool_err(output, output_size, "write_failed", reason);
    }

    mark_written();

    snprintf(output, output_size,
             "{\"ok\":true,\"memory_type\":\"%s\"}", memory_type);

    ESP_LOGI(TAG, "memory_write: type=%s conf=%.2f", memory_type, confidence);
    ws_server_broadcast_monitor("memory", "MEMORY.md updated via tool");

    return ESP_OK;
}

/* ── Tool: memory_append_today ───────────────────────────────── */

esp_err_t tool_memory_append_today_execute(const char *input_json,
                                            char *output, size_t output_size)
{
    /* Rate limit check */
    esp_err_t rl = check_rate_limit(output, output_size);
    if (rl == ESP_OK) {
        ws_server_broadcast_monitor("memory", "append_today rate-limited");
        return ESP_OK;
    }

    /* Parse input */
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        return tool_err(output, output_size,
                        "parse_error", "Invalid JSON input.");
    }

    const char *content = NULL;
    cJSON *jcont = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (cJSON_IsString(jcont)) content = jcont->valuestring;

    if (!content || content[0] == '\0' || is_whitespace_only(content)) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "empty_content", "content must not be empty.");
    }
    if (strlen(content) > TODAY_MAX_BYTES) {
        cJSON_Delete(root);
        return tool_err(output, output_size,
                        "content_too_long", "content exceeds 300 character limit.");
    }

    esp_err_t err = memory_append_today(content);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "memory_append_today failed: %s", esp_err_to_name(err));
        char reason[48];
        snprintf(reason, sizeof(reason), "%s", esp_err_to_name(err));
        return tool_err(output, output_size, "write_failed", reason);
    }

    mark_written();

    snprintf(output, output_size, "{\"ok\":true,\"file\":\"today\"}");

    ESP_LOGI(TAG, "memory_append_today: appended %d bytes", (int)strlen(content));
    ws_server_broadcast_monitor("memory", "today's note appended via tool");

    return ESP_OK;
}
