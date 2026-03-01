#pragma once

#include "esp_err.h"

/**
 * Initialize the Telegram bot.
 */
esp_err_t telegram_bot_init(void);

/**
 * Start the Telegram polling task (long polling on Core 0).
 */
esp_err_t telegram_bot_start(void);

/**
 * Send a text message to a Telegram chat.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  Telegram chat ID (numeric string)
 * @param text     Message text (supports Markdown)
 */
esp_err_t telegram_send_message(const char *chat_id, const char *text);

/**
 * Save the Telegram bot token to NVS.
 */
esp_err_t telegram_set_token(const char *token);

/**
 * Send a plain-text message to a Telegram chat and return the message_id.
 * Returns -1 on failure (no token, network error, API error).
 * Used to send a "thinking..." placeholder before a streaming LLM response.
 */
int32_t telegram_send_get_id(const char *chat_id, const char *text);

/**
 * Edit an existing Telegram message in place.
 * message_id must be > 0 (from telegram_send_get_id).
 * Silently ignores "message is not modified" (idempotent).
 * Truncates text > 4096 chars with "..." suffix.
 */
esp_err_t telegram_edit_message(const char *chat_id, int32_t message_id, const char *text);

