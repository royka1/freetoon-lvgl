/*
 * Toon LVGL UI — boot entry. Initializes LVGL, display, input,
 * starts the BoxTalk client, then hands off to the screen stack.
 */
#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "boxtalk.h"
#include "screens.h"
#include "settings.h"
#include "backlight.h"
#include "homewizard.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "homeassistant.h"
#include "healthcheck.h"
#include "pwa_server.h"
#include "packages.h"
#include "weather.h"
#include "homewizard.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Wrap evdev_read so any PR event marks activity for the idle timer. */
static void evdev_read_with_activity(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    evdev_read(drv, data);
    if (data->state == LV_INDEV_STATE_PR) ui_mark_activity();
}

#define DISP_HOR 1024
#define DISP_VER 600
#define DRAW_BUF_LINES 100

static lv_color_t buf1[DISP_HOR * DRAW_BUF_LINES];
static lv_color_t buf2[DISP_HOR * DRAW_BUF_LINES];

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[main] starting toonui\n");

    lv_init();
    fbdev_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DISP_HOR * DRAW_BUF_LINES);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res  = DISP_HOR;
    disp_drv.ver_res  = DISP_VER;
    lv_disp_drv_register(&disp_drv);

    evdev_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read_with_activity;
    lv_indev_drv_register(&indev_drv);

    settings_load();

    if (boxtalk_start() != 0)
        fprintf(stderr, "[main] boxtalk_start failed\n");
    if (homewizard_start() != 0)
        fprintf(stderr, "[main] homewizard_start failed\n");
    if (weather_start() != 0)
        fprintf(stderr, "[main] weather_start failed\n");
    if (waste_start() != 0)
        fprintf(stderr, "[main] waste_start failed\n");
    if (vent_start() != 0)
        fprintf(stderr, "[main] vent_start failed\n");
    if (ha_start() != 0)
        fprintf(stderr, "[main] ha_start failed\n");
    if (healthcheck_start() != 0)
        fprintf(stderr, "[main] healthcheck_start failed\n");
    if (pwa_start() != 0)
        fprintf(stderr, "[main] pwa_start failed\n");
    packages_start();

    ui_init();

    fprintf(stderr, "[main] entering LVGL loop\n");
    uint32_t last_idle_check = 0;
    while (1) {
        lv_timer_handler();
        usleep(5000);
        lv_tick_inc(5);
        /* Cheap idle check ~5x/sec; ui_idle_tick is internally
           cheap and only acts when timeout elapsed. */
        uint32_t now = lv_tick_get();
        if (now - last_idle_check > 200) {
            ui_idle_tick();
            last_idle_check = now;
        }
    }
    return 0;
}
