/*
 * Lights page — opened via swipe-right from home. Header has the
 * "All Lights" master controls; below, room cards each list the lights
 * in that area with a Toggle button. State is polled by ha_thread.
 */
#include "screens.h"
#include "homeassistant.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>

#define COL_BG        0x0e1a2a
#define COL_CARD      0x1a2940
#define COL_TEXT_HI   0xffffff
#define COL_TEXT_DIM  0x88aabb
#define COL_ON        0xffcc44
#define COL_OFF       0x3a4658
#define COL_OFFLINE   0x6e6e6e

/* Per-light row widgets, kept so refresh_cb can update colours/text
 * without rebuilding the screen. Indexed parallel to ha_lights[]. */
typedef struct {
    lv_obj_t * row;        /* the row container */
    lv_obj_t * lbl_name;   /* "Bank lamp" */
    lv_obj_t * lbl_state;  /* "on" / "off" / "offline" */
    lv_obj_t * btn;        /* toggle button */
    lv_obj_t * btn_lbl;    /* button label "Toggle" */
} light_row_t;

static lv_obj_t   * scr_root = NULL;
static light_row_t  rows[HA_LIGHT_COUNT];
static lv_timer_t * refresh_timer = NULL;

/* ui_pop deferred via lv_async_call — calling it directly from inside an
 * LVGL event dispatch can cause issues when lv_scr_load fires another
 * event before the original is fully unwound. */
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* Pause/resume the refresh ticker with the screen so cached-widget
 * pointers in rows[] never get touched while we're off-screen. */
static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static void on_all_on (lv_event_t * e) { (void)e; ha_lights_all_on_async();  }
static void on_all_off(lv_event_t * e) { (void)e; ha_lights_all_off_async(); }

static void on_light_toggle(lv_event_t * e) {
    const char * id = (const char *)lv_event_get_user_data(e);
    if (id) ha_light_toggle_async(id);
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    for (int i = 0; i < HA_LIGHT_COUNT; i++) {
        if (!rows[i].lbl_state) continue;
        if (!ha_lights[i].available) {
            lv_label_set_text(rows[i].lbl_state, "offline");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_OFFLINE), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_OFFLINE), 0);
        } else if (ha_lights[i].on) {
            int b = ha_lights[i].brightness;
            if (b > 0) lv_label_set_text_fmt(rows[i].lbl_state,
                                             "on  %d%%", b * 100 / 255);
            else       lv_label_set_text(rows[i].lbl_state, "on");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_ON), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_ON), 0);
        } else {
            lv_label_set_text(rows[i].lbl_state, "off");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_OFF), 0);
        }
    }
}

/* Build one light row inside an area card. */
static void build_light_row(lv_obj_t * parent, int i, int y) {
    light_row_t * R = &rows[i];
    R->row = lv_obj_create(parent);
    lv_obj_set_size(R->row, 280, 44);
    lv_obj_set_pos(R->row, 8, y);
    lv_obj_set_style_bg_opa(R->row, 0, 0);
    lv_obj_set_style_border_width(R->row, 0, 0);
    lv_obj_set_style_pad_all(R->row, 4, 0);
    lv_obj_clear_flag(R->row, LV_OBJ_FLAG_SCROLLABLE);

    R->lbl_name = lv_label_create(R->row);
    lv_obj_set_style_text_color(R->lbl_name, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(R->lbl_name, &lv_font_montserrat_18, 0);
    lv_label_set_text(R->lbl_name, ha_lights[i].name);
    lv_obj_align(R->lbl_name, LV_ALIGN_LEFT_MID, 0, -8);

    R->lbl_state = lv_label_create(R->row);
    lv_obj_set_style_text_color(R->lbl_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(R->lbl_state, &lv_font_montserrat_14, 0);
    lv_label_set_text(R->lbl_state, "--");
    lv_obj_align(R->lbl_state, LV_ALIGN_LEFT_MID, 0, 12);

    R->btn = lv_btn_create(R->row);
    lv_obj_set_size(R->btn, 90, 36);
    lv_obj_align(R->btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(R->btn, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(R->btn, 8, 0);
    /* Soak up sloppy taps so finger-on-edge still hits the button. */
    lv_obj_set_ext_click_area(R->btn, 18);
    lv_obj_add_event_cb(R->btn, on_light_toggle, LV_EVENT_CLICKED,
                        (void *)ha_lights[i].entity_id);

    R->btn_lbl = lv_label_create(R->btn);
    lv_obj_set_style_text_color(R->btn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(R->btn_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(R->btn_lbl, "Toggle");
    lv_obj_center(R->btn_lbl);
}

/* Build a card per area; cards are arranged in 3 columns. */
static void build_area_card(int col, const char * area_name) {
    int card_w = 320, card_h = 380;
    int x = 16 + col * (card_w + 12);
    int y = 130;
    lv_obj_t * card = lv_obj_create(scr_root);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * hdr = lv_label_create(card);
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_label_set_text(hdr, area_name);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 0);

    int y_row = 36;
    for (int i = 0; i < HA_LIGHT_COUNT; i++) {
        if (strcmp(ha_lights[i].area, area_name) != 0) continue;
        build_light_row(card, i, y_row);
        y_row += 50;
    }
}

lv_obj_t * screen_lights_create(void) {
    /* Singleton — same lv_obj on repeat calls so re-entry doesn't leak
     * widgets or strand timers. (Matches screen_home_create.) */
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    memset(rows, 0, sizeof(rows));

    /* Top bar: title + Back button (left), All On / All Off (right) */
    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Lights");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 110, 46);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 160, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* All Lights master controls — header strip */
    lv_obj_t * master = lv_obj_create(scr_root);
    lv_obj_set_size(master, 720, 60);
    lv_obj_align(master, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(master, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(master, 0, 0);
    lv_obj_set_style_radius(master, 12, 0);
    lv_obj_set_style_pad_all(master, 8, 0);
    lv_obj_clear_flag(master, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_all = lv_label_create(master);
    lv_obj_set_style_text_color(lbl_all, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_all, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_all, "All Lights");
    lv_obj_align(lbl_all, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t * b_on = lv_btn_create(master);
    lv_obj_set_size(b_on, 180, 44);
    lv_obj_align(b_on, LV_ALIGN_RIGHT_MID, -200, 0);
    lv_obj_set_style_bg_color(b_on, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(b_on, 8, 0);
    lv_obj_set_ext_click_area(b_on, 12);
    lv_obj_add_event_cb(b_on, on_all_on, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_on = lv_label_create(b_on);
    lv_obj_set_style_text_color(lbl_on, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_on, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_on, "All ON");
    lv_obj_center(lbl_on);

    lv_obj_t * b_off = lv_btn_create(master);
    lv_obj_set_size(b_off, 180, 44);
    lv_obj_align(b_off, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(b_off, lv_color_hex(0x6e3a3a), 0);
    lv_obj_set_style_radius(b_off, 8, 0);
    lv_obj_set_ext_click_area(b_off, 12);
    lv_obj_add_event_cb(b_off, on_all_off, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_off = lv_label_create(b_off);
    lv_obj_set_style_text_color(lbl_off, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_off, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_off, "All OFF");
    lv_obj_center(lbl_off);

    /* Three area cards: Woonkamer | Keuken | Boven (slaapkamers) */
    build_area_card(0, "Woonkamer");
    build_area_card(1, "Keuken");
    build_area_card(2, "Boven");

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(refresh_timer);
    return scr_root;
}
