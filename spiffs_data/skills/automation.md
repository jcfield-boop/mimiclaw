# Automation Rules

Create lightweight automation rules that run every 60 seconds without LLM tokens.
Rules evaluate a condition (from a tool call) and fire an action when met.

## When to use
When the user wants automatic monitoring or triggers:
- "Alert me when chip temperature exceeds 70°C"
- "Log a warning when free heap drops below 50KB"
- "Turn on the HA light when door sensor opens"
- "Send email if printer goes offline"

## Available tools
- `rule_create` — create a new rule
- `rule_list` — list all rules with status and last-triggered times
- `rule_delete` — remove a rule by its ID

## Rule structure

| Field | Values | Notes |
|-------|--------|-------|
| name | string | short descriptive name |
| condition_tool | "device_temp", "system_info", "ha_request" | tool to call |
| condition_field | string | field to extract from tool output |
| condition_op | ">", "<", "==", "!=", "contains" | comparison |
| condition_value | string | value to compare against |
| action_type | "telegram", "email", "ha", "log" | what to do when triggered |
| action_params | string | action details (see below) |
| cooldown_s | integer | min seconds between re-firings (default: 300) |

## Condition tools
Only these zero-cost local tools are supported as condition sources:
- `device_temp` — chip temperature, extract field: **Device temperature** (e.g. "45.2")
- `system_info` — system stats, extract fields: **Free heap**, **Uptime**, **WiFi** (RSSI)
- `ha_request` — Home Assistant entity state, extract field: **state**

## Action types

**telegram** — action_params = message text to send via the default chat
```json
{"action_type": "telegram", "action_params": "⚠️ High temperature alert!"}
```

**email** — action_params = "subject|body" (pipe-separated)
```json
{"action_type": "email", "action_params": "Alert: high temp|Chip is overheating"}
```

**ha** — action_params = "METHOD /api/endpoint [json_body]"
```json
{"action_type": "ha", "action_params": "POST /api/services/light/turn_on {\"entity_id\":\"light.desk\"}"}
```

**log** — action_params = note text, appended to today's daily note
```json
{"action_type": "log", "action_params": "low heap warning detected"}
```

## Examples

### High temperature alert (Telegram)
```
rule_create:
  name: "high_temp"
  condition_tool: "device_temp"
  condition_field: "Device temperature"
  condition_op: ">"
  condition_value: "65"
  action_type: "telegram"
  action_params: "⚠️ Chip temp critical!"
  cooldown_s: 600
```

### Low heap warning (log)
```
rule_create:
  name: "low_heap"
  condition_tool: "system_info"
  condition_field: "Free heap"
  condition_op: "<"
  condition_value: "50000"
  action_type: "log"
  action_params: "low heap detected"
  cooldown_s: 300
```

### HA door open alert (Telegram)
```
rule_create:
  name: "door_open"
  condition_tool: "ha_request"
  condition_field: "state"
  condition_op: "=="
  condition_value: "on"
  action_type: "telegram"
  action_params: "🚪 Door sensor is open"
  cooldown_s: 1800
```
Note: for ha_request conditions, pass the entity endpoint in action_params of
the condition_tool call — this currently uses the default tool invocation with `{}`.
For HA conditions, prefer using a cron-based agent approach until ha_request
condition support is extended.

## Limits
- Maximum 8 rules total
- Rules evaluate every 60 seconds
- Cooldown prevents repeated firing (default: 5 minutes)
- Only local/LAN tools as conditions (no web_search or HTTP — too slow)

## Workflow
1. Think about what to monitor and what should happen
2. Choose condition_tool and identify the exact field name in its output
   (run the tool manually first to see the exact output format)
3. Call rule_create with all required fields
4. Wait one evaluation cycle (60s), then call rule_list to confirm it loaded
5. Set a test condition (e.g. low threshold) to verify it fires
