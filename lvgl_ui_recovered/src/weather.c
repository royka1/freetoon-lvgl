/*
 * Buienradar JSON poller. Two sources:
 *
 *  - forecast.buienradar.nl/2.0/forecast/<geonames-id>  (location-specific):
 *    current weather, the 5-day forecast, and 3-hourly slots. Lowercase keys.
 *    The old "beaufort" field is gone — wind force is derived from
 *    "windspeedms" (ms_to_beaufort).
 *
 *  - data.buienradar.nl/2.0/feed/json  (national): the radar-image URL
 *    (Actual.ActualRadarUrl) and the human weather report
 *    (Forecast.WeatherReport). In mid-2026 buienradar renamed every key to
 *    PascalCase and \u-escaped the radar URL's '&'; the parser tracks that.
 *
 * No JSON library: we use small strstr-based extractors.
 */
#include "weather.h"
#include "http.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>

weather_state_t weather_state = {0};

/* Scan a substring of json for "key":<number>; returns parsed double, dflt if missing. */
static double js_num(const char * begin, const char * end, const char * key, double dflt) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":", key);
    const char * p = strstr(begin, n);
    if (!p || p >= end) return dflt;
    p += strlen(n);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' || *p == '"') {
        /* "null" or quoted-number like "13" — handle both */
        if (*p == '"') p++; else return dflt;
    }
    return strtod(p, NULL);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Scan a substring for "key":"VALUE" — copies into out (max outsz-1),
   translating JSON escapes (\n \t \" \\ \/ and \uXXXX → UTF-8). The feed
   escapes the radar URL's '&' as & and may emit \u-encoded accents, so
   copying raw bytes is not enough. */
static int js_str(const char * begin, const char * end, const char * key,
                  char * out, size_t outsz) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":\"", key);
    const char * p = strstr(begin, n);
    if (!p || p >= end) { if (outsz) out[0] = 0; return 0; }
    p += strlen(n);
    size_t o = 0;
    const char * e = p;
    while (e < end && *e != '"' && o + 4 < outsz) {
        if (*e == '\\' && e + 1 < end) {
            char c = e[1];
            if (c == 'u' && e + 5 < end) {
                int cp = (hexval(e[2]) << 12) | (hexval(e[3]) << 8) |
                         (hexval(e[4]) << 4) | hexval(e[5]);
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                e += 6;
            } else {
                switch (c) {
                    case 'n': out[o++] = '\n'; break;
                    case 't': out[o++] = '\t'; break;
                    case 'r': break;                 /* drop CR */
                    default:  out[o++] = c;  break;  /* \" \\ \/ */
                }
                e += 2;
            }
        } else {
            out[o++] = *e++;
        }
    }
    out[o] = 0;
    return 1;
}

/* Compute Dutch short-day label "ma 13-5" from a "2026-05-14T00:00:00" string. */
static const char * dutch_dow[] = {"zo","ma","di","wo","do","vr","za"};

/* Map a buienradar iconcode ("a","aa","b","r",…) to a short Dutch description.
   Used when the forecast endpoint (which doesn't provide human-readable
   weatherdescription) is the primary data source. */
static const char * iconcode_to_desc(const char * code) {
    if (!code || !code[0]) return "";
    if (!strcmp(code, "aa")) return "Helder";
    if (!strcmp(code, "bb")) return "Licht bewolkt";
    if (!strcmp(code, "cc")) return "Zwaar bewolkt";
    if (!strcmp(code, "ff")) return "Buien";
    if (!strcmp(code, "jj")) return "Half bewolkt";
    if (!strcmp(code, "mm")) return "Lichte regen";
    if (!strcmp(code, "a"))  return "Zonnig";
    if (!strcmp(code, "b"))  return "Licht bewolkt";
    if (!strcmp(code, "c"))  return "Zwaar bewolkt";
    if (!strcmp(code, "d"))  return "Nevelig";
    if (!strcmp(code, "f"))  return "Buien";
    if (!strcmp(code, "g"))  return "Onweer";
    if (!strcmp(code, "h"))  return "Zware onweersbuien";
    if (!strcmp(code, "j"))  return "Half bewolkt";
    if (!strcmp(code, "m"))  return "Lichte regen";
    if (!strcmp(code, "n"))  return "Mist";
    if (!strcmp(code, "q"))  return "Zware regen";
    if (!strcmp(code, "r"))  return "Buien";
    if (!strcmp(code, "s"))  return "Sneeuw";
    if (!strcmp(code, "w"))  return "Veel bewolking";
    return "";
}

static void format_day_label(const char * iso_date, char * out, size_t outsz) {
    if (strlen(iso_date) < 10) { snprintf(out, outsz, "?"); return; }
    int y = atoi(iso_date);
    int mo = atoi(iso_date + 5);
    int d = atoi(iso_date + 8);
    /* Zeller's congruence for day of week */
    int yy = (mo < 3) ? y - 1 : y;
    int mm = (mo < 3) ? mo + 12 : mo;
    int K = yy % 100, J = yy / 100;
    int h = (d + (13*(mm+1))/5 + K + K/4 + J/4 + 5*J) % 7;  /* 0=Sat,1=Sun,2=Mon */
    int dow = (h + 6) % 7;  /* 0=Sun..6=Sat */
    snprintf(out, outsz, "%s %d-%d", dutch_dow[dow], d, mo);
}

/* Wind force in Beaufort from a wind speed in m/s. Buienradar dropped the
   precomputed "beaufort"/"WindspeedBeaufort" field (now null), so we derive
   it from "windspeedms" using the standard scale (lower bound of each band). */
static int ms_to_beaufort(double ms) {
    static const double lo[12] = {0.3, 1.6, 3.4, 5.5, 8.0, 10.8, 13.9,
                                  17.2, 20.8, 24.5, 28.5, 32.7};
    int b = 0;
    for (int i = 0; i < 12; i++) if (ms >= lo[i]) b = i + 1;
    return b;
}

/* Parse the national feed (data.buienradar.nl/2.0/feed/json). As of mid-2026
   buienradar renamed every key to PascalCase (Actual/Forecast/...). This feed
   is the only source of the radar-image URL (Actual.ActualRadarUrl) and the
   human weather report (Forecast.WeatherReport). Current conditions and the
   5-day strip are location-specific and come from the forecast endpoint, so
   here we deliberately take only radar + report. Returns 0 if either was
   found, -1 otherwise. */
static int parse_buienradar(const char * body) {
    const char * end = body + strlen(body);
    int got = 0;

    /* Radar image URL — js_str unescapes the & ('&') in the query. */
    if (js_str(body, end, "ActualRadarUrl",
               weather_state.radar_url, sizeof(weather_state.radar_url)) &&
        weather_state.radar_url[0])
        got = 1;

    /* Weather report: Forecast.WeatherReport { Title, Summary, Text }. */
    const char * wr = strstr(body, "\"WeatherReport\":");
    if (wr) {
        const char * wr_end = strstr(wr, "\"ShortTermForecast\"");
        if (!wr_end || wr_end > end) {
            wr_end = (wr + 4096 < end) ? wr + 4096 : end;
        }
        char title[128];
        if (js_str(wr, wr_end, "Title", title, sizeof title) && title[0]) {
            snprintf(weather_state.weatherreport_title,
                     sizeof weather_state.weatherreport_title, "%s", title);
            got = 1;
        }
        /* Prefer the full Text; fall back to the shorter Summary. */
        if (!js_str(wr, wr_end, "Text", weather_state.weatherreport_text,
                    sizeof weather_state.weatherreport_text) ||
            !weather_state.weatherreport_text[0]) {
            js_str(wr, wr_end, "Summary", weather_state.weatherreport_text,
                   sizeof weather_state.weatherreport_text);
        }
        if (weather_state.weatherreport_text[0]) got = 1;
        /* Strip any &nbsp; the report leaks. */
        char * p;
        while ((p = strstr(weather_state.weatherreport_text, "&nbsp;")) != NULL) {
            *p = ' ';
            memmove(p + 1, p + 6, strlen(p + 6) + 1);
        }
    }

    /* 5-day forecast — Forecast.FiveDayForecast is exactly what buienradar.nl
       renders (icon codes, temps, descriptions), so it drives the daily strip.
       (The per-location forecast endpoint sometimes disagrees — e.g. serving a
       haze code where the site shows partly cloudy.) The feed carries no usable
       wind force, so the strip shows wind direction only. */
    const char * fc = strstr(body, "\"FiveDayForecast\":[");
    if (fc) {
        const char * walk = fc;
        int dn = 0;
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            const char * ds = strstr(walk, "\"Day\":\"");
            if (!ds) break;
            const char * de = strstr(ds + 7, "\"Day\":\"");
            const char * dend = de ? de : end;
            weather_day_t * dst = &weather_state.days[i];
            char iso[32];
            if (js_str(ds, dend, "Day", iso, sizeof iso))
                format_day_label(iso, dst->day, sizeof dst->day);
            dst->min_temp    = (float)js_num(ds, dend, "MinTemperature", 0);
            dst->max_temp    = (float)js_num(ds, dend, "MaxTemperature", 0);
            dst->rain_chance = (int)js_num(ds, dend, "RainChance", 0);
            dst->wind_bft    = 0;
            js_str(ds, dend, "WindDirection", dst->wind_dir, sizeof dst->wind_dir);
            for (int k = 0; dst->wind_dir[k]; k++)
                if (dst->wind_dir[k] >= 'a' && dst->wind_dir[k] <= 'z')
                    dst->wind_dir[k] -= 32;
            js_str(ds, dend, "WeatherDescription", dst->desc, sizeof dst->desc);
            /* icon code from IconUrl (.../30x30/f.png -> "f"). */
            char url[160];
            if (js_str(ds, dend, "IconUrl", url, sizeof url)) {
                const char * sl = strrchr(url, '/');
                const char * fn = sl ? sl + 1 : url;
                size_t fl = strlen(fn);
                if (fl > 4 && fn[fl - 4] == '.') {
                    size_t cl = fl - 4;
                    if (cl >= sizeof dst->icon) cl = sizeof dst->icon - 1;
                    memcpy(dst->icon, fn, cl); dst->icon[cl] = 0;
                }
            }
            dn = i + 1;
            walk = dend;
        }
        if (dn > 0) { weather_state.day_count = dn; got = 1; }
    }
    return got ? 0 : -1;
}

/* Buienradar's own weather-icon PNGs (transparent), so the forecast strips show
   the exact icons the website uses instead of mapping letter codes onto local
   artwork. The icon set is static, so each code is fetched ONCE and cached
   persistently under $TOONUI_DATA_DIR (/mnt/data) — it survives reboots and is
   never re-downloaded. A failure just leaves the screens on local icons. */
static void download_wx_icon(const char * code) {
    if (!code || !code[0]) return;
    const char * dir = getenv("TOONUI_DATA_DIR");
    if (!dir || !*dir) dir = "/mnt/data";
    char path[128];
    snprintf(path, sizeof path, "%s/wx_%s.png", dir, code);
    if (access(path, F_OK) == 0) return;            /* already cached — done */
    char up[8]; size_t j = 0;
    for (; code[j] && j < sizeof up - 1; j++) up[j] = (char)toupper((unsigned char)code[j]);
    up[j] = 0;
    char cmd[768];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -s -k -L --max-time 8 -A 'toonui/1.0' -o '%s.tmp' "
        "'https://cdn.buienradar.nl/resources/images/icons/weather/96x96/%s.png' "
        "&& mv '%s.tmp' '%s'", path, up, path, path);
    (void)system(cmd);
}
static void download_wx_icons(void) {
    for (int i = 0; i < weather_state.day_count; i++)  download_wx_icon(weather_state.days[i].icon);
    for (int i = 0; i < weather_state.hour_count; i++) download_wx_icon(weather_state.hours[i].icon);
}

/* Hourly forecast fetcher — calls forecast.buienradar.nl with the
 * configured location id and fills weather_state.hours[] with up to
 * WEATHER_FORECAST_HOURS slots spaced ~3 hours apart starting from the
 * first slot >= now. Returns 0 on success.
 *
 * Endpoint shape:
 *   { "days": [
 *       {"date":"…", "hours":[
 *         {"datetime":"2026-05-15T20:30:00", "temperature":24.1,
 *          "iconcode":"rr", "winddirection":"Z", "beaufort":3, …}, …
 *       ]},
 *       {"date":"…", "hours":[…]}
 *     ] }
 *
 * The first day usually contains 1-hour resolution; later days are
 * sparser. We just iterate days→hours linearly, pick every 3rd entry
 * after the first-future, and stop at WEATHER_FORECAST_HOURS. */
static int parse_buienradar_hourly(const char * body) {
    weather_state.hour_count = 0;
    /* Skip past the day-level prelude (location, afternoon{...}, evening{...})
     * and only parse datetimes from the first "hours":[ array onwards. The
     * afternoon/evening blocks have their own "datetime" fields but no plain
     * "temperature" key — they'd otherwise be picked up as bogus 0 °C slots.
     */
    const char * p = strstr(body, "\"hours\":[");
    if (!p) p = body;
    int picked = 0;
    int skip = 0;
    while (picked < WEATHER_FORECAST_HOURS) {
        const char * dt = strstr(p, "\"datetime\":\"");
        if (!dt) break;
        dt += 12;
        const char * dt_end = strchr(dt, '"');
        if (!dt_end) break;
        char iso[32];
        size_t n = (size_t)(dt_end - dt);
        if (n >= sizeof(iso)) n = sizeof(iso) - 1;
        memcpy(iso, dt, n); iso[n] = 0;

        /* Slot extent — bounded by the next "datetime" or the end of
         * the json. js_num / js_str clamp themselves to (slot_start, slot_end). */
        const char * slot_end = strstr(dt_end, "\"datetime\":\"");
        if (!slot_end) slot_end = body + strlen(body);

        weather_hour_t h = {0};
        /* Time portion of "2026-05-15T20:30:00" → "20:30" */
        if (strlen(iso) >= 16)
            snprintf(h.label, sizeof h.label, "%c%c:%c%c",
                     iso[11], iso[12], iso[14], iso[15]);
        else
            snprintf(h.label, sizeof h.label, "%s", iso);

        /* Skip anything that isn't a per-hour entry — buienradar inserts
         * day-level "afternoon"/"evening" slots that have their own
         * datetime but no plain "temperature" key, which would render as
         * bogus 0 °C. timetype=="Hour" is the only hourly bucket we want. */
        char timetype[16] = {0};
        js_str(dt_end, slot_end, "timetype", timetype, sizeof timetype);
        if (strcmp(timetype, "Hour") != 0) { p = slot_end; continue; }

        h.temperature = (float)js_num(dt_end, slot_end, "temperature", 0);
        h.wind_bft    = ms_to_beaufort(js_num(dt_end, slot_end, "windspeedms", 0));
        js_str(dt_end, slot_end, "winddirection", h.wind_dir, sizeof h.wind_dir);
        for (int k = 0; h.wind_dir[k]; k++)
            if (h.wind_dir[k] >= 'a' && h.wind_dir[k] <= 'z')
                h.wind_dir[k] -= 32;
        js_str(dt_end, slot_end, "iconcode", h.icon, sizeof h.icon);

        /* Buienradar's hourly entries are roughly 1 hour apart on day 1,
         * 3-hour later. Down-sample by taking every third entry so the
         * UI always shows a ~3-hour spaced strip regardless. */
        if ((skip++ % 3) == 0) {
            weather_state.hours[picked++] = h;
        }
        p = slot_end;
    }
    weather_state.hour_count = picked;
    return picked > 0 ? 0 : -1;
}

/* Parse day-level forecast fields from the forecast endpoint JSON (the same
 * body that parse_buienradar_hourly() reads).  Each item in the "days" array
 * has top-level scalar keys — mintemperature, maxtemperature, winddirection,
 * beaufort, iconcode, precipitation — before the nested "hours":[…] array.
 * We extract up to WEATHER_FORECAST_DAYS entries and populate
 * weather_state.days[]. */
static void parse_forecast_daily(const char * body)
{
    weather_state.day_count = 0;
    const char * days_arr = strstr(body, "\"days\":[");
    if (!days_arr) return;
    const char * body_end = body + strlen(body);
    const char * walk = days_arr + 7;

    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        /* Each day object starts with "date":"YYYY-MM-DDT…". */
        const char * dk = strstr(walk, "\"date\":\"");
        if (!dk) break;
        const char * ds = dk + 8;
        const char * de = strchr(ds, '"');
        if (!de) break;
        char iso[32];
        size_t n = (size_t)(de - ds);
        if (n >= sizeof(iso)) n = sizeof(iso) - 1;
        memcpy(iso, ds, n); iso[n] = 0;

        /* The day-LEVEL scalars (the real daily min/max, rain %, icon, wind)
         * sit AFTER this day's "hours":[…] array — before it the only
         * mintemperature/etc. belong to the morning/afternoon/… sub-objects.
         * Skip the hours array with a balanced bracket count: hours_start+9 is
         * the first char INSIDE the array, so depth starts at 1 for its '['.
         * (The old +8 pointed AT the '[' and re-counted it as depth 2, then
         * over-ran a ']' and blew past every remaining day — leaving only the
         * first one.) The landing point is also where the next day's "date"
         * lives, so it doubles as the loop advance. */
        const char * after = de;
        const char * hours_start = strstr(de, "\"hours\":[");
        if (hours_start && hours_start < body_end) {
            const char * p = hours_start + 9;
            int depth = 1;
            while (depth > 0 && *p) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            after = p;
        }

        weather_day_t * dst = &weather_state.days[i];
        format_day_label(iso, dst->day, sizeof(dst->day));
        dst->min_temp    = (float)js_num(after, body_end, "mintemperature", 0);
        dst->max_temp    = (float)js_num(after, body_end, "maxtemperature", 0);
        dst->rain_chance = (int)js_num(after, body_end, "precipitationmm", 0);
        dst->wind_bft    = ms_to_beaufort(js_num(after, body_end, "windspeedms", 0));
        js_str(after, body_end, "winddirection", dst->wind_dir, sizeof(dst->wind_dir));
        for (int k = 0; dst->wind_dir[k]; k++)
            if (dst->wind_dir[k] >= 'a' && dst->wind_dir[k] <= 'z')
                dst->wind_dir[k] -= 32;
        js_str(after, body_end, "iconcode", dst->icon, sizeof(dst->icon));
        snprintf(dst->desc, sizeof(dst->desc), "%s", iconcode_to_desc(dst->icon));
        weather_state.day_count = i + 1;

        walk = after;
    }
}

/* Parse current weather from the first Hour-typed entry of the first day in
 * the forecast JSON.  The forecast endpoint doesn't have a dedicated "current
 * conditions" block, but the first hourly slot is typically 1-2 hours ahead
 * and gives a reasonable approximation. */
static void parse_forecast_current(const char * body)
{
    const char * p = strstr(body, "\"hours\":[");
    if (!p) return;
    /* Scan through "hours":[ … ] for the first "timetype":"Hour" entry. */
    while (1) {
        const char * dt = strstr(p, "\"datetime\":\"");
        if (!dt) return;
        dt += 12;
        const char * dt_end = strchr(dt, '"');
        if (!dt_end) return;

        const char * slot_end = strstr(dt_end, "\"datetime\":\"");
        if (!slot_end) slot_end = body + strlen(body);

        char timetype[16] = {0};
        js_str(dt_end, slot_end, "timetype", timetype, sizeof timetype);
        if (strcmp(timetype, "Hour") == 0) {
            weather_state.current_temp = (float)js_num(dt_end, slot_end, "temperature", 0);
            weather_state.feel_temp    = (float)js_num(dt_end, slot_end, "feeltemperature", 0);
            char ic[16];
            if (js_str(dt_end, slot_end, "iconcode", ic, sizeof ic)) {
                size_t ilen = strlen(ic);
                if (ilen >= sizeof(weather_state.current_icon))
                    ilen = sizeof(weather_state.current_icon) - 1;
                memcpy(weather_state.current_icon, ic, ilen);
                weather_state.current_icon[ilen] = 0;
                /* Derive a readable description from the icon code. */
                const char * desc = iconcode_to_desc(ic);
                snprintf(weather_state.current_desc,
                         sizeof(weather_state.current_desc), "%s", desc);
            }
            return;
        }
        p = slot_end;  /* try the next slot */
    }
}

/* The forecast endpoint carries no human-readable weather report, and the old
 * data.buienradar.nl feed that did is dead — so the forecast screen's report
 * panel next to the radar was blank. Build a short report from today's
 * (days[0]) + tomorrow's (days[1]) parsed forecast. If the old feed ever comes
 * back, parse_buienradar() runs afterwards and overwrites this with the real
 * KNMI text. */
static void synth_weatherreport(void) {
    if (weather_state.day_count <= 0) return;
    const weather_day_t * d0 = &weather_state.days[0];
    snprintf(weather_state.weatherreport_title,
             sizeof weather_state.weatherreport_title,
             "Vandaag: %s", d0->desc[0] ? d0->desc : "wisselend");
    int n = snprintf(weather_state.weatherreport_text,
             sizeof weather_state.weatherreport_text,
             "%s. %d\xc2\xb0 tot %d\xc2\xb0, wind %s %d Bft, %d%% kans op neerslag.",
             d0->desc[0] ? d0->desc : "Wisselend weer",
             (int)(d0->min_temp + 0.5f), (int)(d0->max_temp + 0.5f),
             d0->wind_dir[0] ? d0->wind_dir : "?", d0->wind_bft, d0->rain_chance);
    if (weather_state.day_count > 1 && n > 0 &&
        (size_t)n < sizeof weather_state.weatherreport_text) {
        const weather_day_t * d1 = &weather_state.days[1];
        snprintf(weather_state.weatherreport_text + n,
                 sizeof weather_state.weatherreport_text - (size_t)n,
                 "\n\nMorgen: %s, %d\xc2\xb0 tot %d\xc2\xb0.",
                 d1->desc[0] ? d1->desc : "wisselend",
                 (int)(d1->min_temp + 0.5f), (int)(d1->max_temp + 0.5f));
    }
}

static int fetch_buienradar_hourly(void) {
    int id = settings.weather_location_id;
    if (id <= 0) return -1;
    char url[160];
    snprintf(url, sizeof url,
             "https://forecast.buienradar.nl/2.0/forecast/%d", id);
    static char body[80 * 1024];
    if (http_fetch(url, body, sizeof body) != 0) {
        weather_state.hour_count = 0;
        return -1;
    }
    /* Sanity-gate the response. Buienradar's /forecast/<id> accepts any
     * GeoNames id worldwide — if the user copy-pasted a KNMI station code
     * (e.g. 6249 for Berkhout) they get a forecast for somewhere random
     * (lat 33.75 lon 45.97 → Iraq, 30 °C in May). Drop the data and force
     * the daily fallback if the resolved location is outside the NL/BE
     * bounding box. */
    const char * loc = strstr(body, "\"location\"");
    if (loc) {
        double lat = js_num(loc, loc + 200, "lat", 0);
        double lon = js_num(loc, loc + 200, "lon", 0);
        if (lat < 49.0 || lat > 54.5 || lon < 2.0 || lon > 8.0) {
            fprintf(stderr, "[wx] location id %d resolves to lat=%.2f lon=%.2f "
                            "(outside NL/BE) — dropping forecast\n",
                    id, lat, lon);
            weather_state.hour_count = 0;
            return -1;
        }
    }
    /* Parse everything we can from this one response.  Order matters:
     * hourly first (it advances through "hours" arrays), then daily
     * (re-scans for day-level keys), then current (picks the first
     * Hour slot). */
    int h_ok = parse_buienradar_hourly(body);
    parse_forecast_daily(body);
    parse_forecast_current(body);
    return h_ok;
}

/* Fetches the radar PNG to disk via curl. We don't write our own libcurl
   binding — popen out, redirect to file, atomic rename. The forecast
   screen reads from /tmp/toonui_radar.png via LVGL's stdio FS driver. */
static int fetch_radar_image(void) {
    if (!weather_state.radar_url[0]) return -1;
    /* Quote-safe by construction: the URL came from buienradar's JSON
       which uses only safe chars. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -k -L --max-time 10 -A 'toonui/1.0' "
        "-o /tmp/toonui_radar.gif.tmp '%s' && mv /tmp/toonui_radar.gif.tmp /tmp/toonui_radar.gif",
        weather_state.radar_url);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

/* Resolve a city name to a Buienradar/GeoNames location id via the free
 * Open-Meteo geocoding API (no key). Buienradar's /forecast/<id> uses GeoNames
 * ids, and Open-Meteo returns the same id, so the first result drops straight
 * into settings.weather_location_id. Returns the id, or 0 if not found.
 * The city is percent-encoded (http_fetch rejects spaces/quotes), so a name
 * like "Sint Pancras" is looked up safely. */
int weather_geocode(const char * city) {
    if (!city || !city[0]) return 0;
    char enc[160]; size_t o = 0;
    for (const char * p = city; *p && o + 4 < sizeof enc; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            enc[o++] = (char)c;
        } else {
            snprintf(enc + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    enc[o] = 0;
    char url[256];
    snprintf(url, sizeof url,
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=nl&format=json",
        enc);
    static char body[4096];
    if (http_fetch(url, body, sizeof body) != 0) return 0;
    /* First "id": in the response is the top result's GeoNames id. */
    const char * p = strstr(body, "\"id\":");
    if (!p) return 0;
    int id = atoi(p + 5);
    fprintf(stderr, "[wx] geocode '%s' -> id %d\n", city, id);
    return id;
}

static void * wx_thread(void * arg) {
    (void)arg;
    static char body[128 * 1024];
    int radar_tick = 0;
    while (1) {
        /* Location-specific source — the per-id forecast endpoint provides
         * current weather, the 5-day forecast, and the 3-hourly slots. Still
         * lowercase keys, but its "beaufort" field is gone (we now derive Bft
         * from "windspeedms"). */
        int fc_ok = fetch_buienradar_hourly();
        if (fc_ok == 0)
            fprintf(stderr, "[wx] forecast: %s %.1fC, %d-day, %d hourly slots\n",
                    weather_state.current_desc, weather_state.current_temp,
                    weather_state.day_count, weather_state.hour_count);
        else
            fprintf(stderr, "[wx] forecast fetch/parse failed\n");

        /* National feed — the source of the radar-image URL and the human
         * weather report. buienradar moved to PascalCase keys in 2026; the
         * parser tracks that. Independent of the forecast endpoint. */
        int feed_ok = 0;
        if (http_fetch("https://data.buienradar.nl/2.0/feed/json",
                       body, sizeof(body)) == 0 &&
            parse_buienradar(body) == 0) {
            feed_ok = 1;
            fprintf(stderr, "[wx] feed OK — radar + report (body %zu KB)\n",
                    strlen(body) / 1024);
        } else {
            fprintf(stderr, "[wx] feed fetch/parse failed\n");
        }

        /* Report fallback: if the feed gave no report, synthesise one from the
         * parsed 5-day forecast so the panel is never blank. */
        if (!feed_ok || !weather_state.weatherreport_title[0])
            synth_weatherreport();

        weather_state.connected = (fc_ok == 0) || feed_ok;

        /* Cache buienradar's own icon PNGs for the codes we're about to show. */
        download_wx_icons();

        /* Refresh the radar GIF a few times (5-min cadence) whenever we have a
         * URL, then loop back to re-poll the JSON. */
        if (weather_state.radar_url[0]) {
            for (int i = 0; i < 3; i++) {
                if (fetch_radar_image() == 0)
                    fprintf(stderr, "[wx] radar refreshed (tick %d)\n", radar_tick++);
                else
                    fprintf(stderr, "[wx] radar fetch failed\n");
                sleep(5 * 60);
            }
        } else {
            sleep(60);
        }
    }
    return NULL;
}

int weather_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, wx_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
