#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * klipper_request tool — Moonraker REST API access.
 *
 * Input JSON:
 *   { "method": "GET"|"POST",   (optional, default GET)
 *     "endpoint": "/printer/...",
 *     "body": "{...}"            (optional, POST only) }
 *
 * Output JSON (same lan_result_t shape as ha_request):
 *   { "ok": bool, "status": int, "body": "...", "truncated": bool, "bytes": int }
 *   On failure: { "ok": false, "status": 0, "error": "...", "reason": "...", "bytes": 0 }
 */
esp_err_t tool_klipper_execute(const char *input_json, char *output, size_t output_size);
