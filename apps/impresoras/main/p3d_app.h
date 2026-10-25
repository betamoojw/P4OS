/*
 * P4OS - 3D printers: what the three screens share (LVGL's task only).
 */
#pragma once

#include "aos_app.h"
#include "p3d.h"

enum { P3D_SCR_LIST = 0, P3D_SCR_DETAIL, P3D_SCR_EDIT };

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int32_t     W, H;
    bool        land;
    bool        hidden;
    int         screen;
    int         detail;         /* the machine the detail shows */
    p3d_cfgs_t  cfg;
    uint32_t    cfg_stamp;
} p3d_app_t;

extern p3d_app_t p3d_app;

void p3d_build(void);           /* the current screen, again */
void p3d_go_list(void);
void p3d_go_detail(int i);
void p3d_go_edit(int i);        /* -1: a new one */
bool p3d_cfg_commit(void);      /* saves p3d_app.cfg and restarts what changed */

/* p3d_detail.c */
void p3d_detail_build(void);
void p3d_detail_tick(void);
void p3d_detail_close(void);
bool p3d_detail_back(void);

/* p3d_edit.c */
void p3d_edit_open(int i);
void p3d_edit_build(void);
void p3d_edit_tick(void);
void p3d_edit_keep(void);       /* what was typed, before a rebuild */
bool p3d_edit_back(void);
void p3d_edit_close(void);
int  p3d_edit_threads(void);    /* the search, still running */
