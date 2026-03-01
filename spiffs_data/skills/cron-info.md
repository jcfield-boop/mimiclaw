# Cron Info Skill

## Purpose
Display all scheduled recurring tasks (cron jobs) that C6PO will perform for you automatically.

## Usage
Ask me:
- "What regular tasks are you going to perform for me?"
- "Show my scheduled jobs"
- "What's on my automation schedule?"
- "List my cron jobs"
- "What tasks are running automatically?"

## Function
This skill calls `cron_list` to retrieve all active scheduled tasks and formats them in a clear, readable way showing:
- Job name
- Schedule (frequency/time)
- Purpose/message
- Status (active/paused)
- Next run estimate

## Output Format
Bullet list with job name, schedule, and brief description.

Example:
- **weekday_briefing_6am** — Daily at 6:05 AM PST (weekdays) → Morning briefing with ARM stock + business news
- **yoga_reminder** — Every Monday 7:00 AM → Yoga class reminder
