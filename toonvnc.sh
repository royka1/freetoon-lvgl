#!/bin/sh
# toonvnc.sh — start a VNC server for the Toon's framebuffer (toonui / qt-gui).
#
# Uses x11vnc in -rawfb mode (no X server) to serve /dev/fb0. Touch input
# from the VNC client is injected into the real touchscreen via x11vnc's
# built-in `-uinput direct_abs=` (no external bridge binary needed).
#
# Usage:  toonvnc.sh [start|stop|restart|status|respawn]   (default: start)
# Connect:  vncviewer <toon-ip>:5900
#
# `respawn` is what /etc/inittab calls — exec x11vnc in the foreground (no
# `-bg`) so init owns the pid and restarts it on crash / OOM. Logs go to
# stderr → inittab's `>> /var/volatile/tmp/x11vnc.log 2>&1`.
#
# Password: if /mnt/data/toonvnc.plain exists and is non-empty its first line
# is used as the VNC password (x11vnc -passwdfile). toonui's Settings >
# Remote control modal writes this file. No file / empty = no password.

PORT=5900
INPUT=/mnt/data/fbvnc_input
PASSFILE=/mnt/data/toonvnc.plain
LOG=/tmp/x11vnc.log
TS_DEV=/dev/input/event1
[ -e /dev/input/event0 ] && TS_DEV=/dev/input/event0  # Toon 1

# Prefer the system x11vnc; fall back to the bundled copy. The Toon's opkg
# feed (feed.hae.int) is VPN-only, so the installer ships x11vnc + its libs
# under /mnt/data/x11vnc-bundle and we run it with LD_LIBRARY_PATH.
X11VNC=/usr/bin/x11vnc
if [ ! -x "$X11VNC" ]; then
  if [ -x /mnt/data/x11vnc-bundle/bin/x11vnc ]; then
    X11VNC=/mnt/data/x11vnc-bundle/bin/x11vnc
    export LD_LIBRARY_PATH="/mnt/data/x11vnc-bundle/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  else
    X11VNC=x11vnc
  fi
fi

# busybox pgrep/pkill -x don't reliably match x11vnc on this device (they return
# nothing while the process is clearly running), which broke stop/restart/status
# and the "already running" guard. Match via ps instead.
_vnc_pids() { ps w 2>/dev/null | grep -E '[x]11vnc' | awk '{print $1}'; }
_vnc_running() { [ -n "$(_vnc_pids)" ]; }
_vnc_kill() { for _p in $(_vnc_pids); do kill "$_p" 2>/dev/null; done; }

# Some Toon panels page-flip / pan the framebuffer: /dev/fb0 is double-height
# (e.g. 1024x1200) and the *visible* image sits at a non-zero Y offset. x11vnc
# -rawfb reads from byte 0 by default, so it would serve the stale back page
# (a frozen screenshot). Compute the live offset from the kernel's pan info and
# feed it to x11vnc. On non-panned panels (yoffset 0) this is a no-op.
# Detect framebuffer geometry from sysfs so we don't hardcode 1024x600x32
# (Toon 1 is 800x480x16, Toon 2 is 1024x600x32).
#
# virtual_size gives the full framebuffer (may be padded wider or double-height
# for page-flip).  mode gives the visible window;  serve the VISIBLE width AND
# height — the virtual height runs past the live page and makes x11vnc's mmap
# fail (-> slow lseek + garbage rows).  The live page's byte offset (below)
# targets the panned page.
_fb_w=1024; _fb_h=600; _fb_bpp=32
# virtual_size is COMMA-separated ("800,960"); split on comma, or the whole
# value lands in _fb_w and _fb_h keeps the wrong default (the 600 that made the
# pointer Y-scale wrong and the served image taller than the buffer).
[ -r /sys/class/graphics/fb0/virtual_size ] && \
    IFS=, read _fb_w _fb_h < /sys/class/graphics/fb0/virtual_size 2>/dev/null || true
[ -r /sys/class/graphics/fb0/bits_per_pixel ] && \
    read _fb_bpp < /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null || true
case "$_fb_w"  in ''|*[!0-9]*) _fb_w=1024;; esac
case "$_fb_h"  in ''|*[!0-9]*) _fb_h=600;; esac
case "$_fb_bpp" in ''|*[!0-9]*) _fb_bpp=32;; esac

# Extract visible resolution from the mode string: "U:800x480p-0" or similar.
_vis_w=$_fb_w; _vis_h=$_fb_h
if [ -r /sys/class/graphics/fb0/mode ]; then
    _mode=$(tr -d '\n' < /sys/class/graphics/fb0/mode 2>/dev/null)
    _mode_w="${_mode#*:}"; _mode_w="${_mode_w%%x*}"
    _mode_h="${_mode#*x}";  _mode_h="${_mode_h%%[!0-9]*}"
    case "$_mode_w" in ''|*[!0-9]*) ;; *) _vis_w=$_mode_w;; esac
    case "$_mode_h" in ''|*[!0-9]*) ;; *) _vis_h=$_mode_h;; esac
fi

_pan=$(cat /sys/class/graphics/fb0/pan 2>/dev/null)   # "xoff,yoff"
_yoff=${_pan#*,}
case "$_yoff" in ''|*[!0-9]*) _yoff=0;; esac
_stride=$(cat /sys/class/graphics/fb0/stride 2>/dev/null)
case "$_stride" in ''|*[!0-9]*) _stride=$((_vis_w * _fb_bpp / 8));; esac
_off=$((_yoff * _stride))
# Clamp the served height so [offset, offset + H*stride) stays inside the fb.
_avail_h=$((_fb_h - _yoff))
[ "$_avail_h" -gt 0 ] && [ "$_vis_h" -gt "$_avail_h" ] && _vis_h=$_avail_h
# x11vnc wants the byte offset AFTER the @WxHxB geometry, e.g.
#   map:/dev/fb0@800x480x16+768000
RAWFB="map:/dev/fb0@${_vis_w}x${_vis_h}x${_fb_bpp}"
[ "$_off" -gt 0 ] && RAWFB="map:/dev/fb0@${_vis_w}x${_vis_h}x${_fb_bpp}+$_off"

# ---- Pointer injection (built once, used by start + respawn) -------------
# x11vnc writes ABS events straight to the touchscreen node (direct_abs); this
# kernel has no uinput module, so `nouinput` skips a futile uinput attempt.
#
# Toon 1's TSC2007 reports a 0..4095 ADC range and toonui's toon1_touch rescales
# that to screen pixels. Left alone, x11vnc writes the raw VNC PIXEL as the ABS
# value, so toonui divides it by ~5x and collapses every click into the
# top-left corner. Feed x11vnc a tslib calibration that pre-scales VNC pixels
# (0.._vis-1) up into 0..4095; x11vnc applies it inverted, so the magnitude
# coefficient is s*(dim-1)/ABS_MAX with s=65536, and toonui's /4095 rescale then
# lands the click.
#
# toon1_touch ALSO applies the panel-mount corrections touch_swap_xy /
# touch_invert_x / touch_invert_y to *every* event0 event, including ours — so
# we must pre-compensate or VNC ends up mirrored/rotated relative to real touch.
# Per axis: not inverted -> +mag, offset 0; inverted -> -mag, offset ABS_MAX*mag
# (x11vnc solves raw = (vnc*s - off)/coef). swap puts the X mapping on raw_y and
# the Y mapping on raw_x. (Not needed on Toon 2, whose touch reports pixel range.)
if grep -qi 'tsc2007' /proc/bus/input/devices 2>/dev/null; then
    _absmax=4095; _s=65536
    PCAL=/tmp/toonvnc_pointercal
    _cfg=/mnt/data/toonui.cfg
    _ix=$(sed -n 's/^touch_invert_x=\([0-9]*\).*/\1/p' "$_cfg" 2>/dev/null)
    _iy=$(sed -n 's/^touch_invert_y=\([0-9]*\).*/\1/p' "$_cfg" 2>/dev/null)
    _sw=$(sed -n 's/^touch_swap_xy=\([0-9]*\).*/\1/p'  "$_cfg" 2>/dev/null)
    [ -z "$_ix" ] && _ix=0; [ -z "$_iy" ] && _iy=0; [ -z "$_sw" ] && _sw=0
    _magx=$(( (_s*(_vis_w-1)+_absmax/2)/_absmax ))
    _magy=$(( (_s*(_vis_h-1)+_absmax/2)/_absmax ))
    if [ "$_ix" = 1 ]; then _kx=$((-_magx)); _cx=$((_absmax*_magx)); else _kx=$_magx; _cx=0; fi
    if [ "$_iy" = 1 ]; then _ky=$((-_magy)); _cy=$((_absmax*_magy)); else _ky=$_magy; _cy=0; fi
    if [ "$_sw" = 1 ]; then            # a b c d e f s  (X<-raw_y, Y<-raw_x)
        printf '0 %s %s %s 0 %s %s\n' "$_kx" "$_cx" "$_ky" "$_cy" "$_s" > "$PCAL" 2>/dev/null
    else                               # a b c d e f s  (X<-raw_x, Y<-raw_y)
        printf '%s 0 %s 0 %s %s %s\n' "$_kx" "$_cx" "$_ky" "$_cy" "$_s" > "$PCAL" 2>/dev/null
    fi
    TOUCH="-pipeinput UINPUT:touch,tslib_cal=$PCAL,direct_abs=$TS_DEV,multitouch,btn_touch=0,nouinput"
else
    TOUCH="-pipeinput UINPUT:direct_abs=$TS_DEV,multitouch,btn_touch=0,touchscreen,abs"
fi
if ! "$X11VNC" -help 2>&1 | grep -q 'direct_abs'; then
    TOUCH=""
    if [ -x "$INPUT" ]; then
        TOUCH="-pipeinput reopen:$INPUT"
    else
        echo "WARNING: direct_abs not available and $INPUT missing — view-only mode"
    fi
fi

case "${1:-start}" in
  stop)
    if _vnc_running; then _vnc_kill; echo "x11vnc stopped"; else echo "x11vnc not running"; fi
    ;;
  restart)
    _vnc_kill
    sleep 1
    exec "$0" start
    ;;
  status)
    if _vnc_running; then
      echo "x11vnc running (pid $(_vnc_pids | tr '\n' ' ')) on port $PORT"
    else
      echo "x11vnc not running"
    fi
    ;;
  start)
    if _vnc_running; then
      echo "x11vnc already running (pid $(_vnc_pids | tr '\n' ' '))"
      exit 0
    fi
    if [ -s "$PASSFILE" ]; then
      AUTH="-passwdfile $PASSFILE"
    else
      AUTH="-nopw"
    fi
    "$X11VNC" \
      -rawfb "$RAWFB" \
      $TOUCH \
      -rfbport $PORT -forever -shared -nocursor \
      -desktop ToonUI $AUTH \
      -bg -o "$LOG" >/dev/null 2>&1
    sleep 1
    if _vnc_running; then
      echo "x11vnc started on port $PORT (log: $LOG)"
    else
      echo "x11vnc failed to start — see $LOG"; tail -n 5 "$LOG" 2>/dev/null
    fi
    ;;
  respawn)
    # Foreground variant for inittab. Identical args to `start` minus the
    # `-bg` flag, so init keeps a real child pid and respawns on death.
    if [ -s "$PASSFILE" ]; then
      AUTH="-passwdfile $PASSFILE"
    else
      AUTH="-nopw"
    fi
    # exec replaces the shell with x11vnc, so init sees x11vnc directly
    # (clean `pkill -x x11vnc` from the existing stop/restart actions).
    exec "$X11VNC" \
      -rawfb "$RAWFB" \
      $TOUCH \
      -rfbport $PORT -forever -shared -nocursor \
      -desktop ToonUI $AUTH
    ;;
  *)
    echo "usage: $0 [start|stop|restart|status|respawn]"; exit 1
    ;;
esac
