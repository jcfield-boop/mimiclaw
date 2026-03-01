#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * Result returned by lan_request().
 * On success: ok=true, http_status=2xx, bytes=body length.
 * On HTTP error: ok=false, http_status=4xx/5xx, err_msg empty.
 * On connection failure: ok=false, http_status=0, err_msg=esp_err string.
 */
typedef struct {
    bool  ok;
    int   http_status;
    int   bytes;
    bool  truncated;
    char  err_msg[64];
} lan_result_t;

/**
 * Shared LAN HTTP helper used by ha_request and klipper_request.
 *
 * All buffers are caller-provided — no per-request heap allocation.
 *
 * @param method           "GET" or "POST"
 * @param base_url         e.g. "http://192.168.0.50:8123"
 * @param endpoint         e.g. "/api/states/light.x"
 * @param token_header     Header name for auth, e.g. "Authorization" (NULL = no auth)
 * @param token_value      Header value, e.g. "Bearer eyJ..." (NULL = no auth)
 * @param body             POST body string (NULL or "" = no body)
 * @param response_buf     Caller-provided buffer for response body
 * @param response_buf_size Size of response_buf (response truncated at size-1 bytes)
 * @param result           Output: structured result
 */
esp_err_t lan_request(const char *method,
                      const char *base_url,
                      const char *endpoint,
                      const char *token_header,
                      const char *token_value,
                      const char *body,
                      char       *response_buf,
                      size_t      response_buf_size,
                      lan_result_t *result);

/**
 * Build the standard JSON output string for a lan_result_t.
 * Writes into out/out_size.  body_str is the response_buf content.
 */
void lan_result_to_json(const lan_result_t *result,
                        const char *body_str,
                        char *out, size_t out_size);
