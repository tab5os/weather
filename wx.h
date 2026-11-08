/* SPDX-License-Identifier: Apache-2.0
 * Weather data: Open-Meteo requests and the parsed forecast. No UI here;
 * the fetch functions block and run on the app's worker task. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WX_HOURS 24
#define WX_DAYS 7
#define WX_NAN (-9999.0f)          /* a value the response didn't have */

typedef struct {
    char name[48];
    char admin[48];                /* state or region, may be "" */
    char country[48];
    double lat, lon;
} wx_place_t;

typedef struct {
    char hhmm[6];
    float temp;
    int pop;                       /* precipitation probability, %; -1 unknown */
    int code;                      /* WMO weather code */
    bool day;
} wx_hour_t;

typedef struct {
    char date[11];                 /* YYYY-MM-DD */
    int code;
    float tmax, tmin;
    int pop;
    float rain;                    /* precipitation sum */
    char sunrise[6], sunset[6];
    float uv;
} wx_day_t;

typedef struct {
    char time[17];                 /* local YYYY-MM-DDTHH:MM of the observation */
    char tz[16];                   /* "GMT+5:30", "CET" */
    float temp, feels, dew, humidity, cloud;
    float wind, wind_dir, gusts, pressure, uv, precip;
    float visibility_km;           /* converted from m or ft */
    int code;
    bool day;
    bool fahrenheit;               /* units the data came in */
    char u_temp[8], u_wind[8], u_rain[8];
    int nh, nd;
    wx_hour_t h[WX_HOURS];
    wx_day_t d[WX_DAYS];
} wx_t;

/* Fetch the forecast; on success *raw (malloc'd JSON, for the cache) is set
 * if raw isn't NULL. Returns 0, or a negative t5_err_t / -HTTP status. */
int wx_fetch(const wx_place_t *p, bool fahrenheit, wx_t *out, char **raw, size_t *raw_len);
/* Parse a forecast response (also the cached copy). */
bool wx_parse(const char *js, size_t len, wx_t *out);
/* Places matching q, best first. Returns how many (0..max), or < 0 on error. */
int wx_geocode(const char *q, wx_place_t *out, int max);

typedef struct {
    const char *text;
    const char *icon_day, *icon_night;
    uint32_t color;                /* icon tint, 0xRRGGBB */
    uint8_t mood;                  /* wx_mood_t: picks the hero gradient */
} wx_cond_t;

typedef enum { MOOD_CLEAR, MOOD_CLOUDS, MOOD_FOG, MOOD_RAIN, MOOD_STORM, MOOD_SNOW } wx_mood_t;

const wx_cond_t *wx_cond(int code);
/* "Light rain" style text: the base text with the intensity for this code. */
void wx_cond_text(int code, char *out, size_t cap);

/* Small helpers shared with the UI. */
int wx_round(float v);
const char *wx_compass(float deg);
int wx_weekday(const char *ymd);   /* 0 = Monday */
