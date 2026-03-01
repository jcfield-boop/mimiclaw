# Self-Test v2

Run a 7-test validation suite to verify C6PO tools and the ReAct loop are working.

## When to use
When the user asks to "run self-test", "system check", or "validate C6PO".

## How to run
Run each test in order using the exact tool calls described. Report PASS or FAIL with a brief reason.

### T1 — Clock (tool use verification)
Call get_current_time.
PASS if result contains a valid year (2024 or later) and a unix timestamp > 1700000000.
FAIL if returns an error or a training-data date.

### T2 — Web→Memory chain (hallucination check)
1. Call web_search with query: "ESP32 Arduino latest release 2025"
2. Note a specific unique fact from the search result (a version number, date, or name).
3. Call memory_write with that fact: memory_type="fact", confidence=0.9, content="[self-test T2] <exact fact>".
4. Call read_file on /spiffs/memory/MEMORY.md.
5. PASS if the written fact appears in the file content.
FAIL if: web_search returns empty or error, memory_write fails, or the fact is absent from MEMORY.md.
CRITICAL: If you answer from training data without calling web_search first, this test FAILS.

### T3 — Cron roundtrip
1. Call cron_add: name="selftest", schedule_type="every", interval_s=3600, message="self-test cron ping".
2. Call cron_list. Note the job_id assigned.
3. Call cron_remove with that job_id.
4. Call cron_list again.
PASS if: the job appeared in step 2 and is absent in step 4.
FAIL if any step errors or the job is still present after removal.

### T4 — File roundtrip
1. Call get_current_time. Note the unix epoch as TS.
2. Call write_file: path=/spiffs/memory/selftest-tmp.md, content="selftest-{TS}" (use actual epoch).
3. Call read_file on /spiffs/memory/selftest-tmp.md.
4. Call write_file: path=/spiffs/memory/selftest-tmp.md, content="" (overwrite to clear).
PASS if read_file returns content containing "selftest-" and the epoch value.
FAIL if write or read errors, or content does not match.

### T5 — System coherence
Call system_info.
PASS if: uptime value is present and non-zero, free heap > 50000 bytes, RSSI is present and non-zero.
FAIL if heap is critical (<40000 bytes) or expected fields are missing.

### T6 — Temperature sanity
Call device_temp.
PASS if the returned Celsius value is between 10 and 85.
FAIL if an error is returned or the value is out of range.

### T7 — Tool self-knowledge
Without calling any tools, list which tools you have available.
PASS if you can name at least 5 of: web_search, memory_write, cron_add, cron_list, cron_remove,
get_current_time, read_file, write_file, system_info, device_temp, gpio_read, wifi_scan, rss_fetch.
FAIL if you cannot name any tools or list fewer than 5.

## Cleanup after T2
After completing all tests, call edit_file on /spiffs/memory/MEMORY.md to remove the
"[self-test T2]" fact you wrote, so memory is not polluted with test data.

## Output format
```
Self-Test Results:
- T1 Clock: PASS/FAIL — <detail>
- T2 Web→Memory: PASS/FAIL — <detail>
- T3 Cron roundtrip: PASS/FAIL — <detail>
- T4 File roundtrip: PASS/FAIL — <detail>
- T5 System coherence: PASS/FAIL — heap: NNNkB free, uptime: Xs
- T6 Temperature: PASS/FAIL — NN.N°C
- T7 Tool knowledge: PASS/FAIL — named N tools

N/7 tests passed.
```

If T2 fails due to hallucination, add:
"⚠️ WARNING: LLM may be hallucinating instead of calling tools."
