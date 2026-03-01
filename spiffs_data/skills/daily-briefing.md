# Daily Briefing

Compile a personalized daily briefing for the user.

## When to use
When the user asks for a daily briefing, morning update, or "what's new today".
Also useful as a heartbeat/cron task.

## How to use
1. Use get_current_time for today's date
2. Read /spiffs/memory/MEMORY.md for user preferences and context
3. Read today's daily note if it exists
4. Gather news — prefer rss_fetch over web_search where possible to save API credits:
   - Tech: `rss_fetch https://hnrss.org/frontpage max_items=5`  (Hacker News)
   - ARM/embedded: `rss_fetch https://newsroom.arm.com/rss max_items=3`
   - General: `rss_fetch https://feeds.reuters.com/reuters/technologyNews max_items=3`
   - Use web_search only for topics with no suitable feed, or for weather/stock queries
5. Compile a concise briefing covering:
   - Date and time
   - Weather (if location known from /spiffs/config/USER.md — use web_search)
   - Top news/updates from feeds above, filtered to user interests
   - Any pending tasks from memory
   - Any scheduled cron jobs
6. Before responding: call write_file with append=true to log to today's daily note
   at /spiffs/memory/<YYYY-MM-DD>.md (use the date from step 1).
   Content: "## Daily Briefing\n- <one sentence summary of key topics covered>\n"

## Feed sources (curated)
| Feed | URL | Use for |
|------|-----|---------|
| Hacker News | https://hnrss.org/frontpage | Tech + startups |
| ARM Newsroom | https://newsroom.arm.com/rss | Embedded + SoC news |
| Reuters Tech | https://feeds.reuters.com/reuters/technologyNews | Mainstream tech |
| BBC Tech | http://feeds.bbci.co.uk/news/technology/rss.xml | UK tech news |

## Format
Keep it brief — 5-10 bullet points max. Use the user's preferred language.

## Cost note
Using rss_fetch instead of web_search for news saves ~3 Brave Search API credits
per daily briefing. Reserve web_search for queries needing current/precise data
(weather, stock prices, live events).
