# Log Review Skill

Scan system logs for anomalies and email alerts hourly.

## When to use
When the user asks to monitor logs, review system health, or detect anomalies in real-time.

## How to use
1. **Collect system data:** Call system_info to get device health metrics (heap, SPIFFS, uptime, WiFi RSSI)
2. **Read log files:** Search /spiffs/ for error logs, warnings, or unusual patterns using search_files
3. **Analyze for anomalies:**
   - Free heap < 50% of total
   - SPIFFS usage > 85%
   - WiFi RSSI < -80 dBm (weak signal)
   - Uptime drops (indicates reboots)
   - Cron job failures
   - Temperature > 65°C
4. **Compile report:** Format findings as bullet points
5. **Email results:** Use send_email to deliver anomaly report (or "all clear" if none found)
6. **Schedule recurring:** Use cron_add with interval_s=3600 (hourly) to run this skill

## Thresholds for alerting
- **Heap:** Alert if < 50KB free or < 40% available
- **SPIFFS:** Alert if > 85% full
- **WiFi:** Alert if RSSI < -80 dBm
- **Temperature:** Alert if > 65°C
- **Cron failures:** Alert if any job failed or missed
- **Reboots:** Alert if uptime decreased since last check

## Example
User: "Monitor logs hourly and email me anomalies"
→ Create hourly cron job that calls this skill
→ Each hour: collect metrics, check for issues, email summary
→ Subject: "C6PO Log Review — [TIME] — [Status: Clear/⚠️ Issues]"
