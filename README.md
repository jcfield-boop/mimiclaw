# C6PO — ESP32-C6 AI Assistant

```
      ___
     (o  )~
    ( ___ )>      C6PO
     `---'
      |||         AI assistant on a chip
     ~~~~~
```

*Named after the water flea Daphnia — tiny, resilient, and surprisingly capable.*

C6PO is a personal AI assistant that runs entirely on an **ESP32-C6** microcontroller. It is a port of the [MimiClaw](https://github.com/memovai/mimiclaw) project, adapted for the ESP32-C6's 4MB flash and 512KB SRAM (no PSRAM required).

C6PO connects to Telegram and a browser-based web console, calls an LLM (Claude, OpenRouter, or any OpenAI-compatible API) to handle conversations, and uses tools — web search, file I/O, cron scheduling — to be genuinely useful.

---

## Hardware

| | |
|---|---|
| **Board** | ESP32-C6 (4MB flash) |
| **PSRAM** | Not required |
| **WiFi** | 2.4 GHz (802.11ax / WiFi 6) |

Tested on: ESP32-C6FH4 (revision v0.2).

---

## Features

- **Telegram bot** — send messages, get AI replies, full conversation history per chat
- **Web console** on port 80 — live activity log, file editors, skills manager, memory monitor
- **Chat input** in Live Log — send messages directly from the browser without leaving the log view
- **LLM providers** — Anthropic (Claude), OpenRouter (300+ models), or any OpenAI-compatible endpoint
- **Tool use** — web search (Tavily or Brave Search API), read/write/edit SPIFFS files, cron scheduling, generic HTTPS requests, Gmail SMTP email, chip temperature, SPIFFS grep, device health
- **Skills system** — teach the bot new capabilities via Markdown files; create/edit/delete from the browser
- **Session memory** — per-chat conversation history stored in SPIFFS
- **Long-term memory** — persistent MEMORY.md updated by the agent over time
- **Cron / heartbeat** — schedule recurring tasks and daily briefings
- **Verbose Logs** — toggle extra diagnostics (WiFi IP, full Telegram text, LLM heap/size) in Settings; persisted in NVS
- **Serial CLI** — configure everything over USB without reflashing

---

## Web Console

Browse to `http://<device-ip>` after it connects to WiFi (the IP is printed in the boot log).

> The web console is embedded directly in the firmware binary, so `app-flash` always delivers the latest UI without touching SPIFFS.

| Tab | Description |
|---|---|
| **Live Log** | Real-time stream of LLM calls, tool results, errors, and responses. Shows token counts, the actual model selected (useful with `openrouter/auto`), iteration counter per agent turn, and full tool call arguments (e.g. `read_file {"path":"/spiffs/memory/MEMORY.md"}`). Capped at 250 entries; use the ✕ button to clear. **Chat input bar** at the bottom lets you send messages directly from the browser. |
| **SOUL.md** | The bot's personality and values |
| **USER.md** | Notes about you — the bot reads this on every turn |
| **MEMORY.md** | Long-term memory written by the bot itself (auto-trimmed to 3 KB) |
| **HEARTBEAT.md** | Recurring task list — the bot checks this on a timer and acts on unchecked `- [ ]` items only |
| **SERVICES.md** | Third-party service credentials (email, flight APIs, etc.) — readable and editable from the browser |
| **Skills** | List, create, edit, and delete skill files |
| **Settings** | Set LLM provider/model/API key, Web Search API key (Tavily or Brave), and **Verbose Logs** toggle from the browser |

The header shows live free heap, SPIFFS usage, and session token counts (with cost estimate if using OpenRouter), refreshed every 15 seconds.

---

## Getting Started

### 1. Prerequisites

- An **ESP32-C6** board (4MB flash)
- [ESP-IDF v5.x or v6.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/) installed and sourced
- A **Telegram bot token** — create one via [@BotFather](https://t.me/botfather) on Telegram
- An **LLM API key** — [Anthropic](https://console.anthropic.com), [OpenRouter](https://openrouter.ai/settings/keys), or any OpenAI-compatible provider

### 2. Build & flash

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Replace `/dev/ttyUSB0` with your actual serial port (`/dev/cu.usbmodem*` on macOS).

### 3. Configure over serial (first-time only)

Connect at 115200 baud (e.g. `screen /dev/ttyUSB0 115200`) and run:

```
set_wifi <your-ssid> <your-password>
set_tg_token <your-telegram-bot-token>
set_model_provider openrouter
set_api_key <your-openrouter-key>
set_model openrouter/auto
restart
```

Get an OpenRouter key at [openrouter.ai/settings/keys](https://openrouter.ai/settings/keys) — it gives access to 300+ models. Anthropic and OpenAI keys also work; see the [OpenRouter](#openrouter) section below.

> **Note:** After first boot, all settings (API keys, provider, model, search key) can be changed from the web console **Settings** tab — no USB connection needed.

### 4. Find the device IP and open the Live Log

Check your router's DHCP table for the device IP, or read it from the serial log once:

```
I (1234) wifi: connected, IP: 192.168.x.x
```

Open `http://<device-ip>` in a browser to access the web console.

### 4a. Live Log — primary diagnostic

The **Live Log** tab is your main window into what the device is doing:

- Shows WiFi events, incoming Telegram messages, tool calls, LLM token counts, and errors
- Updates in real time from any browser on the same network — **no USB connection needed**
- **Chat input bar** at the bottom — type a message and press Enter (or ➤) to send it to the bot directly from the browser, without leaving the log view
- Use this for all ongoing diagnostics once the device is running headless

Enable **Verbose Logs** in the Settings tab to unlock extra detail: full WiFi IP on connect, complete incoming Telegram message text, LLM request/response sizes, and heap state before each allocation. Verbose mode is off by default (to keep the log readable) and persisted in NVS.

### 5. Personalise and enable web search

In the web console **Settings** tab you can:
- Set or change your LLM provider, model, and API key
- Set your **Web Search API key** to enable the `web_search` tool — [Tavily](https://tavily.com) (`tvly-...`) is recommended for AI agents; [Brave Search](https://brave.com/search/api/) also works

Then open the **SOUL.md** tab to edit the bot's personality, and **USER.md** to tell the bot about yourself.

### 6. Send your first message

Open Telegram, find your bot, and send it a message. You should get an AI reply within a few seconds. Token counts and cost appear in the web console header.

---

## Build & Flash

### Prerequisites

- [ESP-IDF v5.x or v6.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)

### Build

```bash
idf.py set-target esp32c6
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash
```

**Always use `app-flash` for firmware updates** — it flashes only the application binary and leaves the SPIFFS partition (and everything in it) untouched:

```bash
idf.py -p /dev/ttyUSB0 app-flash
```

> ⚠️ **Never run `idf.py flash` (full flash) on a running device.** It erases the entire SPIFFS partition — destroying MEMORY.md, daily notes, custom skills, and all agent-written files. MEMORY.md is the long-term brain of the device; losing it means C6PO forgets everything it has learned about you. Full flash is only appropriate for first-time setup on a blank device.

### Monitor boot (optional)

```bash
# idf.py monitor requires an interactive TTY; use screen instead:
screen /dev/ttyUSB0 115200
```

The device IP is printed once on boot. After that, use the **Live Log** tab in the web console for all diagnostics — no USB connection needed.

---

## Configuration

All settings are stored in NVS and survive firmware updates. Configure via the web console **Settings** tab (no USB required), or over USB serial:

```
# WiFi
set_wifi <SSID> <password>

# Telegram bot token (from @BotFather)
set_tg_token <token>

# LLM provider
set_model_provider anthropic          # or: openrouter, openai
set_api_key <your-api-key>
set_model claude-opus-4-5

# Optional: web search (Tavily key — tavily.com — or Brave Search key)
set_search_key <key>

# Show current config
config_show
```

---

## OpenRouter

C6PO defaults to [OpenRouter](https://openrouter.ai) — a single API key gives access to 300+ models including Claude, Gemini, GPT-4, and more.

```
set_model_provider openrouter
set_api_key sk-or-<your-key>
set_model openrouter/auto            # auto-selects best model
set_model anthropic/claude-sonnet-4-5
set_model google/gemini-2.0-flash
```

Get an API key at: https://openrouter.ai/settings/keys

You can also set this from the browser: open the web console **Settings** tab.

---

## Web Search

C6PO supports two search providers — set your key in the web console **Settings** tab or via serial CLI:

```
set_search_key <your-key>
```

The key is stored in NVS and survives firmware updates. If no key is set, the `web_search` tool is unavailable and the agent will say so.

### Tavily (recommended)

[Tavily](https://tavily.com) is purpose-built for AI agents and returns clean, structured results with an AI-generated answer. Keys start with `tvly-`.

- Generous free tier for development
- Returns a direct answer + up to 5 source snippets
- Automatically detected by the `tvly-` prefix — no extra configuration needed

### Brave Search

[Brave Search API](https://brave.com/search/api/) is also supported. If your key has access to the Answers/Summarizer plan, C6PO uses the two-step summarizer flow automatically. Free-tier keys fall back to formatting raw web results.

---

## Tools: http_request and send_email

### http_request
Generic HTTPS GET/POST — the agent can call any REST API, webhook, or service. The agent reads credentials from SERVICES.md, builds the request, and calls the tool.

Returns `HTTP <status>\n<response body>`. Response bodies are capped at 4 KB.

### send_email
Sends email via Gmail SMTP/TLS (port 465) using a Gmail App Password. Credentials are read directly from SERVICES.md and never appear in the LLM context window.

Add your credentials to the SERVICES.md tab in the web console:

```markdown
## Email
service: Gmail
smtp_host: smtp.gmail.com
smtp_port: 465
username: you@gmail.com
password: xxxx xxxx xxxx xxxx
from_address: C6PO <you@gmail.com>
to_address: you@gmail.com
```

Generate an App Password at [myaccount.google.com/apppasswords](https://myaccount.google.com/apppasswords) (requires 2-Step Verification). Then ask C6PO: *"send me a test email"*.

---

## Tools: device_temp, search_files, system_info

### device_temp
Reads the ESP32-C6's internal temperature sensor and returns the chip temperature in Celsius. No external hardware required.

```
Ask: "What is your temperature?"
→ Device temperature: 42.3°C (ESP32-C6 internal sensor)
```

### search_files
Case-insensitive grep across all SPIFFS files — searches memory, daily notes, and skills without reading each file individually. Supports an optional path prefix to limit scope.

```
Ask: "Search my notes for flight"
→ /spiffs/memory/2026-02-20.md:3: - BA2490 booked, departs 07:10
```

- Results capped at 20 matches
- Lines truncated to 120 characters
- Skips `console/` and `sessions/` (too large / not useful)

### system_info
Returns live device health: free heap, SPIFFS usage, uptime, WiFi RSSI, and firmware version. Useful for self-diagnostics and daily briefings.

```
Ask: "Check your health"
→ Free heap: 221480 bytes (min: 201320)
  SPIFFS: 57344 / 1798144 bytes used (3%)
  Uptime: 4h 12m
  WiFi RSSI: -61 dBm
  Firmware: 6288bad (built Feb 24 2026 13:44:00)
```

---

## Credential Storage

Service credentials (email API keys, flight tracker tokens, etc.) are stored in `/spiffs/config/SERVICES.md` — a plain Markdown file on SPIFFS. The agent reads it only when a skill requires it and is instructed never to quote or repeat any credential value in a response.

To set up credentials, ask C6PO via Telegram:

> "Create /spiffs/config/SERVICES.md with my Mailjet credentials: api_key abc123, from me@example.com"

The file survives `app-flash` updates. A template with common sections (Email, Flights, Custom) is provided at `spiffs_data/config/SERVICES.md` in the repo.

> **Security note:** Credentials are stored as plaintext in SPIFFS flash. For a personal device on a trusted network this is acceptable — use app-specific passwords or service API tokens rather than primary account passwords, so any leaked key can be revoked independently.

---

## Skills

Skills are Markdown files at `/spiffs/skills/<name>.md`. The agent reads them as part of its system prompt so it knows what capabilities are available and how to use them.

Four built-in skills are installed and kept up to date on every boot:

| Skill | Purpose |
|---|---|
| `weather` | Get current weather and forecasts via web search |
| `daily-briefing` | Compile a personalised morning update |
| `email` | Send email via Gmail SMTP using an App Password from SERVICES.md |
| `skill-creator` | Teach the bot new capabilities by writing skill files |
| `self-test` | Run a 7-point validation checklist (clock, memory read/write, edit, web search, file list) |

Create new skills from the **Skills tab** in the web console, or just ask the bot:

> "Create a skill that translates text to French"

> **Note:** Built-in skills are always refreshed on boot so firmware updates take effect immediately. Custom skills live in SPIFFS and survive `app-flash` updates — only a full `idf.py flash` erases them.

---

## Memory Management

C6PO automatically manages its storage to stay within the 1.9 MB SPIFFS limit:

| File | Behaviour |
|---|---|
| `SOUL.md`, `USER.md`, `HEARTBEAT.md` | Survive `app-flash` updates; only created from defaults on first boot |
| `SERVICES.md` | Third-party service credentials (email, flight APIs, etc.) — read by the agent only when a skill requires it; never quoted in responses |
| `MEMORY.md` | The device's long-term brain — survives power cycles and `app-flash`; **erased by full `idf.py flash`** |
| `memory/YYYY-MM-DD.md` | Daily notes — survive power cycles; older than 7 days deleted on boot |
| Sessions (`sessions/*.json`) | Each chat capped at 15 messages; use `session_clear <id>` to reset |
| Skills (`skills/*.md`) | Built-ins refreshed on every boot; custom skills survive `app-flash` |

---

## Serial CLI Reference (development / optional)

The serial CLI is useful for first-time setup and development. All settings can also be configured from the web console **Settings** tab once WiFi is connected.

Connect at 115200 baud and type `help` for the full command list. Key commands:

| Command | Description |
|---|---|
| `set_wifi <ssid> <pass>` | Configure WiFi |
| `set_tg_token <token>` | Set Telegram bot token |
| `set_model_provider <p>` | Set LLM provider (`anthropic`, `openrouter`, `openai`) |
| `set_api_key <key>` | Set LLM API key |
| `set_model <model>` | Set model name |
| `set_search_key <key>` | Set Brave Search API key |
| `config_show` | Show all current settings |
| `wifi_status` | Show WiFi connection info |
| `wifi_scan` | Scan for nearby networks |
| `heap_info` | Show free heap and minimum heap |
| `session_list` | List active chat sessions |
| `session_clear <id>` | Clear conversation history for a chat |
| `skill_list` | List installed skills |
| `restart` | Reboot the device |

---

## Partition Layout (4MB flash)

```
nvs       0x9000   24KB   NVS config
phy_init  0xf000    4KB   RF calibration
factory  0x10000    2MB   Firmware
spiffs  0x210000  1.9MB   Files (web console, config, skills, sessions)
coredump 0x3f0000  64KB   Crash dumps
```

No OTA — firmware updates are via USB only.

---

## Memory at a glance

| Resource | Value |
|---|---|
| Free heap at boot | ~369 KB |
| SPIFFS | 1.9 MB |
| Max session history | 15 messages |
| LLM response buffer | 8 KB |

---

## Differences from MimiClaw (ESP32-S3)

| | MimiClaw (S3) | C6PO (C6) |
|---|---|---|
| Flash | 16 MB | 4 MB |
| PSRAM | 8 MB | None |
| Cores | 2 | 1 |
| OTA updates | Yes | No (USB only) |
| IMU | Yes | Disabled |
| LLM buffer | 32 KB | 8 KB |
| Session history | 20 msgs | 15 msgs |

---

## License

MIT — see [LICENSE](LICENSE)

Based on [MimiClaw](https://github.com/memovai/mimiclaw) by memovai.
