# C6PO — ESP32-C6 AI Assistant

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
- **Tool use** — web search, read/write/edit SPIFFS files, cron scheduling
- **Skills system** — teach the bot new capabilities via Markdown files; create/edit/delete from the browser
- **Session memory** — per-chat conversation history stored in SPIFFS
- **Long-term memory** — persistent MEMORY.md updated by the agent over time
- **Cron / heartbeat** — schedule recurring tasks and daily briefings
- **Serial CLI** — configure everything over USB without reflashing

---

## Web Console

Browse to `http://c6po.local` (or the device IP) after it connects to WiFi.

| Tab | Description |
|---|---|
| **Live Log** | WebSocket stream of LLM calls, tool executions, and responses in real time |
| **SOUL.md** | The bot's personality and values |
| **USER.md** | Notes about you — the bot reads this on every turn |
| **MEMORY.md** | Long-term memory written by the bot itself |
| **Skills** | List, create, edit, and delete skill files |

The header shows live free heap and SPIFFS usage, refreshed every 15 seconds.

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

This flashes firmware and SPIFFS (web console + default config files) in one step.

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
set_provider anthropic          # or: openrouter, openai
set_api_key <your-api-key>
set_model claude-opus-4-5

# Optional: web search (Serper API key)
set_search_key <key>

# Show current config
config_show
```

---

## OpenRouter

C6PO supports [OpenRouter](https://openrouter.ai) as a drop-in provider, giving access to 300+ models through a single API key.

```
set_provider openrouter
set_api_key sk-or-<your-key>
set_model openrouter/auto            # auto-selects best model
set_model anthropic/claude-sonnet-4-5
set_model google/gemini-2.0-flash
```

Get an API key at: https://openrouter.ai/settings/keys

---

## Skills

Skills are Markdown files at `/spiffs/skills/<name>.md`. The agent reads them as part of its system prompt so it knows what capabilities are available and how to use them.

Three built-in skills are installed on first boot: **weather**, **daily-briefing**, and **skill-creator**.

Create new skills from the **Skills tab** in the web console, or just ask the bot:

> "Create a skill that translates text to French"

Skills live in SPIFFS and survive firmware updates.

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
