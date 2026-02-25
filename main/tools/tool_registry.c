#include "tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_http.h"
#include "tools/tool_smtp.h"
#include "tools/tool_device_temp.h"
#include "tools/tool_search_files.h"
#include "tools/tool_system_info.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 17

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information. Use this when you need up-to-date facts, news, weather, or anything beyond your training data.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write content to a file on SPIFFS. Overwrites by default. Set append=true to append to an existing file instead of overwriting.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"},"
            "\"append\":{\"type\":\"boolean\",\"description\":\"If true, append to existing file instead of overwriting\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. /spiffs/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires. "
                       "IMPORTANT: Never guess unix timestamps from training data — use seconds_from_now for relative times, "
                       "or call get_current_time first if you need an absolute epoch.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"seconds_from_now\":{\"type\":\"integer\",\"description\":\"PREFERRED for 'at': fire this many seconds from now (e.g. 300 = in 5 minutes). Avoids epoch calculation errors.\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Absolute unix timestamp to fire at. Only use this if you have called get_current_time and have a verified current epoch.\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register send_email */
    mimi_tool_t se = {
        .name = "send_email",
        .description = "Send an email via Gmail SMTP using credentials stored in /spiffs/config/SERVICES.md. "
                       "Credentials are never exposed — the tool reads them directly.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"subject\":{\"type\":\"string\",\"description\":\"Email subject line\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Plain text email body\"},"
            "\"to\":{\"type\":\"string\",\"description\":\"Recipient address (optional, defaults to to_address in SERVICES.md)\"}"
            "},"
            "\"required\":[\"subject\",\"body\"]}",
        .execute = tool_smtp_execute,
    };
    register_tool(&se);

    /* Register device_temp */
    mimi_tool_t dt = {
        .name = "device_temp",
        .description = "Read the ESP32-C6 internal chip temperature in Celsius. Useful for device health monitoring.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_device_temp_execute,
    };
    register_tool(&dt);

    /* Register search_files */
    mimi_tool_t sf = {
        .name = "search_files",
        .description = "Search SPIFFS files for text (case-insensitive). Returns matching lines with filename and line number. Use to find content across memory, notes, and skills.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pattern\":{\"type\":\"string\",\"description\":\"Text to search for (case-insensitive)\"},"
            "\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix to limit search, e.g. /spiffs/memory/\"}"
            "},"
            "\"required\":[\"pattern\"]}",
        .execute = tool_search_files_execute,
    };
    register_tool(&sf);

    /* Register system_info */
    mimi_tool_t si = {
        .name = "system_info",
        .description = "Get live device status: free heap, SPIFFS usage, uptime, WiFi signal strength, and firmware version. Use to check device health.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_system_info_execute,
    };
    register_tool(&si);

    /* Register http_request */
    mimi_tool_t hr = {
        .name = "http_request",
        .description = "Make an HTTPS GET or POST request to an external API. Use this to call services like email (Resend), SMS, or any REST API. Read credentials from /spiffs/config/SERVICES.md first.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"Full HTTPS URL to call\"},"
            "\"method\":{\"type\":\"string\",\"description\":\"HTTP method: GET or POST (default: GET)\"},"
            "\"headers\":{\"type\":\"object\",\"description\":\"Optional HTTP headers as key/value pairs\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Optional request body string (for POST)\"}"
            "},"
            "\"required\":[\"url\"]}",
        .execute = tool_http_execute,
    };
    register_tool(&hr);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
