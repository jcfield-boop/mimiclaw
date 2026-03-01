# Network Diagnostics Skill

## Purpose
Discover and scan local network devices (Home Assistant, Klipper/Moonraker, printers, etc.) when user is on home WiFi.

## Trigger
User requests: "scan network", "find HA", "detect devices", "network diagnostics", or similar.

## Prerequisites
- User must be on **home WiFi** (192.168.0.x subnet or similar)
- mDNS/Bonjour must be available on network
- Tools: `http_request`, `web_search`, `system_info`, `wifi_scan`

## Steps

### 1. **Verify Home Network**
```
Call system_info → check WiFi SSID and signal strength
Confirm user is on home network (not cellular/remote)
```

### 1b. **WiFi Environment Scan**
```
Call wifi_scan → returns up to 10 nearby APs with SSID, RSSI, channel, auth
Useful for: confirming own network visible, detecting channel congestion,
finding weak signal (RSSI < -75 dBm = marginal, < -85 = poor)
```

### 2. **mDNS Discovery (via HTTP probe)**
Probe common .local hostnames:
- `homeassistant.local:8123` → Home Assistant
- `klipper.local` → Klipper (if exposed)
- `moonraker.local` → Moonraker API
- `octoprint.local` → OctoPrint (if used)
- `printer.local` → Generic printer discovery

For each:
```
GET http://<hostname>:port/
Capture response → status code, redirect, service signature
```

### 3. **IP Range Scan (optional, if .local fails)**
```
Probe 192.168.0.1–254 on ports: 8123 (HA), 7125 (Klipper), 631 (CUPS)
Report live IPs + open ports
```

### 4. **Query HA API (if found)**
```
GET http://homeassistant.local:8123/api/config
→ Returns list of integrations, entities, devices
Requires: Long-Lived Access Token (user provides or stored in SERVICES.md)
```

### 5. **Query Klipper/Moonraker (if found)**
```
GET http://moonraker.local:7125/api/update/status
→ Printer status, online/offline, firmware version
```

### 6. **Report Results**
```
Format:
- ✅ Home Assistant: http://homeassistant.local:8123 (online)
- ✅ Moonraker: http://moonraker.local:7125 (online)
- ❌ OctoPrint: (no response)
- 📡 WiFi: SSID "YourNetwork", -45 dBm
```

## Delivery
- **Channel:** Telegram (if called via Telegram)
- **Format:** Bullet points + status indicators
- **File output:** Save results to `/spiffs/memory/network-scan-<YYYY-MM-DD>.md`

## Notes
- mDNS discovery is **LAN-only** (no remote access)
- If .local fails, fall back to IP range scan
- HA API key needed for detailed entity queries (optional for basic discovery)
- Klipper typically needs no auth for status queries

## Future Enhancements
- SSDP/UPnP discovery (for printers, media devices)
- Persistent device database (track known devices over time)
- Alert on new/missing devices vs. baseline
