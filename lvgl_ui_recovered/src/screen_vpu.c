/*
 * VPU Video settings screen — Toon 1 only.
 *
 * Detects the i.MX27 VPU driver / firmware / vpu_stream binary, offers
 * one-tap download+install for each missing component, and once everything
 * is ready shows the vpu_stream options plus a QR code linking to the
 * streaming-scripts repository.
 *
 * Layout is scrollable (content exceeds the 480 px Toon 1 panel).  y
 * values are design-space (1024×600) integers; SY() converts them to
 * display pixels at alignment time.  Row-height / gap constants stay in
 * design space so the y counter doesn't double-scale.
 */
#include "screens.h"
#include "display.h"
#include "settings.h"
#include "i18n.h"

#include "lvgl/src/extra/libs/qrcode/lv_qrcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <errno.h>

/* ---- URLs --------------------------------------------------------------- */
#define VPU_DRV_URL \
    "https://github.com/royka1/toon-vpu/raw/refs/heads/main/prebuilt/mxc_vpu.ko"
#define VPU_FW_URL \
    "https://github.com/royka1/toon-vpu/raw/refs/heads/main/prebuilt/firmware/vpu_fw_imx27_TO2.bin"
#define VPU_BIN_URL \
    "https://github.com/royka1/toon-vpu/raw/refs/heads/main/prebuilt/vpu_stream"
#define VPU_SCRIPTS_URL \
    "https://github.com/royka1/toon-vpu/tree/main/scripts"

/* ---- Paths -------------------------------------------------------------- */
#define VPU_DRV_PATH \
    "/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko"
#define VPU_FW_PATH  "/lib/firmware/vpu/vpu_fw_imx27_TO2.bin"
#define VPU_BIN_PATH "/root/vpu/vpu_stream"
#define VPU_DEV_PATH "/dev/mxc_vpu"
#define VPU_MOD_OPTS "mxc_vpu hclk_max=1 iram_size=0xb000 allow_prp=1"

/* ---- Layout constants (design space 1024×600, NOT pre-scaled) ----------- */
#define ROW_H     44         /* option-row height */
#define ROW_GAP   6          /* gap between rows */
#define SEC_GAP   16         /* extra gap before a new section */
#define LABEL_X   10
#define INPUT_X   280
#define INPUT_W   100        /* small textarea (W, H, %, port, pos, prebuf) */
#define INPUT_W_W 500        /* wide textarea (RTSP URL) */
#define SW_W      64
#define SW_H      32

/* ---- Per-component status flags ----------------------------------------- */
static int g_kernel_ok  = 0;
static int g_drv_loaded = 0;
static int g_drv_file   = 0;
static int g_fw_ok      = 0;
static int g_bin_ok     = 0;

static lv_obj_t * g_lbl_status;
static lv_obj_t * g_lbl_kernel;
static lv_obj_t * g_lbl_driver;
static lv_obj_t * g_lbl_fw;
static lv_obj_t * g_lbl_bin;

/* Option controls */
static lv_obj_t * g_ta_w, * g_ta_h, * g_ta_rtp, * g_ta_rtsp;
static lv_obj_t * g_ta_prebuf, * g_ta_x, * g_ta_y;
static lv_obj_t * g_sw_codec, * g_lbl_codec_val;
static lv_obj_t * g_lbl_mode_val;            /* "RTP" / "RTSP" label next to mode switch */
static lv_obj_t * g_lbl_rtp, * g_lbl_rtsp;   /* labels for mode-dependent rows */
static lv_obj_t * g_lbl_warm_url1, * g_lbl_warm_url2;  /* doorbell hint URLs */

/* ---- Helpers ------------------------------------------------------------ */

static int file_exists(const char * path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int dev_exists(const char * path) {
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

static void remount_rw(void) {
    (void)!system("mount -o remount,rw / 2>/dev/null");
}

static int sh(const char * cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "[vpu] cmd failed (rc=%d): %s\n", rc, cmd);
    return rc;
}

/* ---- Detection ---------------------------------------------------------- */

static void detect_all(void) {
    struct utsname u;
    g_kernel_ok = (uname(&u) == 0 && strcmp(u.release, "2.6.36-R10-h28") == 0);
    g_drv_loaded = dev_exists(VPU_DEV_PATH);
    g_drv_file   = file_exists(VPU_DRV_PATH);
    g_fw_ok      = file_exists(VPU_FW_PATH);
    g_bin_ok     = (access(VPU_BIN_PATH, X_OK) == 0);
}

static int all_ready(void) {
    return g_drv_loaded && g_fw_ok && g_bin_ok;
}

/* ---- Status display ----------------------------------------------------- */

static void set_lbl(lv_obj_t * lbl, const char * text, uint32_t color) {
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

static void refresh_status(void) {
    detect_all();
    if (all_ready())
        set_lbl(g_lbl_status, TR(I18N_VPU_ACTIVE), 0x44dd66);
    else
        set_lbl(g_lbl_status, TR(I18N_VPU_NOT_INSTALLED), 0xdd6644);

    if (g_kernel_ok)
        set_lbl(g_lbl_kernel, TR(I18N_VPU_KERNEL_OK), 0x44dd66);
    else
        set_lbl(g_lbl_kernel, TR(I18N_VPU_KERNEL_WRONG), 0xdd6644);

    if (g_drv_loaded)
        set_lbl(g_lbl_driver, TR(I18N_VPU_DRV_LOADED), 0x44dd66);
    else if (g_drv_file)
        set_lbl(g_lbl_driver, TR(I18N_VPU_DRV_ON_DISK), 0xddaa44);
    else
        set_lbl(g_lbl_driver, TR(I18N_VPU_DRV_MISSING), 0xdd6644);

    set_lbl(g_lbl_fw,
            g_fw_ok ? TR(I18N_VPU_FW_OK) : TR(I18N_VPU_FW_MISSING),
            g_fw_ok ? 0x44dd66 : 0xdd6644);
    set_lbl(g_lbl_bin,
            g_bin_ok ? TR(I18N_VPU_BIN_OK) : TR(I18N_VPU_BIN_MISSING),
            g_bin_ok ? 0x44dd66 : 0xdd6644);
}

/* ---- Install callbacks -------------------------------------------------- */

static void on_install_driver(lv_event_t * e) {
    (void)e;
    set_lbl(g_lbl_driver, TR(I18N_VPU_INSTALLING), 0xddaa44);
    lv_refr_now(NULL);
    remount_rw();
    sh("mkdir -p /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/");
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "wget -q -O '%s' '%s' 2>/dev/null", VPU_DRV_PATH, VPU_DRV_URL);
    if (sh(cmd) != 0) {
        set_lbl(g_lbl_driver, "Driver: download failed", 0xdd4444);
        return;
    }
    sh("grep -q '^mxc_vpu ' /etc/modules 2>/dev/null || "
       "echo '" VPU_MOD_OPTS "' >> /etc/modules");
    sh("/sbin/depmod -a 2>/dev/null");
    if (sh("/sbin/insmod " VPU_DRV_PATH " 2>/dev/null") != 0)
        fprintf(stderr, "[vpu] driver installed on disk (insmod failed; try "
                "reboot or 'insmod " VPU_DRV_PATH "' manually)\n");
    refresh_status();
}

static void on_install_firmware(lv_event_t * e) {
    (void)e;
    set_lbl(g_lbl_fw, TR(I18N_VPU_INSTALLING), 0xddaa44);
    lv_refr_now(NULL);
    remount_rw();
    sh("mkdir -p /lib/firmware/vpu/");
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "wget -q -O '%s' '%s' 2>/dev/null", VPU_FW_PATH, VPU_FW_URL);
    if (sh(cmd) != 0) {
        set_lbl(g_lbl_fw, "Firmware: download failed", 0xdd4444);
        return;
    }
    refresh_status();
}

static void on_install_binary(lv_event_t * e) {
    (void)e;
    set_lbl(g_lbl_bin, TR(I18N_VPU_INSTALLING), 0xddaa44);
    lv_refr_now(NULL);
    remount_rw();
    sh("mkdir -p /root/vpu/");
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "wget -q -O '%s' '%s' 2>/dev/null && chmod +x '%s'",
             VPU_BIN_PATH, VPU_BIN_URL, VPU_BIN_PATH);
    if (sh(cmd) != 0) {
        set_lbl(g_lbl_bin, "vpu_stream: download failed", 0xdd4444);
        return;
    }
    refresh_status();
}

/* ---- Options save ------------------------------------------------------- */

static void on_vpu_save(lv_event_t * e) {
    (void)e;
    const char * s;
    s = lv_textarea_get_text(g_ta_w);   if (s && s[0])
        settings.video_src_w = atoi(s);
    s = lv_textarea_get_text(g_ta_h);   if (s && s[0])
        settings.video_src_h = atoi(s);
    /* Mode: if RTSP selected, keep the URL; else clear it so video.c
     * uses --rtp.  RTP port is always saved even when hidden. */
    s = lv_textarea_get_text(g_ta_rtp); if (s && s[0])
        settings.video_rtp = atoi(s);
    s = lv_textarea_get_text(g_ta_rtsp);
    if (!lv_obj_has_flag(g_ta_rtsp, LV_OBJ_FLAG_HIDDEN) && s && s[0])
        snprintf(settings.video_rtsp, sizeof settings.video_rtsp, "%s", s);
    else if (lv_obj_has_flag(g_ta_rtsp, LV_OBJ_FLAG_HIDDEN))
        settings.video_rtsp[0] = '\0';   /* RTP mode — clear RTSP */
    s = lv_textarea_get_text(g_ta_prebuf); if (s && s[0])
        settings.video_prebuffer = atoi(s);
    settings.video_codec = lv_obj_has_state(g_sw_codec, LV_STATE_CHECKED) ? 1 : 0;
    s = lv_textarea_get_text(g_ta_x);  if (s && s[0])
        settings.video_x = atoi(s);
    s = lv_textarea_get_text(g_ta_y);  if (s && s[0])
        settings.video_y = atoi(s);
    settings_save();
    ui_request_restart();
}

/* ---- Switch callbacks --------------------------------------------------- */

static void on_sw_enable(lv_event_t * e) {
    settings.video_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void on_sw_overlay(lv_event_t * e) {
    settings.video_overlay =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void on_sw_deblock(lv_event_t * e) {
    settings.video_deblock =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void on_sw_codec(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    lv_label_set_text(g_lbl_codec_val, on ? "H.264" : "MPEG-4");
    settings.video_codec = on;
    settings_save();
}

static void on_sw_mode(lv_event_t * e) {
    int rtsp = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    lv_label_set_text(g_lbl_mode_val, rtsp ? TR(I18N_VPU_RTSP) : TR(I18N_VPU_RTP));
    if (rtsp) {
        lv_obj_add_flag(g_ta_rtp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_lbl_rtp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_ta_rtsp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_lbl_rtsp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(g_ta_rtp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_lbl_rtp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ta_rtsp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_lbl_rtsp, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_sw_warm(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    if (on) {
        lv_obj_clear_flag(g_lbl_warm_url1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_lbl_warm_url2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_warm_url1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_lbl_warm_url2, LV_OBJ_FLAG_HIDDEN);
    }
    settings.video_warm = on;
    settings_save();
}

/* ---- Window close ------------------------------------------------------- */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* ---- Layout helpers ------------------------------------------------------
 * All y values are in design space (1024×600).  Row helpers return the
 * y for the NEXT row (y + row_h + gap), so callers chain naturally. */

static int row_label(lv_obj_t * parent, int y, const char * text) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                 SY(y) + (SY(ROW_H) - SY(24)) / 2);
    return y + ROW_H + ROW_GAP;
}

/* Row with label + textarea.  If lbl_out is provided, store the label
 * object there (needed for mode-dependent show/hide rows). */
static int row_ta_ex(lv_obj_t * parent, int y, const char * label,
                     lv_obj_t ** ta_out, lv_obj_t ** lbl_out,
                     int w, const char * init, int numeric_only) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                 SY(y) + (SY(ROW_H) - SY(24)) / 2);
    if (lbl_out) *lbl_out = lbl;

    lv_obj_t * ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, SX(w), SY(38));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                 SY(y) + (SY(ROW_H) - SY(38)) / 2);
    lv_textarea_set_one_line(ta, true);
    if (init) lv_textarea_set_text(ta, init);
    if (numeric_only) {
        lv_textarea_set_accepted_chars(ta, "0123456789-");
        lv_textarea_set_max_length(ta, 6);
    }
    *ta_out = ta;
    return y + ROW_H + ROW_GAP;
}

static int row_ta(lv_obj_t * parent, int y, const char * label,
                  lv_obj_t ** ta_out, int w, const char * init,
                  int numeric_only) {
    return row_ta_ex(parent, y, label, ta_out, NULL, w, init, numeric_only);
}

/* Row with label + switch. */
static int row_sw(lv_obj_t * parent, int y, const char * label,
                  lv_obj_t ** sw_out, int checked, lv_event_cb_t cb) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                 SY(y) + (SY(ROW_H) - SY(24)) / 2);

    lv_obj_t * sw = lv_switch_create(parent);
    lv_obj_set_size(sw, SX(SW_W), SY(SW_H));
    lv_obj_align(sw, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                 SY(y) + (SY(ROW_H) - SY(SW_H)) / 2);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    *sw_out = sw;
    return y + ROW_H + ROW_GAP;
}

/* Row with switch + live value label (e.g. "MPEG-4 / H.264"). */
static int row_sw_label(lv_obj_t * parent, int y, const char * label,
                        lv_obj_t ** sw_out, lv_obj_t ** val_out,
                        int checked, const char * txt_off, const char * txt_on,
                        lv_event_cb_t cb) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                 SY(y) + (SY(ROW_H) - SY(24)) / 2);

    lv_obj_t * sw = lv_switch_create(parent);
    lv_obj_set_size(sw, SX(SW_W), SY(SW_H));
    lv_obj_align(sw, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                 SY(y) + (SY(ROW_H) - SY(SW_H)) / 2);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    *sw_out = sw;

    lv_obj_t * vl = lv_label_create(parent);
    lv_label_set_text(vl, checked ? txt_on : txt_off);
    lv_obj_set_style_text_color(vl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(vl, SF(20), 0);
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, SX(INPUT_X + SW_W + 12),
                 SY(y) + (SY(ROW_H) - SY(24)) / 2);
    *val_out = vl;
    return y + ROW_H + ROW_GAP;
}

/* Section heading */
static int row_heading(lv_obj_t * parent, int y, const char * text) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X), SY(y));
    return y + 30;
}

/* Status line (smaller font, indented) */
static int row_status(lv_obj_t * parent, int y, lv_obj_t ** out,
                      const char * text, uint32_t color) {
    lv_obj_t * lbl = lv_label_create(parent);
    if (text) lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, SF(18), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(20), SY(y));
    *out = lbl;
    return y + 28;
}

/* Full-width button */
static int row_btn(lv_obj_t * parent, int y, const char * label,
                   lv_event_cb_t cb) {
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, SX(800), SY(56));
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, SX(LABEL_X), SY(y));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a4a6a), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, SF(20), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl);
    return y + 64;
}

/* Small hint text */
static int row_hint(lv_obj_t * parent, int y, const char * text,
                    uint32_t color, int font_sz) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, SF(font_sz), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(INPUT_X), SY(y));
    return y + font_sz + 4;
}

/* ---- Main screen -------------------------------------------------------- */

void open_vpu_modal(lv_event_t * e) {
    (void)e;
    detect_all();

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0c1a2c), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_height(scr, SY(2000));

    int y = 10;
    int ready = all_ready();

    /* ---- Header ---- */
    lv_obj_t * btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, SX(120), SY(50));
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, SX(10), SY(y));
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x1a2d44), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_t * bl = lv_label_create(btn_back);
    lv_label_set_text(bl, TR(I18N_BACK));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(20), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    /* Title */
    {
        lv_obj_t * t = lv_label_create(scr);
        lv_label_set_text(t, TR(I18N_VPU));
        lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(t, SF(26), 1);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(140), SY(y + 8));
    }

    /* ---- QR code top-right, inline with the header ---- */
    if (ready) {
        lv_obj_t * qr = lv_qrcode_create(scr, SX(120),
            lv_color_hex(0x000000), lv_color_hex(0xffffff));
        lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, SX(-20), SY(y));
        lv_qrcode_update(qr, VPU_SCRIPTS_URL, (uint32_t)strlen(VPU_SCRIPTS_URL));

        {
            lv_obj_t * qh = lv_label_create(scr);
            lv_label_set_text(qh, TR(I18N_VPU_QR_HELP));
            lv_obj_set_style_text_color(qh, lv_color_hex(0xc0d0e0), 0);
            lv_obj_set_style_text_font(qh, SF(14), 0);
            lv_obj_align_to(qh, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        }

        y = 70;

        /* ---- Stream options ---- */
        y = row_heading(scr, y, TR(I18N_VPU_OPTIONS));
        y += 4;
    } else {
        /* Status line under title — only when something is wrong */
        g_lbl_status = lv_label_create(scr);
        lv_obj_set_style_text_font(g_lbl_status, SF(22), 0);
        lv_obj_align(g_lbl_status, LV_ALIGN_TOP_LEFT, SX(140), SY(y + 40));

        y = 120;

        /* ---- Status section (only when not ready) ---- */
        y = row_heading(scr, y, TR(I18N_VPU_STATUS));
        y = row_status(scr, y, &g_lbl_kernel, NULL, 0xffffff);
        y = row_status(scr, y, &g_lbl_driver, NULL, 0xffffff);
        y = row_status(scr, y, &g_lbl_fw,     NULL, 0xffffff);
        y = row_status(scr, y, &g_lbl_bin,    NULL, 0xffffff);
        y += 10;

        refresh_status();

        /* ---- Install buttons ---- */
        if (g_kernel_ok) {
            if (!g_drv_loaded) {
                y = row_btn(scr, y,
                            g_drv_file ? TR(I18N_VPU_LOAD_DRV) : TR(I18N_VPU_INSTALL),
                            on_install_driver);
            }
            if (!g_fw_ok) {
                y = row_btn(scr, y, TR(I18N_VPU_INSTALL_FW), on_install_firmware);
            }
            if (!g_bin_ok) {
                y = row_btn(scr, y, TR(I18N_VPU_INSTALL_BIN), on_install_binary);
            }
            y += 10;
            {
                lv_obj_t * hint = lv_label_create(scr);
                lv_label_set_text(hint, TR(I18N_VPU_AFTER_INSTALL));
                lv_obj_set_style_text_color(hint, lv_color_hex(0x80a0b0), 0);
                lv_obj_set_style_text_font(hint, SF(16), 0);
                lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(LABEL_X), SY(y));
            }
            y += 30;
        }
    }

    /* ---- Options fields (only when ready) ---- */
    if (ready) {

        char buf[256];

        /* Enable */
        {
            lv_obj_t * sw;
            y = row_sw(scr, y, TR(I18N_VPU_ENABLE), &sw,
                       settings.video_enabled, on_sw_enable);
        }

        /* Decode in background (--warm) */
        {
            lv_obj_t * sw;
            int y_warm = y;  /* save for inline URL positioning */
            y = row_sw(scr, y, TR(I18N_VPU_WARM), &sw,
                       settings.video_warm, on_sw_warm);
            /* Doorbell URLs right behind the switch, inline */
            g_lbl_warm_url1 = lv_label_create(scr);
            lv_label_set_text(g_lbl_warm_url1, TR(I18N_VPU_SHOW_URL));
            lv_obj_set_style_text_color(g_lbl_warm_url1, lv_color_hex(0x88aacc), 0);
            lv_obj_set_style_text_font(g_lbl_warm_url1, SF(13), 0);
            lv_obj_align(g_lbl_warm_url1, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + SW_W + 16), SY(y_warm) + (SY(ROW_H) - SY(26)) / 2);

            g_lbl_warm_url2 = lv_label_create(scr);
            lv_label_set_text(g_lbl_warm_url2, TR(I18N_VPU_HIDE_URL));
            lv_obj_set_style_text_color(g_lbl_warm_url2, lv_color_hex(0x88aacc), 0);
            lv_obj_set_style_text_font(g_lbl_warm_url2, SF(13), 0);
            lv_obj_align(g_lbl_warm_url2, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + SW_W + 16 + 240), SY(y_warm) + (SY(ROW_H) - SY(26)) / 2);

            if (!settings.video_warm) {
                lv_obj_add_flag(g_lbl_warm_url1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_lbl_warm_url2, LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* Mode: RTP / RTSP switch */
        {
            int rtsp_mode = (settings.video_rtsp[0] != '\0');
            lv_obj_t * sw;
            y = row_sw_label(scr, y, TR(I18N_VPU_MODE), &sw, &g_lbl_mode_val,
                             rtsp_mode, TR(I18N_VPU_RTP), TR(I18N_VPU_RTSP),
                             on_sw_mode);

            /* Create both mode-dependent rows at the same y.
             * Only the active one is visible. */
            int y_mode = y;

            /* RTP port row */
            g_lbl_rtp = lv_label_create(scr);
            lv_label_set_text(g_lbl_rtp, TR(I18N_VPU_RTP_PORT));
            lv_obj_set_style_text_color(g_lbl_rtp, lv_color_hex(0xe0e0e0), 0);
            lv_obj_set_style_text_font(g_lbl_rtp, SF(20), 0);
            lv_obj_align(g_lbl_rtp, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                         SY(y_mode) + (SY(ROW_H) - SY(24)) / 2);

            g_ta_rtp = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_rtp, SX(INPUT_W), SY(38));
            lv_obj_align(g_ta_rtp, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                         SY(y_mode) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_rtp, true);
            lv_textarea_set_accepted_chars(g_ta_rtp, "0123456789");
            lv_textarea_set_max_length(g_ta_rtp, 5);
            snprintf(buf, sizeof buf, "%d", settings.video_rtp);
            lv_textarea_set_text(g_ta_rtp, buf);

            /* RTSP URL row (same y as RTP row — toggled visibility) */
            g_lbl_rtsp = lv_label_create(scr);
            lv_label_set_text(g_lbl_rtsp, TR(I18N_VPU_RTSP_URL));
            lv_obj_set_style_text_color(g_lbl_rtsp, lv_color_hex(0xe0e0e0), 0);
            lv_obj_set_style_text_font(g_lbl_rtsp, SF(20), 0);
            lv_obj_align(g_lbl_rtsp, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                         SY(y_mode) + (SY(ROW_H) - SY(24)) / 2);

            g_ta_rtsp = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_rtsp, SX(INPUT_W_W), SY(38));
            lv_obj_align(g_ta_rtsp, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                         SY(y_mode) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_rtsp, true);
            lv_textarea_set_max_length(g_ta_rtsp, 255);
            if (settings.video_rtsp[0])
                lv_textarea_set_text(g_ta_rtsp, settings.video_rtsp);

            /* Set initial visibility */
            if (rtsp_mode) {
                lv_obj_add_flag(g_ta_rtp, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_lbl_rtp, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_ta_rtsp, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_lbl_rtsp, LV_OBJ_FLAG_HIDDEN);
            }

            y = y_mode + ROW_H + ROW_GAP;
        }

        /* Output W x H */
        {
            lv_obj_t * lbl = lv_label_create(scr);
            lv_label_set_text(lbl, TR(I18N_VPU_OUTPUT_RES));
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
            lv_obj_set_style_text_font(lbl, SF(20), 0);
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                         SY(y) + (SY(ROW_H) - SY(24)) / 2);

            /* W */
            g_ta_w = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_w, SX(INPUT_W), SY(38));
            lv_obj_align(g_ta_w, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                         SY(y) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_w, true);
            lv_textarea_set_accepted_chars(g_ta_w, "0123456789");
            lv_textarea_set_max_length(g_ta_w, 4);
            snprintf(buf, sizeof buf, "%d", settings.video_src_w ?: 640);
            lv_textarea_set_text(g_ta_w, buf);

            /* × label */
            lv_obj_t * mul = lv_label_create(scr);
            lv_label_set_text(mul, "x");
            lv_obj_set_style_text_color(mul, lv_color_hex(0xc0c0c0), 0);
            lv_obj_set_style_text_font(mul, SF(22), 0);
            lv_obj_align(mul, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + INPUT_W + 8),
                         SY(y) + (SY(ROW_H) - SY(24)) / 2);

            /* H */
            g_ta_h = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_h, SX(INPUT_W), SY(38));
            lv_obj_align(g_ta_h, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + INPUT_W + 24),
                         SY(y) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_h, true);
            lv_textarea_set_accepted_chars(g_ta_h, "0123456789");
            lv_textarea_set_max_length(g_ta_h, 4);
            snprintf(buf, sizeof buf, "%d", settings.video_src_h ?: 480);
            lv_textarea_set_text(g_ta_h, buf);

            y += ROW_H + ROW_GAP;
        }

        /* Position X Y */
        {
            lv_obj_t * lbl = lv_label_create(scr);
            lv_label_set_text(lbl, TR(I18N_VPU_POSITION));
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
            lv_obj_set_style_text_font(lbl, SF(20), 0);
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(LABEL_X),
                         SY(y) + (SY(ROW_H) - SY(24)) / 2);

            g_ta_x = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_x, SX(INPUT_W), SY(38));
            lv_obj_align(g_ta_x, LV_ALIGN_TOP_LEFT, SX(INPUT_X),
                         SY(y) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_x, true);
            lv_textarea_set_accepted_chars(g_ta_x, "0123456789-");
            lv_textarea_set_max_length(g_ta_x, 5);
            snprintf(buf, sizeof buf, "%d", settings.video_x);
            lv_textarea_set_text(g_ta_x, buf);

            lv_obj_t * mul = lv_label_create(scr);
            lv_label_set_text(mul, "x");
            lv_obj_set_style_text_color(mul, lv_color_hex(0xc0c0c0), 0);
            lv_obj_set_style_text_font(mul, SF(22), 0);
            lv_obj_align(mul, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + INPUT_W + 8),
                         SY(y) + (SY(ROW_H) - SY(24)) / 2);

            g_ta_y = lv_textarea_create(scr);
            lv_obj_set_size(g_ta_y, SX(INPUT_W), SY(38));
            lv_obj_align(g_ta_y, LV_ALIGN_TOP_LEFT,
                         SX(INPUT_X + INPUT_W + 24),
                         SY(y) + (SY(ROW_H) - SY(38)) / 2);
            lv_textarea_set_one_line(g_ta_y, true);
            lv_textarea_set_accepted_chars(g_ta_y, "0123456789-");
            lv_textarea_set_max_length(g_ta_y, 5);
            snprintf(buf, sizeof buf, "%d", settings.video_y);
            lv_textarea_set_text(g_ta_y, buf);

            y += ROW_H + ROW_GAP;
        }

        /* FB1 overlay */
        {
            lv_obj_t * sw;
            y = row_sw(scr, y, TR(I18N_VPU_OVERLAY), &sw,
                       settings.video_overlay, on_sw_overlay);
        }

        /* Codec (switch + live label with "(recommended)" hint) */
        {
            int is_h264 = (settings.video_codec == 1);
            y = row_sw_label(scr, y, TR(I18N_VPU_CODEC),
                             &g_sw_codec, &g_lbl_codec_val,
                             is_h264, "MPEG-4", "H.264", on_sw_codec);
            /* Overwrite the label with "(recommended)" annotation for MPEG-4 */
            lv_label_set_text(g_lbl_codec_val,
                              is_h264 ? "H.264" : "MPEG-4 (recommended)");
        }

        /* PP deblock */
        {
            lv_obj_t * sw;
            y = row_sw(scr, y, TR(I18N_VPU_DEBLOCK), &sw,
                       settings.video_deblock, on_sw_deblock);
        }

        /* Prebuffer KB */
        y = row_ta(scr, y, TR(I18N_VPU_PREBUFFER), &g_ta_prebuf, INPUT_W,
                   NULL, 1);
        {
            snprintf(buf, sizeof buf, "%d", settings.video_prebuffer);
            lv_textarea_set_text(g_ta_prebuf, buf);
        }

        y += 8;

        /* Save button */
        y = row_btn(scr, y, TR(I18N_VPU_SAVE), on_vpu_save);
    }

    /* ---- Wrong kernel message ---- */
    if (!g_kernel_ok) {
        y += 10;
        lv_obj_t * w1 = lv_label_create(scr);
        lv_label_set_text(w1, "Your kernel does not match 2.6.36-R10-h28.");
        lv_obj_set_style_text_color(w1, lv_color_hex(0xdd6644), 0);
        lv_obj_set_style_text_font(w1, SF(18), 0);
        lv_obj_align(w1, LV_ALIGN_TOP_LEFT, SX(LABEL_X), SY(y));
        y += 26;
        lv_obj_t * w2 = lv_label_create(scr);
        lv_label_set_text(w2, "Please update the kernel first via the Eneco feed.");
        lv_obj_set_style_text_color(w2, lv_color_hex(0xdd6644), 0);
        lv_obj_set_style_text_font(w2, SF(18), 0);
        lv_obj_align(w2, LV_ALIGN_TOP_LEFT, SX(LABEL_X), SY(y));
        y += 30;
    }

    y += 20;   /* bottom padding */

    lv_obj_set_height(scr, SY(y));
    ui_push(scr);
}
