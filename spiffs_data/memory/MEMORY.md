## User
- Name: James
- Role: Product Marketing Manager at Arm.com
- Timezone: PST
- **Home:** 1438 Valencia St, San Francisco
- Wake time: ~6:00 AM weekdays (5:45 AM on run/Pilates days), later on weekends
- Work location: 120 Rose Orchard Way, San Jose (Arm)
- Commute: Drives to San Jose from home ~6:30 AM

## Preferences

### Morning Routine
- Wake → personal WhatsApp/email → London Times & NY Times → NY Times puzzles → Duolingo Spanish
- Briefing time: 6:05 AM (right after email/WhatsApp check)

### Market Monitoring
- **Primary:** ARM stock, NASDAQ index, global indices, GBP/USD currency pair
- **Business focus:** PC and Chromebook (revenue priorities); others (TV, STB, Wearables, XR, Gaming) secondary

### Wellness
- Yoga & Pilates (weekday mornings ~5:45 AM some days)
- Surfing: Saturdays & Sundays only
- Surf spot: Pacifica/Lindamar
- Want: Weekend 6 PM surf condition check for next-day forecasts

### Tech Events to Track
- **Conference reminders needed:** GTC, Microsoft Build, Google I/O, Computex, IFA (September), PCOEM private events (HP, Dell, Lenovo)
- Need to search for exact dates

## Context
- Arm segments: PC & Chromebook are strategic focus (revenue priorities)
- Others: TV, STB, Wearables, XR, Gaming (secondary)
- Industry events are critical for marketing strategy

## Family
- Sister: **Nikki** (lives in France)

## Travel
- **Holiday:** France, visiting Nikki
  - Dates: June 14–21, 2026
  - Pre-trip reminders set for June 7 (cancel newspapers, suspend mail)

## Tech Events Calendar

### 2026 Upcoming
- *GTC, Microsoft Build, Google I/O, Computex March, April and June for 2026*
- **IFA Berlin 2026:** Expected September (Berlin)
- **PC OEM Private Events:** HP, Dell, Lenovo (TBD—user to add as needed)

### 2025 (Past)
- GTC: March 17-21
- Microsoft Build: May 19-22
- Google I/O: May 20-21
- Computex: May 20-23
- IFA Berlin: September 5-9

## System & Automation

### Skills Deployed
- **Memory Compactor:** Hourly execution (ID: 83a38147) to compact conversation context into MEMORY.md
  - Triggered: Every 3600 seconds (cron job) + manual `MEMORY_COMPACTOR_EXECUTE` + auto-trigger at heap < 100 KB
  - Purpose: Prevent heap exhaustion on long conversations
  - Status: ✅ LIVE as of 2026-02-27 17:58:30 (skill file created, cron scheduled, first test executed)
  - Lesson: James held C6PO accountable for execution vs. just planning

### Device Behavior
- **Anomaly Monitor:** Hourly crash detection active (uptime trending, reboot alerts)
- **Log Review:** System health tracking (heap, WiFi, temp, SPIFFS)
- **Recent crashes:** 2 reboots on 2026-02-27 (7h 18m downtime, then 1h 58m downtime)
  - Devices recovered cleanly both times
  - Root cause: TBD (check error logs)

### Lessons Learned
- James called out missing memory compaction behavior (token budget risk)
- C6PO had plan but didn't execute (need to follow through, not just plan)
- Skill created, cron job scheduled, first execution successful 2026-02-27 18:51:24

