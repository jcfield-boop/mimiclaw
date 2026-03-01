# Cron Fire Verification

Verify the cron subsystem fires jobs at the scheduled time and triggers agent turns.
This is a TWO-PART skill. Run Part 1 now, then Part 2 after 2+ minutes.

## When to use
When the user asks to "verify cron", "test cron firing", "check cron timing", or
"run cron-verify".

---

## Part 1: Setup (run now)

1. Call get_current_time to get the current unix epoch. Note it as T_start.
2. Call cron_add:
   - name: "cron-verify"
   - schedule_type: "at"
   - seconds_from_now: 90
   - message: "cron-verify: CANARY {T_start}" (replace {T_start} with actual epoch number)
3. Call cron_list and confirm the job appears. Note the job_id.
4. Call write_file:
   - path: /spiffs/memory/cron-verify.md
   - content:
     ```
     # Cron Verify State
     job_id: {job_id}
     t_start: {T_start}
     expected_fire: {T_start + 90}
     status: pending
     ```
   (replace all {placeholders} with actual values)
5. Tell the user: "Canary cron job scheduled for 90 seconds from now (job ID: {job_id}).
   Run 'check cron verify' in 2+ minutes."

---

## Part 2: Check (run 2+ minutes after setup)

1. Call read_file on /spiffs/memory/cron-verify.md to get job_id and expected_fire.
2. Call search_files with pattern "CANARY" and prefix /spiffs/memory/ to look for the
   fired message in the daily note.
3. Call get_current_time to get current epoch T_now.

### Interpreting results:

**If "CANARY" found in search results:**
- Extract the epoch embedded in the message ("cron-verify: CANARY {epoch}")
- Calculate latency = T_now - expected_fire
- PASS if |latency| <= 30s (fired within expected window)
- WARN if latency is 31–120s (fired late but eventually)
- FAIL if latency > 120s

**If "CANARY" NOT found in search results:**
- FAIL — job either did not fire or agent did not log it
- Call cron_list → if job still listed (not fired), note "job not yet fired — wait longer"
- If job is gone but not logged, note "job fired but message was not processed"

### Cleanup:
Call write_file on /spiffs/memory/cron-verify.md with content "# Cron Verify\ncleared\n"
to reset state.

---

## Report format
```
Cron Fire Verification:
- Job ID: {job_id}
- Scheduled fire: {expected_fire} (epoch)
- Actual fire: {actual_fire} (epoch, or "not observed")
- Latency: ±Ns
- Result: PASS / WARN / FAIL
- Reason: <brief explanation>
```
