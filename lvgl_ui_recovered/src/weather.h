#ifndef TOON_WEATHER_H
#define TOON_WEATHER_H

#define WEATHER_FORECAST_DAYS  5
/* 6 so the home/dim strips can render 5 future slots after skipping
 * slot 0 (which feeds the "Medemblik - 14.7 C now" header above). */
#define WEATHER_FORECAST_HOURS 6    /* number of 3-hour slots we surface */
#define WEATHER_RAIN_SLOTS     30   /* Buienradar rain nowcast: 5-min steps, ~2 h */

/* One 5-minute slot of the Buienradar precipitation nowcast. */
typedef struct {
    char time[6];   /* "HH:MM" local */
    int  value;     /* 0-255 Buienradar scale; mm/h = 10^((value-109)/32), 0 = dry */
} rain_slot_t;

/* One day in the 5-day forecast — fields parsed from buienradar JSON's
   `forecast.fivedayforecast[]`. */
typedef struct {
    char  day[16];           /* "ma 13-5" — Dutch short-day + dd-m */
    float min_temp;          /* °C */
    float max_temp;
    int   rain_chance;       /* % */
    int   wind_bft;          /* wind force in Beaufort */
    char  wind_dir[6];       /* "ZW", "NW", "ZZW", etc. */
    char  icon[8];           /* single-letter icon code, e.g. "a", "f" */
    char  desc[64];          /* short Dutch weatherdescription */
} weather_day_t;

/* One slot in the 3-hourly forecast — fields parsed from
   forecast.buienradar.nl/2.0/forecast/<id>/days[]/hours[]. */
typedef struct {
    char  label[8];          /* "15:00" — local time of the slot */
    float temperature;       /* °C */
    int   wind_bft;
    char  wind_dir[6];
    char  icon[8];           /* first letter compatible with weather_icon_for */
} weather_hour_t;

typedef struct {
    volatile int    connected;
    volatile float  current_temp;     /* live station temperature */
    volatile float  feel_temp;
    char            current_desc[80];
    char            current_icon[8];
    char            radar_url[128];   /* api.buienradar.nl/.../RadarMapNL?... */
    char            weatherreport_title[128];
    char            weatherreport_text[2400];
    weather_day_t   days[WEATHER_FORECAST_DAYS];
    int             day_count;
    weather_hour_t  hours[WEATHER_FORECAST_HOURS];
    int             hour_count;       /* 0 → use daily band, >0 → use hourly */
    float           lat, lon;         /* resolved location, for the rain nowcast */
    rain_slot_t     rain[WEATHER_RAIN_SLOTS];
    int             rain_count;       /* nowcast slots parsed (0 = none) */
} weather_state_t;

extern weather_state_t weather_state;

int weather_start(void);

/* Resolve a city name to a Buienradar/GeoNames location id (Open-Meteo
 * geocoding). Returns the id or 0 if not found. */
int weather_geocode(const char * city);

#endif
