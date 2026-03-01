# Home Assistant Control

## When to use
User asks to control lights, switches, climate, scenes, automations, or check
entity states in their home. Also use when asked about home device status.

## How to use
Use ha_request. Credentials are loaded automatically from SERVICES.md.

### Get entity state
GET /api/states/light.living_room

### Turn on a light
POST /api/services/light/turn_on
Body: {"entity_id": "light.living_room"}

### Turn off a light
POST /api/services/light/turn_off
Body: {"entity_id": "light.living_room"}

### Set brightness (0–255)
POST /api/services/light/turn_on
Body: {"entity_id": "light.living_room", "brightness": 128}

### Toggle a switch
POST /api/services/switch/toggle
Body: {"entity_id": "switch.garden"}

### Trigger an automation
POST /api/services/automation/trigger
Body: {"entity_id": "automation.morning_routine"}

### Set climate temperature
POST /api/services/climate/set_temperature
Body: {"entity_id": "climate.living_room", "temperature": 21}

## Tips
- Always confirm before bulk or destructive actions ("turn off all lights")
- Use /api/states/<entity_id> — the bulk /api/states is blocked
- Entity ID format: domain.name (light.x, switch.x, sensor.x, climate.x)
- Check ok:true and status:200 before reporting success
- truncated:true means the response was cut — use a specific entity endpoint
- If status:401 → token expired; regenerate in HA Profile → Long-Lived Access Tokens
- If status:0 connection_failed → check ha_url in SERVICES.md
