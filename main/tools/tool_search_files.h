#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Search SPIFFS files for a text pattern (case-insensitive substring).
 *
 * Input JSON:
 *   pattern  (string, required) — text to search for
 *   prefix   (string, optional) — path prefix to limit scope, e.g. "/spiffs/memory/"
 *                                 defaults to "/spiffs/" (all files)
 *
 * Output: matching lines as "path:linenum: content\n", capped at 20 matches.
 *         "No matches found." if nothing matches.
 */
esp_err_t tool_search_files_execute(const char *input_json, char *output, size_t output_size);
