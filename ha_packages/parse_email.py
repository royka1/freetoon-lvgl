#!/usr/bin/env python3
"""Stdin-fed merchant email parser. Receives one email's metadata as JSON
on stdin, applies the merchant adapters, POSTs the parsed event to Toon's
/api/packages (which upserts and fires state-change notifications).

Designed to be called from HA's `shell_command:` integration on every
imap_content event. Stays stdlib-only so it runs anywhere python3 runs —
including inside the HA core container.

Input (stdin, JSON):
    {"from": "...", "subject": "...", "body": "..."}

Env:
    TARGET_URL  default http://192.168.3.212:10081/api/packages

Output:
    JSON line on stdout describing what was upserted (parseable by HA's
    `command_line` integration if you ever want to expose a sensor).
"""
import json, os, re, sys, urllib.request
from datetime import date, timedelta

TARGET_URL = os.environ.get("TARGET_URL",
                             "http://192.168.3.212:10081/api/packages")

# ---- merchant adapters: keep in sync with email_poller.py ----
MERCHANTS = [
    {'name':'Bol.com', 'match': re.compile(r'@bol\.com>?|noreply@bol\.com', re.I),
     'states':[
       # Past-tense / completed delivery only. "morgen bezorgd" = future,
       # don't match. Require "is bezorgd", "is afgeleverd", "delivered"
       # as a word, etc.
       (re.compile(r'\bis\s+(?:bezorgd|afgeleverd)\b|\bafgeleverd\s+(?:om|op)\b|\bdelivered\b|pakket\s+(?:is\s+)?(?:bezorgd|afgeleverd)', re.I), 'delivered'),
       (re.compile(r'onderweg|uit voor bezorging|out for delivery|verzonden|verstuurd|\bshipped\b|wordt\s+(?:morgen|vandaag)\s+bezorgd', re.I),'shipped'),
       (re.compile(r'bevestig|order confirmation|ontvangen', re.I), 'ordered'),
     ],
     'order': re.compile(r'\b(?:bestelnummer|order)[^\d]{0,10}(\d{10,18})|\b(BOL-[A-Z0-9]+)', re.I)},
    {'name':'Coolblue', 'match': re.compile(r'@coolblue\.(nl|be|de)', re.I),
     'states':[
       (re.compile(r'bezorgd|afgeleverd|delivered', re.I), 'delivered'),
       (re.compile(r'onderweg|uit voor bezorging|verzonden|shipped|out for delivery', re.I),'shipped'),
       (re.compile(r'binnengekomen|ontvangen|bevestig', re.I),'ordered'),
     ],
     'order': re.compile(r'(?:bestelnummer|order(?:nummer)?)[^\d]{0,10}(\d{6,12})', re.I)},
    {'name':'AliExpress', 'match': re.compile(r'@(?:aliexpress|notice\.aliexpress|transaction\.aliexpress)\.com', re.I),
     'states':[
       (re.compile(r'delivered|signed.{0,5}by|received', re.I),'delivered'),
       (re.compile(r'on the way|out for delivery|shipped|on its way|verzonden', re.I),'shipped'),
       (re.compile(r'order placed|confirmation|paid|payment received', re.I),'ordered'),
     ],
     'order': re.compile(r'\border\s*(?:no\.?|#|number)?\s*[:#]?\s*(\d{15,17})', re.I)},
    {'name':'Temu', 'match': re.compile(r'@(?:temu|notify\.temu|mail\.temu)\.com', re.I),
     'states':[
       (re.compile(r'delivered|arrived', re.I),'delivered'),
       (re.compile(r'shipped|on the way|out for delivery', re.I),'shipped'),
       (re.compile(r'order (?:placed|confirmed)|payment received',re.I),'ordered'),
     ],
     'order': re.compile(r'\border[^\d]{0,10}(\d{10,20})', re.I)},
    {'name':'Zooplus', 'match': re.compile(r'@zooplus\.(nl|de|com)', re.I),
     'states':[
       (re.compile(r'afgeleverd|bezorgd|delivered', re.I),'delivered'),
       (re.compile(r'onderweg|verzonden|shipped|verstuurd', re.I),'shipped'),
       (re.compile(r'bestelbevestiging|order confirmation', re.I),'ordered'),
     ],
     'order': re.compile(r'(?:bestelling|order)[^\d]{0,10}(\d{8,12})', re.I)},
    {'name':'Amazon DE', 'match': re.compile(r'@(?:amazon\.de|marketplace\.amazon|shipment-tracking\.amazon)', re.I),
     'states':[
       (re.compile(r'delivered|zugestellt', re.I),'delivered'),
       (re.compile(r'shipped|versandt|on the way|unterwegs', re.I),'shipped'),
       (re.compile(r'order confirmation|bestellbestätigung', re.I),'ordered'),
     ],
     'order': re.compile(r'\b(\d{3}-\d{7}-\d{7})\b')},
]

# date extraction (NL+EN); keep tight so unknowns just default to today
def extract_eta(text):
    today = date.today(); tl = text.lower()
    m = re.search(r'\b(\d{4})-(\d{2})-(\d{2})\b', tl)
    if m: return f"{int(m.group(1)):04d}-{int(m.group(2)):02d}-{int(m.group(3)):02d}"
    m = re.search(r'\b(\d{1,2})[-/](\d{1,2})[-/](\d{4})\b', tl)
    if m:
        d,mo,y = map(int, m.groups()); return f"{y:04d}-{mo:02d}-{d:02d}"
    if re.search(r'\bmorgen|tomorrow\b', tl):
        return (today + timedelta(days=1)).isoformat()
    if re.search(r'\bvandaag|today\b', tl):
        return today.isoformat()
    return today.isoformat()

def classify(sender):
    for m in MERCHANTS:
        if m['match'].search(sender): return m
    return None

def extract_status(adapter, txt):
    for pat, st in adapter['states']:
        if pat.search(txt): return st
    return 'ordered'

def extract_order(adapter, subject, body):
    m = adapter['order'].search(subject) or adapter['order'].search(body[:4000])
    if not m: return None
    for g in m.groups():
        if g: return g
    return None

def main():
    raw = sys.stdin.read()
    if not raw:
        print(json.dumps({"ok": False, "err": "empty stdin"}))
        return 1
    try:
        d = json.loads(raw)
    except json.JSONDecodeError as e:
        print(json.dumps({"ok": False, "err": f"json: {e}"}))
        return 1
    sender  = str(d.get("from", ""))
    subject = str(d.get("subject", ""))
    body    = str(d.get("body", ""))

    adapter = classify(sender)
    if not adapter:
        print(json.dumps({"ok": False, "err": "unknown sender", "sender": sender}))
        return 0   # not an error — just nothing to do
    status = extract_status(adapter, subject + "\n" + body[:2000])
    order  = extract_order(adapter, subject, body) or ""
    eta    = extract_eta(subject + " " + body[:1000])
    label  = re.sub(r'\s+', ' ', subject).strip()[:96] or adapter['name']

    payload = {
        "merchant":  adapter['name'],
        "order_id":  order,
        "label":     label,
        "eta":       eta,
        "status":    status,
        "source":    "email",
    }
    req = urllib.request.Request(TARGET_URL,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            print(json.dumps({"ok": True, "post": payload,
                              "http": r.status, "body": r.read().decode()[:200]}))
    except Exception as e:
        print(json.dumps({"ok": False, "err": f"post: {e}", "payload": payload}))
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
