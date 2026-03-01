#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Create a new automation rule.
 * Input: {
 *   "name": str,
 *   "condition_tool": str,      -- tool to call for condition check
 *   "condition_field": str,     -- field name to extract from tool output
 *   "condition_op": str,        -- ">", "<", "==", "!=", "contains"
 *   "condition_value": str,     -- value to compare against
 *   "action_type": str,         -- "telegram", "email", "ha", "log"
 *   "action_params": str,       -- action details (see rule_engine.h)
 *   "cooldown_s": int           -- optional, default 300
 * }
 */
esp_err_t tool_rule_create_execute(const char *input_json, char *output, size_t output_size);

/**
 * List all automation rules as JSON.
 * Input: {} (no parameters)
 */
esp_err_t tool_rule_list_execute(const char *input_json, char *output, size_t output_size);

/**
 * Delete an automation rule by ID.
 * Input: {"rule_id": str}
 */
esp_err_t tool_rule_delete_execute(const char *input_json, char *output, size_t output_size);
