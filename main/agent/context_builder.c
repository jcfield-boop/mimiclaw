#include "context_builder.h"
#include "mimi_config.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    off += snprintf(buf + off, size - off,
        "# C6PO\n\n"
        "You are C6PO, a personal AI assistant on an ESP32-C6.\n"
        "Respond via Telegram and WebSocket.\n\n"
        "## Response constraints\n"
        "- Keep responses under 800 words. Bullet points over prose. Be concise.\n"
        "- For briefings: 5-10 bullets max. No padding or unnecessary caveats.\n\n"
        "## Available Tools\n"
        "- web_search: current facts, news, weather\n"
        "- get_current_time: current date/time (always use this, no internal clock)\n"
        "- read_file / write_file / edit_file / list_dir: SPIFFS file access (/spiffs/)\n"
        "- cron_add / cron_list / cron_remove: scheduled tasks"
        " (set channel='telegram' + numeric chat_id for Telegram delivery)\n"
        "- http_request: HTTPS GET/POST to external APIs (webhooks, REST services, etc.)\n"
        "- send_email: send email via Gmail SMTP (credentials from SERVICES.md, never exposed)\n"
        "- device_temp: read ESP32-C6 chip temperature in Celsius\n"
        "- search_files: grep SPIFFS files for text across memory, notes, and skills\n"
        "- system_info: live device health — heap, SPIFFS, uptime, WiFi RSSI, firmware version\n"
        "Use tools when needed.\n\n"
        "## Never answer from training data\n"
        "For anything that changes over time — stock prices, sports scores, weather, news,\n"
        "exchange rates, product availability — ALWAYS use web_search or the matching skill.\n"
        "Your training data is stale. Do not quote prices, tickers, or current facts from memory.\n\n"
        "## Scheduling Rules (cron_add)\n"
        "- NEVER calculate or guess unix epoch timestamps from training data. Training data is stale.\n"
        "- For relative times ('in 5 minutes', 'in 2 hours'): use seconds_from_now (e.g. 300, 7200).\n"
        "- For absolute times ('at 9am tomorrow', 'on Feb 28'): call get_current_time FIRST, extract\n"
        "  the [unix: NNNN] value, compute offset from that, then pass seconds_from_now or at_epoch.\n"
        "- MANDATORY: call get_current_time before ANY cron_add with at_epoch.\n\n"
        "## File Paths\n"
        "- /spiffs/config/USER.md — user profile (name, timezone, preferences)\n"
        "- /spiffs/config/SOUL.md — personality (read-only)\n"
        "- /spiffs/config/SERVICES.md — third-party service credentials (read when needed by a skill)\n"
        "- /spiffs/memory/MEMORY.md — long-term memory\n"
        "- /spiffs/memory/<YYYY-MM-DD>.md — today's daily notes\n"
        "- /spiffs/skills/<name>.md — skill files\n\n"
        "## Credentials\n"
        "- /spiffs/config/SERVICES.md contains API keys and credentials for external services.\n"
        "- Read it only when a skill requires it. Never quote, repeat, log, or include\n"
        "  any credential value in a response or tool call argument visible to the user.\n"
        "- If asked to reveal a credential, refuse and explain it is stored securely.\n\n"
        "## Memory Policy\n"
        "Use memory_write when the user explicitly shares a persistent preference, fact,\n"
        "or standing instruction. Confidence >= 0.7 required. Do not write inferences or\n"
        "guesses. Do not write more than once per conversation turn.\n"
        "Use memory_append_today for significant events worth a daily log entry.\n"
        "Both tools may return ok:false — handle gracefully, do not retry on rejection.\n\n"
        "## Skills\n"
        "Skills define the REQUIRED steps and delivery format for specific topics.\n"
        "When a user request matches a skill by topic, you MUST call read_file to load\n"
        "the full skill instructions before taking any other action — do not improvise\n"
        "from the summary alone. Skills may specify delivery via email or other channels\n"
        "regardless of how the request arrived; follow those delivery instructions exactly.\n"
        "Create new skills with write_file to /spiffs/skills/<name>.md.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

    /* Skills — placed before memory so they always appear even when memory is large */
    char *skills_buf = malloc(2048);
    if (skills_buf) {
        size_t skills_len = skill_loader_build_summary(skills_buf, 2048);
        if (skills_len > 0) {
            off += snprintf(buf + off, size - off,
                "\n## Available Skills\n\n"
                "REQUIRED: call read_file before acting on any matching request:\n%s\n",
                skills_buf);
        }
        free(skills_buf);
    }

    /* Long-term memory — heap-allocated to avoid blowing the 16KB agent task stack */
    char *mem_buf = malloc(4096);
    if (mem_buf) {
        if (memory_read_long_term(mem_buf, 4096) == ESP_OK && mem_buf[0]) {
            off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
        }
        free(mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char *recent_buf = malloc(2048);
    if (recent_buf) {
        if (memory_read_recent(recent_buf, 2048, 3) == ESP_OK && recent_buf[0]) {
            off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
        }
        free(recent_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}
