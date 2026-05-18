#!/usr/bin/env bash
# Toon Custom UI installer.
#
# Pushes the toonui binary + companion bridges to a stock Toon and wires
# inittab entries so they start on boot. Idempotent — re-running upgrades
# the binaries and refreshes config without duplicating inittab rows.
#
# Companions:
#   - toonui            LVGL UI (replaces qt-gui), serves PWA on :10081
#   - quby_bridge       Quby<->OT bridge: proxy (default), active, or off
#   - p1bridge          HomeWizard P1 -> BoxTalk publisher
#   - toontap           Cross-compiled touch-event injector (debug helper)
#   - ot_mode_switch.sh Helper called by toonui Settings UI to flip
#                       off/proxy/wireless modes (rewrites inittab + OTGW GW)
#   - PWA static files  index.html / app.js / sw.js / icon-192.png in
#                       /mnt/data/pwa/ — served by toonui's :10081 endpoint
#
# Required input:
#   TOON_HOST   (default: 192.168.3.212)
#   TOON_PASS   (default: toon)
#
# Optional input (passed straight through to the bridges):
#   VENT_USER, VENT_PASS         — Itho-Wifi credentials  → /mnt/data/vent.conf
#   P1_TOKEN                     — HomeWizard P1 v2 bearer → /mnt/data/p1bridge.conf
#
# Required external dep on the install host: sshpass, ssh, scp.
#
# Usage:
#   ./install.sh                  # install/upgrade with defaults
#   TOON_HOST=192.168.1.50 ./install.sh
#   ./install.sh --uninstall      # remove inittab entries + delete binaries

set -euo pipefail

TOON_HOST="${TOON_HOST:-192.168.3.212}"
TOON_PASS="${TOON_PASS:-toon}"
TOON_USER="${TOON_USER:-root}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Where artefacts live in this dev tree. Each must already be cross-compiled
# for armv7-hardfloat. If you cloned a binary release tarball these are
# next to install.sh; if you built from source they're inside the build
# subdirs of each component.
TOONUI_BIN="$HERE/lvgl_ui_recovered/build/toonui"
QUBY_BIN="$HERE/quby_bridge/quby_bridge"
P1_BIN="$HERE/p1bridge/p1bridge"
TOONTAP_BIN="$HERE/../qt_rebuild/toontap"           # cross-compiled in /tmp/qt_rebuild
# Prefer the in-tree copies so the install bundle is self-contained; fall
# back to the dev /tmp paths if the user hasn't run ./tools/sync-static.sh
OT_MODE_SCRIPT="${OT_MODE_SCRIPT:-$HERE/scripts/ot_mode_switch.sh}"
PWA_DIR="${PWA_DIR:-$HERE/pwa_static}"

SSH="sshpass -p $TOON_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $TOON_USER@$TOON_HOST"
SCP="sshpass -p $TOON_PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# Inittab rows we own. <id>:<runlevels>:<action>:<command>
# Keep the id stable so re-runs upgrade cleanly instead of stacking.
TOONUI_LINE="toon:345:respawn:/mnt/data/toonui >> /var/volatile/tmp/toonui.log 2>&1"
# Default to proxy mode — shuttles bytes happ_thermstat<->keteladapter 1:1
# AND publishes BoilerInfo to BoxTalk. Original heat path preserved, PWA
# boiler card lit. Users can flip to off/wireless via toonui Settings UI
# (which rewrites this row via /mnt/data/ot_mode_switch.sh).
QUBY_LINE="qbri:345:respawn:/mnt/data/quby_bridge -m proxy >> /var/volatile/tmp/quby_bridge.log 2>&1"
P1_LINE="p1br:345:respawn:/mnt/data/p1bridge >> /var/volatile/tmp/p1bridge.log 2>&1"

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: '$1' not on PATH — apt-get install sshpass" >&2
        exit 2
    }
}

remote() { $SSH "$@"; }

check_artefacts() {
    local missing=0
    for f in "$TOONUI_BIN" "$QUBY_BIN" "$P1_BIN" "$TOONTAP_BIN"; do
        if [[ ! -x "$f" ]]; then
            echo "  missing: $f" >&2
            missing=1
        fi
    done
    if [[ ! -f "$OT_MODE_SCRIPT" ]]; then
        echo "  missing: $OT_MODE_SCRIPT (mode-switch helper)" >&2
        missing=1
    fi
    if [[ ! -f "$PWA_DIR/index.html" ]]; then
        echo "  missing PWA static dir: $PWA_DIR/index.html" >&2
        missing=1
    fi
    if (( missing )); then
        echo "Run 'make' in lvgl_ui_recovered/src/, p1bridge/, quby_bridge/" >&2
        echo "(and compile toontap.c via toolchain) before installing." >&2
        echo "PWA files should be checked in under pwa_static/." >&2
        exit 3
    fi
}

# Push a binary atomically: scp to .new, rename in place — survives
# overwriting a running ELF (kernel keeps the old text mapped).
push_atomic() {
    local src="$1" dest="$2"
    echo "  → $dest"
    $SCP "$src" "$TOON_USER@$TOON_HOST:${dest}.new"
    remote "mv -f ${dest}.new ${dest} && chmod +x ${dest}"
}

write_remote_file() {
    local content="$1" dest="$2"
    echo "  → $dest"
    printf '%s' "$content" | remote "cat > ${dest}.new && mv -f ${dest}.new ${dest}"
}

# Replace-or-append a single inittab row keyed on its leading "id:" field.
# Always pkill the existing process so respawn picks the new binary.
upsert_inittab_row() {
    local row="$1"
    local id="${row%%:*}"
    remote "grep -v '^${id}:' /etc/inittab > /etc/inittab.new && echo '${row}' >> /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

drop_inittab_row() {
    local id="$1"
    remote "grep -v '^${id}:' /etc/inittab > /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

reload_init() {
    # kill -HUP 1 makes init re-read /etc/inittab without rebooting.
    remote "kill -HUP 1"
}

# ----------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------
do_install() {
    echo "[1/6] Checking artefacts..."
    check_artefacts

    echo "[2/6] Pushing binaries to $TOON_HOST..."
    push_atomic "$TOONUI_BIN"   "/mnt/data/toonui"
    push_atomic "$QUBY_BIN"     "/mnt/data/quby_bridge"
    push_atomic "$P1_BIN"       "/mnt/data/p1bridge"
    push_atomic "$TOONTAP_BIN"  "/mnt/data/toontap"
    push_atomic "$OT_MODE_SCRIPT" "/mnt/data/ot_mode_switch.sh"

    echo "[2b/6] Pushing PWA static files to /mnt/data/pwa/..."
    remote "mkdir -p /mnt/data/pwa"
    for f in index.html app.js sw.js manifest.json icon-192.png; do
        if [[ -f "$PWA_DIR/$f" ]]; then
            echo "  → /mnt/data/pwa/$f"
            $SCP "$PWA_DIR/$f" "$TOON_USER@$TOON_HOST:/mnt/data/pwa/${f}.new"
            remote "mv -f /mnt/data/pwa/${f}.new /mnt/data/pwa/${f}"
        fi
    done

    echo "[3/6] Writing companion configs..."
    if [[ -n "${VENT_USER:-}" && -n "${VENT_PASS:-}" ]]; then
        write_remote_file "$VENT_USER:$VENT_PASS"$'\n' /mnt/data/vent.conf
    else
        echo "  (skip vent.conf — set VENT_USER + VENT_PASS to write it)"
    fi
    if [[ -n "${P1_TOKEN:-}" ]]; then
        # p1bridge.conf maps <p1_host>=<v2 token> per line. Default host
        # is 192.168.99.69 — override by adding more lines after install.
        write_remote_file "192.168.99.69=$P1_TOKEN"$'\n' /mnt/data/p1bridge.conf
    else
        echo "  (skip p1bridge.conf — set P1_TOKEN to write it)"
    fi

    echo "[4/6] Stopping any running instances so the new binary respawns..."
    remote "pkill -x toonui      2>/dev/null; pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    # quby_bridge bind-mounts a PTY over /dev/ttymxc0. A stale mount blocks
    # the new instance; lazy-unmount lets existing happ_thermstat fd linger
    # while freeing the path so re-bind succeeds.
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"

    echo "[5/6] Wiring /etc/inittab..."
    upsert_inittab_row "$TOONUI_LINE"
    upsert_inittab_row "$QUBY_LINE"
    upsert_inittab_row "$P1_LINE"

    echo "[6/6] Reloading init (kill -HUP 1)..."
    reload_init

    echo
    echo "Install complete. Sanity-check:"
    sleep 4
    remote "pgrep -fa 'toonui|quby_bridge|p1bridge'"
}

# ----------------------------------------------------------------------
# Uninstall
# ----------------------------------------------------------------------
do_uninstall() {
    echo "[1/3] Dropping inittab rows..."
    drop_inittab_row toon
    drop_inittab_row qbri
    drop_inittab_row p1br

    echo "[2/3] Killing running processes..."
    remote "pkill -x toonui      2>/dev/null; pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"

    echo "[3/3] Removing binaries + configs + PWA + script..."
    remote "rm -f /mnt/data/toonui /mnt/data/quby_bridge /mnt/data/p1bridge /mnt/data/toontap /mnt/data/ot_mode_switch.sh /mnt/data/vent.conf /mnt/data/p1bridge.conf"
    remote "rm -rf /mnt/data/pwa"

    reload_init
    echo "Uninstalled."
}

# ----------------------------------------------------------------------
require sshpass
require ssh
require scp

case "${1:-install}" in
    install|"")  do_install   ;;
    --uninstall) do_uninstall ;;
    -h|--help)
        sed -n '2,30p' "$0"
        ;;
    *) echo "Unknown command '$1' — see --help" >&2; exit 1 ;;
esac
