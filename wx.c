/* SPDX-License-Identifier: Apache-2.0 */
#include "wx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/net.h>
#include "json.h"
#include "md/md_icons.h"

#define FORECAST_URL "https://api.open-meteo.com/v1/forecast"
#define GEOCODE_URL "https://geocoding-api.open-meteo.com/v1/search"
#define RESP_MAX (32 * 1024)
#define TOK_MAX 2048

/* ---- conditions (WMO codes, Open-Meteo docs) ------------------------------------------------ */
#define SUN 0xFFC107
#define MOON 0xB3C5FF
#define CLOUD 0xCFD8DC
#define RAIN 0x64B5F6
#define SNOW 0xB3E5FC
#define STORM 0xB39DDB

static const struct {
    uint8_t lo, hi;
    wx_cond_t c;
} s_conds[] = {
    { 0, 0, { "Clear", MD_ICON_WB_SUNNY, MD_ICON_NIGHTS_STAY, SUN, MOOD_CLEAR } },
    { 1, 1, { "Mainly clear", MD_ICON_WB_SUNNY, MD_ICON_NIGHTS_STAY, SUN, MOOD_CLEAR } },
    { 2, 2, { "Partly cloudy", MD_ICON_WB_CLOUDY, MD_ICON_WB_CLOUDY, CLOUD, MOOD_CLOUDS } },
    { 3, 3, { "Overcast", MD_ICON_CLOUD, MD_ICON_CLOUD, CLOUD, MOOD_CLOUDS } },
    { 45, 48, { "Fog", MD_ICON_FOGGY, MD_ICON_FOGGY, CLOUD, MOOD_FOG } },
    { 51, 55, { "Drizzle", MD_ICON_GRAIN, MD_ICON_GRAIN, RAIN, MOOD_RAIN } },
    { 56, 57, { "Freezing drizzle", MD_ICON_GRAIN, MD_ICON_GRAIN, SNOW, MOOD_RAIN } },
    { 61, 65, { "Rain", MD_ICON_UMBRELLA, MD_ICON_UMBRELLA, RAIN, MOOD_RAIN } },
    { 66, 67, { "Freezing rain", MD_ICON_UMBRELLA, MD_ICON_UMBRELLA, SNOW, MOOD_RAIN } },
    { 71, 75, { "Snow", MD_ICON_AC_UNIT, MD_ICON_AC_UNIT, SNOW, MOOD_SNOW } },
    { 77, 77, { "Snow grains", MD_ICON_AC_UNIT, MD_ICON_AC_UNIT, SNOW, MOOD_SNOW } },
    { 80, 82, { "Rain showers", MD_ICON_UMBRELLA, MD_ICON_UMBRELLA, RAIN, MOOD_RAIN } },
    { 85, 86, { "Snow showers", MD_ICON_AC_UNIT, MD_ICON_AC_UNIT, SNOW, MOOD_SNOW } },
    { 95, 95, { "Thunderstorm", MD_ICON_THUNDERSTORM, MD_ICON_THUNDERSTORM, STORM, MOOD_STORM } },
    { 96, 99, { "Thunderstorm with hail", MD_ICON_THUNDERSTORM, MD_ICON_THUNDERSTORM, STORM, MOOD_STORM } },
};

static const wx_cond_t s_unknown = { "Unknown", MD_ICON_CLOUD, MD_ICON_CLOUD, CLOUD, MOOD_CLOUDS };

const wx_cond_t *wx_cond(int code)
{
    for (size_t i = 0; i < sizeof s_conds / sizeof s_conds[0]; i++) {
        if (code >= s_conds[i].lo && code <= s_conds[i].hi) return &s_conds[i].c;
    }
    return &s_unknown;
}

void wx_cond_text(int code, char *out, size_t cap)
{
    const char *base = wx_cond(code)->text;
    const char *pre = NULL;
    switch (code) {
    case 51: case 61: case 71: case 80: case 85: pre = "Light"; break;
    case 55: case 65: case 75: case 86: pre = "Heavy"; break;
    case 82: pre = "Violent"; break;
    default: break;
    }
    if (!pre) {
        snprintf(out, cap, "%s", base);
        return;
    }
    /* "Light rain": the base text in lower case after the intensity. */
    char low[32];
    snprintf(low, sizeof low, "%s", base);
    if (low[0] >= 'A' && low[0] <= 'Z') low[0] = (char)(low[0] - 'A' + 'a');
    snprintf(out, cap, "%s %s", pre, low);
}

int wx_round(float v)
{
    return (int)(v >= 0 ? v + 0.5f : v - 0.5f);
}

const char *wx_compass(float deg)
{
    static const char *const d[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int i = (int)((deg + 22.5f) / 45.0f);
    return d[((i % 8) + 8) % 8];
}

int wx_weekday(const char *ymd)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = atoi(ymd), m = atoi(ymd + 5), d = atoi(ymd + 8);
    if (m < 1 || m > 12) return 0;
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d + 6) % 7;     /* Sakamoto, Monday = 0 */
}

/* ---- HTTP --------------------------------------------------------------------------------- */
/* GET url into a malloc'd, NUL-terminated buffer. */
static int get(const char *url, char **out, size_t *len)
{
    char *buf = malloc(RESP_MAX + 1);
    if (!buf) return T5_ERR_NO_MEM;
    t5_http_req_t req = { .url = url, .timeout_ms = 15000 };
    int status = 0;
    int n = t5_http_fetch(&req, buf, RESP_MAX, &status);
    if (n < 0 || status != 200) {
        free(buf);
        return n < 0 ? n : -status;
    }
    buf[n] = 0;
    *out = buf;
    *len = (size_t)n;
    return 0;
}

/* ---- forecast ------------------------------------------------------------------------------ */
static float num(const char *js, const jtok_t *t, int n, int obj, const char *key)
{
    double v;
    return json_num(js, t, json_get(js, t, n, obj, key), &v) ? (float)v : WX_NAN;
}

static float num_at(const char *js, const jtok_t *t, int n, int obj, const char *key, int k)
{
    double v;
    return json_num(js, t, json_at(t, n, json_get(js, t, n, obj, key), k), &v) ? (float)v : WX_NAN;
}

static void str_at(const char *js, const jtok_t *t, int n, int obj, const char *key, int k, char *out, size_t cap)
{
    if (json_str(js, t, json_at(t, n, json_get(js, t, n, obj, key), k), out, cap) < 0) out[0] = 0;
}

static void hhmm(const char *iso, char out[6])
{
    /* "2026-11-08T06:15" -> "06:15" */
    const char *p = strchr(iso, 'T');
    snprintf(out, 6, "%.5s", p ? p + 1 : "--:--");
}

bool wx_parse(const char *js, size_t len, wx_t *w)
{
    jtok_t *t = malloc(TOK_MAX * sizeof *t);
    if (!t) return false;
    int n = json_parse(js, len, t, TOK_MAX);
    bool ok = false;
    if (n <= 0 || t[0].type != J_OBJ) goto out;
    memset(w, 0, sizeof *w);

    int cur = json_get(js, t, n, 0, "current");
    if (cur < 0) goto out;
    json_str(js, t, json_get(js, t, n, cur, "time"), w->time, sizeof w->time);
    if (json_str(js, t, json_get(js, t, n, 0, "timezone_abbreviation"), w->tz, sizeof w->tz) < 0) w->tz[0] = 0;
    w->temp = num(js, t, n, cur, "temperature_2m");
    w->feels = num(js, t, n, cur, "apparent_temperature");
    w->dew = num(js, t, n, cur, "dew_point_2m");
    w->humidity = num(js, t, n, cur, "relative_humidity_2m");
    w->cloud = num(js, t, n, cur, "cloud_cover");
    w->wind = num(js, t, n, cur, "wind_speed_10m");
    w->wind_dir = num(js, t, n, cur, "wind_direction_10m");
    w->gusts = num(js, t, n, cur, "wind_gusts_10m");
    w->pressure = num(js, t, n, cur, "pressure_msl");
    float vis = num(js, t, n, cur, "visibility");
    w->uv = num(js, t, n, cur, "uv_index");
    w->precip = num(js, t, n, cur, "precipitation");
    float code = num(js, t, n, cur, "weather_code");
    w->code = code == WX_NAN ? -1 : (int)code;
    w->day = num(js, t, n, cur, "is_day") != 0.0f;

    int cu = json_get(js, t, n, 0, "current_units");
    if (json_str(js, t, json_get(js, t, n, cu, "temperature_2m"), w->u_temp, sizeof w->u_temp) < 0)
        snprintf(w->u_temp, sizeof w->u_temp, "°C");
    if (json_str(js, t, json_get(js, t, n, cu, "wind_speed_10m"), w->u_wind, sizeof w->u_wind) < 0)
        snprintf(w->u_wind, sizeof w->u_wind, "km/h");
    if (json_str(js, t, json_get(js, t, n, cu, "precipitation"), w->u_rain, sizeof w->u_rain) < 0)
        snprintf(w->u_rain, sizeof w->u_rain, "mm");
    w->fahrenheit = strstr(w->u_temp, "F") != NULL;
    /* Open-Meteo's spellings: "mp/h", "inch"; visibility in m or (imperial) ft. */
    if (!strcmp(w->u_wind, "mp/h")) snprintf(w->u_wind, sizeof w->u_wind, "mph");
    if (!strcmp(w->u_rain, "inch")) snprintf(w->u_rain, sizeof w->u_rain, "in");
    char u_vis[8] = "m";
    json_str(js, t, json_get(js, t, n, cu, "visibility"), u_vis, sizeof u_vis);
    w->visibility_km = vis == WX_NAN ? WX_NAN : !strcmp(u_vis, "ft") ? vis * 0.0003048f : vis / 1000.0f;

    /* Hourly data starts at the top of an hour that may already be over:
     * begin with the hour of the observation. */
    int hr = json_get(js, t, n, 0, "hourly");
    int htime = json_get(js, t, n, hr, "time");
    int total = htime >= 0 ? t[htime].size : 0, first = 0;
    char now_hour[17];
    snprintf(now_hour, sizeof now_hour, "%.13s:00", w->time);
    for (; first < total; first++) {
        char iso[20];
        str_at(js, t, n, hr, "time", first, iso, sizeof iso);
        if (strcmp(iso, now_hour) >= 0) break;
    }
    if (first == total) first = 0;
    w->nh = total - first;
    if (w->nh > WX_HOURS) w->nh = WX_HOURS;
    for (int i = 0; i < w->nh; i++) {
        int k = first + i;
        wx_hour_t *h = &w->h[i];
        char iso[20];
        str_at(js, t, n, hr, "time", k, iso, sizeof iso);
        hhmm(iso, h->hhmm);
        h->temp = num_at(js, t, n, hr, "temperature_2m", k);
        float pop = num_at(js, t, n, hr, "precipitation_probability", k);
        h->pop = pop == WX_NAN ? -1 : (int)pop;
        float c = num_at(js, t, n, hr, "weather_code", k);
        h->code = c == WX_NAN ? -1 : (int)c;
        h->day = num_at(js, t, n, hr, "is_day", k) != 0.0f;
    }

    int dy = json_get(js, t, n, 0, "daily");
    int dtime = json_get(js, t, n, dy, "time");
    w->nd = dtime >= 0 ? t[dtime].size : 0;
    if (w->nd > WX_DAYS) w->nd = WX_DAYS;
    for (int k = 0; k < w->nd; k++) {
        wx_day_t *d = &w->d[k];
        char iso[20];
        str_at(js, t, n, dy, "time", k, d->date, sizeof d->date);
        float c = num_at(js, t, n, dy, "weather_code", k);
        d->code = c == WX_NAN ? -1 : (int)c;
        d->tmax = num_at(js, t, n, dy, "temperature_2m_max", k);
        d->tmin = num_at(js, t, n, dy, "temperature_2m_min", k);
        float pop = num_at(js, t, n, dy, "precipitation_probability_max", k);
        d->pop = pop == WX_NAN ? -1 : (int)pop;
        d->rain = num_at(js, t, n, dy, "precipitation_sum", k);
        d->uv = num_at(js, t, n, dy, "uv_index_max", k);
        str_at(js, t, n, dy, "sunrise", k, iso, sizeof iso);
        hhmm(iso, d->sunrise);
        str_at(js, t, n, dy, "sunset", k, iso, sizeof iso);
        hhmm(iso, d->sunset);
    }
    ok = w->time[0] && w->temp != WX_NAN;
out:
    free(t);
    return ok;
}

int wx_fetch(const wx_place_t *p, bool fahrenheit, wx_t *out, char **raw, size_t *raw_len)
{
    char url[768];
    snprintf(url, sizeof url,
             FORECAST_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,"
             "wind_speed_10m,wind_direction_10m,wind_gusts_10m,pressure_msl,visibility,uv_index,dew_point_2m,cloud_cover"
             "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
             "precipitation_sum,sunrise,sunset,uv_index_max"
             "&timezone=auto&forecast_days=7&forecast_hours=26%s",
             p->lat, p->lon,
             fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch" : "");
    char *js;
    size_t len;
    int e = get(url, &js, &len);
    if (e) return e;
    if (!wx_parse(js, len, out)) {
        free(js);
        return T5_ERR_FAIL;
    }
    if (raw) {
        *raw = js;
        *raw_len = len;
    } else {
        free(js);
    }
    return 0;
}

/* ---- geocoding ------------------------------------------------------------------------------ */
static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

int wx_geocode(const char *q, wx_place_t *out, int max)
{
    char enc[160], url[320];
    url_encode(q, enc, sizeof enc);
    snprintf(url, sizeof url, GEOCODE_URL "?name=%s&count=%d&language=en&format=json", enc, max);
    char *js;
    size_t len;
    int e = get(url, &js, &len);
    if (e) return e;
    jtok_t *t = malloc(TOK_MAX * sizeof *t);
    int found = 0;
    int n = t ? json_parse(js, len, t, TOK_MAX) : -1;
    int res = n > 0 ? json_get(js, t, n, 0, "results") : -1;
    for (int k = 0; res >= 0 && found < max; k++) {
        int o = json_at(t, n, res, k);
        if (o < 0) break;
        wx_place_t *p = &out[found];
        memset(p, 0, sizeof *p);
        double lat, lon;
        if (json_str(js, t, json_get(js, t, n, o, "name"), p->name, sizeof p->name) < 0) continue;
        if (!json_num(js, t, json_get(js, t, n, o, "latitude"), &lat) ||
            !json_num(js, t, json_get(js, t, n, o, "longitude"), &lon)) continue;
        p->lat = lat;
        p->lon = lon;
        json_str(js, t, json_get(js, t, n, o, "admin1"), p->admin, sizeof p->admin);
        json_str(js, t, json_get(js, t, n, o, "country"), p->country, sizeof p->country);
        found++;
    }
    free(t);
    free(js);
    return n < 0 ? T5_ERR_FAIL : found;
}
