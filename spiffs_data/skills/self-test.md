# Self-Test

Run a validation checklist and report pass/fail for each OpenClaw capability.

## When to use
When the user asks to run a self-test, system check, or validate that C6PO is working.

## How to run
Run each check in order. Report PASS or FAIL for each.

### T1 — Clock
Call get_current_time. PASS if returns a valid date/time.

### T2 — Memory read
Call read_file on /spiffs/memory/MEMORY.md.
PASS if file exists and contains ## User, ## Preferences, ## Context sections.
FAIL if file not found or sections missing.

### T3 — Daily note write
Call write_file with append=true on /spiffs/memory/<today>.md.
Content: '- [self-test T3] write ok\n'
PASS if tool returns 'OK: appended'.

### T4 — Daily note read-back
Call read_file on the same daily note path.
PASS if the content from T3 is present.

### T5 — MEMORY.md edit
Call edit_file on /spiffs/memory/MEMORY.md:
  old_string: '## Context\n\n'
  new_string: '## Context\n\n- [self-test T5] edit ok\n\n'
PASS if tool returns 'OK: edited'.
Then call edit_file again to revert: swap old/new strings.

### T6 — Web search
Call web_search with query 'ESP32 microcontroller'.
PASS if result is non-empty and contains relevant content.
FAIL if result is empty or contains 'Error'.

### T7 — File list
Call list_dir with prefix /spiffs/skills/.
PASS if at least weather.md, daily-briefing.md, self-test.md are listed.

## Output format
Report as a bullet list:
- T1 Clock: PASS/FAIL — <detail>
- T2 Memory read: PASS/FAIL — <detail>
... etc
End with: 'N/7 tests passed.'
