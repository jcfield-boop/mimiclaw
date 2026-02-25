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
        "## Memory\n"
        "- Before your final response: if you learned anything new about the user,\n"
        "  call edit_file to append it to /spiffs/memory/MEMORY.md.\n"
        "- Use write_file with append=true to add to daily notes (creates file if missing).\n"
        "- Use get_current_time to get today's date before writing daily notes.\n\n"
        "## Skills\n"
        "Skills are Markdown files in /spiffs/skills/. Read the skill file for full instructions.\n"
        "Create new skills with write_file to /spiffs/skills/<name>.md.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == ESP_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == ESP_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    /* Skills */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0) {
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n"
            "Available skills (use read_file to load full instructions):\n%s\n",
            skills_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}
