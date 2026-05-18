#!/usr/bin/env python3
"""Merchant-aware delivery-email poller.

Connects to an IMAP inbox, classifies each UNSEEN message into one of the
known merchant adapters by SENDER first, falls back to a generic
shipping-language matcher. For each parsed event, POSTs an *upsert* to
Toon's /api/packages — the server dedups by (merchant, order_id) and
fires a state-change webhook when status advances.

Architecture:
    inbox ──► classify ──► merchant adapter ──► normalized event
                                                        │
                                          POST /api/packages on Toon
                                          (Toon handles dedup + state-change)

Add new merchants by appending to MERCHANTS. Each adapter is just:
    {
      'name':    "Bol.com",
      'match':   re.compile(...),         # match against From: header
      'states':  [(re.compile(...), 'shipped'), ...],   # subject/body → status
      'order':   re.compile(...),         # extract order id from subject/body
      'eta':     None or re.compile(...)  # optional override for date extraction
    }
"""
import email
import imaplib
import json
import os
import re
import sys
import urllib.request
from datetime import date, datetime, timedelta
from email.header import decode_header

IMAP_HOST    = os.environ.get("IMAP_HOST")
IMAP_PORT    = int(os.environ.get("IMAP_PORT", "993"))
IMAP_USER    = os.environ.get("IMAP_USER")
IMAP_PASS    = os.environ.get("IMAP_PASS")
IMAP_MAILBOX = os.environ.get("IMAP_MAILBOX", "INBOX")
TARGET_URL   = os.environ.get("TARGET_URL") or \
               "http://192.168.3.212:10081/api/packages"
DRY_RUN      = os.environ.get("DRY_RUN") == "1"

if not (IMAP_HOST and IMAP_USER and IMAP_PASS):
    sys.stderr.write("missing IMAP_HOST/USER/PASS\n"); sys.exit(2)

# ---------------------------------------------------------------------------
# Merchant adapters. Order matters — first match wins. Each `states` list is
# checked in order; the FIRST regex that matches the subject or body wins,
# so put "delivered" before "shipped" before "ordered" to short-circuit on
# the latest status mention.
# ---------------------------------------------------------------------------
MERCHANTS = [
    {
        'name':  'Bol.com',
        'match': re.compile(r'@bol\.com>?|noreply@bol\.com', re.I),
        'states': [
            (re.compile(r'\bafgeleverd|\bbezorgd|delivered', re.I),    'delivered'),
            (re.compile(r'onderweg|uit voor bezorging|out for delivery|verzonden|verstuurd|shipped', re.I),
                                                                       'shipped'),
            (re.compile(r'bevestig|order confirmation|ontvangen', re.I),'ordered'),
        ],
        # Bol order ids look like "9000000123456789" (16 digits) or "BOL-..."
        'order': re.compile(r'\b(?:bestelnummer|order)[^\d]{0,10}(\d{10,18})|\b(BOL-[A-Z0-9]+)', re.I),
    },
    {
        'name':  'Coolblue',
        'match': re.compile(r'@coolblue\.(nl|be|de)', re.I),
        'states': [
            (re.compile(r'bezorgd|afgeleverd|delivered', re.I),        'delivered'),
            (re.compile(r'onderweg|uit voor bezorging|verzonden|shipped|out for delivery', re.I),
                                                                       'shipped'),
            (re.compile(r'binnengekomen|ontvangen|bevestig', re.I),    'ordered'),
        ],
        'order': re.compile(r'(?:bestelnummer|order(?:nummer)?)[^\d]{0,10}(\d{6,12})', re.I),
    },
    {
        'name':  'AliExpress',
        'match': re.compile(r'@(?:aliexpress|notice\.aliexpress|transaction\.aliexpress)\.com', re.I),
        'states': [
            (re.compile(r'delivered|signed.{0,5}by|received', re.I),   'delivered'),
            (re.compile(r'on the way|out for delivery|shipped|on its way|verzonden', re.I),
                                                                       'shipped'),
            (re.compile(r'order placed|confirmation|paid|payment received', re.I), 'ordered'),
        ],
        # AliExpress order ids are 16-digit
        'order': re.compile(r'\border\s*(?:no\.?|#|number)?\s*[:#]?\s*(\d{15,17})', re.I),
    },
    {
        'name':  'Temu',
        'match': re.compile(r'@(?:temu|notify\.temu|mail\.temu)\.com', re.I),
        'states': [
            (re.compile(r'delivered|arrived', re.I),                   'delivered'),
            (re.compile(r'shipped|on the way|out for delivery', re.I), 'shipped'),
            (re.compile(r'order (?:placed|confirmed)|payment received',re.I),'ordered'),
        ],
        'order': re.compile(r'\border[^\d]{0,10}(\d{10,20})', re.I),
    },
    {
        'name':  'Zooplus',
        'match': re.compile(r'@zooplus\.(nl|de|com)', re.I),
        'states': [
            (re.compile(r'afgeleverd|bezorgd|delivered', re.I),        'delivered'),
            (re.compile(r'onderweg|verzonden|shipped|verstuurd', re.I),'shipped'),
            (re.compile(r'bestelbevestiging|order confirmation', re.I),'ordered'),
        ],
        'order': re.compile(r'(?:bestelling|order)[^\d]{0,10}(\d{8,12})', re.I),
    },
    {
        'name':  'Amazon DE',
        'match': re.compile(r'@(?:amazon\.de|marketplace\.amazon|shipment-tracking\.amazon)', re.I),
        'states': [
            (re.compile(r'delivered|zugestellt', re.I),                'delivered'),
            (re.compile(r'shipped|versandt|on the way|unterwegs', re.I),'shipped'),
            (re.compile(r'order confirmation|bestellbestätigung', re.I),'ordered'),
        ],
        'order': re.compile(r'\b(\d{3}-\d{7}-\d{7})\b'),
    },
]

# Generic fallback for "looks like shipping but unknown sender"
GENERIC = {
    'name': 'unknown',
    'states': [
        (re.compile(r'delivered|bezorgd|afgeleverd|zugestellt', re.I), 'delivered'),
        (re.compile(r'onderweg|uit voor bezorging|out for delivery|shipped|verstuurd|versandt', re.I),
                                                                      'shipped'),
        (re.compile(r'bestelbevestiging|order confirmation|order placed|bevestiging',re.I),'ordered'),
    ],
    'order': re.compile(r'\b(?:order|bestelling)[^\d]{0,10}([A-Z0-9]{6,20})', re.I),
}

# Date hints — same patterns as v1, kept inline so this script remains
# stdlib-only and droppable on any host with python3.
NL_MONTHS = {'jan':1,'feb':2,'maa':3,'mrt':3,'apr':4,'mei':5,'jun':6,
             'jul':7,'aug':8,'sep':9,'okt':10,'nov':11,'dec':12,
             'januari':1,'februari':2,'maart':3,'april':4,'mei':5,'juni':6,
             'juli':7,'augustus':8,'september':9,'oktober':10,'november':11,'december':12}
EN_MONTHS = {'january':1,'february':2,'march':3,'april':4,'may':5,'june':6,
             'july':7,'august':8,'september':9,'october':10,'november':11,'december':12,
             'jan':1,'feb':2,'mar':3,'apr':4,'jun':6,'jul':7,'aug':8,
             'sep':9,'oct':10,'nov':11,'dec':12}
def parse_month(s):
    s = s.lower().strip().rstrip('.')
    if s.isdigit(): return int(s)
    return NL_MONTHS.get(s) or EN_MONTHS.get(s)

DATE_PATTERNS = [
    (r'(?:verwacht|verwachte|expected|scheduled|delivery|levering|geleverd|bezorging)\s*(?:op|on|date|datum)?\s*(\d{1,2})[\s\-/](\w+|\d{1,2})[\s\-/]?(\d{2,4})?', 'parse_dmy'),
    (r'\b(\d{4})-(\d{2})-(\d{2})\b', 'iso'),
    (r'\b(\d{1,2})[-/](\d{1,2})[-/](\d{4})\b', 'dmy'),
    (r'\b(\d{1,2})[-/](\d{1,2})\b', 'dm'),
    (r'\b(?:morgen|tomorrow)\b', 'tomorrow'),
    (r'\b(?:vandaag|today)\b', 'today'),
]
def extract_eta(text):
    today = date.today(); tl = text.lower()
    for pat, kind in DATE_PATTERNS:
        m = re.search(pat, tl)
        if not m: continue
        try:
            if kind == 'parse_dmy':
                d = int(m.group(1)); mo = parse_month(m.group(2)); y = m.group(3)
                y = int(y) if y else today.year
                if y < 100: y += 2000
                if mo:
                    c = date(y, mo, d)
                    if c >= today - timedelta(days=2): return c.isoformat()
            elif kind == 'iso':
                y, mo, d = map(int, m.groups()); return f"{y:04d}-{mo:02d}-{d:02d}"
            elif kind == 'dmy':
                d, mo, y = map(int, m.groups()); return f"{y:04d}-{mo:02d}-{d:02d}"
            elif kind == 'dm':
                d, mo = map(int, m.groups()); y = today.year
                if mo < today.month or (mo == today.month and d < today.day - 2): y += 1
                return f"{y:04d}-{mo:02d}-{d:02d}"
            elif kind == 'today':    return today.isoformat()
            elif kind == 'tomorrow': return (today + timedelta(days=1)).isoformat()
        except (ValueError, TypeError):
            continue
    return None

def decode_h(s):
    if not s: return ""
    out = []
    for v, enc in decode_header(s):
        if isinstance(v, bytes):
            try:    out.append(v.decode(enc or 'utf-8', errors='replace'))
            except: out.append(v.decode('latin-1', errors='replace'))
        else:
            out.append(v)
    return "".join(out)

def first_text(msg):
    if msg.is_multipart():
        for part in msg.walk():
            if part.get_content_type() == 'text/plain':
                try: return part.get_payload(decode=True).decode(
                          part.get_content_charset() or 'utf-8', errors='replace')
                except: continue
        return ""
    try: return msg.get_payload(decode=True).decode(
              msg.get_content_charset() or 'utf-8', errors='replace')
    except: return ""

def classify(sender):
    """Return matching merchant adapter dict or None."""
    for m in MERCHANTS:
        if m['match'].search(sender): return m
    return None

def detect_status(adapter, subject, body):
    """Return first matching status from the adapter's `states` list."""
    text = subject + "\n" + body[:2000]
    for pat, st in adapter['states']:
        if pat.search(text): return st
    return 'ordered'  # default for known-merchant emails

def extract_order(adapter, subject, body):
    m = adapter['order'].search(subject) or adapter['order'].search(body[:4000])
    if not m: return None
    # First non-None capture group
    for g in m.groups():
        if g: return g
    return None

def post_event(merchant, order_id, label, eta, status, url, source):
    body = json.dumps({
        "merchant":  merchant,
        "order_id":  order_id,
        "label":     label,
        "eta":       eta,
        "status":    status,
        "url":       url,
        "source":    source,
    }).encode()
    req = urllib.request.Request(TARGET_URL, data=body,
                                  headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status

def run():
    with imaplib.IMAP4_SSL(IMAP_HOST, IMAP_PORT) as M:
        M.login(IMAP_USER, IMAP_PASS)
        M.select(IMAP_MAILBOX)
        typ, data = M.search(None, 'UNSEEN')
        if typ != 'OK' or not data[0]:
            print("no unseen messages"); return
        ids = data[0].split()
        print(f"scanning {len(ids)} unseen messages")
        for uid in ids:
            typ, msgdata = M.fetch(uid, '(BODY.PEEK[])')
            if typ != 'OK': continue
            msg = email.message_from_bytes(msgdata[0][1])
            sender  = decode_h(msg.get('From', ''))
            subject = decode_h(msg.get('Subject', ''))
            body    = first_text(msg)

            adapter = classify(sender)
            if not adapter:
                # fallback to generic if shipping language present
                txt = (subject + " " + body[:500]).lower()
                if not any(t in txt for t in
                       ['verwacht','bezorg','geleverd','verzonden','shipped',
                        'shipment','delivery','tracking','pakket','parcel',
                        'arriveert','order confirmation','bestelbevestiging']):
                    continue
                adapter = GENERIC
                adapter['name'] = sender.split('@')[-1].rstrip('>').rstrip('"').strip() or 'unknown'

            status = detect_status(adapter, subject, body)
            order  = extract_order(adapter, subject, body)
            eta    = extract_eta(subject) or extract_eta(body) or date.today().isoformat()
            label  = (re.sub(r'\s+', ' ', subject).strip())[:96] or adapter['name']
            url_m  = re.search(r'https?://\S+', body)
            url    = url_m.group(0)[:256] if url_m else ''

            print(f"  + [{adapter['name']:10}] {status:9} order={order!r:14} eta={eta} :: {label[:60]!r}")
            if DRY_RUN:
                continue
            try:
                post_event(adapter['name'], order, label, eta, status, url, 'email')
                M.store(uid, '+FLAGS', '\\Seen')
            except Exception as e:
                print(f"    post failed: {e}")

if __name__ == '__main__':
    run()
