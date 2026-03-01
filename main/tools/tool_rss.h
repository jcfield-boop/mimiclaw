#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Fetch and parse an RSS/Atom feed, returning the latest items as JSON.
 * Input:  {"url": "https://...", "max_items": 5}   (max_items optional, default 5)
 * Output: JSON array: [{"title":"...","link":"...","date":"...","summary":"..."}]
 */
esp_err_t tool_rss_execute(const char *input_json, char *output, size_t output_size);
