/*
 * P4OS - Datalog: readings of the header's sensors, logged to CSV files on
 * the card on a schedule, with alarms (apps/datalog/README.md).
 *
 * The pieces:
 *
 *   dl_cfg.c     the lines to read, kept in <data>/datalog.txt (the portal's
 *                page reads it too)
 *   dl_engine.c  the thread that reads each line when it is due, writes the
 *                CSV, checks the alarms and drives their GPIO outputs; it
 *                runs while the app is alive, hidden or not
 *   dl_src.c     where a value comes from: the system's sensor service
 *                (I2C, SPI, 1-Wire, the ADS1115), a GPIO read here, or the
 *                board itself; units and the words for them
 *   dl_csv.c     the files: appending, the tail for the cards, a series
 *                sampled out of a file of any size for the charts
 *   dl_ui.c      small widgets and the sheets (text, number, time, confirm)
 *   dl_list.c    the main screen: one card per line
 *   dl_view.c    one line: its chart over a time range, its numbers
 *   dl_edit.c    the form that makes or changes a line
 *   datalog.c    the app: life cycle, the top bar, the alarms on screen, the
 *                portal's live channel
 *
 * The engine and the UI share the configuration and the latest readings
 * under one mutex (dl_lock). The UI never writes the configuration itself:
 * it hands a whole line to dl_ch_put() and the engine saves.
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_sensors.h"
#include "aos_sensor_text.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

/* The app's own catalog, not the front app's (dl_i18n.c): _() is dl_tr() */
const char *dl_tr(const char *es);
void        dl_i18n_load(void);
#undef _
#define _(s) dl_tr(s)

#define DL_APP_ID   "aos.datalog"
#define DL_OWNER    "Datalog"
#define DL_MAX_CH   16
#define DL_MAX_AT   8
#define DL_HIST     90      /* readings kept in memory per line, for the cards */
#define DL_CHART_N  240     /* points across the detail chart */

#define DL_C_MAIN   lv_color_hex(0x2DD4BF)
#define DL_C_DARK   lv_color_hex(0x0F766E)
#define DL_C_HIGH   lv_color_hex(0xFF453A)
#define DL_C_LOW    lv_color_hex(0x64D2FF)

/* What kind of source a line has: the form's first choice */
/* GPIO: a pin's level or an ADS1115 input; I2C, SPI, 1-Wire, PIN (a DHT,
 * an HC-SR04, a scale, pulses) and UART: a sensor of the service; SYS: the
 * board itself */
enum { DL_K_GPIO = 0, DL_K_I2C, DL_K_SPI, DL_K_OW, DL_K_PIN, DL_K_UART, DL_K_SYS, DL_K_COUNT };
#define DL_K_SERVICE(k) ((k) >= DL_K_I2C && (k) <= DL_K_UART)
enum { DL_PULL_NONE = 0, DL_PULL_UP, DL_PULL_DOWN };
/* An ADS1115 input: volts, the converter's counts, or a straight line
 * through two points into the user's own unit */
enum { DL_AN_VOLT = 0, DL_AN_RAW, DL_AN_SCALE };
enum { DL_SYS_RSSI = 0, DL_SYS_PSRAM, DL_SYS_RAM, DL_SYS_UPTIME, DL_SYS_COUNT };
enum { DL_AL_NONE = 0, DL_AL_LOW, DL_AL_HIGH };

typedef struct {
    char     id[24];                /* the CSV's name: <card>/datalog/<id>.csv */
    char     name[32];
    /* when: every every_s seconds, or at the minutes of the day in at[] */
    bool     sched_at;
    uint32_t every_s;
    uint8_t  n_at;
    uint16_t at[DL_MAX_AT];
    /* what */
    uint8_t  kind;                  /* DL_K_* */
    bool     analog;                /* DL_K_GPIO: an ADS1115 input, not a pin */
    int8_t   pin;                   /* DL_K_GPIO digital */
    uint8_t  pull;
    bool     invert;
    char     sensor[24];            /* the sensor service's name ("bme280", "ds18b20_A1B2C3") */
    char     key[12];               /* its value ("temp", "a0") */
    uint8_t  an_mode;               /* DL_AN_* */
    float    x1, y1, x2, y2;        /* DL_AN_SCALE: x1 V reads y1, x2 V reads y2 */
    char     unit[12];
    uint8_t  sys;                   /* DL_K_SYS: DL_SYS_* */
    /* alarms */
    bool     lo_on, hi_on;
    float    lo, hi, hyst;
    bool     sound;                 /* a sound and a message on screen; else only the CSV's note */
    int8_t   out_pin;               /* -1: none */
    bool     out_high;              /* the output's active level: 3V3, else GND */
} dl_ch_t;

typedef struct {
    bool    running;
    bool    semicolon;              /* CSV with ';' and a decimal comma (a Spanish spreadsheet) */
    int     n;
    dl_ch_t ch[DL_MAX_CH];
} dl_cfg_t;

/* The latest of each line, kept by the engine */
typedef struct {
    bool     have;                  /* v is a reading */
    bool     ok;                    /* the last attempt read */
    float    v;
    char     when[24];              /* "18/10 14:05:10", the last attempt */
    char     err[64];
    uint32_t reads, errors;
    uint8_t  alarm;                 /* DL_AL_* */
    int      hn, hhead;
    float    hist[DL_HIST];         /* ring, the latest at hhead - 1 */
} dl_rt_t;

typedef struct {
    int     ch;
    uint8_t alarm;                  /* DL_AL_*: NONE = back to normal */
    float   v;
    char    name[32];
    char    text[96];
} dl_alert_t;

/* ---- the engine (dl_engine.c) ---- */
void       dl_engine_start(void);
void       dl_engine_stop(void);
void       dl_lock(void);
void       dl_unlock(void);
dl_cfg_t  *dl_cfg(void);            /* with dl_lock held */
dl_rt_t   *dl_rt(int i);            /* with dl_lock held */
uint32_t   dl_seq(void);            /* moves with every reading and every change */
bool       dl_ch_put(int i, const dl_ch_t *c);   /* i -1: a new line; saves */
void       dl_ch_delete(int i);
void       dl_set_running(bool on);
void       dl_set_semicolon(bool on);
void       dl_reload(void);         /* datalog.txt changed under us (the portal) */
bool       dl_alert_pop(dl_alert_t *out);
int        dl_alarms_on(void);
int64_t    dl_now(bool *valid);     /* local time as seconds of a plain calendar, or uptime */
void       dl_fmt_when(char *out, size_t n);
const char *dl_pin_owner(int gpio); /* who has it, NULL nobody or us */

/* ---- the configuration file (dl_cfg.c) ---- */
void dl_cfg_defaults(dl_ch_t *c);
bool dl_cfg_load(dl_cfg_t *cfg);
bool dl_cfg_save(const dl_cfg_t *cfg);
void dl_cfg_path(char *out, size_t n);
void dl_cfg_slug(const char *name, char *out, size_t n);
void dl_sched_text(const dl_ch_t *c, char *out, size_t n);

/* ---- sources (dl_src.c) ---- */
bool        dl_src_read(const dl_ch_t *c, float *v, char *err, size_t n);
const char *dl_src_unit(const dl_ch_t *c);
int         dl_src_decimals(const dl_ch_t *c);
void        dl_src_desc(const dl_ch_t *c, char *out, size_t n);
bool        dl_src_ready(const dl_ch_t *c);        /* the form chose enough to read */
const char *dl_src_glyph(const dl_ch_t *c);
const char *dl_key_name(const char *key);
const char *dl_kind_name(int kind);
const char *dl_sys_name(int sys);
void        dl_fmt_value(const dl_ch_t *c, float v, bool unit, char *out, size_t n);
void        dl_fmt_num(float v, int dec, char *out, size_t n);   /* decimal comma */
bool        dl_parse_num(const char *s, float *out);
int         dl_src_sensors(int kind, bool analog, aos_sensor_t *out, int max);
bool        dl_src_find(const char *name, aos_sensor_t *out);
int         dl_src_values(const char *name, aos_sensor_value_t *out, int max);
void        dl_sensor_where(const aos_sensor_t *s, char *out, size_t n);
void        dl_err_text(const char *err, char *out, size_t n);
int         dl_ads_gain(const char *sensor);
bool        dl_ads_set_gain(const char *sensor, int gain_mv);
int         dl_pins(int8_t *out, int max);         /* the header's GPIOs a line may take */
const char *dl_pin_port(int gpio);
void        dl_pin_label(int gpio, char *out, size_t n);

/* ---- the CSV files (dl_csv.c) ---- */
typedef struct {
    int     n;                      /* points */
    float   v[DL_CHART_N];          /* NAN: nothing in that slice of time */
    float   lo, hi, sum, last;
    int     cnt;                    /* readings that went in */
    int64_t t0, t1;                 /* the span the points cover */
    bool    sampled;                /* a big file: read at points, not whole */
    bool    ok;
} dl_series_t;

void    dl_csv_dir(char *out, size_t n);
void    dl_csv_path(const char *id, char *out, size_t n);
bool    dl_csv_append(bool semicolon, const dl_ch_t *c, const char *date, const char *tim,
                      bool have, float v, const char *note);
int     dl_csv_tail(const char *id, float *vals, int max, float *last, char *when, size_t wn);
bool    dl_csv_series(const char *id, int64_t span_s, int64_t now, dl_series_t *out);
long    dl_csv_size(const char *id);
bool    dl_csv_clear(const char *id);
int64_t dl_civil(int y, int mo, int d, int hh, int mi, int ss);
void    dl_uncivil(int64_t t, int *y, int *mo, int *d, int *hh, int *mi, int *ss);

/* ---- the UI (datalog.c and the dl_*.c views) ---- */
enum { DL_V_LIST = 0, DL_V_DETAIL, DL_V_EDIT };

typedef struct {
    lv_obj_t *root, *bar, *content, *overlay, *alert;
    int32_t   W, H, bar_h;
    bool      land;
    int       view, sel;
    uint32_t  seen_seq;
    /* list */
    lv_obj_t *clock, *date, *status, *play;
    lv_obj_t *c_val[DL_MAX_CH], *c_when[DL_MAX_CH], *c_tag[DL_MAX_CH], *c_chart[DL_MAX_CH];
    lv_chart_series_t *c_ser[DL_MAX_CH];
    int       last_sec;
} dl_ui_t;

/* every app links into one simulator: the global has the app's prefix */
#define U dl_u
extern dl_ui_t U;

void dl_go(int view, int ch);
void dl_bar_clean(void);

void dl_list_build(void);
void dl_list_refresh(void);
void dl_view_build(int ch);
void dl_view_refresh(void);
void dl_view_tick(void);
void dl_view_leave(void);
void dl_edit_open(int ch);
bool dl_edit_back(void);
void dl_edit_tick(void);

/* widgets (dl_ui.c) */
void      dl_scpy(char *d, size_t n, const char *s);
lv_obj_t *dl_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dl_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dl_column(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *dl_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *dl_round(lv_obj_t *parent, const char *glyph, int32_t d, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *dl_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *dl_chips(lv_obj_t *parent, int32_t w);
lv_obj_t *dl_caption(lv_obj_t *parent, const char *text, int32_t w);
lv_obj_t *dl_section(lv_obj_t *parent, const char *text);
lv_obj_t *dl_row(lv_obj_t *parent, int32_t w, const char *label, const char *value, lv_event_cb_t cb, void *ud);
void      dl_sheet_close(void);
lv_obj_t *dl_sheet_open(const char *title, int32_t w);
void      dl_text_entry(const char *title, const char *value, int max, void (*done)(const char *));
void      dl_num_entry(const char *title, float value, int dec, void (*done)(float));
void      dl_time_entry(const char *title, int minutes, void (*done)(int));
void      dl_confirm(const char *text, const char *yes, void (*done)(void));
