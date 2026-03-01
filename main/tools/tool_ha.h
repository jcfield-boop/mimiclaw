#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Tool: ha_request
 * Query or control Home Assistant via its REST API.
 * Schema: { method: "GET"|"POST", endpoint: "/api/...", body: "..." (optional) }
 * Base URL and Bearer token are read from SERVICES.md ## Home Assistant section.
 */
esp_err_t tool_ha_execute(const char *input_json, char *output, size_t output_size);
