#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Maximum number of concurrent rules */
#define RULE_MAX_COUNT  8

/* Rule condition operators */
typedef enum {
    RULE_OP_GT       = 0,   /* numeric > */
    RULE_OP_LT       = 1,   /* numeric < */
    RULE_OP_EQ       = 2,   /* string == (exact match) */
    RULE_OP_CONTAINS = 3,   /* string contains */
    RULE_OP_NEQ      = 4,   /* string != */
} rule_op_t;

/* Rule action types */
typedef enum {
    RULE_ACTION_TELEGRAM = 0,   /* send Telegram message */
    RULE_ACTION_EMAIL    = 1,   /* send email via send_email tool */
    RULE_ACTION_HA       = 2,   /* call ha_request */
    RULE_ACTION_LOG      = 3,   /* append to today's daily note */
} rule_action_t;

/* A single automation rule */
typedef struct {
    char          id[9];                /* 8-char hex ID + null */
    char          name[32];
    bool          enabled;

    /* Condition: call condition_tool, parse condition_field from result,
     * apply condition_op against condition_value */
    char          condition_tool[24];   /* e.g. "device_temp", "system_info" */
    char          condition_field[32];  /* field name to extract from result */
    rule_op_t     condition_op;
    char          condition_value[64];  /* comparison value (as string) */

    /* Action */
    rule_action_t action_type;
    char          action_params[256];   /* depends on action_type:
                                           TELEGRAM: message text
                                           EMAIL:    "subject|body"
                                           HA:       "METHOD /api/endpoint [body]"
                                           LOG:      note text */

    /* Timing */
    uint32_t      cooldown_s;           /* minimum seconds between firings */
    int64_t       last_triggered;       /* unix epoch of last trigger, 0=never */
} rule_t;

/**
 * Initialize the rule engine. Loads rules from SPIFFS.
 * Must be called after SPIFFS is mounted.
 */
esp_err_t rule_engine_init(void);

/**
 * Start the rule evaluation timer (every 60 seconds).
 * Call after WiFi is connected and time is synced.
 */
esp_err_t rule_engine_start(void);

/**
 * Stop the rule evaluation timer.
 */
void rule_engine_stop(void);

/**
 * Add a new rule. Generates an ID and persists to SPIFFS.
 * @param rule  Rule to add (id will be generated)
 */
esp_err_t rule_engine_add(rule_t *rule);

/**
 * Remove a rule by ID. Persists the change.
 */
esp_err_t rule_engine_remove(const char *rule_id);

/**
 * Get a snapshot of all current rules.
 * @param out_rules  Buffer to fill (RULE_MAX_COUNT entries)
 * @param out_count  Number of rules filled
 */
void rule_engine_list(rule_t *out_rules, int *out_count);
