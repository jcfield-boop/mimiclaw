# Email

Send an email via Gmail using the send_email tool.

## When to use
When the user asks to send an email, or when a skill needs to deliver a report by email
(e.g. daily briefing, flight alert, price watch result).

## How to use
1. Identify subject and body from context
2. Call send_email with subject and body. Optionally pass 'to' to override the default recipient.
3. Report success or failure to the user.
   Do not mention credentials or internal details.

## SERVICES.md format required
## Email
service: Gmail
smtp_host: smtp.gmail.com
smtp_port: 465
username: you@gmail.com
password: xxxx xxxx xxxx xxxx
from_address: C6PO <you@gmail.com>
to_address: you@gmail.com

## Notes
- Requires a Gmail App Password (not your main password).
  Generate one at: myaccount.google.com/apppasswords
- 2-Step Verification must be enabled on the Gmail account.
- If credentials are missing or wrong, tell the user to check SERVICES.md.
