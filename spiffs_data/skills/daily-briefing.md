# Daily Briefing

Compile a personalized daily briefing for the user.

## When to use
When the user asks for a daily briefing, morning update, or "what's new today".
Also useful as a heartbeat/cron task.

## How to use
1. Use get_current_time for today's date
2. Read /spiffs/memory/MEMORY.md for user preferences and context
3. Read today's daily note if it exists
4. Use web_search for relevant news based on user interests
5. Compile a concise briefing covering:
   - Date and time
   - Weather (if location known from /spiffs/config/USER.md)
   - Relevant news/updates based on user interests
   - Any pending tasks from memory
   - Any scheduled cron jobs
6. Before responding: call write_file with append=true to log to today's daily note
   at /spiffs/memory/<YYYY-MM-DD>.md (use the date from step 1).
   Content: "## Daily Briefing\n- <one sentence summary of key topics covered>\n"

## Format
Keep it brief — 5-10 bullet points max. Use the user's preferred language.
