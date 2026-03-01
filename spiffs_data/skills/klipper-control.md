# Klipper Control

Monitor and control Klipper 3D printer via REST API (Moonraker).

## When to use
When the user asks to:
- Check printer status (temperature, Z-height, job progress)
- Start/cancel/pause print jobs
- Get print queue status
- Monitor nozzle/bed temperature
- Query printer statistics
- Emergency stop (E-stop) the printer

## How to use
1. Get Klipper IP (typically 192.168.0.50 or similar) and Moonraker port (default 7125)
2. Use http_request with:
   - Base URL: `http://<KLIPPER_IP>:7125/api/`
   - Headers: `{"Content-Type": "application/json"}`
3. Common endpoints:
   - `GET /printer/info` — printer status & firmware
   - `GET /printer/query_endstops/status` — endstop state
   - `GET /printer/objects/query?objects=extruder,heater_bed,print_stats` — temps, print progress
   - `POST /printer/print/start?filename=<file>` — start print job
   - `POST /printer/print/cancel` — cancel active print
   - `POST /printer/print/pause` — pause print
   - `POST /printer/print/resume` — resume print
   - `POST /printer/emergency_stop` — E-stop
4. Parse JSON response and report results

## Example
User: "What's the printer status?"
→ GET `/printer/objects/query?objects=print_stats,extruder,heater_bed`
→ Report: "Idle | Nozzle: 25°C | Bed: 25°C"

User: "Cancel the current print"
→ POST `/printer/print/cancel`
→ Report: "✅ Print cancelled"

User: "Start printing test.gcode"
→ POST `/printer/print/start?filename=test.gcode`
→ Report: "✅ Print started: test.gcode"

## Notes
- Klipper uses HTTP (not HTTPS) on port 7125 by default
- Moonraker is the REST API layer for Klipper
- No auth required (local network only)
- Can be scheduled with cron_add for periodic status checks
- Emergency stop is irreversible in the current print session
