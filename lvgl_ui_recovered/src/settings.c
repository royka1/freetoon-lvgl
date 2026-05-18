/*
 * Tiny key=value config file at /mnt/data/toonui.cfg.
 * No JSON parser dependency. Loads defaults if file missing.
 */
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_PATH "/mnt/data/toonui.cfg"

settings_t settings = {
    .auto_dim_enabled  = 1,
    .auto_dim_seconds  = 10,
    .active_brightness = 800,
    .dim_brightness    = 80,
    .temp_offset_centi = 0,
    .show_dim_weather  = 1,
    .show_dim_waste    = 1,
    .dim_waste_lead_days = 2,
    .vnc_enabled       = 0,
    .vnc_pass          = "",
    .weather_location    = "Medemblik",
    /* GeoNames id understood by forecast.buienradar.nl/2.0/forecast/<id>.
     * 2757783 = De Bilt — central NL national fallback. The KNMI station
     * code 6249 happens to collide with an unrelated mid-east location
     * in buienradar's GeoNames mapping, so we default to De Bilt and let
     * the user paste their own id into /mnt/data/toonui.cfg if they want
     * Medemblik-specific hourly data (the URL bar on
     * https://www.buienradar.nl/weer/medemblik/nl/<ID> reveals it). */
    .weather_location_id = 2757783,
    .forecast_mode       = FORECAST_AUTO,
    .ot_bridge_mode      = "proxy",
    .otgw_host           = "192.168.99.21",
    .otgw_user           = "",
    .otgw_pass           = "",
    /* MQTT defaults match the existing /mnt/data/mqtt.cfg so first boot
     * after the upgrade keeps the banner subscriber alive without manual
     * config. User edits in Settings → MQTT take precedence and survive
     * a reboot. */
    .mqtt_host           = "192.168.3.101",
    .mqtt_port           = 1883,
    .mqtt_user           = "brakero1",
    .mqtt_pass           = "",
    .mqtt_topics         = {"home/packages/banner", "home/packages/state"},
    .mqtt_topic_count    = 2,
};

float display_indoor_temp(float raw) {
    if (raw <= 0) return raw;
    return raw + (float)settings.temp_offset_centi / 100.0f;
}

void settings_load(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[64];
        if (sscanf(line, "%63[^=]=%63s", k, v) != 2) continue;
        int iv = atoi(v);
        if      (strcmp(k, "auto_dim_enabled")  == 0) settings.auto_dim_enabled  = iv;
        else if (strcmp(k, "auto_dim_seconds")  == 0) settings.auto_dim_seconds  = iv;
        else if (strcmp(k, "active_brightness") == 0) settings.active_brightness = iv;
        else if (strcmp(k, "dim_brightness")    == 0) settings.dim_brightness    = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "show_dim_waste")    == 0) settings.show_dim_waste    = iv;
        else if (strcmp(k, "dim_waste_lead_days") == 0) settings.dim_waste_lead_days = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "weather_location_id") == 0) settings.weather_location_id = iv;
        else if (strcmp(k, "forecast_mode")     == 0) settings.forecast_mode       = iv;
        else if (strcmp(k, "weather_location")  == 0)
            snprintf(settings.weather_location,
                     sizeof settings.weather_location, "%s", v);
        else if (strcmp(k, "ot_bridge_mode")    == 0) {
            /* Migrate legacy values to the new 3-way naming. */
            const char *m = v;
            if      (strcmp(v, "keteladapter") == 0) m = "proxy";
            else if (strcmp(v, "otgw")         == 0) m = "wireless";
            snprintf(settings.ot_bridge_mode,
                     sizeof settings.ot_bridge_mode, "%s", m);
        }
        else if (strcmp(k, "otgw_host")         == 0)
            snprintf(settings.otgw_host,
                     sizeof settings.otgw_host, "%s", v);
        else if (strcmp(k, "otgw_user")         == 0)
            snprintf(settings.otgw_user,
                     sizeof settings.otgw_user, "%s", v);
        else if (strcmp(k, "otgw_pass")         == 0)
            snprintf(settings.otgw_pass,
                     sizeof settings.otgw_pass, "%s", v);
        else if (strcmp(k, "mqtt_host")         == 0)
            snprintf(settings.mqtt_host, sizeof settings.mqtt_host, "%s", v);
        else if (strcmp(k, "mqtt_port")         == 0) settings.mqtt_port = iv;
        else if (strcmp(k, "mqtt_user")         == 0)
            snprintf(settings.mqtt_user, sizeof settings.mqtt_user, "%s", v);
        else if (strcmp(k, "mqtt_pass")         == 0)
            snprintf(settings.mqtt_pass, sizeof settings.mqtt_pass, "%s", v);
        else if (strcmp(k, "mqtt_topic_count")  == 0) {
            settings.mqtt_topic_count = iv;
            if (settings.mqtt_topic_count > 8) settings.mqtt_topic_count = 8;
            if (settings.mqtt_topic_count < 0) settings.mqtt_topic_count = 0;
        }
        else if (strncmp(k, "mqtt_topic_", 11)  == 0) {
            int idx = atoi(k + 11);
            if (idx >= 0 && idx < 8)
                snprintf(settings.mqtt_topics[idx],
                         sizeof settings.mqtt_topics[0], "%s", v);
        }
    }
    fclose(f);

    /* Migration: if mqtt_pass is still empty, try to read /mnt/data/mqtt.cfg
     * (host:user:pass format from the legacy single-line tooling). Lets the
     * new settings-driven subscriber connect on first boot without the user
     * having to retype the broker password. */
    if (!settings.mqtt_pass[0]) {
        FILE * mf = fopen("/mnt/data/mqtt.cfg", "r");
        if (mf) {
            char line[256];
            if (fgets(line, sizeof(line), mf)) {
                char * nl = strchr(line, '\n'); if (nl) *nl = 0;
                /* host:user:pass */
                char * p1 = strchr(line, ':');
                char * p2 = p1 ? strchr(p1 + 1, ':') : NULL;
                if (p1 && p2) {
                    *p1 = 0; *p2 = 0;
                    if (!settings.mqtt_host[0])
                        snprintf(settings.mqtt_host, sizeof(settings.mqtt_host),
                                 "%s", line);
                    if (!settings.mqtt_user[0])
                        snprintf(settings.mqtt_user, sizeof(settings.mqtt_user),
                                 "%s", p1 + 1);
                    snprintf(settings.mqtt_pass, sizeof(settings.mqtt_pass),
                             "%s", p2 + 1);
                }
            }
            fclose(mf);
        }
    }
}

void settings_save(void) {
    FILE * f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "auto_dim_enabled=%d\n",  settings.auto_dim_enabled);
    fprintf(f, "auto_dim_seconds=%d\n",  settings.auto_dim_seconds);
    fprintf(f, "active_brightness=%d\n", settings.active_brightness);
    fprintf(f, "dim_brightness=%d\n",    settings.dim_brightness);
    fprintf(f, "temp_offset_centi=%d\n", settings.temp_offset_centi);
    fprintf(f, "show_dim_weather=%d\n",  settings.show_dim_weather);
    fprintf(f, "show_dim_waste=%d\n",    settings.show_dim_waste);
    fprintf(f, "dim_waste_lead_days=%d\n", settings.dim_waste_lead_days);
    fprintf(f, "vnc_enabled=%d\n",       settings.vnc_enabled);
    fprintf(f, "vnc_pass=%s\n",          settings.vnc_pass);
    fprintf(f, "weather_location=%s\n",    settings.weather_location);
    fprintf(f, "weather_location_id=%d\n", settings.weather_location_id);
    fprintf(f, "forecast_mode=%d\n",       settings.forecast_mode);
    fprintf(f, "ot_bridge_mode=%s\n",      settings.ot_bridge_mode);
    fprintf(f, "otgw_host=%s\n",           settings.otgw_host);
    fprintf(f, "otgw_user=%s\n",           settings.otgw_user);
    fprintf(f, "otgw_pass=%s\n",           settings.otgw_pass);
    fprintf(f, "mqtt_host=%s\n",           settings.mqtt_host);
    fprintf(f, "mqtt_port=%d\n",           settings.mqtt_port);
    fprintf(f, "mqtt_user=%s\n",           settings.mqtt_user);
    fprintf(f, "mqtt_pass=%s\n",           settings.mqtt_pass);
    fprintf(f, "mqtt_topic_count=%d\n",    settings.mqtt_topic_count);
    for (int i = 0; i < settings.mqtt_topic_count && i < 8; i++)
        fprintf(f, "mqtt_topic_%d=%s\n", i, settings.mqtt_topics[i]);
    fclose(f);
}
