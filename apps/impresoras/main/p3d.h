/*
 * P4OS - 3D printers: what the app's files share.
 *
 *   impresoras.c   the app: the list of machines, the timer, the notices
 *                  when a print ends, the portal's live data (LVGL's task)
 *   p3d_detail.c   one machine: status, controls, files, camera, console
 *   p3d_edit.c     the editor of a machine and the mDNS search
 *   p3d_ui.c       small UI pieces shared by the three screens
 *   p3d_core.c     one thread per machine: the driver's loop, the queue of
 *                  commands, the status the UI copies, the history
 *   p3d_cfg.c      impresoras.txt, the saved machines
 *   p3d_net.c      HTTP/1.1 over the HAL's connection (keep-alive, chunked,
 *                  Basic and Digest), raw sockets, URL and JSON escaping
 *   p3d_json.c     reading values out of JSON by path, without a tree
 *   p3d_mqtt.c     a small MQTT 3.1.1 client (Bambu Lab)
 *   p3d_cam.c      the camera: snapshots, MJPEG, Bambu's port 6000
 *   drv_*.c        one per protocol
 *
 * Nothing here blocks LVGL: the drivers run in their machine's thread and
 * the UI only copies what they publish under one mutex.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define P3D_TAG     "imp3d"
#define P3D_APP_ID  "aos.impresoras"
#define P3D_MAX     12              /* machines in the list */

/* PSRAM first (docs/MEMORY.md); plain malloc in the simulator */
void *p3d_psram(size_t n);

/* src into dst, cut to fit (snprintf("%s") warns on every possible cut) */
static inline void p3d_copy(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memmove(dst, src, n);
    dst[n] = '\0';
}

/* ---- the kinds of machine ---------------------------------------------- */

typedef enum {
    P3D_OCTOPRINT = 0,
    P3D_PRUSALINK,
    P3D_BAMBU,
    P3D_MOONRAKER,
    P3D_DUET,
    P3D_ELEGOO,
    P3D_FLASHFORGE,
    P3D_CREALITY,
    P3D_KIND_N
} p3d_kind_t;

/* which fields of the editor a kind uses */
enum {
    P3D_F_KEY    = 1u << 0,
    P3D_F_USER   = 1u << 1,
    P3D_F_SERIAL = 1u << 2,
    P3D_F_TLS    = 1u << 3,
    P3D_F_KEY_NEEDED    = 1u << 4,
    P3D_F_SERIAL_NEEDED = 1u << 5,
};

/* ---- one saved machine (p3d_cfg.c) ------------------------------------- */

typedef struct {
    char name[48];
    int  kind;                  /* p3d_kind_t */
    char host[96];
    int  port;                  /* 0: the kind's own */
    bool tls;                   /* https / wss */
    char key[128];              /* API key, access code, password, check code */
    char user[32];
    char serial[48];            /* Bambu and FlashForge serial, Elegoo mainboard */
    char camera[192];           /* "": found by the driver; "off"; or a URL */
} p3d_cfg_t;

typedef struct {
    p3d_cfg_t m[P3D_MAX];
    int       count;
} p3d_cfgs_t;

const char *p3d_cfg_path(void);
uint32_t    p3d_cfg_stamp(void);
void        p3d_cfg_load(p3d_cfgs_t *c);
bool        p3d_cfg_save(const p3d_cfgs_t *c);
void        p3d_cfg_defaults(p3d_cfg_t *m, int kind);
int         p3d_cfg_port(const p3d_cfg_t *m);       /* the port actually used */

/* ---- what a machine is doing ------------------------------------------- */

typedef enum {
    P3D_ST_OFFLINE = 0,         /* no answer */
    P3D_ST_CONNECTING,
    P3D_ST_IDLE,                /* ready */
    P3D_ST_BUSY,                /* heating, homing, calibrating, preparing */
    P3D_ST_PRINTING,
    P3D_ST_PAUSED,
    P3D_ST_DONE,
    P3D_ST_CANCELLED,
    P3D_ST_ERROR,
    P3D_ST_N
} p3d_state_t;

/* what a driver can do; the detail screen shows only these */
enum {
    P3D_CAP_PAUSE      = 1u << 0,
    P3D_CAP_CANCEL     = 1u << 1,
    P3D_CAP_TEMP       = 1u << 2,   /* set targets */
    P3D_CAP_JOG        = 1u << 3,
    P3D_CAP_HOME       = 1u << 4,
    P3D_CAP_EXTRUDE    = 1u << 5,
    P3D_CAP_FAN        = 1u << 6,
    P3D_CAP_SPEED      = 1u << 7,   /* a percentage */
    P3D_CAP_SPEED_LVL  = 1u << 8,   /* Bambu's four levels */
    P3D_CAP_FLOW       = 1u << 9,
    P3D_CAP_GCODE      = 1u << 10,
    P3D_CAP_FILES      = 1u << 11,
    P3D_CAP_LIGHT      = 1u << 12,
    P3D_CAP_CONNECT    = 1u << 13,  /* OctoPrint's serial link */
    P3D_CAP_ESTOP      = 1u << 14,
    P3D_CAP_MOTORS_OFF = 1u << 15,
    P3D_CAP_REPLIES    = 1u << 16,  /* the console sees answers */
    P3D_CAP_DELETE     = 1u << 17,
    P3D_CAP_CHAMBER    = 1u << 18,  /* the chamber's target can be set */
};

typedef struct {
    float cur, target;
    bool  on;                   /* the machine has it */
} p3d_temp_t;

#define P3D_TRAYS 8
typedef struct {
    uint32_t rgb;
    char     type[12];
    int8_t   remain;            /* %, -1 unknown */
    bool     on;                /* a spool is there */
} p3d_tray_t;

typedef struct {
    int        state;           /* p3d_state_t */
    char       raw[32];         /* the state in the machine's words */
    char       msg[128];        /* why it is offline, an error, a message */
    p3d_temp_t tool[2];
    p3d_temp_t bed, chamber;
    float      progress;        /* 0-100, < 0 unknown */
    int32_t    elapsed_s;       /* < 0 unknown */
    int32_t    remaining_s;
    char       file[128];
    int        layer, layers;   /* 0 unknown */
    int        fan;             /* part fan %, -1 unknown */
    int        speed;           /* %, -1 */
    int        speed_lvl;       /* Bambu 1-4, 0 none */
    int        flow;            /* %, -1 */
    float      pos[3];
    bool       has_pos;
    int        light;           /* -1 none, 0 off, 1 on */
    char       model[48];
    char       fw[48];
    char       cam_url[192];    /* the camera the driver found */
    uint32_t   thumb_gen;       /* the job's picture changed (p3d_thumb_take) */
    p3d_tray_t tray[P3D_TRAYS];
    int        ntrays;
    int        tray_now;        /* -1 none */
    uint32_t   caps;
    uint32_t   updated_ms;
} p3d_status_t;

/* ---- files and the console --------------------------------------------- */

typedef struct {
    char     name[96];          /* what is shown */
    char     path[192];         /* what the machine wants back */
    uint32_t size;
    uint32_t date;              /* epoch, 0 unknown */
    int32_t  est_s;             /* the slicer's estimate, < 0 unknown */
} p3d_file_t;

#define P3D_FILES_MAX  150
#define P3D_CON_LINES  80
#define P3D_CON_LEN    100
#define P3D_HIST       240      /* temperature samples kept */
#define P3D_HIST_MS    5000

/* ---- commands from the UI ---------------------------------------------- */

typedef enum {
    P3D_C_NONE = 0,
    P3D_C_PAUSE,
    P3D_C_RESUME,
    P3D_C_CANCEL,
    P3D_C_TEMP,                 /* a: 0 tool0, 1 tool1, 2 bed, 3 chamber; v[0] */
    P3D_C_JOG,                  /* v[0..2] mm, relative */
    P3D_C_HOME,                 /* a: bits 1 X, 2 Y, 4 Z */
    P3D_C_EXTRUDE,              /* v[0] mm, < 0 retracts */
    P3D_C_FAN,                  /* v[0] 0-100 */
    P3D_C_SPEED,                /* v[0] % */
    P3D_C_SPEED_LVL,            /* a 1-4 */
    P3D_C_FLOW,                 /* v[0] % */
    P3D_C_GCODE,                /* text, lines split by '\n' */
    P3D_C_START,                /* text: a file's path */
    P3D_C_DELETE,               /* text: a file's path */
    P3D_C_FILES,                /* read the list again */
    P3D_C_LIGHT,                /* a 0/1 */
    P3D_C_CONNECT,              /* a 0/1 */
    P3D_C_ESTOP,
    P3D_C_MOTORS_OFF,
} p3d_op_t;

typedef struct {
    int   op;
    int   a;
    float v[3];
    char  text[200];
} p3d_cmd_t;

/* ---- a machine and its thread (p3d_core.c) ----------------------------- */

typedef struct p3d_machine p3d_machine_t;

typedef struct {
    const char *word;           /* the type in impresoras.txt */
    const char *label;          /* what the editor shows */
    const char *makers;         /* which machines speak it (a hint, N_()) */
    const char *key_label;      /* what the key field is called (N_()) */
    const char *key_hint;       /* where to find it (N_()) */
    int         port, tls_port;
    uint32_t    fields;         /* P3D_F_* */
    void      (*run)(p3d_machine_t *m);
} p3d_driver_t;

extern const p3d_driver_t p3d_drv_octoprint, p3d_drv_prusalink, p3d_drv_bambu, p3d_drv_moonraker,
                          p3d_drv_duet, p3d_drv_elegoo, p3d_drv_flashforge, p3d_drv_creality;
const p3d_driver_t *p3d_driver(int kind);
int                 p3d_kind_from_id(const char *id);      /* -1 unknown */

struct p3d_machine {
    int            index;
    p3d_cfg_t      cfg;         /* the thread's own copy */
    p3d_status_t   st;          /* under p3d_lock() */
    uint32_t       gen;
    int16_t        hist[2][P3D_HIST];   /* tool and bed, tenths of a degree */
    int            hist_n, hist_head;
    uint64_t       hist_ms;
    p3d_file_t    *files;
    int            nfiles;
    uint32_t       files_gen;
    bool           files_busy;
    char           files_err[96];
    uint16_t      *thumb_px;    /* the job's picture, decoded */
    int            thumb_w, thumb_h;
    char           con[P3D_CON_LINES][P3D_CON_LEN];
    int            con_n, con_head;
    uint32_t       con_gen;
    p3d_cmd_t      q[8];
    int            qh, qt;
    volatile bool  stop;
    volatile bool  done;        /* the thread has returned */
    volatile int   poll_ms;     /* how often the UI wants news */
    volatile bool  want_console;
    volatile bool  wake;
    void          *drv;         /* the driver's own */
};

void p3d_core_init(void);
/* Starts a thread for each machine of cfg; stops those that went. */
void p3d_core_sync(const p3d_cfgs_t *cfg);
/* Stops every thread and waits for them, up to max_ms. */
void p3d_core_stop_all(int max_ms);
int  p3d_core_count(void);
p3d_machine_t *p3d_core_at(int i);

void p3d_lock(void);
void p3d_unlock(void);

/* the UI's side */
bool p3d_snapshot(int i, p3d_status_t *out, uint32_t *gen);
bool p3d_send(int i, const p3d_cmd_t *c);
void p3d_set_poll(int ms_detail, int detail_index, int ms_others);
void p3d_want_console(int i, bool on);
/* history copy, oldest first: returns how many */
int  p3d_history(int i, int16_t *tool, int16_t *bed, int max);
/* the console, oldest first, joined by '\n' */
int  p3d_console(int i, char *out, size_t n, uint32_t *gen);
/* the files: copies up to max, returns how many; *busy while reading */
int  p3d_files(int i, p3d_file_t *out, int max, uint32_t *gen, bool *busy, char *err, size_t errn);

/* the driver's side */
bool p3d_wait(p3d_machine_t *m, int ms);            /* false: stop */
bool p3d_take(p3d_machine_t *m, p3d_cmd_t *c);
void p3d_publish(p3d_machine_t *m);                 /* after changing st, with the lock held */
void p3d_offline(p3d_machine_t *m, const char *why); /* takes the lock */
void p3d_con_add(p3d_machine_t *m, const char *prefix, const char *text);
void p3d_files_set(p3d_machine_t *m, p3d_file_t *f, int n, const char *err);    /* takes f */
void p3d_files_begin(p3d_machine_t *m);
/* the job's picture (PNG or JPEG bytes), saved and decoded in the machine's
 * thread; NULL takes it away */
void p3d_thumb_store(p3d_machine_t *m, const void *data, int len);
bool p3d_thumb_take(int i, uint16_t **px, int *w, int *h, uint32_t *gen);
/* the next poll's wait, longer while the machine does not answer */
int  p3d_backoff(p3d_machine_t *m, int fails);
void p3d_status_reset(p3d_status_t *s);

/* G-code helpers shared by the drivers that take G-code */
int  p3d_gcode_of(const p3d_cmd_t *c, char *out, size_t n);   /* 0: not a G-code command */

/* ---- HTTP and sockets (p3d_net.c) -------------------------------------- */

#define P3D_ERR_CONNECT (-2)
#define P3D_ERR_TLS     (-8)

typedef struct {
    int   status;               /* HTTP code, or < 0 */
    char *body;                 /* NUL-terminated, PSRAM, p3d_resp_free() */
    int   len;
    bool  cut;                  /* longer than max */
    char  ctype[64];
    char  err[96];
} p3d_resp_t;

typedef struct {
    char     host[96];
    int      port;
    bool     tls;
    char     user[32];
    char     pass[128];
    bool     digest_only;       /* never send Basic */
    char     extra[256];        /* whole header lines */
    /* Digest, learnt from the last 401 */
    char     realm[64], nonce[128], opaque[96];
    bool     qop, have_digest;
    uint32_t nc;
    /* the connection kept open */
    void    *c;
    uint64_t used_ms;
    int      conn_ms;           /* connect / handshake timeout */
    const volatile bool *stop;  /* the machine's: a request gives up when set */
} p3d_http_t;

void p3d_http_init(p3d_http_t *h, const char *host, int port, bool tls);
/* One request; reuses the open connection and retries once on a fresh one.
 * path may be a whole URL to another host (a camera). */
int  p3d_http(p3d_http_t *h, const char *method, const char *path, const char *body, int body_len,
              const char *ctype, p3d_resp_t *r, int max, int timeout_ms);
void p3d_http_close(p3d_http_t *h);
void p3d_resp_free(p3d_resp_t *r);
/* a person's line for a failed request: "no responde", "clave incorrecta"... */
const char *p3d_http_why(const p3d_resp_t *r);

/* a device that signs its own certificates: the authorities to trust and the
 * name its certificate carries (aos_tls_open_trust, p3d_trust.c) */
typedef struct {
    const char *pem;
    size_t      len;
    const char *name;
} p3d_trust_t;
bool  p3d_bambu_trust(const char *serial, p3d_trust_t *out);
void  p3d_trust_free(void);

void *p3d_sock_open(const char *host, int port, bool tls, int timeout_ms, char *why, size_t n);
/* always TLS, verified against trust's authorities and name only */
void *p3d_sock_open_trust(const char *host, int port, const p3d_trust_t *trust, int timeout_ms, char *why, size_t n);
bool  p3d_sock_send(void *s, const void *b, int n, int timeout_ms);
int   p3d_sock_recv(void *s, void *b, int max, int timeout_ms);  /* > 0, 0 nothing yet, -1 gone */
void  p3d_sock_close(void *s);

bool  p3d_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls);
size_t p3d_url_enc(char *out, size_t n, const char *in);         /* for a query or a path */
size_t p3d_json_esc(char *out, size_t n, const char *in);
size_t p3d_b64(char *out, size_t n, const void *in, size_t len);
bool  p3d_save_file(const char *path, const void *data, int len);

/* ---- JSON (p3d_json.c) ------------------------------------------------- */

/* path: "a.b.0.c" (array elements by number). NULL: not there. */
const char *pj_get(const char *js, const char *path);
const char *pj_skip(const char *p);
bool pj_num(const char *js, const char *path, float *out);  /* also "12.5" and true/false */
bool pj_int(const char *js, const char *path, int *out);
bool pj_str(const char *js, const char *path, char *out, size_t n);
bool pj_bool(const char *js, const char *path, bool *out);
bool pj_null(const char *v);
/* calls cb for each member (key, klen) or element (key NULL) of the value v */
typedef bool (*pj_cb_t)(const char *key, int klen, const char *val, void *ctx);
bool pj_each(const char *v, pj_cb_t cb, void *ctx);
/* the value v as a string (it must be one), unescaped */
bool pj_strv(const char *v, char *out, size_t n);
bool pj_numv(const char *v, float *out);

/* ---- MQTT (p3d_mqtt.c) ------------------------------------------------- */

typedef struct p3d_mqtt p3d_mqtt_t;
/* trust NULL: plain TCP, or the HAL's store with tls */
p3d_mqtt_t *p3d_mqtt_open(const char *host, int port, bool tls, const p3d_trust_t *trust, const char *client,
                          const char *user, const char *pass, int timeout_ms, char *why, size_t n);
bool p3d_mqtt_sub(p3d_mqtt_t *q, const char *topic);
bool p3d_mqtt_pub(p3d_mqtt_t *q, const char *topic, const char *payload, int len);
/* the next PUBLISH: its length (> 0) with *topic and *payload pointing at
 * them (NUL-terminated, valid until the next call), 0 none in timeout_ms,
 * -1 the connection is gone. Pings go out inside. */
int  p3d_mqtt_poll(p3d_mqtt_t *q, const char **topic, const char **payload, int timeout_ms);
void p3d_mqtt_close(p3d_mqtt_t *q);

/* ---- the camera (p3d_cam.c) -------------------------------------------- */

typedef struct p3d_cam p3d_cam_t;
/* url: http(s)://... a snapshot or an MJPEG stream; "bambu:" for the P1 and
 * A1's port 6000. Frames are scaled down to fit max_w x max_h. */
p3d_cam_t *p3d_cam_start(const char *url, const p3d_cfg_t *cfg, int max_w, int max_h);
void       p3d_cam_stop(p3d_cam_t *c);              /* waits for the thread, up to a moment */
/* copies the newest frame into *px (reallocated to fit) when it is newer
 * than *gen: true then */
bool       p3d_cam_take(p3d_cam_t *c, uint16_t **px, int *w, int *h, uint32_t *gen);
void       p3d_cam_status(p3d_cam_t *c, char *out, size_t n, float *fps);
/* the camera's own size and what it sends a second (the last window) */
void       p3d_cam_info(p3d_cam_t *c, int *src_w, int *src_h, uint32_t *kbps);
/* the URL a machine's camera is at, "" none */
void       p3d_cam_url(const p3d_cfg_t *cfg, const p3d_status_t *st, char *out, size_t n);
int        p3d_cam_threads(void);                   /* still running */

/* ---- UI pieces (p3d_ui.c) ---------------------------------------------- */

lv_obj_t   *p3d_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t   *p3d_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud);
lv_obj_t   *p3d_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t   *p3d_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t   *p3d_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb,
                    void *ud);
void        p3d_set_text(lv_obj_t *l, const char *t);
const char *p3d_state_name(int st);
lv_color_t  p3d_state_color(int st);
void        p3d_fmt_dur(char *out, size_t n, int32_t s);           /* "1 h 05 min" */
void        p3d_fmt_eta(char *out, size_t n, int32_t remaining_s); /* "18:40", "" without a clock */
void        p3d_fmt_temp(char *out, size_t n, const p3d_temp_t *t);
void        p3d_fmt_pct(char *out, size_t n, float progress);      /* "0,4%" under 10, "37%" above */
bool        p3d_active(int st);                                   /* printing or paused or busy */
