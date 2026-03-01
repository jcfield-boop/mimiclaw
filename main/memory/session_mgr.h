#pragma once

#include "esp_err.h"
#include <stddef.h>
#include "cJSON.h"

/**
 * Initialize session manager.
 */
esp_err_t session_mgr_init(void);

/**
 * Append a message to a session file (JSONL format).
 * @param chat_id   Session identifier (e.g., "12345")
 * @param role      "user" or "assistant"
 * @param content   Message text
 */
esp_err_t session_append(const char *chat_id, const char *role, const char *content);

/**
 * Load session history as a JSON array string suitable for LLM messages.
 * Returns the last max_msgs messages as:
 * [{"role":"user","content":"..."},{"role":"assistant","content":"..."},...]
 *
 * @param chat_id   Session identifier
 * @param buf       Output buffer (caller allocates)
 * @param size      Buffer size
 * @param max_msgs  Maximum number of messages to return
 */
esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs);

/**
 * Load session history as a cJSON array for direct use with LLM messages.
 * Returns the last max_msgs messages. Caller owns the result and must cJSON_Delete it.
 * Returns an empty array (never NULL) if no history exists.
 *
 * @param chat_id   Session identifier
 * @param max_msgs  Maximum number of messages to return
 */
cJSON *session_get_history_cjson(const char *chat_id, int max_msgs);

/**
 * Compact a session file to at most max_msgs lines.
 * Uses two-pass file I/O: first counts lines, then rewrites keeping the last max_msgs.
 * Safe to call after every session_append(); no-ops if already compact.
 *
 * @param chat_id   Session identifier
 * @param max_msgs  Maximum number of lines (messages) to retain
 */
esp_err_t session_trim(const char *chat_id, int max_msgs);

/**
 * Clear a session (delete the file).
 */
esp_err_t session_clear(const char *chat_id);

/**
 * List all session files (prints to log).
 */
void session_list(void);
