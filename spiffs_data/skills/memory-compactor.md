# Memory Archive Skill

## Trigger
- Hourly cron execution
- Manual command: `MEMORY_COMPACTOR_EXECUTE`
- Auto-alert when heap < 100 KB

## Purpose
**Archive conversation context** into persistent `/spiffs/memory/MEMORY.md` for long-term reference and continuity. 

**⚠️ IMPORTANT:** This skill does NOT free heap space. It backs up facts but cannot trigger garbage collection from within Python. It is a **backup tool, not a memory compactor**.

## Steps

### 1. Check Current Heap
- Report current heap status
- Compare to baseline/min observed
- If < 100 KB: Flag as warning (device may need restart)

### 2. Extract New Facts from Conversation
Look for NEW information about:
- **User preferences** (schedule, hobbies, work focus)
- **Events** (conferences, travel, reminders)
- **Family/contacts** (names, locations, relationships)
- **Tech stack** (devices, services, automations)
- **Market monitoring** (stocks, indices, currencies to track)
- **Lessons learned** (system insights, skill improvements)

### 3. Append to MEMORY.md
- Write with ISO timestamp: `[2026-02-27 18:00:00 PST]`
- Format: `- **Topic:** detail (source: conversation context)`
- Skip duplicates (don't re-add existing facts)
- Keep sections organized (User, Work, Wellness, Tech Events, Family, System, Lessons)

### 4. Log Archive Execution
- Record to `/spiffs/memory/compaction-log.md`:
  ```
  | [timestamp] | Heap Before | Heap After | Facts Extracted | Status | Note |
  ```
- Heap "After" will typically equal "Before" (no freeing occurs)

### 5. Alert if Critical
- If heap < 100 KB: Send Telegram alert
  - Message: "⚠️ HEAP WARNING: X KB remaining. Archive complete. Consider restart if heap keeps declining."

### 6. Return Summary
- Report: facts archived, heap status, baseline min, recommendation

## Output Format
```
✅ MEMORY ARCHIVE COMPLETE
- Heap: 170.9 KB (min baseline: 86.9 KB)
- Facts extracted: 0 (all current)
- Appended to MEMORY.md
- Status: ✅ HEALTHY (no restart needed)
```

## Design Notes
- **Non-destructive:** Only appends to MEMORY.md
- **No heap recovery:** File writes don't trigger garbage collection
- **Backup function:** Ensures facts survive session restarts
- **Safety valve:** Alerts when heap pressure rises
- **Next step:** If heap < 50 KB and not recovering, manual restart recommended

## Limitations
- Cannot free heap from within Python session
- Only Python GC (automatic) or device restart will recover memory
- Archiving is fast but doesn't reduce in-session memory pressure
- Designed for continuity, not for preventing OOM crashes

## Recommended Safety Automation
- Pair with external monitoring (cron alert if heap < 50 KB for N consecutive cycles)
- Manual or scheduled device restart when heap consistently < 75 KB
- Consider breaking large conversations into sessions (restart between sessions)
