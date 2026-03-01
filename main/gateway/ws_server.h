#pragma once

#include "esp_err.h"

/**
 * Initialize and start the WebSocket/HTTP server on MIMI_WS_PORT.
 *
 * HTTP endpoints:
 *   GET    /                               Serves the web console (embedded binary)
 *   GET    /api/file?name=soul|user|memory Returns file content
 *   POST   /api/file?name=soul|user|memory Writes file content
 *   POST   /api/reboot                     Triggers esp_restart() after 500 ms
 *   GET    /api/crons                      Returns all cron jobs as JSON
 *   DELETE /api/cron?id=<id>               Removes a cron job by ID
 *
 * WebSocket endpoint (moved from / to /ws):
 *   WS   /ws   Inbound:  {"type":"message","content":"hello","chat_id":"ws_1"}
 *              Outbound: {"type":"response","content":"Hi!","chat_id":"ws_1"}
 *              Monitor:  {"type":"monitor","event":"tool","msg":"get_current_time"}
 */
esp_err_t ws_server_start(void);

/**
 * Send a text message to a specific WebSocket client by chat_id.
 */
esp_err_t ws_server_send(const char *chat_id, const char *text);

/**
 * Broadcast a monitor event to all connected WebSocket clients.
 * Used by the agent loop to stream progress to the web console.
 * @param event  Short category string: "llm", "tool", "done", "err"
 * @param msg    Human-readable detail
 */
esp_err_t ws_server_broadcast_monitor(const char *event, const char *msg);

/** Broadcast verbose-only monitor event; no-op when verbose logs disabled. */
esp_err_t ws_server_broadcast_monitor_verbose(const char *event, const char *msg);

/** Returns true if verbose logging is enabled. */
bool ws_server_get_verbose_logs(void);

/**
 * Stop the WebSocket/HTTP server.
 */
esp_err_t ws_server_stop(void);
