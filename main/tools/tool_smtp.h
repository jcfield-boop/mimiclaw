#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Send an email via SMTP/TLS.
 *
 * Reads SMTP credentials from /spiffs/config/SERVICES.md (## Email section).
 * Credentials never appear in the agent's context window.
 *
 * Input JSON fields:
 *   subject  (string, required)
 *   body     (string, required)
 *   to       (string, optional) — overrides to_address from SERVICES.md
 *
 * Output: "OK: message sent" or error description.
 */
esp_err_t tool_smtp_execute(const char *input_json, char *output, size_t output_size);
