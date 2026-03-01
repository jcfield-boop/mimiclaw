# ESP32-C6 Health Monitor - Session Notes

## Latest Health Check: 2026-03-01 (Phase 6 Verification)

### OVERALL STATUS: HEALTHY ✓

### Key Metrics (Baseline)
- **Heap Health**: 199 KB free, 134 KB min observed → excellent for 512KB SRAM
- **SPIFFS**: 7.6% utilization (59 KB / 775 KB) → ample room
- **Uptime**: Stable operation, multiple LLM calls processed cleanly
- **WiFi**: Connected at 192.168.0.25, stable (no disconnects)
- **LLM**: OpenRouter/Claude-Haiku streaming working, SSE callbacks firing correctly
- **Known Issue**: Token counts not parsed from OpenRouter streaming response (shows 0↑ 0↓ tok)

### All Systems PASS
1. **Build**: 1331 KB (65% of 2MB partition) — no errors
2. **Boot**: Clean ESP-IDF v6.0-dev-2039 startup, all services initialized
3. **WiFi**: Connected to blighty_IoT (192.168.0.25)
4. **TLS/Telegram**: Cross-signed cert verification working (no errors observed)
5. **HTTP API**: `/api/sysinfo`, `/api/config`, web interface all responsive
6. **Memory**: Heap free 199KB (healthy), SPIFFS at 7.6% capacity
7. **LLM Proxy (Phase 6)**: OpenRouter/Claude-Haiku-4-5 working end-to-end
8. **Streaming (Phase 6)**: SSE callbacks confirmed ("stream +" events), responses returned to WebSocket
9. **Config API (Phase 6)**: Provider, model, API key, search key all exposed via GET /api/config

### Phase 6 Features Verified (2026-03-01)
- OpenRouter provider configured and responding
- Claude Haiku 4.5 model selected and working
- Streaming callbacks firing correctly ("calling LLM streaming", "stream +", "LLM stream done")
- Live monitor events broadcasting to WebSocket clients (task/llm/stream/done events)
- Web interface loads (433 lines HTML), settings tab accessible
- Config endpoint returns provider, model, API key, search key (all masked when logged)
- Two successful test messages: "say hello in 3 words" → "Hello, I'm C6PO.", "What is 2+2?" → "2 + 2 = **4**"

### Known Limitation (Phase 6)
- Token counts not parsed from streaming response: shows "0↑ 0↓ tok" in logs, tokens_in/out=0 in sysinfo
- Root cause: OpenRouter's streaming API may not include usage block or it's in different format
- Impact: Token metrics unavailable for cost tracking, but LLM response functionality unaffected
- Code location: llm_proxy.c:972-978 attempts to parse top-level usage field from stream chunks

### Known Working Features
- **Serial CLI**: Functional (at `/mimi> ` prompt)
- **WebSocket Server**: Running on port 80, client connection tested (ws_61)
- **Verbose Logs**: Toggle available, currently disabled (can enable for diagnostics)
- **Cron Service**: Loaded and running (5 jobs, 60s check interval)
- **Heartbeat**: Started (30-min interval)
- **Outbound Dispatch**: Active and queuing responses
- **Agent Loop**: Single-core (core 0) running correctly

### Recommendations
- **No immediate actions required**
- Monitor heap_min over extended sessions (target >100KB safety margin)
- Heap currently at excellent baseline; safe to run compute-intensive operations
- Device ready for production deployment

### Environment
- Device: ESP32-C6FH4 (revision v0.2)
- Flash: 4MB (partitions: 24KB nvs, 4KB phy_init, 2MB factory, 1.9MB spiffs, 64KB coredump)
- Clock: Using build timestamp fallback
- Port: /dev/cu.usbmodem144201 at 115200 baud (serial connected but logs go to WebSocket only)
- Network IP: 192.168.0.25 (blighty_IoT SSID)
