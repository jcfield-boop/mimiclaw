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
#include "tools/tool_memory.h"
#include "tools/tool_ha.h"
#include "tools/tool_klipper.h"
#include "tools/tool_gpio.h"
#include "tools/tool_wifi_scan.h"
#include "tools/tool_rss.h"
#include "tools/tool_rule.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 30

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

    /* Register memory_write */
    mimi_tool_t mw = {
        .name = "memory_write",
        .description =
            "Write a persistent memory to MEMORY.md. Use when the user explicitly shares "
            "a preference, fact, or standing instruction they want remembered. "
            "Requires confidence >= 0.7. Do not use for inferences or guesses. "
            "Rate limited: once per conversation turn, 60s cooldown between turns.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"memory_type\":{\"type\":\"string\","
            "\"enum\":[\"preference\",\"fact\",\"instruction\"],"
            "\"description\":\"Category: preference (user likes/dislikes), fact (factual info about user), instruction (standing order)\"},"
            "\"content\":{\"type\":\"string\",\"maxLength\":500,"
            "\"description\":\"The memory to store. Be concise and specific.\"},"
            "\"confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1,"
            "\"description\":\"How confident you are this is worth remembering (>= 0.7 required)\"}"
            "},"
            "\"required\":[\"memory_type\",\"content\",\"confidence\"]}",
        .execute = tool_memory_write_execute,
    };
    register_tool(&mw);

    /* Register memory_append_today */
    mimi_tool_t mat = {
        .name = "memory_append_today",
        .description =
            "Append a note to today's daily log file. Use for significant events, "
            "completed tasks, or things worth a daily record. "
            "Rate limited: shares the same once-per-turn, 60s cooldown as memory_write.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"content\":{\"type\":\"string\",\"maxLength\":300,"
            "\"description\":\"The note to append to today's log.\"}}"
            ",\"required\":[\"content\"]}",
        .execute = tool_memory_append_today_execute,
    };
    register_tool(&mat);

    /* Register ha_request */
    mimi_tool_t ha = {
        .name = "ha_request",
        .description =
            "Query or control Home Assistant via its REST API. "
            "Use for entity state, service calls (lights, switches, climate), and automations. "
            "Reads ha_url and ha_token from SERVICES.md. "
            "Always use specific entity endpoints — /api/states bulk is blocked.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\"],"
            "\"description\":\"HTTP method\"},"
            "\"endpoint\":{\"type\":\"string\","
            "\"description\":\"HA API path starting with /api/ (e.g. /api/states/light.living_room)\"},"
            "\"body\":{\"type\":\"string\","
            "\"description\":\"JSON body for POST requests (e.g. {\\\"entity_id\\\":\\\"light.x\\\"})\"}"
            "},"
            "\"required\":[\"method\",\"endpoint\"]}",
        .execute = tool_ha_execute,
    };
    register_tool(&ha);

    /* Register klipper_request */
    mimi_tool_t kl = {
        .name = "klipper_request",
        .description =
            "Query or control a Klipper 3D printer via Moonraker's REST API. "
            "Use for printer status, temperatures, print jobs, and gcode commands. "
            "Reads moonraker_url (and optional moonraker_apikey) from SERVICES.md. "
            "Machine/system power endpoints are blocked for safety.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\"],"
            "\"description\":\"HTTP method\"},"
            "\"endpoint\":{\"type\":\"string\","
            "\"description\":\"Moonraker API path (e.g. /printer/info, /printer/objects/query?heater_bed&extruder)\"},"
            "\"body\":{\"type\":\"string\","
            "\"description\":\"JSON body for POST requests (e.g. {\\\"script\\\":\\\"G28\\\"})\"}"
            "},"
            "\"required\":[\"method\",\"endpoint\"]}",
        .execute = tool_klipper_execute,
    };
    register_tool(&kl);

    /* Register gpio_read */
    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read the current logic level (HIGH or LOW) of an ESP32-C6 GPIO pin. "
                       "Safe pins: 0-7, 10-17, 22. Blocked: 8 (LED), 9 (BOOT), 18-21 (flash).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\","
            "\"description\":\"GPIO pin number (0-22, excluding 8, 9, 18-21)\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    /* Register gpio_write */
    mimi_tool_t gw = {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Pin must be configured as output first using gpio_mode.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"state\":{\"type\":\"string\",\"description\":\"\\\"HIGH\\\", \\\"LOW\\\", 1, or 0\"}"
            "},\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_execute,
    };
    register_tool(&gw);

    /* Register gpio_mode */
    mimi_tool_t gm = {
        .name = "gpio_mode",
        .description = "Configure the mode of a GPIO pin. Call before gpio_read or gpio_write.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"mode\":{\"type\":\"string\","
            "\"enum\":[\"input\",\"output\",\"input_pullup\",\"input_pulldown\"],"
            "\"description\":\"Pin mode\"}"
            "},\"required\":[\"pin\",\"mode\"]}",
        .execute = tool_gpio_mode_execute,
    };
    register_tool(&gm);

    /* Register wifi_scan */
    mimi_tool_t ws2 = {
        .name = "wifi_scan",
        .description = "Scan for nearby WiFi networks. Returns up to 10 access points "
                       "sorted by signal strength (RSSI), with SSID, channel, and auth type. "
                       "Useful for network diagnostics and connectivity troubleshooting.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_wifi_scan_execute,
    };
    register_tool(&ws2);

    /* Register rss_fetch */
    mimi_tool_t rss = {
        .name = "rss_fetch",
        .description = "Fetch and parse an RSS or Atom feed, returning the latest items as JSON. "
                       "Use this for news, blog updates, or any RSS/Atom source. "
                       "More efficient than web_search for structured news feeds.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"Full URL of the RSS/Atom feed\"},"
            "\"max_items\":{\"type\":\"integer\","
            "\"description\":\"Maximum items to return (1-10, default 5)\"}"
            "},\"required\":[\"url\"]}",
        .execute = tool_rss_execute,
    };
    register_tool(&rss);

    /* Register rule_create */
    mimi_tool_t rc = {
        .name = "rule_create",
        .description = "Create an automation rule that evaluates a condition every 60 seconds "
                       "and fires an action when the condition is met. "
                       "Conditions use local tools (device_temp, system_info). "
                       "Actions: telegram, email, ha (Home Assistant), or log.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short rule name\"},"
            "\"condition_tool\":{\"type\":\"string\","
            "\"description\":\"Tool to call for condition: device_temp, system_info\"},"
            "\"condition_field\":{\"type\":\"string\","
            "\"description\":\"Field name to extract from tool output\"},"
            "\"condition_op\":{\"type\":\"string\","
            "\"enum\":[\">\",\"<\",\"==\",\"!=\",\"contains\"],"
            "\"description\":\"Comparison operator\"},"
            "\"condition_value\":{\"type\":\"string\","
            "\"description\":\"Value to compare against\"},"
            "\"action_type\":{\"type\":\"string\","
            "\"enum\":[\"telegram\",\"email\",\"ha\",\"log\"],"
            "\"description\":\"Action to fire when condition is met\"},"
            "\"action_params\":{\"type\":\"string\","
            "\"description\":\"Action details (message text, email subject|body, HA endpoint, or log note)\"},"
            "\"cooldown_s\":{\"type\":\"integer\","
            "\"description\":\"Minimum seconds between re-firings (default: 300)\"}"
            "},\"required\":[\"name\",\"condition_tool\",\"condition_field\","
            "\"condition_op\",\"condition_value\",\"action_type\",\"action_params\"]}",
        .execute = tool_rule_create_execute,
    };
    register_tool(&rc);

    /* Register rule_list */
    mimi_tool_t rl = {
        .name = "rule_list",
        .description = "List all automation rules with their conditions, actions, and last-triggered times.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_rule_list_execute,
    };
    register_tool(&rl);

    /* Register rule_delete */
    mimi_tool_t rd = {
        .name = "rule_delete",
        .description = "Delete an automation rule by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"rule_id\":{\"type\":\"string\","
            "\"description\":\"The 8-character rule ID to delete\"}},"
            "\"required\":[\"rule_id\"]}",
        .execute = tool_rule_delete_execute,
    };
    register_tool(&rd);

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
