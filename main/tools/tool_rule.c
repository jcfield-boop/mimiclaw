#include "tool_rule.h"
#include "rules/rule_engine.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_rule";

/* Map operator string to enum */
static rule_op_t parse_op(const char *s)
{
    if (!s) return RULE_OP_GT;
    if (strcmp(s, ">")        == 0) return RULE_OP_GT;
    if (strcmp(s, "<")        == 0) return RULE_OP_LT;
    if (strcmp(s, "==")       == 0) return RULE_OP_EQ;
    if (strcmp(s, "!=")       == 0) return RULE_OP_NEQ;
    if (strcmp(s, "contains") == 0) return RULE_OP_CONTAINS;
    return RULE_OP_EQ;
}

static const char *op_to_str(rule_op_t op)
{
    switch (op) {
        case RULE_OP_GT:       return ">";
        case RULE_OP_LT:       return "<";
        case RULE_OP_EQ:       return "==";
        case RULE_OP_NEQ:      return "!=";
        case RULE_OP_CONTAINS: return "contains";
        default:               return "?";
    }
}

/* Map action type string to enum */
static rule_action_t parse_action_type(const char *s)
{
    if (!s) return RULE_ACTION_LOG;
    if (strcasecmp(s, "telegram") == 0) return RULE_ACTION_TELEGRAM;
    if (strcasecmp(s, "email")    == 0) return RULE_ACTION_EMAIL;
    if (strcasecmp(s, "ha")       == 0) return RULE_ACTION_HA;
    if (strcasecmp(s, "log")      == 0) return RULE_ACTION_LOG;
    return RULE_ACTION_LOG;
}

static const char *action_type_to_str(rule_action_t at)
{
    switch (at) {
        case RULE_ACTION_TELEGRAM: return "telegram";
        case RULE_ACTION_EMAIL:    return "email";
        case RULE_ACTION_HA:       return "ha";
        case RULE_ACTION_LOG:      return "log";
        default:                   return "log";
    }
}

/* ── rule_create ─────────────────────────────────────────── */

esp_err_t tool_rule_create_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

#define GET_STR(var, key) \
    cJSON *j_##var = cJSON_GetObjectItem(input, key); \
    const char *var = (j_##var && cJSON_IsString(j_##var)) ? j_##var->valuestring : NULL

    GET_STR(name,             "name");
    GET_STR(cond_tool,        "condition_tool");
    GET_STR(cond_field,       "condition_field");
    GET_STR(cond_op_str,      "condition_op");
    GET_STR(cond_val,         "condition_value");
    GET_STR(action_type_str,  "action_type");
    GET_STR(action_params,    "action_params");
#undef GET_STR

    cJSON *j_cooldown = cJSON_GetObjectItem(input, "cooldown_s");

    /* Validate required fields */
    if (!name || !cond_tool || !cond_field || !cond_val || !action_type_str || !action_params) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: required fields: name, condition_tool, condition_field, "
                 "condition_value, action_type, action_params");
        return ESP_ERR_INVALID_ARG;
    }

    rule_t rule;
    memset(&rule, 0, sizeof(rule));

    strncpy(rule.name,            name,            sizeof(rule.name)            - 1);
    strncpy(rule.condition_tool,  cond_tool,       sizeof(rule.condition_tool)  - 1);
    strncpy(rule.condition_field, cond_field,      sizeof(rule.condition_field) - 1);
    strncpy(rule.condition_value, cond_val,        sizeof(rule.condition_value) - 1);
    strncpy(rule.action_params,   action_params,   sizeof(rule.action_params)   - 1);
    rule.condition_op  = parse_op(cond_op_str);
    rule.action_type   = parse_action_type(action_type_str);
    rule.enabled       = true;
    rule.cooldown_s    = (j_cooldown && cJSON_IsNumber(j_cooldown))
                         ? (uint32_t)j_cooldown->valueint : 300;
    rule.last_triggered = 0;

    cJSON_Delete(input);

    esp_err_t err = rule_engine_add(&rule);
    if (err == ESP_ERR_NO_MEM) {
        snprintf(output, output_size, "Error: rule limit reached (max %d)", RULE_MAX_COUNT);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to save rule");
        return err;
    }

    ESP_LOGI(TAG, "Created rule '%s' id=%s", rule.name, rule.id);
    snprintf(output, output_size,
             "{\"ok\":true,\"rule_id\":\"%s\",\"name\":\"%s\"}",
             rule.id, rule.name);
    return ESP_OK;
}

/* ── rule_list ───────────────────────────────────────────── */

esp_err_t tool_rule_list_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    rule_t rules[RULE_MAX_COUNT];
    int count = 0;
    rule_engine_list(rules, &count);

    int64_t now = (int64_t)time(NULL);

    int pos = 0;
    int cap = (int)output_size;
    pos += snprintf(output + pos, cap - pos, "[");

    for (int i = 0; i < count && pos < cap - 2; i++) {
        const rule_t *r = &rules[i];
        int64_t next_fire = (r->cooldown_s > 0 && r->last_triggered > 0)
                            ? r->last_triggered + (int64_t)r->cooldown_s : 0;
        int64_t next_in   = (next_fire > 0) ? (next_fire - now) : -1;

        int n = snprintf(output + pos, cap - pos,
                 "%s{"
                 "\"id\":\"%s\","
                 "\"name\":\"%s\","
                 "\"enabled\":%s,"
                 "\"condition\":\"%s %s %s %s\","
                 "\"action_type\":\"%s\","
                 "\"cooldown_s\":%" PRIu32 ","
                 "\"last_triggered\":%" PRId64 ","
                 "\"next_eval_in_s\":%" PRId64
                 "}",
                 i > 0 ? "," : "",
                 r->id,
                 r->name,
                 r->enabled ? "true" : "false",
                 r->condition_tool, r->condition_field,
                 op_to_str(r->condition_op), r->condition_value,
                 action_type_to_str(r->action_type),
                 r->cooldown_s,
                 r->last_triggered,
                 next_in);
        if (n <= 0 || n >= cap - pos) break;
        pos += n;
    }

    if (pos < cap - 1) { output[pos++] = ']'; output[pos] = '\0'; }
    else { output[cap - 2] = ']'; output[cap - 1] = '\0'; }

    return ESP_OK;
}

/* ── rule_delete ─────────────────────────────────────────── */

esp_err_t tool_rule_delete_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_id = cJSON_GetObjectItem(input, "rule_id");
    if (!j_id || !cJSON_IsString(j_id)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'rule_id' is required");
        return ESP_ERR_INVALID_ARG;
    }

    char rule_id[16];
    strncpy(rule_id, j_id->valuestring, sizeof(rule_id) - 1);
    cJSON_Delete(input);

    esp_err_t err = rule_engine_remove(rule_id);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: rule '%s' not found", rule_id);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to remove rule");
        return err;
    }

    ESP_LOGI(TAG, "Deleted rule %s", rule_id);
    snprintf(output, output_size, "{\"ok\":true,\"rule_id\":\"%s\"}", rule_id);
    return ESP_OK;
}
