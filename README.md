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
- **LLM providers** — Anthropic (Claude), OpenRouter (300+ models), or any OpenAI-compatible endpoint
- **Tool use** — web search (Brave Search API), read/write/edit SPIFFS files, cron scheduling
- **Skills system** — teach the bot new capabilities via Markdown files; create/edit/delete from the browser
- **Session memory** — per-chat conversation history stored in SPIFFS
- **Long-term memory** — persistent MEMORY.md updated by the agent over time
- **Cron / heartbeat** — schedule recurring tasks and daily briefings
- **Serial CLI** — configure everything over USB without reflashing

---

## Web Console

Browse to `http://<device-ip>` after it connects to WiFi (the IP is printed in the boot log).

| Tab | Description |
|---|---|
| **Live Log** | WebSocket stream of LLM calls, tool executions, and responses in real time |
| **SOUL.md** | The bot's personality and values |
| **USER.md** | Notes about you — the bot reads this on every turn |
| **MEMORY.md** | Long-term memory written by the bot itself |
| **Skills** | List, create, edit, and delete skill files |
| **HEARTBEAT.md** | Recurring task list — the bot checks this on a timer and acts on uncompleted items |
| **Settings** | Set LLM provider/model/API key and Brave Search key from the browser |

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

### 3. Configure over serial

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

### 4. Verify boot

After restart, the serial log will print the device IP address once WiFi connects:

```
I (1234) wifi: connected, IP: 192.168.x.x
```

Open `http://<device-ip>` in a browser to access the web console.

### 5. Personalise and enable web search

In the web console **Settings** tab you can:
- Set or change your LLM provider, model, and API key
- Set your **Brave Search** API key (get one free at [brave.com/search/api](https://brave.com/search/api/)) to enable the `web_search` tool

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

This flashes firmware and SPIFFS (web console + default skill files) in one step.

> **Tip:** To preserve user-edited files (SOUL.md, USER.md, HEARTBEAT.md, custom skills, MEMORY.md) when updating firmware, use `idf.py app-flash` instead — it flashes only the application binary and leaves the SPIFFS partition untouched:
> ```bash
> idf.py -p /dev/ttyUSB0 app-flash
> ```
>
> **Note:** `idf.py flash` (full flash) re-initialises the entire SPIFFS partition. **SOUL.md, USER.md, and HEARTBEAT.md are re-created from defaults only if they are missing**, so they survive a full flash once they have been created. Custom skills and MEMORY.md are reset on full flash.

### Monitor boot

```bash
# idf.py monitor requires an interactive TTY; use screen instead:
screen /dev/ttyUSB0 115200
```

---

## Configuration

All settings are stored in NVS and survive firmware updates. Configure over USB serial:

```
# WiFi
set_wifi <SSID> <password>

# Telegram bot token (from @BotFather)
set_tg_token <token>

# LLM provider
set_model_provider anthropic          # or: openrouter, openai
set_api_key <your-api-key>
set_model claude-opus-4-5

# Optional: web search (Brave Search API key — see brave.com/search/api)
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

Web search uses the [Brave Search API](https://brave.com/search/api/) (free tier: 2,000 queries/month).

```
set_search_key <your-brave-search-key>
```

Or set it from the browser in the web console **Settings** tab. The key is stored in NVS and survives firmware updates. If the key is not set, the `web_search` tool will be unavailable and the agent will say so.

---

## Skills

Skills are Markdown files at `/spiffs/skills/<name>.md`. The agent reads them as part of its system prompt so it knows what capabilities are available and how to use them.

Three built-in skills are installed on first boot: **weather**, **daily-briefing**, and **skill-creator**.

Create new skills from the **Skills tab** in the web console, or just ask the bot:

> "Create a skill that translates text to French"

> **Note:** Skills are stored in SPIFFS. Custom skills are reset by `idf.py flash` (full flash). Use `idf.py app-flash` to update firmware without touching SPIFFS.

---

## Serial CLI Reference

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
