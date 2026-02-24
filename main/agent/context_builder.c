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
        "Use tools when needed.\n\n"
        "## Memory\n"
        "Persistent: MEMORY.md (long-term), daily/<YYYY-MM-DD>.md (notes).\n"
        "- Read MEMORY.md before writing; use edit_file to update, not overwrite.\n"
        "- Proactively save user facts and preferences without being asked.\n"
        "- Use get_current_time before writing daily notes.\n\n"
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
