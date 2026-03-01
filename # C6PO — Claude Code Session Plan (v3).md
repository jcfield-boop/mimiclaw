# C6PO — Claude Code Session Plan (v3)

*jcfield-boop/mimiclaw · ESP32-C6 · 2026-02-28*

Work through phases in order. Each phase is a self-contained Claude Code session —
complete it, verify success criteria on device, update baseline numbers, commit, move on.

Start each session: **“We’re working on C6PO phase N — read this file first”**
and paste this document so Claude Code has full context.

-----

## Definition of done — applies to every phase

Before moving to the next phase, confirm all three:

- [ ] Success criteria verified on device
- [ ] Baseline numbers updated in this file (where applicable)
- [ ] Commit pushed

-----

## Phase 0 — Baseline (~15 min)

**Goal:** Know your numbers before touching code. These drive decisions in every
subsequent phase — especially Phase 5 partition sizing.

```bash
cd ~/path/to/mimiclaw
idf.py set-target esp32c6
idf.py build          # clean build if needed
idf.py size           # binary size
idf.py monitor        # capture boot log
```

In the boot log, look for (or add temporarily to app_main if not present):

```c
ESP_LOGI(TAG, "Free heap:         %lu", esp_get_free_heap_size());
ESP_LOGI(TAG, "Min free heap:     %lu", esp_get_minimum_free_heap_size());
ESP_LOGI(TAG, "Largest free block:%lu", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
ESP_LOGI(TAG, "Free DMA heap:     %lu", heap_caps_get_free_size(MALLOC_CAP_DMA));
```

“Free heap” alone can lie — heavily fragmented heaps show high free totals but
fail large allocations. `largest_free_block` is the number that matters for
TLS and JSON buffer sizing.

Also add these to the existing `system_info` tool response so they’re
queryable at runtime throughout development.

Record here:

- Binary size: **1352 KB** (0x151f00 bytes; 34% free in 2MB factory partition)
- Free heap at boot: **366,620 bytes** (before WiFi/Telegram)
- Min free heap (after WiFi + Telegram up): _query `system_info` after connect_
- Largest free block (MALLOC_CAP_8BIT): **335,872 bytes** ← most important
- Free DMA heap: **352,016 bytes**
- SPIFFS used at boot: **310,989 / 1,799,921 bytes** (~17% used)

**Phase 0 success criteria:**

- All numbers recorded above
- Device boots, connects to WiFi, responds to a Telegram message
- Baseline committed (update this file or a BASELINE.md in the repo)

-----

## Phase 1 — Memory write: tool exposure + guardrails (~1 hour)

**Goal:** Agent persists memories via tool_use with rate limiting, typed payload
validation, and non-fatal error handling. Survives SPIFFS errors gracefully.

**Context for Claude Code:**
`memory_write_long_term()` and `memory_append_today()` exist but are only
reachable via serial CLI. The agent never calls them autonomously.
Wire them into the tool registry with guardrails.

### 1a. Tool schemas

```c
// Tool: memory_write
// {
//   "memory_type": "preference" | "fact" | "instruction",
//   "content":     string, maxLength 500,
//   "confidence":  number 0.0–1.0
// }
// All fields required.
//
// Tool: memory_append_today
// {
//   "content": string, maxLength 300
// }
// Required.
```

### 1b. Rate limiting

Use `esp_timer_get_time()` (monotonic microseconds), not `time(NULL)`.
Wall-clock can be unset or jump on embedded devices.

```c
// In tool dispatcher (tools.c or agent.c):
static int64_t s_last_memory_write_us = 0;
#define MEMORY_WRITE_COOLDOWN_US (60LL * 1000000LL)

// Per-turn flag — reset at the user message boundary (when a new inbound
// message is dequeued from the FreeRTOS queue), not at "agent turn" boundaries,
// because tool_use can occur multiple times inside one ReAct loop iteration
// over a single user message.
static bool s_memory_written_this_turn = false;
// Reset: call memory_reset_turn_flag() when dequeueing a new user message

if (tool == memory_write || tool == memory_append_today) {
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_memory_write_us < MEMORY_WRITE_COOLDOWN_US) {
        return tool_result_error("rate_limited",
            "Memory write cooldown active.");
    }
    if (s_memory_written_this_turn) {
        return tool_result_error("rate_limited",
            "Memory already written this turn.");
    }
    s_last_memory_write_us = now_us;
    s_memory_written_this_turn = true;
}
```

### 1c. Payload validation

Hard gate at confidence < 0.5 (reject with structured error).
Prompt policy says >= 0.7 — the gap between 0.5 and 0.7 is intentional:
model instruction sets the bar high; hard gate catches genuine junk.

```c
if (confidence < 0.5f) {
    return tool_result("{\"ok\":false,\"error\":\"low_confidence\","
                       "\"reason\":\"confidence %.2f below minimum 0.5\"}", confidence);
}
if (strlen(content) == 0 || is_whitespace_only(content)) {
    return tool_result_error("empty_content", "Content must not be empty.");
}
// memory_type enum check — belt-and-braces even if schema enforces it
```

### 1d. Non-fatal error returns

```c
esp_err_t err = memory_write_long_term(type, content);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "memory_write failed: %s", esp_err_to_name(err));
    return tool_result("{\"ok\":false,\"error\":\"write_failed\","
                       "\"reason\":\"%s\"}", esp_err_to_name(err));
}
return tool_result("{\"ok\":true,\"memory_type\":\"%s\"}", type);
```

Common failures to handle explicitly: `ESP_ERR_NO_MEM` (SPIFFS full),
file lock contention, path too long.

### 1e. System prompt guidance (SOUL.md or hardcoded preamble)

```
## Memory Policy
Use memory_write when the user explicitly shares a persistent preference,
fact, or standing instruction. Confidence >= 0.7 required. Do not write
inferences or guesses. Do not write more than once per conversation turn.
Use memory_append_today for significant events worth a daily log entry.
Both tools may be rejected — handle ok:false responses gracefully, do not retry.
```

### 1f. Test sequence

```
"Remember that I prefer metric units"         → ok:true, check Live Log
"Remember that I prefer imperial units"        → rate limited (same turn)
Wait 60s → "Remember I use a Neptune 3 Pro"   → ok:true
"Remember this" (vague)                        → model declines or low_confidence rejection
Fill SPIFFS (or mock ESP_ERR_NO_MEM)           → ok:false write_failed, no panic
Reboot → read MEMORY.md via web console        → verify persistence
```

**Phase 1 success criteria:**

- memory_write invoked via tool_use, confirmed in Live Log
- Per-turn rate limit fires on second write attempt
- 60s cooldown fires correctly (use esp_timer_get_time)
- Write survives reboot
- SPIFFS error returns ok:false to model, no panic/reboot

**Commit:** `feat: memory_write as agent tool with rate limiting and payload validation`

-----

## Phase 2 — Home Assistant tool (~1 hour)

**Goal:** Agent queries and controls HA via REST. TLS handled by URL scheme.
Sensitive endpoints blocked. Responses structured with truncation metadata.

**Context for Claude Code:**
Model on existing `http_request` tool in `main/tools.c`.
HA REST API: `Authorization: Bearer <token>`, base URL from SERVICES.md.

### 2a. Tool schema

```c
// Tool: ha_request
// {
//   "method":   "GET" | "POST",
//   "endpoint": "/api/states/light.living_room",  ← path only
//   "body":     "{...}"                           ← optional, "" if GET
// }
// Construct full URL as: ha_url + endpoint
// ha_url already contains scheme + host + port, endpoint begins with /api/
```

### 2b. TLS strategy — runtime, not compile-time

```c
// Detect from ha_url at SERVICES.md parse time:
// "http://"  → plain HTTP, no TLS config needed
// "https://" → TLS, set skip_cert_common_name_check = true for now
//
// TODO(security): cert verification skipped for LAN HTTPS.
// To harden: add "ha_cert_fingerprint: AA:BB:CC:..." to SERVICES.md
// and use esp_http_client_config_t.cert_pem or a pinned SHA-1 check.
```

### 2c. Blocked endpoints — prefix match, not exact

```c
static const char *HA_BLOCKED_PREFIXES[] = {
    "/api/config",          // exposes secrets and internal config
    "/api/services/hassio", // supervisor / add-on access
    "/api/states",          // bulk dump — too large, heap risk
                            // ↑ agent must use specific entity endpoint instead
    NULL
};

// Use strncmp(endpoint, HA_BLOCKED_PREFIXES[i], strlen(HA_BLOCKED_PREFIXES[i]))
// Catches /api/config, /api/config/, /api/config?foo etc.
// /api/states is blocked as a bulk call; /api/states/<entity_id> is allowed
// because it's longer than the prefix — adjust blocking logic accordingly:
// block exact "/api/states" but allow "/api/states/"
```

### 2d. Structured response

```c
// Success:
// {"ok":true,"status":200,"body":"...","truncated":false,"bytes":843}
// Truncated:
// {"ok":true,"status":200,"body":"...","truncated":true,"bytes":2048}
// HTTP error:
// {"ok":false,"status":401,"error":"unauthorized","bytes":0}
// Connection failure:
// {"ok":false,"status":0,"error":"connection_failed","reason":"<esp_err string>","bytes":0}
//
// Truncate body at 2048 bytes. Always include truncated + bytes fields.
```

### 2e. SERVICES.md additions

```markdown
## Home Assistant
ha_url: http://192.168.x.x:8123
ha_token: eyJ...your-long-lived-access-token
# ha_cert_fingerprint:   (future: SHA-1 hex for self-signed HTTPS cert)
```

Generate token: HA → Profile → Long-Lived Access Tokens → Create Token.

### 2f. Skill file `spiffs_data/skills/home-assistant.md`

```markdown
# Home Assistant Control

Use ha_request to control and query Home Assistant.

## Get a specific entity state
GET /api/states/light.living_room

## Turn on a light
POST /api/services/light/turn_on
Body: {"entity_id": "light.living_room"}

## Turn off a light
POST /api/services/light/turn_off
Body: {"entity_id": "light.living_room"}

## Set brightness (0–255)
POST /api/services/light/turn_on
Body: {"entity_id": "light.living_room", "brightness": 128}

## Trigger an automation
POST /api/services/automation/trigger
Body: {"entity_id": "automation.morning_routine"}

## Tips
- Always confirm before bulk or destructive actions
- Use /api/states/<entity_id> not /api/states (bulk call is blocked)
- Entity ID format: domain.name (light.x, switch.x, sensor.x, automation.x)
- Check ok:true and status:200 before reporting success to the user
- truncated:true means the response was cut — ask for a specific entity if needed
```

### 2g. Test sequence

```
"What's the state of light.living_room?"      → GET, ok:true, status:200
"Turn on the kitchen lights"                   → POST, verify in HA UI
"Turn off all lights"                          → model should confirm first
"Get all entity states" (/api/states)          → blocked, error returned
Wrong IP in SERVICES.md                        → ok:false, connection_failed
```

**Phase 2 success criteria:**

- Successful GET and POST end-to-end
- /api/states bulk call blocked (prefix match)
- Connection failure returns structured error, no crash
- truncated + bytes fields visible in Live Log

**Commit:** `feat: ha_request tool with endpoint blocking and structured errors`

-----

## Phase 3 — Klipper tool (~45 min)

**Goal:** Agent monitors and controls Klipper via Moonraker. Shares
implementation with Phase 2 via a common `lan_request()` helper.

**Context for Claude Code:**
Moonraker = Klipper’s HTTP API server, port 7125, plain HTTP on LAN.
Optional `X-Api-Key` header if auth is enabled.
Pattern is identical to Phase 2 — implement a shared `lan_request()` helper
to avoid duplicating esp_http_client setup.

### 3a. Shared `lan_request()` helper

```c
// lan_request(method, base_url, endpoint, token_header, token_value,
//             body, response_buf, response_buf_size, out_result)
//
// Rules:
// - All buffers caller-provided (stack or static) — no per-request malloc
// - token_header / token_value are NULL if unused (Moonraker without auth)
// - Returns lan_result_t: { ok, http_status, bytes, truncated, err_msg }
// - Uniform structured JSON builder used by both ha_request and klipper_request
//   so response shape is identical across all LAN tools
```

### 3b. Tool schema

```c
// Tool: klipper_request
// {
//   "method":   "GET" | "POST",
//   "endpoint": "/printer/objects/query?print_stats&extruder",
//   "body":     "{...}"  ← optional
// }
```

### 3c. SERVICES.md additions

```markdown
## Klipper / Moonraker
moonraker_url: http://192.168.x.x:7125
moonraker_apikey:   ← leave blank if auth not enabled
```

### 3d. Skill file `spiffs_data/skills/klipper.md`

```markdown
# Klipper 3D Printer Control

Use klipper_request to monitor and control the printer via Moonraker.

## Check printer status and temperatures
GET /printer/objects/query?print_stats&heater_bed&extruder

## Send a GCode command
POST /printer/gcode/script
Body: {"script": "G28"}

## Start a print
POST /printer/print/start
Body: {"filename": "myfile.gcode"}

## Cancel current print
POST /printer/print/cancel

## List gcode files
GET /server/files/list?root=gcodes

## Emergency stop
POST /printer/emergency_stop

## Tips
- Always confirm before starting or cancelling a print
- Print progress: print_stats.progress × 100 = percent complete
- Only send movement GCode when printer state is "ready" or "printing"
- heater_bed.target == 0 means bed heater is off
- Check ok:true before reporting success; truncated:true means partial response
```

### 3e. Test sequence

```
"What's the printer doing?"              → GET status + temperatures
"What temperature is the bed?"           → confirm value matches Mainsail/Fluidd
"List my gcode files"                    → GET file list
"Start a print" (no filename given)      → model asks which file
Printer off / wrong IP                   → ok:false, structured error
```

**Phase 3 success criteria:**

- Successful GET and POST end-to-end
- lan_request() shared with Phase 2 (no duplicated HTTP client setup)
- Structured error on connection failure

**Commit:** `feat: klipper_request tool with shared lan_request helper`

-----

## Phase 4 — History budget & compaction (~2 hours)

**Goal:** Session history bounded in RAM and SPIFFS regardless of conversation
length. Line-by-line SPIFFS reads — no bulk heap loads. Summaries tagged to
prevent recursive summarisation.

**Context for Claude Code:**
Sessions are JSONL in `/spiffs/sessions/<chat_id>.jsonl`.
Currently fully loaded into heap before each LLM call.
ReAct loop in `main/agent.c`, session cap currently 15 messages.

### 4a. Sliding window load (implement first, independent of compaction)

```c
#define SESSION_WINDOW_SIZE 6
#define SESSION_MAX_LINE_BYTES 512  // hard max per JSONL line; truncate safely if exceeded

// Read JSONL line-by-line using a fixed 512-byte line buffer (stack-allocated)
// Keep a ring of SESSION_WINDOW_SIZE line slots (6 × 512 = 3KB stack, acceptable)
// If a line exceeds SESSION_MAX_LINE_BYTES: truncate, null-terminate, mark truncated
//   in a flag so the compaction pass knows not to trust that line's content fully
// Build LLM messages array from ring contents only — never load full file
```

Validate sliding window before adding compaction. This alone bounds heap usage.

### 4b. Line-by-line compaction

Trigger when session file line count >= `SESSION_COMPACT_THRESHOLD` (10).
Count lines with a cheap pass (no parsing) before deciding to compact.

```c
#define SESSION_COMPACT_THRESHOLD  10
#define SESSION_COMPACT_TARGET      8   // try to summarise oldest N lines
#define SESSION_COMPACT_INPUT_CAP   4096 // hard byte cap on summarisation input

// Algorithm:
// 1. Open session JSONL, read line by line
// 2. Accumulate oldest lines into input buffer until:
//    - SESSION_COMPACT_TARGET lines read, OR
//    - input buffer reaches SESSION_COMPACT_INPUT_CAP bytes (stop early, ok)
// 3. Skip lines whose content field contains "[Summary @" substring
//    — detect by scanning content value, not raw line prefix:
//      parse enough JSON to find "content":"..." then check for "[Summary @"
//      (cheap scan: strstr on the content value, not the raw line)
//    — never summarise a summary
// 4. Call Claude haiku (no tools, max_tokens=120):
//    "Summarise in 2-3 sentences preserving key facts and decisions: <input>"
// 5. Write summary as:
//    {"role":"assistant","content":"[Summary @2026-02-28T14:30Z: ...]","ts":<epoch>}
// 6. Write summary + remaining (non-compacted) lines to .tmp file
// 7. Atomic commit: remove original, rename .tmp → original
```

### 4c. Hysteresis — one pass per turn, check after compaction

```c
static bool s_compacted_this_turn = false;
// Reset when new user message dequeued (same boundary as s_memory_written_this_turn)

// At start of agent turn:
if (!s_compacted_this_turn && session_line_count() >= SESSION_COMPACT_THRESHOLD) {
    session_compact(chat_id);
    s_compacted_this_turn = true;
    // Do NOT recheck threshold — one pass max regardless of result
}
```

### 4d. Power-loss recovery on boot

```c
// On boot, in session_init():
// Scan /spiffs/sessions/ for *.tmp files
// For each .tmp:
//   - If corresponding .jsonl exists → .tmp is a failed compaction, delete .tmp
//   - If no .jsonl → .tmp is the only copy, rename to .jsonl (partial recovery)
```

### 4e. Test sequence

```
Have a 12-message conversation
Read session file via web console — should see [Summary @...] + recent messages
Verify summary contains "[Summary @" and is not re-summarised next turn
Have another 10-message conversation — verify second compaction fires
run system_info tool at turn 5, 20, 50 — heap should stay flat after Phase 4
Power-pull during compaction → reboot → verify session is readable
```

**Phase 4 success criteria:**

- Peak heap during LLM call stays within documented bound across 50+ turns
- Session file never exceeds ~12 lines (compaction fires correctly)
- Summaries tagged with `[Summary @` and not re-summarised
- Boot recovery handles stale .tmp files without panic
- Line > 512 bytes truncated safely, not buffer-overflowed

**Commit:** `feat: session history budget and line-by-line compaction`

-----

## Phase 5 — Single-bank OTA (~1.5 hours)

**Prerequisite:** Binary size from Phase 0. Skip this phase if binary > 1800KB.
Binary size confirmed: **1352 KB** ✓ (proceed with Phase 5)

**Goal:** Firmware updates over WiFi via authenticated POST. SPIFFS preserved.
ROM USB bootloader is the recovery path — no automatic rollback.

### 5a. Partition arithmetic — work this out on paper first

⚠️ Fill in actual numbers before writing any code or CSV.

```
binary_size_bytes  = 1384192  (0x151f00, from idf.py size, Phase 0)
binary_size_kb     = 1352

ota_0_size_bytes   = ceil(1384192 × 1.3) = 1799450 → rounded up to 4KB = 1802240 (0x1B8000)
ota_0_size_kb      = 1760
ota_0_size_hex     = 0x1B8000

# Fixed anchor points for ESP32-C6:
# bootloader:  0x0000 – 0x8FFF  (do not touch)
# nvs:         0x9000, 24KB
# phy_init:    0xF000, 4KB
# Flash end:   0x400000 (4MB — verify this matches your module)

# Computed layout:
otadata_offset  = 0x10000       (fixed, ESP-IDF expects this)
otadata_size    = 0x2000        (8KB)
ota_0_offset    = 0x12000
ota_0_size      = 0x_____       (from above)
spiffs_offset   = ota_0_offset + ota_0_size  = 0x_____
coredump_offset = 0x3F0000      (64KB before end of 4MB)
spiffs_size     = coredump_offset - spiffs_offset = 0x_____ (must be >= 0x100000 = 1MB)
coredump_size   = 0x10000

# Verify:
# coredump_offset + coredump_size = 0x400000 ✓
# spiffs_size >= 0x100000 ✓
# ota_0_size > binary_size_bytes ✓
```

Record computed values:

- ota_0 size: _____ KB (0x_____)
- SPIFFS size: _____ KB (was 1966 KB)
- SPIFFS reduction: _____ KB lost

### 5b. `partitions_c6_ota.csv`

```csv
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x6000
phy_init,   data, phy,      0xF000,   0x1000
otadata,    data, ota,      0x10000,  0x2000
ota_0,      app,  ota_0,    0x12000,  0x_____   ← fill from worksheet
spiffs,     data, spiffs,   0x_____,  0x_____   ← fill from worksheet
coredump,   data, coredump, 0x3F0000, 0x10000
```

Verify with ESP-IDF partition tool before flashing:

```bash
python $IDF_PATH/components/partition_table/parttool.py \
  --partition-table-file partitions_c6_ota.csv \
  get_partition_info --partition-name ota_0
```

### 5c. sdkconfig.defaults.esp32c6 additions

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_c6_ota.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Note on rollback: with a single OTA slot, `APP_ROLLBACK_ENABLE` catches panics
before `esp_ota_mark_app_valid_cancel_rollback()` is called, but has nowhere to
roll back *to* — it will attempt the same image again. **This is not a safety net.**
Manual recovery via ROM bootloader is the real fallback. Document this clearly.

### 5d. OTA endpoint (`POST /ota`)

```c
// 1. Read ota_secret from SERVICES.md (reject with 401 if missing or wrong)
// 2. Check Content-Length header <= ota_0 partition size (reject with 413 if over)
// 3. esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &handle)
// 4. Stream request body in 4KB chunks → esp_ota_write(handle, chunk, len)
// 5. esp_ota_end(handle) — if returns error, return 400 {"ok":false,"error":"invalid_image"}
//    Do NOT call esp_ota_set_boot_partition() if end() fails
// 6. esp_ota_set_boot_partition(ota_partition)
// 7. Flush HTTP response 200 {"ok":true} before esp_restart()
// 8. Note: bind to LAN — do not port-forward this endpoint
```

### 5e. App validity marking in app_main

```c
// After WiFi connects successfully — mark image valid:
const esp_partition_t *running = esp_ota_get_running_partition();
esp_ota_img_states_t state;
if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA: image marked valid");
    }
}
```

### 5f. SERVICES.md additions

```markdown
## OTA
ota_secret: choose-a-strong-random-string-here
```

### 5g. `scripts/ota_flash.sh`

```bash
#!/bin/bash
set -e
IP=${1:?Usage: ota_flash.sh <device-ip>}
SECRET=${OTA_SECRET:?Set OTA_SECRET env var before running}
idf.py build
BINARY=build/mimiclaw.bin
BYTES=$(wc -c < "$BINARY")
echo "Uploading ${BYTES} bytes to http://$IP/ota ..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
  -X POST "http://$IP/ota" \
  -H "Authorization: Bearer $SECRET" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @"$BINARY")
if [ "$HTTP_CODE" = "200" ]; then
  echo "Upload accepted (HTTP 200) — device rebooting"
else
  echo "Upload failed (HTTP $HTTP_CODE)" && exit 1
fi
```

### 5h. First flash — write new partition table

```bash
idf.py fullclean && idf.py build
idf.py -p /dev/cu.usbmodem* flash     # full flash ONCE to write partition table
# All subsequent updates: OTA_SECRET=xxx ./scripts/ota_flash.sh <ip>
```

### 5i. Recovery procedure (add to README)

```
ROM bootloader recovery (when OTA produces unbootable firmware):
1. Hold GPIO9 (BOOT) low while pressing and releasing RESET
2. Release GPIO9
3. idf.py -p /dev/cu.usbmodem* flash
Note: this is the only recovery path — there is no automatic rollback.
```

### 5j. Test sequence

```
Run ota_flash.sh → verify reboot + Telegram response
Read MEMORY.md via web console → SPIFFS preserved ✓
Run ota_flash.sh again → second OTA succeeds (regression)
Try wrong OTA_SECRET → HTTP 401
Try truncated binary (head -c 10000 build/mimiclaw.bin > /tmp/bad.bin) → HTTP 400
```

**Phase 5 success criteria:**

- OTA works twice in a row
- SPIFFS content preserved across both updates
- Invalid image rejected (400) before boot partition switch
- Wrong secret returns 401
- Partition arithmetic recorded and verified with parttool.py

**Commit:** `feat: single-bank OTA with auth and image validation`

-----

## Phase 6 — LLM streaming (~2–3 hours, do last)

**Goal:** LLM responses chunk-by-chunk. ~7KB peak heap reduction.
tool_use works under streaming. Telegram edits throttled.

**Context for Claude Code:**
Current: POST → wait for complete 8KB response → parse.
Target: POST with `"stream":true` → HTTP_EVENT_ON_DATA → SSE reassembly
→ delta parsing → progressive Telegram delivery.

**Critical implementation order:** Build and unit-test the SSE reassembly layer
*before* wiring it to the LLM call. Chunks from HTTP_EVENT_ON_DATA do not align
with SSE line boundaries — this is where most streaming bugs live.

### 6a. SSE line reassembly

Buffer sized at 1024 (not 512 — tool_use deltas and future event types can be large):

```c
typedef struct {
    char   buf[1024];  // line reassembly — 1024 is safer than 512 for tool payloads
    size_t len;
} sse_state_t;
// Allocate once per streaming request, not per chunk

// In HTTP_EVENT_ON_DATA:
// Use memchr(data, '\n', len) to find line boundaries efficiently
// For each complete line found:
//   if strncmp(line, "data: ", 6) == 0 → pass line+6 to json_delta_parse()
//   if strcmp(line, "") == 0 → SSE frame separator, ignore
//   other prefixes (event:, id:, :comment) → ignore
// If line exceeds 1024 bytes: discard line, reset buf, log warning — don't crash
// Partial line at end of chunk: retain in buf, prepend to next chunk's data
```

### 6b. Delta JSON parsing — targeted fields only

Do not build a full cJSON DOM per delta. Use jsmn or manual strstr/sscanf:

```c
// Event types to handle:
// "content_block_delta" + delta.type=="text_delta"  → extract delta.text → accumulate
// "content_block_start" + content_block.type=="tool_use" → start tool input buffering,
//                                                           capture tool name + id
// "input_json_delta"                                → append delta.partial_json to tool buf
// "message_delta" + stop_reason=="end_turn"         → flush text, signal completion
// "message_delta" + stop_reason=="tool_use"         → execute tool with accumulated input
// "message_stop"                                    → streaming fully complete
// All other event types                             → log at DEBUG level, skip — never crash
```

### 6c. Tool input buffer under streaming

Tool JSON arrives fragmented. Use a heap-allocated buffer with a hard cap:

```c
#define TOOL_INPUT_BUF_MAX 2048  // covers typical tool calls; return error if exceeded

char *s_tool_input_buf = NULL;   // malloc on content_block_start type==tool_use
size_t s_tool_input_len = 0;

// On content_block_start: malloc(TOOL_INPUT_BUF_MAX), reset len
// On input_json_delta: strncat, check overflow
//   if len + new_len > TOOL_INPUT_BUF_MAX:
//     free buf, return structured error "tool_input_too_large" to continue loop
// On stop_reason==tool_use: execute tool, free buf, continue ReAct loop
// On any exit: always free buf if non-NULL
```

### 6d. Telegram progressive delivery

```c
#define TELEGRAM_EDIT_INTERVAL_MS  750
#define SENTENCE_END_CHARS         ".!?\n"  // flush on sentence end regardless of timer

static int64_t s_last_edit_us = 0;
static char    s_tg_accum[512];  // accumulated text pending edit
static size_t  s_tg_accum_len = 0;

// On each delta.text chunk:
//   append to s_tg_accum
//   int64_t now_us = esp_timer_get_time();
//   bool timer_fired    = (now_us - s_last_edit_us) >= (TELEGRAM_EDIT_INTERVAL_MS * 1000LL);
//   bool sentence_end   = strchr(SENTENCE_END_CHARS, last_char_of_chunk) != NULL;
//   if (timer_fired || sentence_end) → telegram_edit_message(s_tg_accum), update timer
// On end_turn: always flush remaining s_tg_accum (final edit)
```

750ms throttle = ~1.3 edits/sec, safely under Telegram’s 20 edits/minute per message.

### 6e. Reduce response buffer — after full validation only

```c
// Only after Phase 6 is fully stable:
// Reduce LLM_RESPONSE_BUF_SIZE from 8192 to 512
// Re-run Phase 0 heap metrics and record:
// - New largest free block: _____ bytes (was _____ bytes)
// - New min free heap: _____ bytes (was _____ bytes)
```

### 6f. Test sequence

```
Factual question → response streams progressively in Telegram, no spammy edits
Web_search question → tool executes, result incorporated, final answer streams
Long response (ask for a detailed explanation) → Telegram message updates smoothly
Rapid 3 messages → all complete without heap corruption
system_info before and after → record heap improvement
Unknown SSE event in response → logged, not crashed (inject manually to test)
```

**Phase 6 success criteria:**

- Peak heap measurably reduced — record before/after numbers here: _____ → _____
- tool_use (at least web_search) works correctly under streaming
- Telegram edit rate ≤ ~1.3/sec (no rate limit errors in Live Log)
- Unknown SSE event types logged and skipped, no crash
- tool_input_too_large returns structured error, doesn’t corrupt heap

**Commit:** `feat: streaming LLM, SSE reassembly, tool_use, throttled Telegram edits`

-----

## Claude Code session prompts

**Phase 0:**

> “C6PO — ESP32-C6 AI assistant, ESP-IDF C. Task: extend the boot log and
> system_info tool to report heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
> heap_caps_get_free_size(MALLOC_CAP_DMA), and min free heap alongside the
> existing free heap figure. I need these at boot and queryable at runtime.”

**Phase 1:**

> “C6PO — ESP32-C6, ESP-IDF C. Task: expose memory_write_long_term and
> memory_append_today as tool_use tools. Typed schema: {memory_type
> (preference|fact|instruction), content (max 500), confidence (0-1)}.
> Rate limiting: use esp_timer_get_time() (monotonic), cooldown 60s +
> once-per-user-message flag reset at inbound queue dequeue. Hard gate:
> confidence < 0.5 rejected. All failures non-fatal, return {ok, error}.
> System prompt guidance: confidence >= 0.7 required, no retries on rejection.”

**Phase 2:**

> “C6PO — ESP32-C6, ESP-IDF C. Task: add ha_request tool for HA REST API.
> Full URL = ha_url (from SERVICES.md) + endpoint (path only). TLS: detect
> from URL scheme at runtime; skip verify for https:// with TODO comment for
> cert pinning. Blocked endpoint prefixes (not exact match): /api/config,
> /api/services/hassio, /api/states (exact). Response: {ok, status, body,
> truncated, bytes}. Create spiffs_data/skills/home-assistant.md.”

**Phase 3:**

> “C6PO — ESP32-C6, ESP-IDF C. Task: add klipper_request tool for Moonraker
> (port 7125, plain HTTP). Extract shared lan_request() helper used by both
> ha_request and klipper_request — caller-provided buffers, no per-request malloc,
> uniform structured response shape. Reads moonraker_url, moonraker_apikey from
> SERVICES.md; omit X-Api-Key if blank. Create spiffs_data/skills/klipper.md.”

**Phase 4:**

> “C6PO — ESP32-C6, ESP-IDF C. Task: session history budget and compaction.
> Part 1: sliding window — read JSONL line-by-line with 512-byte max per line,
> keep only last 6 in a ring buffer, truncate oversized lines safely.
> Part 2: compaction — when session >= 10 lines, read oldest line-by-line with
> 4KB input cap, skip lines whose content contains ‘[Summary @’ (scan content
> field, not raw line), call haiku to summarise, tag as ‘[Summary @<ISO date>: …]’,
> write via .tmp + rename. One pass per user message turn, no recursive compaction.
> Boot: recover stale .tmp files on startup.”

**Phase 5:**

> “C6PO — ESP32-C6, ESP-IDF C. Binary size: ___KB (from Phase 0).
> Task: single-bank OTA. Work out partition offsets on paper first using the
> worksheet in the plan (ota_0 = ceil(binary × 1.3), verify end = 0x400000).
> Verify with parttool.py before any flash. /ota POST endpoint: auth via Bearer
> from SERVICES.md (401 if wrong), reject if Content-Length > partition size (413),
> validate with esp_ota_end() before switching boot (400 if invalid), flush 200
> before restart. Mark app valid after WiFi up. ota_flash.sh checks HTTP status.
> Document: single slot means manual ROM recovery only — no automatic rollback.”

**Phase 6:**

> “C6PO — ESP32-C6, ESP-IDF C. Task: streaming LLM via Anthropic SSE API.
> Build SSE line reassembly first (1024-byte buf, memchr for \n, handles chunks
> not aligning with line boundaries). Parse only needed event types with jsmn;
> unknown types logged and skipped. Tool input: heap-alloc 2048-byte buf on
> content_block_start, accumulate input_json_delta, execute on stop_reason==
> tool_use, always free. Telegram: accumulate text, edit every 750ms (monotonic)
> OR on sentence end (. ! ? \n), always flush on end_turn. Reduce
> LLM_RESPONSE_BUF_SIZE to 512 only after full validation.”

-----

## Quick reference

|Task                             |Command                                                                                                                                         |
|---------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|
|Build                            |`idf.py set-target esp32c6 && idf.py build`                                                                                                     |
|Flash app only (preserves SPIFFS)|`idf.py -p /dev/cu.usbmodem* app-flash`                                                                                                         |
|Full flash (wipes SPIFFS)        |`idf.py -p /dev/cu.usbmodem* flash`                                                                                                             |
|Serial monitor                   |`screen /dev/cu.usbmodem* 115200` (Ctrl-A K to exit)                                                                                            |
|OTA (Phase 5+)                   |`OTA_SECRET=xxx ./scripts/ota_flash.sh <device-ip>`                                                                                             |
|Verify partition table           |`python $IDF_PATH/components/partition_table/parttool.py --partition-table-file partitions_c6_ota.csv get_partition_info --partition-name ota_0`|
|ROM recovery                     |Hold GPIO9 (BOOT) low at reset, release, then `idf.py flash`                                                                                    |
|Heap check                       |Ask bot: “check your health”                                                                                                                    |
|Read SPIFFS files                |Web console → Files tab                                                                                                                         |

**Reference docs:**

- Anthropic streaming: https://docs.anthropic.com/en/api/messages-streaming
- Moonraker API: https://moonraker.readthedocs.io/en/latest/web_api/
- HA REST API: https://developers.home-assistant.io/docs/api/rest/
- ESP-IDF OTA: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/ota.html
- ESP-IDF heap caps: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/mem_alloc.html