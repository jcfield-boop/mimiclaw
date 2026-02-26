#!/usr/bin/env python3
"""
MimiClaw ESP32-C6 serial monitor.
Watches for errors every 20 minutes; auto-resets the device on crash if it
doesn't recover within 90 seconds.
"""

import serial
import time
import re
import datetime
import sys
import os

PORT     = "/dev/cu.usbmodem14101"
BAUD     = 115200
CHECK_INTERVAL_S = 20 * 60   # analyse window every 20 minutes
CRASH_RECOVER_S  = 90        # wait this long after crash before reset attempt
MAX_LINES        = 2000      # rolling buffer

LOG_FILE = "/tmp/mimiclaw_monitor.log"

# ── Patterns ──────────────────────────────────────────────────────────────────

CRASH_PATTERNS = [
    (r"Guru Meditation",       "PANIC"),
    (r"abort\(\) was called",  "ABORT"),
    (r"stack overflow",        "STACK_OVERFLOW"),
    (r"LoadProhibited|StoreProhibited", "MEM_FAULT"),
]

WARN_PATTERNS = [
    (r"\[error\]",             "APP_ERROR"),
    (r"E \([0-9]+\)",          "ESP_ERROR"),
    (r"OOM|alloc.*fail|malloc.*fail|out of memory", "OOM"),
    (r"WiFi.*disconnect",      "WIFI_DROP"),
    (r"\[poll\] ",             "POLL_ERROR"),
    (r"\[app\] send:",         "SEND_ERROR"),
]

HEALTHY_PATTERN = re.compile(
    r"\[heartbeat\]|\[telegram\]|\[cron\]|\[agent\]|\[llm\]", re.IGNORECASE
)

# ── Helpers ───────────────────────────────────────────────────────────────────

def ts():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

def log(msg):
    line = f"[{ts()}] {msg}"
    print(line, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

def reset_device(ser):
    """Toggle RTS to pulse EN (reset) pin on ESP32-C6."""
    log(">>> Attempting hardware reset via RTS toggle")
    try:
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.1)
        ser.rts = True
        log(">>> Reset pulse sent")
        return True
    except Exception as e:
        log(f">>> Reset failed: {e}")
        return False

def open_port():
    """Open serial port WITHOUT toggling DTR/RTS (avoids triggering ESP32 auto-reset)."""
    for attempt in range(1, 6):
        try:
            s = serial.Serial()
            s.port     = PORT
            s.baudrate = BAUD
            s.timeout  = 1
            s.dtr      = False   # must be set before open()
            s.rts      = False   # must be set before open()
            s.open()
            log(f"Connected to {PORT} (DTR/RTS held low — no reset)")
            return s
        except Exception as e:
            log(f"Open attempt {attempt}/5 failed: {e}")
            time.sleep(5)
    return None

# ── Analysis ──────────────────────────────────────────────────────────────────

def analyse(lines):
    """Examine a list of log lines. Returns (crash_lines, warn_dict, healthy_count)."""
    crash_lines = []
    warns = {}
    healthy = 0

    for line in lines:
        for pattern, label in CRASH_PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):
                crash_lines.append((label, line))
        for pattern, label in WARN_PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):
                warns.setdefault(label, []).append(line)
        if HEALTHY_PATTERN.search(line):
            healthy += 1

    return crash_lines, warns, healthy

# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    log(f"=== MimiClaw C6 monitor started (check every 20 min) ===")
    log(f"Log file: {LOG_FILE}")

    ser = open_port()
    if ser is None:
        log("Could not open serial port — exiting")
        sys.exit(1)

    rolling   = []          # (timestamp, line)
    last_check   = time.time()
    crash_at     = None     # time of last detected crash
    reset_done   = False    # only reset once per crash event

    while True:
        try:
            raw = ser.readline()
        except serial.SerialException as e:
            log(f"Serial read error: {e} — reconnecting")
            ser.close()
            time.sleep(3)
            ser = open_port()
            if ser is None:
                log("Could not reconnect — exiting")
                sys.exit(1)
            continue

        if raw:
            line = raw.decode("utf-8", errors="replace").rstrip()
            now  = time.time()
            rolling.append((now, line))
            if len(rolling) > MAX_LINES:
                rolling.pop(0)

            # Immediate crash detection
            for pattern, label in CRASH_PATTERNS:
                if re.search(pattern, line, re.IGNORECASE):
                    log(f"!!! CRASH detected ({label}): {line[:120]}")
                    crash_at   = now
                    reset_done = False
                    break

            # If we had a crash, watch for recovery or trigger reset
            if crash_at and not reset_done:
                # Check if device recovered (healthy output after crash)
                recent = [l for t, l in rolling if t > crash_at]
                recovered = any(HEALTHY_PATTERN.search(l) for l in recent)
                if recovered:
                    log("Device recovered after crash — no reset needed")
                    crash_at   = None
                    reset_done = False
                elif now - crash_at > CRASH_RECOVER_S:
                    log(f"Device did not recover within {CRASH_RECOVER_S}s — resetting")
                    reset_device(ser)
                    crash_at   = None
                    reset_done = True

        # ── Periodic 20-min health check ─────────────────────────────────
        now = time.time()
        if now - last_check >= CHECK_INTERVAL_S:
            last_check = now
            cutoff  = now - CHECK_INTERVAL_S
            window  = [l for t, l in rolling if t >= cutoff]

            crashes, warns, healthy = analyse(window)

            log(f"=== 20-min check: {len(window)} lines, {healthy} healthy signals ===")

            if crashes:
                log(f"  CRASHES in window: {len(crashes)}")
                for label, ex in crashes[-3:]:
                    log(f"    [{label}] {ex[:100]}")

            if warns:
                for label, examples in warns.items():
                    log(f"  {label}: {len(examples)} occurrence(s)")
                    log(f"    last: {examples[-1][:100]}")

            if not crashes and not warns:
                if healthy > 0:
                    log("  Status: healthy")
                else:
                    log("  WARNING: no healthy signals in window — device may be silent")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Monitor stopped by user")
