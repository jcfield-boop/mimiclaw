#include "skills/skill_loader.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read /spiffs/memory/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from /spiffs/config/USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "6. Before responding: call write_file with append=true to log to today's daily note\n" \
    "   at /spiffs/memory/<YYYY-MM-DD>.md (use the date from step 1).\n" \
    "   Content: \"## Daily Briefing\\n- <one sentence summary of key topics covered>\\n\"\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SELF_TEST \
    "# Self-Test\n" \
    "\n" \
    "Run a validation checklist and report pass/fail for each OpenClaw capability.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to run a self-test, system check, or validate that C6PO is working.\n" \
    "\n" \
    "## How to run\n" \
    "Run each check in order. Report PASS or FAIL for each.\n" \
    "\n" \
    "### T1 — Clock\n" \
    "Call get_current_time. PASS if returns a valid date/time.\n" \
    "\n" \
    "### T2 — Memory read\n" \
    "Call read_file on /spiffs/memory/MEMORY.md.\n" \
    "PASS if file exists and contains ## User, ## Preferences, ## Context sections.\n" \
    "FAIL if file not found or sections missing.\n" \
    "\n" \
    "### T3 — Daily note write\n" \
    "Call write_file with append=true on /spiffs/memory/<today>.md.\n" \
    "Content: '- [self-test T3] write ok\\n'\n" \
    "PASS if tool returns 'OK: appended'.\n" \
    "\n" \
    "### T4 — Daily note read-back\n" \
    "Call read_file on the same daily note path.\n" \
    "PASS if the content from T3 is present.\n" \
    "\n" \
    "### T5 — MEMORY.md edit\n" \
    "Call edit_file on /spiffs/memory/MEMORY.md:\n" \
    "  old_string: '## Context\\n\\n'\n" \
    "  new_string: '## Context\\n\\n- [self-test T5] edit ok\\n\\n'\n" \
    "PASS if tool returns 'OK: edited'.\n" \
    "Then call edit_file again to revert: swap old/new strings.\n" \
    "\n" \
    "### T6 — Web search\n" \
    "Call web_search with query 'ESP32 microcontroller'.\n" \
    "PASS if result is non-empty and contains relevant content.\n" \
    "FAIL if result is empty or contains 'Error'.\n" \
    "\n" \
    "### T7 — File list\n" \
    "Call list_dir with prefix /spiffs/skills/.\n" \
    "PASS if at least weather.md, daily-briefing.md, self-test.md are listed.\n" \
    "\n" \
    "## Output format\n" \
    "Report as a bullet list:\n" \
    "- T1 Clock: PASS/FAIL — <detail>\n" \
    "- T2 Memory read: PASS/FAIL — <detail>\n" \
    "... etc\n" \
    "End with: 'N/7 tests passed.'\n"

#define BUILTIN_EMAIL \
    "# Email\n" \
    "\n" \
    "Send an email via Gmail using the send_email tool.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to send an email, or when a skill needs to deliver a report by email\n" \
    "(e.g. daily briefing, flight alert, price watch result).\n" \
    "\n" \
    "## How to use\n" \
    "1. Identify subject and body from context\n" \
    "2. Call send_email with subject and body. Optionally pass 'to' to override the default recipient.\n" \
    "3. Report success or failure to the user.\n" \
    "   Do not mention credentials or internal details.\n" \
    "\n" \
    "## SERVICES.md format required\n" \
    "## Email\n" \
    "service: Gmail\n" \
    "smtp_host: smtp.gmail.com\n" \
    "smtp_port: 465\n" \
    "username: you@gmail.com\n" \
    "password: xxxx xxxx xxxx xxxx\n" \
    "from_address: C6PO <you@gmail.com>\n" \
    "to_address: you@gmail.com\n" \
    "\n" \
    "## Notes\n" \
    "- Requires a Gmail App Password (not your main password).\n" \
    "  Generate one at: myaccount.google.com/apppasswords\n" \
    "- 2-Step Verification must be enabled on the Gmail account.\n" \
    "- If credentials are missing or wrong, tell the user to check SERVICES.md.\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for C6PO.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `/spiffs/skills/<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"/spiffs/skills/translate.md\" content=\"# Translate\\n\\nTranslate text between languages.\\n\\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "email",          BUILTIN_EMAIL          },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
    { "self-test",      BUILTIN_SELF_TEST      },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static void install_builtin(const builtin_skill_t *skill)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s.md", MIMI_SKILLS_PREFIX, skill->filename);

    /* L3: Only write if missing — preserves user edits across firmware updates.
     * (Previously always overwrote; that discarded any customisation made via
     * the web editor or agent skill-creator tool.) */
    FILE *test = fopen(path, "r");
    if (test) {
        fclose(test);
        ESP_LOGD(TAG, "Built-in skill already present, keeping user version: %s", path);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write skill: %s", path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    ESP_LOGI(TAG, "Installed built-in skill: %s", path);
}

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    ESP_LOGI(TAG, "Skills system ready (%d built-in)", (int)NUM_BUILTINS);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

/**
 * Scan from current file position for a "## When to use" section.
 * Reads body lines (flattened) until the next ## section or EOF.
 */
static void extract_when_to_use(FILE *f, char *out, size_t out_size)
{
    char line[256];
    bool in_section = false;
    size_t off = 0;

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);
        /* Trim trailing whitespace */
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';

        if (!in_section) {
            if (len >= 2 && line[0] == '#' && line[1] == '#') {
                const char *h = line + 2;
                while (*h == ' ') h++;
                if (strncasecmp(h, "when to use", 11) == 0) in_section = true;
            }
        } else {
            if (len >= 2 && line[0] == '#' && line[1] == '#') break; /* next section */
            if (len == 0) continue;

            /* Separate multiple lines with a space */
            if (off > 0 && off < out_size - 2) out[off++] = ' ';

            size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
            memcpy(out + off, line, copy);
            off += copy;
        }
    }
    out[off] = '\0';
}

/* M9: soft cap to prevent overflowing the system-prompt slot for skills */
#define SKILL_SUMMARY_SOFT_CAP 1800

size_t skill_loader_build_summary(char *buf, size_t size)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS for skill enumeration");
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;
    /* SPIFFS readdir returns filenames relative to the mount point (e.g. "skills/weather.md").
       We match entries that start with "skills/" and end with ".md". */
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;

        /* Match files under skills/ with .md extension */
        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;

        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;  /* at least "skills/x.md" */
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[128];
        extract_description(f, desc, sizeof(desc));

        /* Scan remainder for ## When to use trigger conditions */
        char when[192];
        extract_when_to_use(f, when, sizeof(when));
        fclose(f);

        /* Append to summary — include trigger conditions so the LLM knows when to fire */
        if (when[0]) {
            off += snprintf(buf + off, size - off,
                "- **%s** [trigger: %s] → MUST call: read_file %s\n",
                title, when, full_path);
        } else {
            off += snprintf(buf + off, size - off,
                "- **%s**: %s → MUST call: read_file %s\n",
                title, desc, full_path);
        }

        /* M9: stop appending once we hit the soft cap to avoid overflowing
         * the system-prompt slot allocated for skills. */
        if (off >= SKILL_SUMMARY_SOFT_CAP) {
            off += snprintf(buf + off, size - off,
                "(additional skills available — use list_dir on %s to see all)\n",
                MIMI_SKILLS_PREFIX);
            break;
        }
    }

    closedir(dir);

    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills summary: %d bytes", (int)off);
    return off;
}
