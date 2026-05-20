#!/bin/sh
# freetoon — on-device installer / updater.
#
# Run THIS on the Toon itself (you have local/SSH access). It pulls the
# latest GitHub release, swaps in the new toonui binary + helper scripts,
# makes sure the inittab launch row is present, and restarts the UI.
#
# One-liner:
#   curl -fsSL https://github.com/Ierlandfan/freetoon-lvgl/releases/latest/download/toon-selfinstall.sh | sh
#
# Re-running is safe (idempotent): it only adds the inittab row if missing
# and always backs up the current binary to /mnt/data/toonui.bak first.
set -e

REPO="Ierlandfan/freetoon-lvgl"
BASE="https://github.com/$REPO/releases/latest/download"
DEST="/mnt/data"
TMP="/tmp/freetoon.$$"
mkdir -p "$TMP"

say() { echo "[freetoon] $*"; }

dl() {  # dl <asset> <out>
    say "fetching $1"
    curl -fSL --connect-timeout 8 --max-time 120 -o "$2" "$BASE/$1"
}

# 1) toonui binary (required) — sanity-check the size (a GitHub error page or
# truncated download is tiny; the real binary is ~1 MB). busybox-safe.
dl toonui "$TMP/toonui"
SZ=$(wc -c < "$TMP/toonui" 2>/dev/null || echo 0)
if [ "$SZ" -lt 500000 ]; then
    say "ERROR: toonui download too small ($SZ bytes) — aborting."
    rm -rf "$TMP"; exit 1
fi

# 2) helper scripts — install only if missing so we never clobber local edits.
for s in ui_launcher.sh companion_gate.sh ot_mode_switch.sh; do
    if [ ! -f "$DEST/$s" ]; then
        dl "$s" "$TMP/$s" && cp "$TMP/$s" "$DEST/$s" && chmod +x "$DEST/$s"
    fi
done

# 3) swap the binary (back up the old one).
[ -f "$DEST/toonui" ] && cp "$DEST/toonui" "$DEST/toonui.bak"
cp "$TMP/toonui" "$DEST/toonui"
chmod +x "$DEST/toonui"
rm -rf "$TMP"
say "installed $(ls -l "$DEST/toonui" | awk '{print $5}') bytes -> $DEST/toonui"

# 4) make sure the launcher is wired into inittab (idempotent).
ROW="toon:345:respawn:$DEST/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
if [ -f "$DEST/ui_launcher.sh" ] && ! grep -q "^toon:" /etc/inittab 2>/dev/null; then
    say "adding inittab launch row"
    echo "$ROW" >> /etc/inittab
    telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true
fi

# 5) restart the UI (inittab respawns it).
say "restarting toonui"
kill "$(pidof toonui 2>/dev/null)" 2>/dev/null || pkill -x toonui 2>/dev/null || true
sleep 6
if pidof toonui >/dev/null 2>&1; then
    say "done — toonui is running (pid $(pidof toonui))."
else
    say "toonui not detected yet; it should respawn from inittab within ~10s."
fi
