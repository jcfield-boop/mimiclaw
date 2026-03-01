# Anomaly Monitor Skill

Monitor system health hourly and log all metrics to `/spiffs/memory/uptime-log.md`.

## When to use
Triggered hourly by cron job to:
1. Collect system metrics
2. **WRITE snapshot to log file**
3. Alert via Telegram if anomalies detected

## How to use (for cron automation)
```
Message: ANOMALY_MONITOR_EXECUTE
Frequency: Every 3600 seconds (hourly)
Channel: Telegram (5538967144)
```

## Steps
1. **Collect metrics:** Call system_info, device_temp, get_current_time
2. **Load previous snapshot:** Read last line from `/spiffs/memory/uptime-log.md`
3. **Compare uptime:**
   - If current_uptime < previous_uptime → **CRASH DETECTED**
   - Calculate downtime window (time since last check)
   - Log crash with timestamp + downtime estimate
   - Send Telegram alert immediately
4. **Calculate snapshot:**
   - Timestamp (human-readable + unix)
   - Free heap (bytes + %)
   - SPIFFS usage (bytes + %)
   - WiFi RSSI (dBm)
   - Device temp (°C)
   - Uptime (formatted)
5. **APPEND to log:** Write formatted entry to `/spiffs/memory/uptime-log.md`
6. **Check thresholds:**
   - Heap < 50 KB → ALERT
   - SPIFFS > 85% → ALERT
   - WiFi < -80 dBm → ALERT
   - Temp > 65°C → ALERT
7. **Send alert:** If any threshold exceeded OR crash detected, send Telegram message with details

## Log Format
```
| Timestamp | Heap (KB) | Heap % | SPIFFS % | WiFi (dBm) | Temp (°C) | Uptime | Crash? | Status |
|-----------|-----------|--------|----------|-----------|-----------|--------|--------|--------|
| 2026-02-27 06:33:37 | 168.4 | 72% | 11% | -58 | 35.6 | 0h 22m | NO | ✅ OK |
| 2026-02-27 07:33:37 | 145.2 | 65% | 12% | -57 | 36.1 | 0h 05m | **YES** (↓ from 0h 22m) | ⚠️ CRASH |
```

**Crash Detection Logic:**
- Compare `current_uptime` vs `previous_uptime`
- If downward jump: Flag as crash
- Log estimated downtime (current timestamp − last check time − observed uptime)
- Alert with crash details

## Alert Thresholds
- **Heap:** < 50 KB free
- **SPIFFS:** > 85% used
- **WiFi:** RSSI < -80 dBm (weak signal)
- **Temperature:** > 65°C (overheating)

## Returns
- **Success:** "✅ Snapshot logged. All clear." (or "⚠️ Alert sent if issues found")
- **Failure:** "❌ Log write failed"
| 2026-02-27 21:56:03 | Heap: 110.7 KB (67%) | SPIFFS: 14% | WiFi: -58 dBm | Temp: 38.6°C | Uptime: 1h 29m | Crash? NO | ✅ OK |
