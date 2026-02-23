#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize web search tool.
 */
esp_err_t tool_web_search_init(void);

/**
 * Execute a web search.
 *
 * @param input_json   JSON string with "query" field
 * @param output       Output buffer for formatted search results
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size);

/**
 * Save Brave Search API key to NVS.
 */
esp_err_t tool_web_search_set_key(const char *api_key);

/**
 * Return the currently configured Brave Search API key (may be empty).
 */
const char *tool_web_search_get_key(void);

/**
 * Return cumulative session search stats.
 * @param calls           Total successful searches this session (may be NULL)
 * @param cost_millicents Estimated cost in 1/1000 cents at Brave Pro rate (may be NULL)
 */
void tool_web_search_get_stats(uint32_t *calls, uint32_t *cost_millicents);
