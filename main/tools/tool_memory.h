#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Reset the per-turn memory write flag.
 * Call this once each time a new inbound user message is dequeued,
 * before the ReAct loop begins.
 */
void memory_tool_reset_turn(void);

/**
 * Tool: memory_write
 * Schema: { memory_type: "preference"|"fact"|"instruction",
 *           content: string (max 500),
 *           confidence: number 0.0–1.0 }
 * Writes content to MEMORY.md with rate limiting and validation.
 */
esp_err_t tool_memory_write_execute(const char *input_json, char *output, size_t output_size);

/**
 * Tool: memory_append_today
 * Schema: { content: string (max 300) }
 * Appends a note to today's daily memory file.
 */
esp_err_t tool_memory_append_today_execute(const char *input_json, char *output, size_t output_size);
