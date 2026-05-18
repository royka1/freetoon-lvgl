#!/usr/bin/env python3
"""Deploy the HA-owned delivery tracker via REST + WebSocket APIs.

No SSH needed. No YAML edits needed. Idempotent.

Architecture: HA processes the email, dedups by (merchant, order_id),
fires push to mobile_app + writes to a compact state map on every
state advancement. Toon's role shrinks to "display + screen notify".

What this creates / refreshes:
  • input_text.pkg_notify_target — your mobile_app entity (default Pixel 6a)
  • input_text.pkg_state_map     — compact {hash: {status,label,eta,ts}} json
  • script.pkg_upsert            — the state-machine core
  • script.pkg_handle_email      — per-merchant Jinja classifier
  • automation.pkg_imap_router   — triggers on imap_content
  • Cleans up the older packages_* helpers/scripts we created earlier
"""
import asyncio, json, os, sys, urllib.request
import websockets

TOKEN = open(os.path.expanduser("~/.ha_llt")).read().strip()
HA    = "http://192.168.3.101:8123"
WS    = "ws://192.168.3.101:8123/api/websocket"

def rest(method, path, body=None, expect=(200,)):
    req = urllib.request.Request(
        HA + path, method=method,
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Content-Type": "application/json"},
        data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            data = r.read()
            if r.status not in expect:
                print(f"  ! {method} {path} → HTTP {r.status} {data[:300]}")
            return r.status, data
    except urllib.request.HTTPError as e:
        body = e.read()
        print(f"  ! {method} {path} → HTTP {e.code} {body[:400]}")
        return e.code, body

async def ws_call(ws, mid, msg):
    msg = dict(msg, id=mid)
    await ws.send(json.dumps(msg))
    while True:
        r = json.loads(await ws.recv())
        if r.get("id") == mid: return r

async def main():
    async with websockets.connect(WS, max_size=2**22) as ws:
        assert json.loads(await ws.recv())["type"] == "auth_required"
        await ws.send(json.dumps({"type":"auth","access_token":TOKEN}))
        assert json.loads(await ws.recv())["type"] == "auth_ok"
        i = [0]
        def nx(): i[0]+=1; return i[0]

        # ------------------- MQTT DISCOVERY: pkg_state_map sensor -------------------
        # Publishes a one-shot discovery message that creates `sensor.pkg_state_map`
        # mirroring the retained MQTT topic `home/packages/state`. This lifts the
        # 255-char input_text cap. Idempotent — HA dedupes by unique_id.
        print("→ MQTT discovery sensor.pkg_state_map")
        disc = {
            "name":"Pkg State Map",
            "state_topic":"home/packages/state",
            "value_template":"{{ value }}",   # we publish raw JSON string
            "unique_id":"pkg_state_map",
            "icon":"mdi:package-variant",
        }
        rest("POST", "/api/services/mqtt/publish", {
            "topic":"homeassistant/sensor/pkg_state_map/config",
            "payload": json.dumps(disc),
            "retain": True,
        })
        # Seed empty {} ONLY if no payload is retained yet — avoids
        # clobbering live state on a re-deploy. We probe by reading the
        # sensor's current state; "unknown" means no retained payload.
        s, body = rest("GET", "/api/states/sensor.pkg_state_map")
        if s == 200:
            try:
                st = json.loads(body).get("state", "unknown")
                if st in ("unknown", "unavailable", ""):
                    print("  seeding empty state")
                    rest("POST", "/api/services/mqtt/publish", {
                        "topic":"home/packages/state", "payload":"{}", "retain":True})
                else:
                    print(f"  state already retained ({len(st)} chars), keeping it")
            except json.JSONDecodeError:
                pass

        # ------------------- CLEANUP previous attempt -------------------
        print("→ cleanup previous deployment")
        for sid in ("packages_add", "packages_remove", "packages_complete"):
            rest("DELETE", f"/api/config/script/config/{sid}")
        rest("DELETE", "/api/config/automation/config/packages_webhook_add")
        # Old input_text.deliveries_json (created via WS)
        r = await ws_call(ws, nx(), {"type":"input_text/list"})
        for it in r.get("result", []):
            if it.get("id") in ("deliveries_json",):
                await ws_call(ws, nx(), {"type":"input_text/delete","input_text_id":it["id"]})
                print(f"  - removed input_text.{it['id']}")

        # ------------------- HELPERS (input_text) -------------------
        print("→ helpers")
        r = await ws_call(ws, nx(), {"type":"input_text/list"})
        existing = {it["id"] for it in r.get("result", [])}
        for name, init in [
            ("Pkg Notify Target",  "notify.mobile_app_pixel_6a"),
            ("Pkg State Map",      "{}"),
        ]:
            slug = name.lower().replace(" ", "_")
            if slug in existing:
                await ws_call(ws, nx(), {"type":"input_text/update","input_text_id":slug,
                                          "name":name,"max":255,"initial":init})
                print(f"  ~ updated input_text.{slug}")
            else:
                r = await ws_call(ws, nx(), {"type":"input_text/create",
                                              "name":name,"max":255,"initial":init})
                print(f"  + created {r.get('result',{}).get('id','?')}")

        # ------------------- SCRIPT pkg_upsert -------------------
        # Reads input_text.pkg_state_map (a {hash: {status, label, eta, ts}} dict).
        # Computes new status rank vs existing; only writes + notifies on advancement.
        # Hash key is sha1-ish (md5 unavailable in jinja, so just merchant|order).
        print("→ script.pkg_upsert")
        upsert = {
            "alias": "Packages: upsert (state-machine + notify)",
            "mode": "queued", "max": 50,
            "fields": {
                "merchant": {"description": "Merchant name (Bol.com, Coolblue, …)"},
                "order_id": {"description": "Order id (or url-<hash> fallback)"},
                "label":    {"description": "Display label (usually email subject)"},
                "eta":      {"description": "YYYY-MM-DD"},
                "status":   {"description": "ordered|shipped|delivered"},
                "url":      {"description": "Tracking URL (often the only useful identifier)"},
            },
            "sequence": [
                {"variables": {
                    "key": "{{ merchant ~ '|' ~ order_id }}",
                    "now_iso": "{{ now().strftime('%Y-%m-%dT%H:%M:%S') }}",
                    # Read current state from input_text (255-char cap, but
                    # the MQTT-discovery sensor proved unreliable). We
                    # still publish to MQTT for Toon to consume.
                    "current_map": "{{ states('input_text.pkg_state_map') | from_json | default({}, true) }}",
                    "old_status":  ("{{ (states('input_text.pkg_state_map') | from_json"
                                    "    | default({}, true)).get(merchant ~ '|' ~ order_id, {}).get('status', 'none') }}"),
                    "rank_old":    ("{% set m = {'none':0,'pending':1,'ordered':2,'shipped':3,"
                                    "'pickup_ready':4,'delivered':5,'received':6} %}{{ m.get(old_status, 0) }}"),
                    "rank_new":    ("{% set m = {'none':0,'pending':1,'ordered':2,'shipped':3,"
                                    "'pickup_ready':4,'delivered':5,'received':6} %}{{ m.get(status, 0) }}"),
                    "advanced":    "{{ rank_new|int > rank_old|int }}",
                    # Full entry — pushed via MQTT to Toon, includes URL.
                    "new_entry":   ("{{ {'status':status,'label':label,'eta':eta,"
                                    "'url':url|default(''),"
                                    "'ts':now().strftime('%Y-%m-%dT%H:%M:%S')} }}"),
                    # Compact entry — used as the input_text mirror. Omits
                    # url (too long for 255-char cap) and shortens ts to
                    # date-only. This keeps the rank-check + dedup logic
                    # working while we hit the HA storage limit.
                    "compact_entry":("{{ {'status':status,'label':(label or '')[:40],"
                                    "'eta':eta} }}"),
                    "merged_map": (
                        "{%- if rank_new|int >= rank_old|int -%}"
                          "{{ current_map | combine({key: new_entry}) | to_json }}"
                        "{%- else -%}"
                          "{{ current_map | to_json }}"
                        "{%- endif -%}"
                    ),
                    "merged_compact": (
                        "{%- if rank_new|int >= rank_old|int -%}"
                          "{{ current_map | combine({key: compact_entry}) | to_json }}"
                        "{%- else -%}"
                          "{{ current_map | to_json }}"
                        "{%- endif -%}"
                    ),
                }},
                # Persist to BOTH input_text (cheap, HA-side reads — uses
                # the compact entry to fit ~5 packages in 255 chars) AND
                # MQTT (Toon push, full payload with url + ts).
                {"service": "input_text.set_value",
                 "target": {"entity_id": "input_text.pkg_state_map"},
                 "data": {"value": "{{ merged_compact | string | truncate(255, true, '') }}"}},
                {"service": "mqtt.publish",
                 "data": {
                   "topic":   "home/packages/state",
                   "payload": "{{ merged_map }}",
                   "retain":  True,
                 }},
                # Mobile push on actual state advancement only
                {"if": [{"condition":"template","value_template":"{{ advanced }}"}],
                 "then":[
                    {"service": "{{ states('input_text.pkg_notify_target') }}",
                     "data": {
                       "title": "{{ merchant }}: {{ status }}",
                       "message": "{{ label }} — eta {{ eta }}",
                     }},
                    {"service": "persistent_notification.create",
                     "data": {
                       "notification_id": "{{ key | replace('|','-') }}-{{ status }}",
                       "title": "{{ merchant }}: {{ status }}",
                       "message": "{{ label }}\n eta {{ eta }} · {{ now_iso }}",
                     }},
                 ]},
            ],
        }
        s,_ = rest("POST", "/api/config/script/config/pkg_upsert", upsert)
        print(f"  pkg_upsert  HTTP {s}")

        # ------------------- SCRIPT pkg_handle_email -------------------
        print("→ script.pkg_handle_email")
        # Per-merchant detection via Jinja `choose`. Add new merchants by
        # extending the conditions list.
        handle = {
            "alias": "Packages: classify email + call upsert",
            "mode": "parallel", "max": 20,
            "fields": {
                "sender":  {"description": "From: header"},
                "subject": {"description": "Subject"},
                "body":    {"description": "Plain-text body"},
            },
            "sequence": [
                {"variables": {
                    "haystack": "{{ (subject + '\\n' + body[:2000]) | lower }}",
                    # Merchant detection scans sender + first 3 KB of body so
                    # forwarded emails (where the outer From: is the user's
                    # own account) get classified by the carrier mentioned in
                    # the quoted body — e.g. a Starlink confirmation routed
                    # via "DHL Parcel: …" / dhl.de URL gets DHL DE.
                    "needle": "{{ (sender ~ '\\n' ~ body[:3000]) | lower }}",
                    "merchant": ("{%- if '@bol.com' in needle -%}Bol.com"
                                 "{%- elif '@coolblue.' in needle -%}Coolblue"
                                 "{%- elif 'aliexpress.com' in needle -%}AliExpress"
                                 "{%- elif 'temu.com' in needle -%}Temu"
                                 "{%- elif '@zooplus.' in needle -%}Zooplus"
                                 "{%- elif 'amazon.de' in needle or 'shipment-tracking.amazon' in needle -%}Amazon DE"
                                 "{%- elif '@dhl.de' in needle or '@dhl-paket.de' in needle or 'noreply@dhl' in needle or 'dhl.de/' in needle or 'dhl-paket' in needle or 'dhl parcel' in needle -%}DHL DE"
                                 "{%- elif '@trunkrs.nl' in needle or '@trunkrs.com' in needle or 'parcel.trunkrs.nl' in needle -%}Trunkrs"
                                 "{%- else -%}{%- endif -%}"),
                    "status": (
                        # pickup_ready: matched BEFORE delivered because a
                        # "delivered to parcel-shop" mail contains both
                        # "zugestellt" and pickup wording. The pickup hint
                        # wins so the user knows it still needs collection.
                        "{%- if haystack | regex_search('abholbereit|filiale abholber|in der (postfiliale|paketshop)|packstation|paketshop abholber|afhaalpunt|pakketpunt|ophaalpunt|pickup point|parcel (shop|point|locker)|ready for (pick.?up|collection)|available for collection|klaar (om )?af te halen|wordt afgehaald') -%}pickup_ready"
                        # delivered: require past-tense markers. "zugestellt"
                        # alone matches "wird zugestellt" (future) too, so
                        # we demand "ist/wurde/heute/am <past date>" near it.
                        "{%- elif haystack | regex_search('\\\\bis (bezorgd|afgeleverd)|\\\\bdelivered\\\\b|\\\\bafgeleverd om|(?:ist|wurde) zugestellt|signed by|\\\\barrived\\\\b|paket erhalten') -%}delivered"
                        "{%- elif haystack | regex_search('onderweg|uit voor bezorging|out for delivery|\\\\bshipped|verzonden|verstuurd|versandt|unterwegs|on its way|wordt (morgen|vandaag) bezorgd|ist unterwegs|wird (heute|morgen|am) .{0,30}zugestellt') -%}shipped"
                        "{%- elif haystack | regex_search('bevestig|order confirmation|order placed|bestellbest|payment received|paid|ontvangen') -%}ordered"
                        "{%- else -%}ordered{%- endif -%}"),
                    # First URL in the body — useful as a fallback identifier
                    # for emails that have nothing but a "click here" link
                    # (Trunkrs, some Temu/Ali campaigns, manual forwards).
                    "url": (
                        "{%- set u = (body ~ ' ' ~ subject) | regex_findall('https?://[^\\\\s<>\"]+') -%}"
                        "{%- if u -%}{{ u[0][:256] }}{%- endif -%}"),
                    "order_id": (
                        "{%- set h = (subject ~ ' ' ~ body[:4000]) | lower -%}"
                        # 1. Explicit DHL alpha-prefix tracking (e.g. CG976476655DE)
                        "{%- set dhl = h | regex_findall('\\\\b([a-z]{2}\\\\d{9}de)\\\\b') -%}"
                        "{%- if dhl -%}{{ dhl[0] | upper }}"
                        "{%- else -%}"
                          # 2. Word-prefixed numeric id (bestelnummer/order/sendungs/tracking)
                          "{%- set m = h | regex_findall('(?:bestelnummer|order(?:\\\\s*number|\\\\s*no\\\\.?|nummer)?|sendungsnummer|tracking[\\\\s-]?(?:number|nummer)?)[^a-z0-9]{0,10}([a-z0-9-]{6,30})') -%}"
                          "{%- if m -%}{{ m[0] }}"
                          "{%- else -%}"
                            # 3. Trunkrs URL: parcel.trunkrs.nl/<digits>/...
                            "{%- set tr = h | regex_findall('parcel\\\\.trunkrs\\\\.nl/(\\\\d{6,12})') -%}"
                            "{%- if tr -%}{{ tr[0] }}"
                            "{%- else -%}"
                              "{%- set a = h | regex_findall('\\\\b(\\\\d{15,17})\\\\b') -%}"
                              "{%- if a -%}{{ a[0] }}"
                              "{%- else -%}"
                                "{%- set d = h | regex_findall('\\\\b(\\\\d{3}-\\\\d{7}-\\\\d{7})\\\\b') -%}"
                                "{%- if d -%}{{ d[0] }}"
                                "{%- else -%}"
                                  # URL fallback (md5-prefixed)
                                  "{%- set u2 = (body ~ ' ' ~ subject) | regex_findall('https?://[^\\\\s<>\"]+') -%}"
                                  "{%- if u2 -%}url-{{ u2[0] | md5 | truncate(10, true, '') }}{%- endif -%}"
                                "{%- endif -%}"
                              "{%- endif -%}"
                            "{%- endif -%}"
                          "{%- endif -%}"
                        "{%- endif -%}"),
                    "eta": (
                        # ISO yyyy-mm-dd first
                        "{%- set m = (subject ~ ' ' ~ body[:1500]) | regex_findall('(\\\\d{4})-(\\\\d{2})-(\\\\d{2})') -%}"
                        "{%- if m -%}{{ '%04d-%02d-%02d' | format(m[0][0]|int, m[0][1]|int, m[0][2]|int) }}"
                        "{%- else -%}"
                          # German/EU dd.mm.yyyy (matches "am 21.05.2026")
                          "{%- set d = (subject ~ ' ' ~ body[:1500]) | regex_findall('\\\\b(\\\\d{1,2})\\\\.(\\\\d{1,2})\\\\.(\\\\d{4})') -%}"
                          "{%- if d -%}{{ '%04d-%02d-%02d' | format(d[0][2]|int, d[0][1]|int, d[0][0]|int) }}"
                          "{%- else -%}"
                            # NL/EU dd-mm-yyyy or dd/mm/yyyy
                            "{%- set d2 = (subject ~ ' ' ~ body[:1500]) | regex_findall('\\\\b(\\\\d{1,2})[-/](\\\\d{1,2})[-/](\\\\d{4})\\\\b') -%}"
                            "{%- if d2 -%}{{ '%04d-%02d-%02d' | format(d2[0][2]|int, d2[0][1]|int, d2[0][0]|int) }}"
                            "{%- elif (subject ~ body[:500]) | lower | regex_search('\\\\bmorgen|\\\\btomorrow') -%}"
                              "{{ (now() + timedelta(days=1)).strftime('%Y-%m-%d') }}"
                            "{%- elif (subject ~ body[:500]) | lower | regex_search('\\\\bvandaag|\\\\btoday') -%}"
                              "{{ now().strftime('%Y-%m-%d') }}"
                            "{%- else -%}{{ now().strftime('%Y-%m-%d') }}{%- endif -%}"
                          "{%- endif -%}"
                        "{%- endif -%}"),
                }},
                # Bail if not a known merchant — silent
                {"if": [{"condition":"template",
                         "value_template":"{{ merchant != '' and order_id != '' }}"}],
                 "then":[
                    {"service": "script.pkg_upsert",
                     "data": {
                       "merchant": "{{ merchant }}",
                       "order_id": "{{ order_id }}",
                       "label":    "{{ subject[:96] }}",
                       "eta":      "{{ eta }}",
                       "status":   "{{ status }}",
                       "url":      "{{ url }}",
                     }},
                 ]},
            ],
        }
        s,_ = rest("POST", "/api/config/script/config/pkg_handle_email", handle)
        print(f"  pkg_handle_email  HTTP {s}")

        # ------------------- AUTOMATION pkg_imap_router -------------------
        print("→ automation.pkg_imap_router")
        auto = {
            "id": "pkg_imap_router",
            "alias": "Packages: route incoming IMAP email",
            "description": "Fires on every imap_content event from the configured IMAP integration. Hands the email to pkg_handle_email.",
            "mode": "queued", "max": 20,
            "trigger": [{"platform": "event", "event_type": "imap_content"}],
            "action": [
                {"service": "script.pkg_handle_email",
                 "data": {
                   "sender":  "{{ trigger.event.data.sender }}",
                   "subject": "{{ trigger.event.data.subject }}",
                   "body":    "{{ trigger.event.data.text | default('') }}",
                 }},
            ],
        }
        s,_ = rest("POST", "/api/config/automation/config/pkg_imap_router", auto)
        print(f"  automation HTTP {s}")

        # ------------------- Reload everything -------------------
        print("→ reload integrations")
        for d, svc in [("input_text","reload"),("script","reload"),("automation","reload")]:
            await ws_call(ws, nx(), {"type":"call_service","domain":d,"service":svc})
        print("done.")

if __name__ == "__main__":
    asyncio.run(main())
