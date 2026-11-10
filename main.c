/* SPDX-License-Identifier: Apache-2.0
 * Weather: current conditions, the next 24 hours and 7 days for saved
 * places, from Open-Meteo (https://open-meteo.com, free, no API key).
 *
 * Layout: a hero card tinted by the sky, eight detail tiles, an hourly
 * strip with a temperature line, and a 7-day list with range bars. Two
 * columns in landscape, one in portrait; it re-lays out on rotation.
 *
 * Threading: requests run on a short-lived worker task. It leaves its
 * result in a mutex-guarded slot that an LVGL timer (UI task) picks up,
 * so nothing touches the UI from the worker, and nothing queued can run
 * after the app has exited (the OS deletes the timer and the task).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>
#include "wx.h"

#define MAX_PLACES 8
#define MAX_RESULTS 8
#define REFRESH_MS (15u * 60u * 1000u)
#define HOUR_COL_DP 56
#define WHITE 0xFFFFFF

typedef enum { JOB_FORECAST, JOB_SEARCH } job_kind_t;

typedef struct {
    job_kind_t kind;
    wx_place_t place;
    bool fahrenheit;
    char query[64];
    struct app *s;
} job_t;

typedef struct app {
    t5_app_t *app;
    lv_obj_t *root, *body, *spin;
    lv_obj_t *page, *field, *status, *results;
    lv_timer_t *tick;
    t5_mutex_t mtx;

    /* settings */
    wx_place_t places[MAX_PLACES];
    int nplaces, sel;
    bool fahrenheit;

    /* the forecast on screen */
    wx_t wx;
    bool have;
    wx_place_t have_place;
    bool have_f;
    uint32_t fetched;            /* lv_tick of the last fetch, 0 = never this session */
    bool busy;
    bool reset_scroll;           /* the next render starts at the top (rotation) */

    /* the worker's result (under mtx) */
    struct {
        bool ready;
        job_kind_t kind;
        int err;
        wx_t wx;
        char *raw;
        size_t raw_len;
        wx_place_t place;
        bool fahrenheit;
        wx_place_t res[MAX_RESULTS];
        int nres;
    } done;
    wx_place_t results_v[MAX_RESULTS];
    int nresults;
} app_t;

static void render(app_t *s);
static void open_places(app_t *s);
static void refresh(app_t *s);

/* ---- small things ---------------------------------------------------------------------------- */
static const char *const DAYS[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static bool same_place(const wx_place_t *a, const wx_place_t *b)
{
    double dl = a->lat - b->lat, dn = a->lon - b->lon;
    return dl < 0.01 && dl > -0.01 && dn < 0.01 && dn > -0.01;
}

static const wx_place_t *current_place(app_t *s)
{
    return s->nplaces ? &s->places[s->sel] : NULL;
}

static void deg(char *out, size_t cap, float v)
{
    if (v == WX_NAN) snprintf(out, cap, "--");
    else snprintf(out, cap, "%d°", wx_round(v));
}

/* One decimal without printf's float support: 7.46 -> "7.5". */
static void dec1(char *out, size_t cap, float v)
{
    int t = wx_round(v * 10.0f);
    snprintf(out, cap, "%s%d.%d", t < 0 ? "-" : "", abs(t) / 10, abs(t) % 10);
}

static void place_line(const wx_place_t *p, char *out, size_t cap)
{
    if (p->admin[0] && strcmp(p->admin, p->name)) snprintf(out, cap, "%s, %s", p->admin, p->country);
    else snprintf(out, cap, "%s", p->country);
}

/* Temperature (°C) to a colour: icy blue through green and amber to red. */
static lv_color_t temp_color(float c)
{
    static const struct { float t; uint32_t rgb; } stops[] = {
        { -10, 0x9FA8DA }, { 0, 0x81D4FA }, { 10, 0x4FC3F7 }, { 17, 0x81C784 },
        { 23, 0xFFD54F }, { 29, 0xFFB74D }, { 35, 0xFF8A65 }, { 41, 0xE57373 },
    };
    const int n = (int)(sizeof stops / sizeof stops[0]);
    if (c <= stops[0].t) return lv_color_hex(stops[0].rgb);
    for (int i = 1; i < n; i++) {
        if (c <= stops[i].t) {
            float f = (c - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            return lv_color_mix(lv_color_hex(stops[i].rgb), lv_color_hex(stops[i - 1].rgb), (uint8_t)(f * 255));
        }
    }
    return lv_color_hex(stops[n - 1].rgb);
}

static float to_c(app_t *s, float v)
{
    return s->wx.fahrenheit ? (v - 32.0f) * 5.0f / 9.0f : v;
}

/* ---- widgets ----------------------------------------------------------------------------------- */
static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow, int gap_dp)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_gap(o, T5_DP(gap_dp), 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_event_bubble(o, true);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, md_type_role_t role, lv_opa_t opa)
{
    lv_obj_t *l = md_label_create(parent, t, role);
    if (opa != LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

static lv_obj_t *on_hero(lv_obj_t *l, lv_opa_t opa)
{
    lv_obj_set_style_text_color(l, lv_color_hex(WHITE), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent, int pad_dp)
{
    lv_obj_t *c = box(parent, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_style_bg_color(c, md_color(md_surface_at(md_scheme(), 1)), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, T5_DP(16), 0);
    lv_obj_set_style_pad_all(c, T5_DP(pad_dp), 0);
    return c;
}

static lv_obj_t *card_title(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = md_label_create(parent, "", MD_TYPE_OVERLINE);
    md_label_set_text(l, t, MD_TYPE_OVERLINE);
    lv_obj_set_style_text_opa(l, md_scheme()->on_surface.medium, 0);
    return l;
}

static void set_busy(app_t *s, bool busy)
{
    s->busy = busy;
    if (s->spin) lv_obj_set_hidden(s->spin, !busy);
}

/* ---- hero ------------------------------------------------------------------------------------- */
static void hero_colors(int mood, bool day, uint32_t *top, uint32_t *bottom)
{
    static const uint32_t d[][2] = {
        [MOOD_CLEAR] = { 0x1565C0, 0x42A5F5 }, [MOOD_CLOUDS] = { 0x37506E, 0x6A87A8 },
        [MOOD_FOG] = { 0x455A64, 0x78909C }, [MOOD_RAIN] = { 0x1C3A52, 0x3E6A8A },
        [MOOD_STORM] = { 0x2D1E5A, 0x5B4A8F }, [MOOD_SNOW] = { 0x3D5A73, 0x8AAFC9 },
    };
    static const uint32_t n[][2] = {
        [MOOD_CLEAR] = { 0x0B1638, 0x283A6E }, [MOOD_CLOUDS] = { 0x161D2E, 0x34405A },
        [MOOD_FOG] = { 0x1E2428, 0x46525A }, [MOOD_RAIN] = { 0x0F1E2B, 0x2A4459 },
        [MOOD_STORM] = { 0x160F2E, 0x3A2D63 }, [MOOD_SNOW] = { 0x1D2B38, 0x4F6B82 },
    };
    const uint32_t *c = day ? d[mood] : n[mood];
    *top = c[0];
    *bottom = c[1];
}

static lv_obj_t *hero_shell(app_t *s, lv_obj_t *parent, int mood, bool day, const char *place)
{
    uint32_t top, bottom;
    hero_colors(mood, day, &top, &bottom);
    lv_obj_t *h = box(parent, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_style_radius(h, T5_DP(20), 0);
    lv_obj_set_style_pad_all(h, T5_DP(20), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(h, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_clip_corner(h, true, 0);

    lv_obj_t *top_row = box(h, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    on_hero(md_icon_create(top_row, MD_ICON_LOCATION_ON, 18), 204);
    lv_obj_t *name = on_hero(md_label_create(top_row, place, MD_TYPE_HEADLINE6), LV_OPA_COVER);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    s->spin = lv_spinner_create(top_row);
    lv_obj_set_size(s->spin, T5_DP(20), T5_DP(20));
    lv_obj_set_style_arc_width(s->spin, T5_DP(2), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s->spin, T5_DP(2), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s->spin, lv_color_hex(WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s->spin, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->spin, lv_color_hex(WHITE), LV_PART_MAIN);
    lv_obj_set_hidden(s->spin, !s->busy);
    return h;
}

static lv_obj_t *build_hero(app_t *s, lv_obj_t *parent)
{
    const wx_t *w = &s->wx;
    const wx_cond_t *c = wx_cond(w->code);
    lv_obj_t *h = hero_shell(s, parent, c->mood, w->day, s->have_place.name);

    /* "Sunday 8 Nov, 11:00" */
    char when[64];
    int month = atoi(w->time + 5);
    snprintf(when, sizeof when, "%s %d %s, %.5s", DAYS[wx_weekday(w->time)], atoi(w->time + 8),
             MONTHS[(month >= 1 && month <= 12) ? month - 1 : 0], w->time + 11);
    on_hero(md_label_create(h, when, MD_TYPE_BODY2), 191);

    lv_obj_t *main = box(h, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(main, T5_DP(14), 0);

    /* The condition icon in a soft disc. */
    lv_obj_t *disc = lv_obj_create(main);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, T5_DP(72), T5_DP(72));
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(WHITE), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_10, 0);
    lv_obj_set_scrollable(disc, false);
    lv_obj_t *ic = md_icon_create(disc, w->day ? c->icon_day : c->icon_night, 40);
    lv_obj_set_style_text_color(ic, lv_color_hex(c->color), 0);
    lv_obj_center(ic);

    /* 26 °C, the unit small and raised. */
    lv_obj_t *temp = box(main, LV_FLEX_FLOW_ROW, 2);
    lv_obj_set_width(temp, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(temp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    char buf[80];
    snprintf(buf, sizeof buf, "%d", wx_round(w->temp));
    on_hero(md_label_create(temp, buf, MD_TYPE_HEADLINE4), LV_OPA_COVER);
    lv_obj_t *unit = on_hero(md_label_create(temp, w->u_temp, MD_TYPE_SUBTITLE1), 204);
    lv_obj_set_style_pad_top(unit, T5_DP(6), 0);

    /* Condition, then high, low and feels like, under the temperature: the
     * hero is narrow in both layouts, so nothing goes beside it. */
    char cond[40];
    wx_cond_text(w->code, cond, sizeof cond);
    lv_obj_t *cl = on_hero(md_label_create(h, cond, MD_TYPE_SUBTITLE1), LV_OPA_COVER);
    lv_obj_set_style_pad_top(cl, T5_DP(10), 0);
    char fl[16], hi[16], lo[16];
    deg(fl, sizeof fl, w->feels);
    if (w->nd) {
        deg(hi, sizeof hi, w->d[0].tmax);
        deg(lo, sizeof lo, w->d[0].tmin);
        snprintf(buf, sizeof buf, "High %s, low %s\nFeels like %s", hi, lo, fl);
    } else {
        snprintf(buf, sizeof buf, "Feels like %s", fl);
    }
    lv_obj_t *sub = on_hero(md_label_create(h, buf, MD_TYPE_BODY2), 204);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    return h;
}

/* ---- detail tiles ----------------------------------------------------------------------------- */
/* unit (may be NULL) follows the value in a lighter, smaller type. */
static void tile(lv_obj_t *grid, int w_dp, const char *icon, uint32_t tint, const char *label, const char *value,
                 const char *unit, const char *detail)
{
    lv_obj_t *t = card(grid, 14);
    lv_obj_set_width(t, T5_DP(w_dp));
    lv_obj_set_style_pad_gap(t, T5_DP(4), 0);
    /* Label on its own line (tiles are narrow), the icon beside the value. */
    lv_obj_t *l = card_title(t, label);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *vrow = box(t, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ic = md_icon_create(vrow, icon, 24);
    lv_obj_set_style_text_color(ic, lv_color_hex(tint), 0);
    text(vrow, value, MD_TYPE_HEADLINE6, LV_OPA_COVER);
    if (unit && unit[0]) {
        lv_obj_t *u = text(vrow, unit, MD_TYPE_BODY2, md_scheme()->on_surface.medium);
        lv_obj_set_style_margin_left(u, -T5_DP(4), 0);
    }
    lv_obj_t *d = text(t, detail && detail[0] ? detail : " ", MD_TYPE_CAPTION, md_scheme()->on_surface.medium);
    lv_obj_set_width(d, LV_PCT(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
}

static const char *uv_word(float uv)
{
    if (uv < 3) return "Low";
    if (uv < 6) return "Moderate";
    if (uv < 8) return "High";
    if (uv < 11) return "Very high";
    return "Extreme";
}

static void build_tiles(app_t *s, lv_obj_t *parent, int width_dp)
{
    const wx_t *w = &s->wx;
    int cols = width_dp >= 520 ? 4 : 2;
    int tw = (width_dp - 8 * (cols - 1)) / cols;
    lv_obj_t *grid = box(parent, LV_FLEX_FLOW_ROW_WRAP, 8);
    char v[32], d[48], a[16], b[16];

    /* Dew point: how the air feels, unlike humidity it doesn't swing with the temperature. */
    deg(v, sizeof v, w->dew);
    float dc = to_c(s, w->dew);
    snprintf(d, sizeof d, "%s", w->dew == WX_NAN ? "" : dc < 10 ? "Dry" : dc < 16 ? "Comfortable" : dc < 20 ? "Humid"
                                                      : "Muggy");
    tile(grid, tw, MD_ICON_THERMOSTAT, 0xFFB74D, "Dew point", v, NULL, d);

    snprintf(v, sizeof v, "%d%%", wx_round(w->humidity));
    snprintf(d, sizeof d, "Cloud cover %d%%", wx_round(w->cloud));
    tile(grid, tw, MD_ICON_WATER_DROP, 0x64B5F6, "Humidity", v, NULL, d);

    snprintf(v, sizeof v, "%d", wx_round(w->wind));
    snprintf(d, sizeof d, "%s, gusts %d", wx_compass(w->wind_dir), wx_round(w->gusts));
    tile(grid, tw, MD_ICON_AIR, 0x80CBC4, "Wind", v, w->u_wind, d);

    float uv = w->uv != WX_NAN ? w->uv : (w->nd ? w->d[0].uv : 0);
    snprintf(v, sizeof v, "%d", wx_round(uv));
    if (w->nd && w->d[0].uv != WX_NAN) snprintf(d, sizeof d, "%s, up to %d today", uv_word(uv), wx_round(w->d[0].uv));
    else snprintf(d, sizeof d, "%s", uv_word(uv));
    tile(grid, tw, MD_ICON_LIGHT_MODE, 0xFFD54F, "UV index", v, NULL, d);

    if (w->nd) {
        dec1(a, sizeof a, w->d[0].rain);
        snprintf(v, sizeof v, "%s", a);
        if (w->d[0].pop >= 0) snprintf(d, sizeof d, "%d%% chance today", w->d[0].pop);
        else d[0] = 0;
    } else {
        snprintf(v, sizeof v, "--");
        d[0] = 0;
    }
    tile(grid, tw, MD_ICON_UMBRELLA, 0x64B5F6, "Rain", v, w->nd ? w->u_rain : NULL, d);

    snprintf(v, sizeof v, "%d", wx_round(w->pressure));
    if (w->visibility_km != WX_NAN) {
        float vis = w->fahrenheit ? w->visibility_km / 1.609344f : w->visibility_km;
        dec1(b, sizeof b, vis);
        snprintf(d, sizeof d, "Visibility %s %s", b, w->fahrenheit ? "mi" : "km");
    } else {
        d[0] = 0;
    }
    tile(grid, tw, MD_ICON_SPEED, 0xB39DDB, "Pressure", v, "hPa", d);

    tile(grid, tw, MD_ICON_WB_TWILIGHT, 0xFFB74D, "Sunrise", w->nd ? w->d[0].sunrise : "--", NULL, "");
    tile(grid, tw, MD_ICON_NIGHTS_STAY, 0x9FA8DA, "Sunset", w->nd ? w->d[0].sunset : "--", NULL, "");
}

/* ---- next 24 hours ---------------------------------------------------------------------------- */
static lv_obj_t *build_hourly(app_t *s, lv_obj_t *parent)
{
    const wx_t *w = &s->wx;
    if (!w->nh) return NULL;
    lv_obj_t *c = card(parent, 0);
    lv_obj_set_style_pad_ver(c, T5_DP(16), 0);
    lv_obj_t *title = card_title(c, "Next 24 hours");
    lv_obj_set_style_pad_left(title, T5_DP(16), 0);

    lv_obj_t *scroll = box(c, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_scrollable(scroll, true);
    lv_obj_set_scroll_dir(scroll, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(scroll, T5_DP(8), 0);
    lv_obj_set_scroll_chain_hor(scroll, false);

    /* Temperature line, centred over the columns. */
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < w->nh; i++) {
        if (w->h[i].temp == WX_NAN) continue;
        if (w->h[i].temp < lo) lo = w->h[i].temp;
        if (w->h[i].temp > hi) hi = w->h[i].temp;
    }
    if (hi >= lo) {
        lv_obj_t *ch = lv_chart_create(scroll);
        lv_obj_set_size(ch, T5_DP(HOUR_COL_DP * (w->nh - 1)), T5_DP(48));
        lv_obj_set_style_margin_left(ch, T5_DP(HOUR_COL_DP / 2), 0);
        lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ch, 0, 0);
        lv_obj_set_style_pad_all(ch, 0, 0);
        lv_obj_set_style_pad_ver(ch, T5_DP(4), 0);
        lv_obj_set_style_line_width(ch, T5_DP(3), LV_PART_ITEMS);
        lv_obj_set_style_line_rounded(ch, true, LV_PART_ITEMS);
        lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
        lv_chart_set_div_line_count(ch, 0, 0);
        lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(ch, (uint32_t)w->nh);
        int ymin = (int)((lo - 0.5f) * 10), ymax = (int)((hi + 0.5f) * 10);
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
        lv_chart_series_t *ser = lv_chart_add_series(ch, temp_color(to_c(s, (lo + hi) / 2)), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < w->nh; i++) {
            int32_t v = w->h[i].temp == WX_NAN ? LV_CHART_POINT_NONE : (int32_t)(w->h[i].temp * 10);
            lv_chart_set_series_value_by_id(ch, ser, (uint32_t)i, v);
        }
        lv_chart_refresh(ch);
    }

    lv_obj_t *row = box(scroll, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    for (int i = 0; i < w->nh; i++) {
        const wx_hour_t *hr = &w->h[i];
        const wx_cond_t *c2 = wx_cond(hr->code);
        lv_obj_t *col = box(row, LV_FLEX_FLOW_COLUMN, 6);
        lv_obj_set_width(col, T5_DP(HOUR_COL_DP));
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(col, T5_DP(6), 0);
        text(col, i == 0 ? "Now" : hr->hhmm, MD_TYPE_CAPTION,
             i == 0 ? LV_OPA_COVER : md_scheme()->on_surface.medium);
        lv_obj_t *ic = md_icon_create(col, hr->day ? c2->icon_day : c2->icon_night, 24);
        lv_obj_set_style_text_color(ic, lv_color_hex(c2->color), 0);
        char t[16];
        deg(t, sizeof t, hr->temp);
        text(col, t, MD_TYPE_SUBTITLE1, LV_OPA_COVER);
        char p[16] = " ";
        if (hr->pop >= 10) snprintf(p, sizeof p, "%d%%", hr->pop);
        lv_obj_t *pl = text(col, p, MD_TYPE_CAPTION, LV_OPA_COVER);
        lv_obj_set_style_text_color(pl, lv_color_hex(0x64B5F6), 0);
    }
    /* Title at the top, the strip at the bottom if the card is stretched. */
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return c;
}

/* ---- 7 days ------------------------------------------------------------------------------------- */
static void build_daily(app_t *s, lv_obj_t *parent)
{
    const wx_t *w = &s->wx;
    if (!w->nd) return;
    lv_obj_t *c = card(parent, 0);
    lv_obj_set_style_pad_ver(c, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(c, 0, 0);
    lv_obj_t *title = card_title(c, "7-day forecast");
    lv_obj_set_style_pad_left(title, T5_DP(16), 0);
    lv_obj_set_style_pad_bottom(title, T5_DP(4), 0);

    float wmin = 1e9f, wmax = -1e9f;
    for (int i = 0; i < w->nd; i++) {
        if (w->d[i].tmin != WX_NAN && w->d[i].tmin < wmin) wmin = w->d[i].tmin;
        if (w->d[i].tmax != WX_NAN && w->d[i].tmax > wmax) wmax = w->d[i].tmax;
    }
    float span = wmax > wmin ? wmax - wmin : 1;

    for (int i = 0; i < w->nd; i++) {
        const wx_day_t *d = &w->d[i];
        const wx_cond_t *cd = wx_cond(d->code);
        lv_obj_t *row = box(c, LV_FLEX_FLOW_ROW, 10);
        lv_obj_set_height(row, T5_DP(48));
        lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        char name[16];
        snprintf(name, sizeof name, "%s", i == 0 ? "Today" : DAYS[wx_weekday(d->date)]);
        if (i) name[3] = 0;     /* Mon, Tue */
        lv_obj_t *nl = text(row, name, MD_TYPE_SUBTITLE1, LV_OPA_COVER);
        lv_obj_set_width(nl, T5_DP(56));

        lv_obj_t *ic = md_icon_create(row, cd->icon_day, 24);
        lv_obj_set_style_text_color(ic, lv_color_hex(cd->color), 0);
        lv_obj_set_width(ic, T5_DP(28));

        char p[16] = "";
        if (d->pop >= 10) snprintf(p, sizeof p, "%d%%", d->pop);
        lv_obj_t *pl = text(row, p, MD_TYPE_CAPTION, LV_OPA_COVER);
        lv_obj_set_style_text_color(pl, lv_color_hex(0x64B5F6), 0);
        lv_obj_set_width(pl, T5_DP(34));

        char t[16];
        deg(t, sizeof t, d->tmin);
        lv_obj_t *lo = text(row, t, MD_TYPE_BODY1, md_scheme()->on_surface.medium);
        lv_obj_set_width(lo, T5_DP(36));
        lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_RIGHT, 0);

        /* The day's range on the week's scale. */
        lv_obj_t *track = lv_obj_create(row);
        lv_obj_remove_style_all(track);
        lv_obj_set_height(track, T5_DP(6));
        lv_obj_set_width(track, 1);
        lv_obj_set_flex_grow(track, 1);
        lv_obj_set_style_radius(track, T5_DP(3), 0);
        lv_obj_set_style_bg_color(track, md_color(md_scheme()->c.on_surface), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_10, 0);
        lv_obj_set_scrollable(track, false);
        if (d->tmin != WX_NAN && d->tmax != WX_NAN) {
            int x = (int)((d->tmin - wmin) / span * 100);
            int wd = (int)((d->tmax - d->tmin) / span * 100);
            if (wd < 4) wd = 4;
            if (x + wd > 100) x = 100 - wd;
            lv_obj_t *seg = lv_obj_create(track);
            lv_obj_remove_style_all(seg);
            lv_obj_set_size(seg, LV_PCT(wd), LV_PCT(100));
            lv_obj_set_x(seg, LV_PCT(x));
            lv_obj_set_style_radius(seg, T5_DP(3), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(seg, temp_color(to_c(s, d->tmin)), 0);
            lv_obj_set_style_bg_grad_color(seg, temp_color(to_c(s, d->tmax)), 0);
            lv_obj_set_style_bg_grad_dir(seg, LV_GRAD_DIR_HOR, 0);
        }

        deg(t, sizeof t, d->tmax);
        lv_obj_t *hi = text(row, t, MD_TYPE_SUBTITLE1, LV_OPA_COVER);
        lv_obj_set_width(hi, T5_DP(36));
        lv_obj_set_style_text_align(hi, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ---- screens --------------------------------------------------------------------------------- */
static void search_clicked(lv_event_t *e)
{
    open_places(lv_event_get_user_data(e));
}

static void build_empty(app_t *s)
{
    lv_obj_t *col = box(s->body, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, T5_DP(48), 0);
    lv_obj_t *disc = lv_obj_create(col);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, T5_DP(88), T5_DP(88));
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, md_color(md_scheme()->c.primary), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_20, 0);
    lv_obj_t *ic = md_icon_create(disc, MD_ICON_WB_SUNNY, 40);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFC107), 0);
    lv_obj_center(ic);
    text(col, "Choose a place", MD_TYPE_HEADLINE6, LV_OPA_COVER);
    lv_obj_t *l = text(col, "Search for a city to see its weather, the next 24 hours and the week ahead.",
                       MD_TYPE_BODY2, md_scheme()->on_surface.medium);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(l, T5_DP(320), 0);
    lv_obj_t *b = md_button_create(col, "Search", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(b, search_clicked, LV_EVENT_CLICKED, s);
}

static void build_loading(app_t *s)
{
    const wx_place_t *p = current_place(s);
    lv_obj_t *h = hero_shell(s, s->body, MOOD_CLOUDS, true, p->name);
    lv_obj_set_hidden(s->spin, false);
    on_hero(md_label_create(h, "Getting the forecast…", MD_TYPE_BODY2), 191);
    lv_obj_t *pad = box(h, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_height(pad, T5_DP(72));
}

/* The standby screen's weather card (t5_ambient_set): the place now, kept
 * for three hours, so it shows while the app isn't running. */
static void post_card(app_t *s)
{
    const wx_cond_t *c = wx_cond(s->wx.code);
    t5_ambient_card_t card = { .expires_s = 3 * 3600 };
    snprintf(card.icon, sizeof card.icon, "%s", s->wx.day ? c->icon_day : c->icon_night);
    snprintf(card.title, sizeof card.title, "%.31s", s->have_place.name);   /* long names cut */
    deg(card.value, sizeof card.value, s->wx.temp);
    char hi[16], lo[16];
    if (s->wx.nd > 0) {
        deg(hi, sizeof hi, s->wx.d[0].tmax);
        deg(lo, sizeof lo, s->wx.d[0].tmin);
        snprintf(card.detail, sizeof card.detail, "%s, high %s, low %s", c->text, hi, lo);
    } else {
        snprintf(card.detail, sizeof card.detail, "%s", c->text);
    }
    T5_IGNORE(t5_ambient_set(&card));     /* only the standby card misses the update */
}

static void render(app_t *s)
{
    int32_t y = s->reset_scroll ? 0 : lv_obj_get_scroll_y(s->body);
    s->reset_scroll = false;
    if (y < 0) y = 0;
    lv_obj_clean(s->body);
    s->spin = NULL;
    int W = (int)(lv_display_get_horizontal_resolution(NULL) / T5_DP(1));
    bool wide = W >= 560;
    int pad = wide ? 16 : 12;
    lv_obj_set_style_pad_all(s->body, T5_DP(pad), 0);
    lv_obj_set_flex_flow(s->body, LV_FLEX_FLOW_COLUMN);

    if (!s->nplaces) {
        build_empty(s);
        return;
    }
    if (!s->have || !same_place(&s->have_place, current_place(s))) {
        build_loading(s);
        return;
    }

    /* Portrait: one column. Landscape: the hero beside the hourly strip,
     * then the tiles four across and the week, full width. */
    int inner = W - 2 * pad;
    lv_obj_t *hero_parent = s->body, *hourly_parent = s->body, *top = NULL;
    if (wide) {
        top = box(s->body, LV_FLEX_FLOW_ROW, 12);
        hero_parent = box(top, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_width(hero_parent, T5_DP((inner - 12) * 45 / 100));
        hourly_parent = box(top, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_width(hourly_parent, 1);
        lv_obj_set_flex_grow(hourly_parent, 1);
    }
    lv_obj_t *hero = build_hero(s, hero_parent);
    lv_obj_t *hourly = build_hourly(s, hourly_parent);
    build_tiles(s, s->body, inner);
    build_daily(s, s->body);
    if (top && hourly) {
        /* Side by side, the two cards end level. */
        lv_obj_update_layout(top);
        int32_t hh = lv_obj_get_height(hero), ch = lv_obj_get_height(hourly);
        if (hh > ch) lv_obj_set_height(hourly, hh);
        else lv_obj_set_height(hero, ch);
    }

    char foot[96];
    snprintf(foot, sizeof foot, "Updated %.5s %s\nWeather data by Open-Meteo.com", s->wx.time + 11, s->wx.tz);
    lv_obj_t *f = text(s->body, foot, MD_TYPE_CAPTION, md_scheme()->on_surface.disabled);
    lv_obj_set_width(f, LV_PCT(100));
    lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(f, T5_DP(4), 0);

    lv_obj_update_layout(s->body);
    lv_obj_scroll_to_y(s->body, y, LV_ANIM_OFF);
}

/* ---- settings and cache ----------------------------------------------------------------------- */
static void path(app_t *s, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", t5_app_data_dir(s->app), name);
}

static void save_settings(app_t *s)
{
    char p[160], tmp[168];
    path(s, "settings.txt", p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "units=%s\nsel=%d\n", s->fahrenheit ? "f" : "c", s->sel);
    for (int i = 0; i < s->nplaces; i++) {
        const wx_place_t *pl = &s->places[i];
        /* Coordinates as integer micro-degrees: no float printf needed. */
        fprintf(f, "place=%ld|%ld|%s|%s|%s\n", (long)(pl->lat * 1e6), (long)(pl->lon * 1e6), pl->name, pl->admin,
                pl->country);
    }
    fclose(f);
    rename(tmp, p);
}

static void field(char **cur, char *out, size_t cap)
{
    char *bar = *cur ? strchr(*cur, '|') : NULL;
    if (bar) *bar = 0;
    snprintf(out, cap, "%s", *cur ? *cur : "");
    *cur = bar ? bar + 1 : NULL;
}

static void load_settings(app_t *s)
{
    char p[160], line[256];
    path(s, "settings.txt", p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        for (char *c = line; *c; c++) {
            if (*c == '\r' || *c == '\n') {
                *c = 0;
                break;
            }
        }
        if (!strncmp(line, "units=", 6)) s->fahrenheit = line[6] == 'f';
        else if (!strncmp(line, "sel=", 4)) s->sel = atoi(line + 4);
        else if (!strncmp(line, "place=", 6) && s->nplaces < MAX_PLACES) {
            wx_place_t *pl = &s->places[s->nplaces];
            char *cur = line + 6, num[24];
            field(&cur, num, sizeof num);
            pl->lat = atol(num) / 1e6;
            field(&cur, num, sizeof num);
            pl->lon = atol(num) / 1e6;
            field(&cur, pl->name, sizeof pl->name);
            field(&cur, pl->admin, sizeof pl->admin);
            field(&cur, pl->country, sizeof pl->country);
            if (pl->name[0]) s->nplaces++;
        }
    }
    fclose(f);
    if (s->sel < 0 || s->sel >= s->nplaces) s->sel = 0;
}

/* The cache: a header line naming the place and units, then the JSON. */
static void save_cache(app_t *s, const wx_place_t *p, bool fahrenheit, const char *js, size_t len)
{
    char fp[160];
    path(s, "forecast.json", fp, sizeof fp);
    FILE *f = fopen(fp, "w");
    if (!f) return;
    fprintf(f, "%ld,%ld,%c\n", (long)(p->lat * 1e6), (long)(p->lon * 1e6), fahrenheit ? 'f' : 'c');
    fwrite(js, 1, len, f);
    fclose(f);
}

static void load_cache(app_t *s)
{
    const wx_place_t *p = current_place(s);
    if (!p) return;
    char fp[160], head[64];
    path(s, "forecast.json", fp, sizeof fp);
    FILE *f = fopen(fp, "r");
    if (!f) return;
    char want[64];
    snprintf(want, sizeof want, "%ld,%ld,%c\n", (long)(p->lat * 1e6), (long)(p->lon * 1e6), s->fahrenheit ? 'f' : 'c');
    if (!fgets(head, sizeof head, f) || strcmp(head, want)) {
        fclose(f);
        return;
    }
    char *js = malloc(32 * 1024);
    size_t n = js ? fread(js, 1, 32 * 1024 - 1, f) : 0;
    fclose(f);
    if (js && n && wx_parse(js, n, &s->wx)) {
        s->have = true;
        s->have_place = *p;
        s->have_f = s->fahrenheit;
    }
    free(js);
}

/* ---- worker ------------------------------------------------------------------------------------ */
static void worker(void *arg)
{
    job_t *j = arg;
    app_t *s = j->s;
    wx_t *w = malloc(sizeof *w);
    wx_place_t res[MAX_RESULTS];
    int err = 0, nres = 0;
    char *raw = NULL;
    size_t raw_len = 0;
    if (j->kind == JOB_FORECAST) {
        err = w ? wx_fetch(&j->place, j->fahrenheit, w, &raw, &raw_len) : T5_ERR_NO_MEM;
    } else {
        nres = wx_geocode(j->query, res, MAX_RESULTS);
        if (nres < 0) err = nres;
    }
    t5_mutex_lock(s->mtx);
    free(s->done.raw);
    s->done.raw = raw;
    s->done.raw_len = raw_len;
    s->done.kind = j->kind;
    s->done.err = err;
    s->done.place = j->place;
    s->done.fahrenheit = j->fahrenheit;
    if (j->kind == JOB_FORECAST && !err) s->done.wx = *w;
    if (nres > 0) memcpy(s->done.res, res, sizeof res[0] * (size_t)nres);
    s->done.nres = nres > 0 ? nres : 0;
    s->done.ready = true;
    t5_mutex_unlock(s->mtx);
    free(w);
    free(j);
}

static bool start_job(app_t *s, job_t *j)
{
    if (s->busy) {
        free(j);
        return false;
    }
    j->s = s;
    set_busy(s, true);
    if (t5_task_create("weather.net", worker, j, 12 * 1024, 5, T5_CORE_ANY, NULL) != T5_OK) {
        free(j);
        set_busy(s, false);
        md_snackbar_show("Couldn't start the request", NULL, NULL, NULL);
        return false;
    }
    return true;
}

static bool online(void)
{
    t5_net_status_t st;
    t5_net_status(&st);
    return st.state == T5_NET_CONNECTED;
}

static void retry_cb(void *ud)
{
    refresh(ud);
}

static void refresh(app_t *s)
{
    const wx_place_t *p = current_place(s);
    if (!p || s->busy) return;
    if (!online()) {
        md_snackbar_show("Not connected to Wi-Fi", "Retry", retry_cb, s);
        return;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_FORECAST;
    j->place = *p;
    j->fahrenheit = s->fahrenheit;
    start_job(s, j);
}

static void show_results(app_t *s);

static void tick(lv_timer_t *t)
{
    app_t *s = lv_timer_get_user_data(t);
    bool ready = false;
    job_kind_t kind = JOB_FORECAST;
    int err = 0;
    char *raw = NULL;
    size_t raw_len = 0;
    wx_place_t place;
    bool f = false;
    t5_mutex_lock(s->mtx);
    if (s->done.ready) {
        ready = true;
        kind = s->done.kind;
        err = s->done.err;
        place = s->done.place;
        f = s->done.fahrenheit;
        raw = s->done.raw;
        raw_len = s->done.raw_len;
        s->done.raw = NULL;
        if (kind == JOB_FORECAST && !err) s->wx = s->done.wx;
        if (kind == JOB_SEARCH) {
            s->nresults = s->done.nres;
            memcpy(s->results_v, s->done.res, sizeof s->results_v);
        }
        s->done.ready = false;
    }
    t5_mutex_unlock(s->mtx);

    if (ready) {
        set_busy(s, false);
        if (kind == JOB_FORECAST) {
            if (err) {
                md_snackbar_show(err == T5_ERR_TIMEOUT ? "Open-Meteo didn't answer in time" : "Couldn't update the weather",
                                 "Retry", retry_cb, s);
                if (!s->have) render(s);
            } else {
                s->have = true;
                s->have_place = place;
                s->have_f = f;
                s->fetched = lv_tick_get();
                if (raw) save_cache(s, &place, f, raw, raw_len);
                post_card(s);
                render(s);
            }
            /* The selection or units changed while this was in flight. */
            const wx_place_t *p = current_place(s);
            if (p && (!same_place(p, &s->have_place) || s->have_f != s->fahrenheit)) refresh(s);
        } else {
            show_results(s);
            if (err && s->status) lv_label_set_text(s->status, "Couldn't search. Check the Wi-Fi connection and try again.");
        }
        free(raw);
    }

    /* Keep it fresh while the app is open. */
    if (s->nplaces && !s->busy && s->fetched && lv_tick_elaps(s->fetched) > REFRESH_MS) refresh(s);
}

/* ---- places page ------------------------------------------------------------------------------ */
static void close_places(app_t *s)
{
    if (!s->page) return;
    t5_shell_keyboard_hide();
    lv_obj_delete(s->page);
    s->page = s->field = s->status = s->results = NULL;
}

static void places_back(lv_event_t *e)
{
    close_places(lv_event_get_user_data(e));
}

static void select_place(app_t *s, int i)
{
    s->sel = i;
    save_settings(s);
    close_places(s);
    render(s);
    refresh(s);
}

static void saved_clicked(lv_event_t *e)
{
    app_t *s = lv_event_get_user_data(e);
    select_place(s, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
}

static void build_places(app_t *s);

typedef struct {
    app_t *s;
    int index;
} rm_t;

static void remove_answer(int button, void *ud)
{
    rm_t *r = ud;
    app_t *s = r->s;
    if (button == 0 && r->index < s->nplaces) {
        memmove(&s->places[r->index], &s->places[r->index + 1],
                sizeof s->places[0] * (size_t)(s->nplaces - r->index - 1));
        s->nplaces--;
        if (s->sel >= r->index && s->sel > 0) s->sel--;
        save_settings(s);
        if (s->page) build_places(s);
        render(s);
        if (s->nplaces) refresh(s);
    }
    free(r);
}

static void saved_long(lv_event_t *e)
{
    app_t *s = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (i >= s->nplaces) return;
    rm_t *r = malloc(sizeof *r);
    if (!r) return;
    *r = (rm_t){ s, i };
    char msg[96];
    snprintf(msg, sizeof msg, "Remove %s from your places?", s->places[i].name);
    md_dialog_show("Remove place", msg, "Remove", "Cancel", remove_answer, r);
}

static void result_clicked(lv_event_t *e)
{
    app_t *s = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (i >= s->nresults) return;
    const wx_place_t *p = &s->results_v[i];
    for (int k = 0; k < s->nplaces; k++) {
        if (same_place(&s->places[k], p)) {
            select_place(s, k);
            return;
        }
    }
    /* New: first in the list, the oldest falls off. */
    int n = s->nplaces < MAX_PLACES ? s->nplaces : MAX_PLACES - 1;
    memmove(&s->places[1], &s->places[0], sizeof s->places[0] * (size_t)n);
    s->places[0] = *p;
    s->nplaces = n + 1;
    select_place(s, 0);
}

static void show_results(app_t *s)
{
    if (!s->results) return;
    lv_obj_clean(s->results);
    if (s->status) {
        lv_label_set_text(s->status, s->nresults ? "" : "No places found. Try another spelling.");
        lv_obj_set_hidden(s->status, s->nresults > 0);
    }
    for (int i = 0; i < s->nresults; i++) {
        char sub[112];
        place_line(&s->results_v[i], sub, sizeof sub);
        lv_obj_t *r = md_row_create(s->results, MD_ICON_LOCATION_ON, s->results_v[i].name, sub);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        md_row_set_clickable(r, result_clicked, s);
    }
}

static void do_search(app_t *s)
{
    if (!s->field) return;
    const char *q = md_textfield_text(s->field);
    while (q && *q == ' ') q++;
    if (!q || !*q) return;
    if (!online()) {
        lv_label_set_text(s->status, "Connect to Wi-Fi to search for places.");
        lv_obj_set_hidden(s->status, false);
        return;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_SEARCH;
    snprintf(j->query, sizeof j->query, "%s", q);
    if (start_job(s, j)) {
        t5_shell_keyboard_hide();
        lv_label_set_text(s->status, "Searching…");
        lv_obj_set_hidden(s->status, false);
        lv_obj_clean(s->results);
    }
}

static void search_go(lv_event_t *e)
{
    do_search(lv_event_get_user_data(e));
}

static void units_changed(lv_event_t *e)
{
    app_t *s = lv_event_get_user_data(e);
    s->fahrenheit = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    save_settings(s);
    refresh(s);
}

static void build_places(app_t *s)
{
    lv_obj_t *body = md_page_body(s->page);
    lv_obj_clean(body);
    lv_obj_set_style_pad_gap(body, T5_DP(4), 0);

    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
    lv_obj_set_style_pad_top(row, T5_DP(12), 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s->field = md_textfield_create(row, "Search for a city", false);
    lv_obj_set_width(s->field, 1);
    lv_obj_set_flex_grow(s->field, 1);
    lv_obj_add_event_cb(md_textfield_ta(s->field), search_go, LV_EVENT_READY, s);
    lv_obj_t *go = md_button_create(row, "Search", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(go, search_go, LV_EVENT_CLICKED, s);

    s->status = text(body, "", MD_TYPE_BODY2, md_scheme()->on_surface.medium);
    lv_obj_set_style_pad_hor(s->status, T5_DP(16), 0);
    lv_obj_set_style_pad_top(s->status, T5_DP(8), 0);
    lv_obj_set_hidden(s->status, true);
    s->results = box(body, LV_FLEX_FLOW_COLUMN, 0);

    if (s->nplaces) {
        md_section_create(body, "Saved places");
        for (int i = 0; i < s->nplaces; i++) {
            char sub[112];
            place_line(&s->places[i], sub, sizeof sub);
            lv_obj_t *r = md_row_create(body, i == s->sel ? MD_ICON_CHECK_CIRCLE : MD_ICON_LOCATION_ON,
                                        s->places[i].name, sub);
            lv_obj_set_user_data(r, (void *)(intptr_t)i);
            md_row_set_clickable(r, saved_clicked, s);
            lv_obj_add_event_cb(r, saved_long, LV_EVENT_LONG_PRESSED, s);
        }
        lv_obj_t *hint = text(body, "Touch and hold a place to remove it.", MD_TYPE_CAPTION,
                              md_scheme()->on_surface.disabled);
        lv_obj_set_style_pad_hor(hint, T5_DP(16), 0);
    }

    md_section_create(body, "Units");
    lv_obj_t *u = md_row_create(body, MD_ICON_THERMOSTAT, "Use Fahrenheit", "°F, mph and inches");
    lv_obj_t *sw = md_row_switch(u, s->fahrenheit);
    lv_obj_add_event_cb(sw, units_changed, LV_EVENT_VALUE_CHANGED, s);

    lv_obj_t *credit = text(body, "Places and forecasts from Open-Meteo.com (CC BY 4.0).", MD_TYPE_CAPTION,
                            md_scheme()->on_surface.disabled);
    lv_obj_set_style_pad_hor(credit, T5_DP(16), 0);
    lv_obj_set_style_pad_top(credit, T5_DP(16), 0);
}

static void open_places(app_t *s)
{
    if (s->page) return;
    s->page = md_page_create(s->root, "Places", places_back, s, NULL);
    lv_obj_set_floating(s->page, true);
    lv_obj_set_pos(s->page, 0, 0);
    build_places(s);
    if (!s->nplaces) lv_obj_send_event(md_textfield_ta(s->field), LV_EVENT_CLICKED, NULL);
}

/* ---- app ------------------------------------------------------------------------------------- */
static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void places_clicked(lv_event_t *e)
{
    open_places(lv_event_get_user_data(e));
}

static void refresh_clicked(lv_event_t *e)
{
    app_t *s = lv_event_get_user_data(e);
    if (!s->nplaces) open_places(s);
    else refresh(s);
}

static void on_create(t5_app_t *app)
{
    app_t *s = calloc(1, sizeof *s);
    if (!s) return;
    t5_app_set_user(app, s);
    s->app = app;
    s->mtx = t5_mutex_create();
    load_settings(s);
    load_cache(s);

    s->root = t5_app_screen(app);
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s->root, 0, 0);     /* the body starts right under the bar */
    lv_obj_t *bar = md_appbar_create(s->root, "Weather", MD_ICON_ARROW_BACK, back, NULL);
    md_appbar_add_action(bar, MD_ICON_REFRESH, refresh_clicked, s);
    md_appbar_add_action(bar, MD_ICON_SEARCH, places_clicked, s);

    s->body = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->body);
    lv_obj_set_width(s->body, LV_PCT(100));
    lv_obj_set_flex_grow(s->body, 1);
    lv_obj_set_style_pad_gap(s->body, T5_DP(12), 0);
    lv_obj_set_scroll_dir(s->body, LV_DIR_VER);
    lv_obj_set_flex_align(s->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    render(s);
    s->tick = lv_timer_create(tick, 200, s);
    if (s->nplaces) refresh(s);
}

static void on_resume(t5_app_t *app)
{
    app_t *s = t5_app_get_user(app);
    if (s && s->nplaces && !s->busy && (!s->fetched || lv_tick_elaps(s->fetched) > REFRESH_MS)) refresh(s);
}

static void on_rotate(t5_app_t *app, int degrees)
{
    (void)degrees;
    app_t *s = t5_app_get_user(app);
    if (!s) return;
    s->reset_scroll = true;
    render(s);
}

/* No on_destroy: the state stays allocated until the OS has deleted a
 * worker that may still be running, then the OS frees it with the rest of
 * the app's heap. */
T5_APP(.on_create = on_create, .on_resume = on_resume, .on_rotate = on_rotate);
