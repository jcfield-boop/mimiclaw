# Home Assistant Control

Monitor and control Home Assistant automations, entities, and services via REST API.

## When to use
When the user asks to:
- Check HA service status
- Trigger automations or scenes
- Get entity state (lights, switches, sensors, climate)
- Send notifications to HA
- Query HA API for current state

## How to use
1. Get HA IP address and long-lived access token from user or /spiffs/config/SERVICES.md
2. Use http_request with:
   - Base URL: `https://<HA_IP>:8123/api/`
   - Headers: `{"Authorization": "Bearer <TOKEN>", "Content-Type": "application/json"}`
3. Common endpoints:
   - `GET /states` — list all entities and current state
   - `GET /states/light.living_room` — get specific entity state
   - `POST /services/light/turn_on` — call service with body `{"entity_id": "light.living_room"}`
   - `POST /services/automation/trigger` — trigger automation
   - `GET /config/core` — system info
4. Parse JSON response and report results

## Example
User: "Turn on the living room light"
→ POST to `/services/light/turn_on`
→ Body: `{"entity_id": "light.living_room"}`
→ Report: "✅ Living room light is on"

User: "What's the current temperature?"
→ GET `/states/sensor.living_room_temperature`
→ Report: "Living room: 72°F"

## Notes
- Store HA IP & token in SERVICES.md for security
- Always use HTTPS (port 8123)
- Handle API errors gracefully (timeouts, auth failures)
- Can be chained with cron_add for scheduled automations
