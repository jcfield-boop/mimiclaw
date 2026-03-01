#include "rule_engine.h"
#include "mimi_config.h"
#include "tools/tool_registry.h"
#include "gateway/ws_server.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "rules";

#define RULE_FILE       "/spiffs/rules/rules.json"
#define RULE_EVAL_S     60          /* evaluation interval in seconds */
#define TOOL_OUT_SIZE   1024        /* output buffer for condition tool calls */
#define RULE_DIR        "/spiffs/rules"

static rule_t       s_rules[RULE_MAX_COUNT];
static int          s_rule_count = 0;
static TimerHandle_t s_timer = NULL;

/* ── ID generation ─────────────────────────────────────────── */

static void rule_generate_id(char *id_buf)
{
    uint32_t r = esp_random();
    snprintf(id_buf, 9, "r%07x", (unsigned int)(r & 0x0FFFFFFF));
}

/* ── Persistence ───────────────────────────────────────────── */

static void ensure_rules_dir(void)
{
    /* SPIFFS is flat; the "directory" is just a path prefix.
     * Nothing to create, but validate we can open the file path. */
}

static esp_err_t rule_save(void)
{
    ensure_rules_dir();

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_rule_count; i++) {
        const rule_t *r = &s_rules[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",                r->id);
        cJSON_AddStringToObject(obj, "name",              r->name);
        cJSON_AddBoolToObject(obj,   "enabled",           r->enabled);
        cJSON_AddStringToObject(obj, "condition_tool",    r->condition_tool);
        cJSON_AddStringToObject(obj, "condition_field",   r->condition_field);
        cJSON_AddNumberToObject(obj, "condition_op",      (double)r->condition_op);
        cJSON_AddStringToObject(obj, "condition_value",   r->condition_value);
        cJSON_AddNumberToObject(obj, "action_type",       (double)r->action_type);
        cJSON_AddStringToObject(obj, "action_params",     r->action_params);
        cJSON_AddNumberToObject(obj, "cooldown_s",        (double)r->cooldown_s);
        cJSON_AddNumberToObject(obj, "last_triggered",    (double)r->last_triggered);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return ESP_ERR_NO_MEM;

    FILE *f = fopen(RULE_FILE, "w");
    if (!f) {
        free(json);
        ESP_LOGW(TAG, "Cannot write rules file: %s", RULE_FILE);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    ESP_LOGI(TAG, "Saved %d rules", s_rule_count);
    return ESP_OK;
}

static esp_err_t rule_load(void)
{
    FILE *f = fopen(RULE_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No rules file, starting fresh");
        s_rule_count = 0;
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        fclose(f);
        s_rule_count = 0;
        return ESP_OK;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[fsize] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    free(buf);

    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        s_rule_count = 0;
        return ESP_OK;
    }

    s_rule_count = 0;
    cJSON *obj;
    cJSON_ArrayForEach(obj, arr) {
        if (s_rule_count >= RULE_MAX_COUNT) break;
        rule_t *r = &s_rules[s_rule_count];
        memset(r, 0, sizeof(*r));

#define LOAD_STR(field, key) do { \
    cJSON *_j = cJSON_GetObjectItem(obj, key); \
    if (_j && cJSON_IsString(_j)) \
        strncpy(r->field, _j->valuestring, sizeof(r->field) - 1); \
} while (0)

        LOAD_STR(id,              "id");
        LOAD_STR(name,            "name");
        LOAD_STR(condition_tool,  "condition_tool");
        LOAD_STR(condition_field, "condition_field");
        LOAD_STR(condition_value, "condition_value");
        LOAD_STR(action_params,   "action_params");
#undef LOAD_STR

        cJSON *j;
        j = cJSON_GetObjectItem(obj, "enabled");
        r->enabled = j ? cJSON_IsTrue(j) : true;

        j = cJSON_GetObjectItem(obj, "condition_op");
        if (j && cJSON_IsNumber(j)) r->condition_op = (rule_op_t)j->valueint;

        j = cJSON_GetObjectItem(obj, "action_type");
        if (j && cJSON_IsNumber(j)) r->action_type = (rule_action_t)j->valueint;

        j = cJSON_GetObjectItem(obj, "cooldown_s");
        if (j && cJSON_IsNumber(j)) r->cooldown_s = (uint32_t)j->valueint;

        j = cJSON_GetObjectItem(obj, "last_triggered");
        if (j && cJSON_IsNumber(j)) r->last_triggered = (int64_t)j->valuedouble;

        s_rule_count++;
    }
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Loaded %d rules", s_rule_count);
    return ESP_OK;
}

/* ── Condition evaluation ──────────────────────────────────── */

/*
 * Extract a numeric or string value from a freeform tool output string.
 * Looks for patterns like "field: 42.3" or "field: value" (newline/end delimited).
 */
static bool extract_field(const char *tool_output, const char *field,
                           char *out_val, size_t out_size)
{
    /* Search case-insensitively for "field:" or "field :" */
    size_t flen = strlen(field);
    const char *p = tool_output;
    while (*p) {
        /* Case-insensitive comparison */
        if (strncasecmp(p, field, flen) == 0) {
            const char *after = p + flen;
            while (*after == ' ' || *after == ':') after++;
            if (*after == '\0') break;
            /* Extract value to end-of-line */
            const char *eol = strchr(after, '\n');
            size_t vlen = eol ? (size_t)(eol - after) : strlen(after);
            /* Strip trailing whitespace/CR */
            while (vlen > 0 && (after[vlen-1] == '\r' || after[vlen-1] == ' '))
                vlen--;
            if (vlen >= out_size) vlen = out_size - 1;
            memcpy(out_val, after, vlen);
            out_val[vlen] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

static bool evaluate_condition(const rule_t *rule, char *tool_output, size_t out_sz)
{
    /* Call the condition tool */
    esp_err_t err = tool_registry_execute(rule->condition_tool, "{}", tool_output, out_sz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Rule %s: condition tool '%s' failed: %s",
                 rule->id, rule->condition_tool, esp_err_to_name(err));
        return false;
    }

    /* Extract the field value */
    char field_val[128] = {0};
    if (!extract_field(tool_output, rule->condition_field, field_val, sizeof(field_val))) {
        ESP_LOGW(TAG, "Rule %s: field '%s' not found in tool output",
                 rule->id, rule->condition_field);
        return false;
    }

    ESP_LOGI(TAG, "Rule %s: field='%s' val='%s' op=%d cmp='%s'",
             rule->id, rule->condition_field, field_val,
             rule->condition_op, rule->condition_value);

    switch (rule->condition_op) {
        case RULE_OP_GT: {
            double fv = strtod(field_val, NULL);
            double cv = strtod(rule->condition_value, NULL);
            return fv > cv;
        }
        case RULE_OP_LT: {
            double fv = strtod(field_val, NULL);
            double cv = strtod(rule->condition_value, NULL);
            return fv < cv;
        }
        case RULE_OP_EQ:
            return strcmp(field_val, rule->condition_value) == 0;
        case RULE_OP_NEQ:
            return strcmp(field_val, rule->condition_value) != 0;
        case RULE_OP_CONTAINS:
            return strstr(field_val, rule->condition_value) != NULL;
        default:
            return false;
    }
}

/* ── Action dispatch ───────────────────────────────────────── */

static void fire_action(const rule_t *rule)
{
    char mon[128];
    snprintf(mon, sizeof(mon), "[rule] '%s' fired", rule->name);
    ws_server_broadcast_monitor("rule", mon);
    ESP_LOGI(TAG, "Rule '%s' (%s) fired, action=%d", rule->name, rule->id, rule->action_type);

    switch (rule->action_type) {
        case RULE_ACTION_TELEGRAM: {
            /* Push a message onto the agent inbound queue to deliver via Telegram */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "rule", sizeof(msg.chat_id) - 1);
            /* Format: "send telegram: <params>" for agent to dispatch */
            /* name[32] + action_params[256] + literal overhead ~20 = 308 minimum */
            char content[320];
            snprintf(content, sizeof(content),
                     "[rule triggered: %s] %s", rule->name, rule->action_params);
            msg.content = strdup(content);
            if (msg.content) {
                message_bus_push_inbound(&msg);
            }
            break;
        }
        case RULE_ACTION_EMAIL: {
            /* Build send_email tool call input */
            /* subject[128] + body[192] + JSON overhead ~28 = 348 minimum */
            char email_input[360];
            /* action_params format: "subject|body" */
            char subject[128] = {0};
            char body[192]    = {0};
            const char *pipe = strchr(rule->action_params, '|');
            if (pipe) {
                size_t slen = (size_t)(pipe - rule->action_params);
                if (slen >= sizeof(subject)) slen = sizeof(subject) - 1;
                memcpy(subject, rule->action_params, slen);
                strncpy(body, pipe + 1, sizeof(body) - 1);
            } else {
                strncpy(subject, rule->action_params, sizeof(subject) - 1);
                strncpy(body, rule->action_params, sizeof(body) - 1);
            }
            snprintf(email_input, sizeof(email_input),
                     "{\"subject\":\"%s\",\"body\":\"%s\"}", subject, body);
            char email_out[256];
            tool_registry_execute("send_email", email_input, email_out, sizeof(email_out));
            break;
        }
        case RULE_ACTION_HA: {
            /* action_params: "METHOD /api/endpoint [json_body]" */
            char method[8] = "GET";
            char endpoint[128] = {0};
            char ha_body[128] = {0};
            char ha_input[320];
            sscanf(rule->action_params, "%7s %127s %127[^\n]", method, endpoint, ha_body);
            if (ha_body[0]) {
                snprintf(ha_input, sizeof(ha_input),
                         "{\"method\":\"%s\",\"endpoint\":\"%s\",\"body\":\"%s\"}",
                         method, endpoint, ha_body);
            } else {
                snprintf(ha_input, sizeof(ha_input),
                         "{\"method\":\"%s\",\"endpoint\":\"%s\"}", method, endpoint);
            }
            char ha_out[512];
            tool_registry_execute("ha_request", ha_input, ha_out, sizeof(ha_out));
            break;
        }
        case RULE_ACTION_LOG: {
            /* Append a note to today's daily log */
            char log_input[320];
            snprintf(log_input, sizeof(log_input),
                     "{\"content\":\"[rule: %s] %s\"}", rule->name, rule->action_params);
            char log_out[128];
            tool_registry_execute("memory_append_today", log_input, log_out, sizeof(log_out));
            break;
        }
    }
}

/* ── Evaluation loop ───────────────────────────────────────── */

static void rule_evaluate_all(void)
{
    int64_t now = (int64_t)time(NULL);
    char *tool_output = malloc(TOOL_OUT_SIZE);
    if (!tool_output) return;

    for (int i = 0; i < s_rule_count; i++) {
        rule_t *r = &s_rules[i];
        if (!r->enabled) continue;
        if (r->id[0] == '\0') continue;

        /* Check cooldown */
        if (r->last_triggered > 0 && r->cooldown_s > 0) {
            if ((int64_t)(now - r->last_triggered) < (int64_t)r->cooldown_s) {
                continue;
            }
        }

        tool_output[0] = '\0';
        bool triggered = evaluate_condition(r, tool_output, TOOL_OUT_SIZE);
        if (triggered) {
            fire_action(r);
            r->last_triggered = now;
            rule_save();  /* persist updated last_triggered */
        }
    }

    free(tool_output);
}

static void rule_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    rule_evaluate_all();
}

/* ── Public API ────────────────────────────────────────────── */

esp_err_t rule_engine_init(void)
{
    return rule_load();
}

esp_err_t rule_engine_start(void)
{
    if (s_timer) return ESP_OK;  /* already started */

    s_timer = xTimerCreate("rule_engine",
                            pdMS_TO_TICKS(RULE_EVAL_S * 1000),
                            pdTRUE,   /* auto-reload */
                            NULL,
                            rule_timer_cb);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create rule engine timer");
        return ESP_FAIL;
    }

    if (xTimerStart(s_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start rule engine timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Rule engine started (eval every %ds, %d rules loaded)",
             RULE_EVAL_S, s_rule_count);
    return ESP_OK;
}

void rule_engine_stop(void)
{
    if (s_timer) {
        xTimerStop(s_timer, 0);
        xTimerDelete(s_timer, 0);
        s_timer = NULL;
    }
}

esp_err_t rule_engine_add(rule_t *rule)
{
    if (s_rule_count >= RULE_MAX_COUNT) {
        ESP_LOGW(TAG, "Rule limit reached (%d)", RULE_MAX_COUNT);
        return ESP_ERR_NO_MEM;
    }

    rule_generate_id(rule->id);
    if (!rule->enabled) rule->enabled = true;  /* default enabled */
    if (rule->cooldown_s == 0) rule->cooldown_s = 300;  /* default 5min cooldown */

    s_rules[s_rule_count++] = *rule;
    return rule_save();
}

esp_err_t rule_engine_remove(const char *rule_id)
{
    for (int i = 0; i < s_rule_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            /* Shift remaining rules down */
            memmove(&s_rules[i], &s_rules[i + 1],
                    sizeof(rule_t) * (size_t)(s_rule_count - i - 1));
            s_rule_count--;
            return rule_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void rule_engine_list(rule_t *out_rules, int *out_count)
{
    *out_count = s_rule_count;
    memcpy(out_rules, s_rules, sizeof(rule_t) * (size_t)s_rule_count);
}
