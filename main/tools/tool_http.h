#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a generic HTTP GET or POST request.
 *
 * Input JSON fields:
 *   url     (string, required)  — full HTTPS URL
 *   method  (string, optional)  — "GET" or "POST" (default: "GET")
 *   headers (object, optional)  — key/value header pairs
 *   body    (string, optional)  — request body for POST
 *
 * Output: "HTTP <status>\n<response body>" (truncated to output_size)
 */
esp_err_t tool_http_execute(const char *input_json, char *output, size_t output_size);
