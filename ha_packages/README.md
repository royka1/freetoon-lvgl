# HA-owned Delivery Tracker (Strato → HA → mobile push)

Everything lives in HA. Toon's role is "show the manual list in the PWA"
plus future "screen-toast on state change" (not wired tonight). The full
state-machine, parsing, dedup, and notification path is HA-side, deployed
via REST/WebSocket API — **no SSH, no YAML edits, no add-ons required**.

## Architecture

```
   Strato mailbox (pakketten@beyondbounds.nl)
                  │
        HA IMAP integration (added once via Settings → Devices → Integrations)
                  │  fires imap_content event per new mail
                  ▼
   automation.pkg_imap_router
                  │  → service: script.pkg_handle_email(sender, subject, body)
                  ▼
   script.pkg_handle_email   (Jinja per-merchant classification)
                  │  → service: script.pkg_upsert(merchant, order_id, label, eta, status)
                  ▼
   script.pkg_upsert         (state machine)
       ├─ writes input_text.pkg_state_map (compact JSON, capped at 255 char)
       ├─ persistent_notification.create  (HA bell icon, dedup by notification_id)
       └─ notify.<input_text.pkg_notify_target>  (mobile_app push)
```

## What was deployed (all idempotent, via `deploy.py`)

| Object | Purpose |
|---|---|
| `input_text.pkg_notify_target` | The mobile-app entity to push to. Default `notify.mobile_app_pixel_6a`. **Edit in HA UI** (Settings → Devices & Services → Helpers) to change. |
| `input_text.pkg_state_map`     | Compact `{"merchant\|order_id": {status, label, eta, ts}, …}` map. The single source of truth. |
| `script.pkg_upsert`            | State-machine core. Won't downgrade (`delivered > shipped > ordered`); fires push **only** on advancement. |
| `script.pkg_handle_email`      | Jinja classifier. Sender → merchant; subject/body → status; regex → order_id; date → eta. |
| `automation.pkg_imap_router`   | Triggers on every `imap_content` event from your IMAP integration; passes (sender, subject, body) to `script.pkg_handle_email`. |

## Setup checklist

### 1. Create the Strato mailbox
You're doing this: `pakketten@beyondbounds.nl`. Set forward rules in
your other inboxes (Gmail/Outlook/etc.) to send any order/shipping
confirmation to it.

### 2. Add the IMAP integration in HA UI (one-time)
Settings → Devices & Services → **Add Integration** → **IMAP**.
Fill in:
- Username: `pakketten@beyondbounds.nl`
- Password: <Strato mailbox password>
- Server: `imap.strato.de`
- Port: `993`
- Charset: `utf-8`
- Folder: `INBOX`
- Enable push: yes (HA will use IDLE so emails arrive in seconds, not minutes)

Credentials live in HA's encrypted config-flow storage — **never in
YAML, never on disk in plaintext, never on Toon**.

### 3. Verify the mobile-app target
HA → Developer Tools → States → check `input_text.pkg_notify_target`.
Default is `notify.mobile_app_pixel_6a`. Edit (Settings → Devices &
Services → Helpers → "Pkg Notify Target") if you want a different
phone (e.g. `notify.mobile_app_cph2483`).

Available right now: `mobile_app_cph2483`, `mobile_app_galaxy_watch4_wgyr`,
`mobile_app_lenovo_tb_x104f`, `mobile_app_pixel_6a`.

### 4. Test end-to-end
Forward yourself a Bol.com / Coolblue / AliExpress / Temu / Zooplus /
Amazon DE confirmation to the Strato address. Within ~10s of HA seeing
it:
- `input_text.pkg_state_map` gains an entry
- Phone buzzes with "Merchant: status — subject (eta YYYY-MM-DD)"
- HA bell icon shows persistent notification

A "delivered" follow-up for the same order: phone buzzes once with
"delivered" then **silent** on duplicate "delivered" emails.

## Adding a merchant

Edit `deploy.py`, find the `merchant` Jinja in `script.pkg_handle_email`
section. Add a new `elif` to the sender chain:

```python
"{%- elif 'wehkamp.nl' in sender -%}Wehkamp"
```

Re-run `python3 deploy.py` — it's idempotent.

## Tuning regex per merchant

Same place — the `status` Jinja and `order_id` Jinja in
`script.pkg_handle_email`. Test patterns first in HA Developer Tools →
Template, e.g.:

```jinja
{{ "Bestelnummer 9000099999999111." | lower | regex_findall("(?:bestelnummer|order)[^0-9]{0,10}(\\d{6,18})") }}
```

(must lowercase first — HA's `regex_findall` is case-sensitive)

## Storage limit + escape hatch

`input_text.pkg_state_map` is capped at 255 chars by HA. Each entry is
~80 chars, so ~3 active packages fit. Beyond that, swap storage to MQTT
(your broker is at `192.168.99.62`) — `script.pkg_upsert` would
`mqtt.publish` to a retained topic instead of `input_text.set_value`.
Trivial change once you actually hit the limit.

## Files in this folder

| File | What |
|---|---|
| `deploy.py`              | Run from any host with HA LLT to (re-)deploy the full stack |
| `parse_email.py`         | Optional standalone parser (cron-based alternative to the HA-IMAP path; same logic in Python) |
| `email_poller.py`        | Older IMAP-poller variant (now superseded by the HA-IMAP path; keep for reference) |
| `deliveries.yaml`        | Older HA YAML package (now superseded — kept for reference) |
| `ha_imap_strato.yaml`    | Older HA YAML (the shell_command approach we abandoned because YAML can't be created via REST) |
| `README.md`              | This |

## Files NO LONGER used by this design

- Toon's `/mnt/data/packages.json` is still there for manual adds via
  the PWA card; HA-side state is independent.
- `/mnt/data/parse_email.py`, `/mnt/data/ha.cfg`, `/mnt/data/notify.cfg`
  on Toon — leftover from the brief Toon-side parser attempt. Safe to
  delete or ignore; they're not in any active path.
