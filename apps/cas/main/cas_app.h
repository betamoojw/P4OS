/*
 * P4OS - CAS: what the app's views share (cas_app.c).
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

/* --- the engine, one job at a time ----------------------------------------
 * Jobs queue up and run in order; the callback runs in the LVGL task. A
 * view that goes away while its jobs are pending cancels them. */

typedef void (*cas_job_cb_t)(void *user, const char *result, bool error, uint32_t ms);

bool cas_job(const char *expr, cas_job_cb_t cb, void *user);
void cas_job_cancel(void *user);
bool cas_engine_ok(void);
void cas_engine_stop_job(void);
void cas_app_set_gestures(bool graph);   /* the back gesture off while the graph is shown */
void cas_app_show(int view);              /* CAS_V_CALC... */

/* --- the views ------------------------------------------------------------ */

enum { CAS_V_CALC, CAS_V_GRAPH, CAS_V_GEO, CAS_V_STATS, CAS_V_FIN, CAS_V_COUNT };

void cas_calc_build(lv_obj_t *parent, bool landscape);
void cas_calc_submit_text(const char *expr);    /* as if typed and entered */
void cas_calc_clear(void);
void cas_calc_save(void);
void cas_calc_free(void);

void cas_graph_build(lv_obj_t *parent, bool landscape);
int  cas_graph_put(const char *expr);          /* into the first free f(x); -1 if none */
void cas_graph_shown(bool shown);
void cas_graph_free(void);

void cas_geo_build(lv_obj_t *parent, bool landscape);
void cas_geo_shown(bool shown);
void cas_geo_free(void);

void cas_stats_build(lv_obj_t *parent, bool landscape);
void cas_stats_hidden(void);
void cas_stats_free(void);

void cas_fin_build(lv_obj_t *parent, bool landscape);
void cas_fin_hidden(void);
void cas_fin_free(void);
