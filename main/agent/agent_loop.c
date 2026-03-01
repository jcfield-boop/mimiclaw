#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "gateway/ws_server.h"
#include "led/led_status.h"
#include "tools/tool_memory.h"
#include "telegram/telegram_bot.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

/* ── Streaming progress context ───────────────────────────────── */

typedef struct {
    size_t last_broadcast_len;
} tg_stream_ctx_t;

/* Progress callback: broadcasts partial text preview to web console.
 * No Telegram HTTP calls here — avoids nested SSL on limited stack. */
static void tg_stream_progress(const char *text, size_t len, void *ctx)
{
    tg_stream_ctx_t *sc = (tg_stream_ctx_t *)ctx;
    if (len <= sc->last_broadcast_len) return;

    /* Show the last ~50 chars of what just arrived */
    size_t tail = (len > 50) ? (len - 50) : 0;
    char preview[80];
    snprintf(preview, sizeof(preview), "+%zu chars: ...%.50s",
             len - sc->last_broadcast_len, text + tail);
    /* Strip newlines from preview */
    for (char *p = preview; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    ws_server_broadcast_monitor_verbose("stream", preview);
    sc->last_broadcast_len = len;
}

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- If using cron_add for Telegram in this turn, set channel='telegram' and chat_id to source_chat_id.\n"
        "- Never use chat_id 'cron' for Telegram messages.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const mimi_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 &&
        strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) {
            tool_input = patched_input;
        }

        /* Execute tool */
        tool_output[0] = '\0';
        {
            char tool_mon[128];
            snprintf(tool_mon, sizeof(tool_mon), "%s %.80s", call->name, tool_input);
            for (char *p = tool_mon; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("tool", tool_mon);
        }
        led_set_state(LED_TOOL);
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
        free(patched_input);

        int tool_out_len = (int)strlen(tool_output);
        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, tool_out_len);

        /* Broadcast tool error or a brief result preview to the live log */
        if (strncmp(tool_output, "Error:", 6) == 0) {
            char emsg[160];
            snprintf(emsg, sizeof(emsg), "[%s] %.100s", call->name, tool_output);
            ws_server_broadcast_monitor("error", emsg);
        } else {
            char preview[128];
            snprintf(preview, sizeof(preview), "[%s] %d bytes: %.40s%s",
                     call->name, tool_out_len,
                     tool_output,
                     tool_out_len > 40 ? "..." : "");
            /* strip newlines in preview */
            for (char *p = preview; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("tool", preview);
        }

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large persistent buffers (ESP32-C6: no PSRAM, all in 512KB SRAM) */
    char *system_prompt = calloc(1, MIMI_CONTEXT_BUF_SIZE);  /* 12KB */
    char *tool_output   = calloc(1, TOOL_OUTPUT_SIZE);        /*  8KB */

    if (!system_prompt || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        /* Reset per-turn memory write flag at the start of each user message */
        memory_tool_reset_turn();

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        {
            char mon[64];
            snprintf(mon, sizeof(mon), "from %s:%s", msg.channel, msg.chat_id);
            ws_server_broadcast_monitor("task", mon);
        }

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history directly as cJSON (no serialize→parse round-trip) */
        cJSON *messages = session_get_history_cjson(msg.chat_id, MIMI_AGENT_MAX_HISTORY);

        /* 2a. Content-byte budget: drop oldest user/assistant pairs until the
         *     total content length fits within MIMI_SESSION_HISTORY_MAX_BYTES.
         *     This caps token cost and heap usage for long-running sessions. */
        {
            int total_chars = 0;
            int arr_size = cJSON_GetArraySize(messages);
            for (int i = 0; i < arr_size; i++) {
                cJSON *m = cJSON_GetArrayItem(messages, i);
                cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
                if (c && cJSON_IsString(c))
                    total_chars += (int)strlen(c->valuestring);
            }
            int dropped = 0;
            int drop_limit = arr_size; /* H3: cap drops to initial count — prevents infinite loop if any single msg exceeds budget */
            while (dropped < drop_limit &&
                   total_chars > MIMI_SESSION_HISTORY_MAX_BYTES &&
                   cJSON_GetArraySize(messages) >= 2) {
                cJSON *oldest = cJSON_GetArrayItem(messages, 0);
                cJSON *c = cJSON_GetObjectItemCaseSensitive(oldest, "content");
                if (c && cJSON_IsString(c))
                    total_chars -= (int)strlen(c->valuestring);
                cJSON_DeleteItemFromArray(messages, 0);
                dropped++;
            }
            if (dropped > 0) {
                char vmsg[64];
                snprintf(vmsg, sizeof(vmsg), "history trimmed: dropped %d msgs (%d bytes remain)",
                         dropped, total_chars);
                ws_server_broadcast_monitor_verbose("task", vmsg);
            }

            {
                char vmsg[96];
                snprintf(vmsg, sizeof(vmsg),
                         "agent ctx: %d msgs, ~%d hist bytes, prompt %d bytes, heap %u free",
                         cJSON_GetArraySize(messages), total_chars,
                         (int)strnlen(system_prompt, MIMI_CONTEXT_BUF_SIZE),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                ws_server_broadcast_monitor_verbose("task", vmsg);
            }
        }

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;
        bool recovery_tried = false;
        bool retry_done = false;
        bool oom_restart = false;

        /* Send a Telegram placeholder before the first LLM call so the user
         * sees immediate feedback.  We edit this message with the final reply. */
        int32_t tg_placeholder_id = -1;
        bool is_telegram = (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0);
        if (is_telegram) {
            tg_placeholder_id = telegram_send_get_id(msg.chat_id, "\xF0\x9F\xA6\x85 thinking...");
            if (tg_placeholder_id > 0) {
                ws_server_broadcast_monitor_verbose("task", "telegram: placeholder sent");
            }
        }

        tg_stream_ctx_t stream_ctx = { .last_broadcast_len = 0 };

        /* Detect memory trigger keywords so we can force tool_choice="any" on
         * the first LLM iteration. Only applied to iter 0 — subsequent iters
         * let the model choose freely (it may need to call other tools too). */
        bool force_memory_tool = false;
        {
            const char *txt = msg.content ? msg.content : "";
            /* Simple case-insensitive prefix/substring scan for common triggers */
            char lower[64] = {0};
            size_t tlen = strlen(txt);
            for (size_t ci = 0; ci < tlen && ci < sizeof(lower) - 1; ci++)
                lower[ci] = (char)tolower((unsigned char)txt[ci]);
            if (strstr(lower, "remember") || strstr(lower, "save that") ||
                strstr(lower, "note that") || strstr(lower, "don't forget") ||
                strstr(lower, "dont forget") || strstr(lower, "make a note") ||
                strstr(lower, "keep in mind")) {
                force_memory_tool = true;
                ws_server_broadcast_monitor_verbose("task", "memory trigger detected — forcing tool call");
            }
        }

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            /* Send "working" indicator before each API call (skip if placeholder sent) */
#if MIMI_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && tg_placeholder_id < 0 &&
                strcmp(msg.channel, MIMI_CHAN_SYSTEM) != 0) {
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("\xF0\x9F\xA6\x90 C6PO is on it...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif

            {
                char itermsg[48];
                snprintf(itermsg, sizeof(itermsg), "calling LLM streaming (iter %d)...", iteration + 1);
                ws_server_broadcast_monitor("llm", itermsg);
                led_set_state(LED_THINKING);
            }
            llm_response_t resp;
            bool force_this_iter = (force_memory_tool && iteration == 0);
            err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                           force_this_iter,
                                           tg_stream_progress, &stream_ctx,
                                           &resp);

            /* Retry once on connection failure after a short delay */
            if (err == ESP_ERR_HTTP_CONNECT && !retry_done) {
                retry_done = true;
                ws_server_broadcast_monitor("error", "LLM: connection failed, retrying in 2s...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                               force_this_iter,
                                               tg_stream_progress, &stream_ctx,
                                               &resp);
            }

            if (err != ESP_OK) {
                char emsg[80];
                snprintf(emsg, sizeof(emsg), "LLM call failed: %s", esp_err_to_name(err));
                ESP_LOGE(TAG, "%s", emsg);
                ws_server_broadcast_monitor("error", emsg);
                if (err == ESP_ERR_NO_MEM) { oom_restart = true; }
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    llm_response_free(&resp);
                    if (!final_text) {
                        /* M8: strdup OOM — treat as memory exhaustion */
                        ESP_LOGE(TAG, "strdup OOM for final_text (%d bytes)", (int)resp.text_len);
                        oom_restart = true;
                    }
                    break;
                }
                /* Empty response: if truncated at max_tokens, inject recovery once */
                bool do_recovery = resp.truncated && !recovery_tried;
                llm_response_free(&resp);
                if (do_recovery) {
                    recovery_tried = true;
                    iteration++;
                    ws_server_broadcast_monitor("error", "LLM: truncated, injecting recovery");
                    cJSON *recovery = cJSON_CreateObject();
                    cJSON_AddStringToObject(recovery, "role", "user");
                    cJSON_AddStringToObject(recovery, "content",
                        "Your response was cut off. Please give a brief, direct text answer now. Do not use any tools.");
                    cJSON_AddItemToArray(messages, recovery);
                    continue;
                }
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            iteration++;
        }

        cJSON_Delete(messages);

        /* Warn if we hit the tool iteration cap without a final response */
        if (!final_text && iteration >= MIMI_AGENT_MAX_TOOL_ITER) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "agent: max tool iterations (%d) reached", MIMI_AGENT_MAX_TOOL_ITER);
            ESP_LOGW(TAG, "%s", emsg);
            ws_server_broadcast_monitor("error", emsg);
        }

        /* 5. Send response */
        ws_server_broadcast_monitor("done", msg.chat_id);
        led_set_state(LED_IDLE);

        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            esp_err_t save_user = session_append(msg.chat_id, "user", msg.content);
            esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
            if (save_user != ESP_OK || save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for chat %s (user=%s, assistant=%s)",
                         msg.chat_id,
                         esp_err_to_name(save_user),
                         esp_err_to_name(save_asst));
            } else {
                ESP_LOGI(TAG, "Session saved for chat %s", msg.chat_id);
            }

            /* Compact the session file to MIMI_SESSION_MAX_MSGS lines. */
            session_trim(msg.chat_id, MIMI_SESSION_MAX_MSGS);

            ESP_LOGI(TAG, "Dispatching final response to %s:%s (%d bytes)",
                     msg.channel, msg.chat_id, (int)strlen(final_text));

            if (tg_placeholder_id > 0 && is_telegram) {
                /* Edit the placeholder in place with the final answer */
                telegram_edit_message(msg.chat_id, tg_placeholder_id, final_text);
                free(final_text);
            } else {
                /* Normal outbound dispatch */
                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = final_text;  /* transfer ownership */
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    ESP_LOGW(TAG, "Outbound queue full, drop final response");
                    free(final_text);
                } else {
                    final_text = NULL;
                }
            }
        } else {
            /* Error or empty response */
            free(final_text);
            ws_server_broadcast_monitor("error", "agent: LLM returned empty response");
            const char *err_text = oom_restart
                ? "Memory exhausted \xe2\x80\x94 restarting..."
                : "Sorry, I encountered an error.";

            if (tg_placeholder_id > 0 && is_telegram) {
                telegram_edit_message(msg.chat_id, tg_placeholder_id, err_text);
            } else {
                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = strdup(err_text);
                if (out.content) {
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop error response");
                        free(out.content);
                    }
                }
            }
        }

        /* Free inbound message content */
        free(msg.content);

        /* OOM recovery: give outbound task time to dispatch the error reply, then restart */
        if (oom_restart) {
            led_set_state(LED_OOM);
            ws_server_broadcast_monitor("system", "OOM restart in 3s...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    const uint32_t stack_candidates[] = {
        MIMI_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_size = stack_candidates[i];
        BaseType_t ret = xTaskCreate(
            agent_loop_task, "agent_loop",
            stack_size, NULL,
            MIMI_AGENT_PRIO, NULL);

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes", (unsigned)stack_size);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "agent_loop create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying...",
                 (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return ESP_FAIL;
}
