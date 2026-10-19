/*
 * P4OS - The sensor service: the table of sensors, autodetect, and the
 * thread that reads them (aos_sensors.h, docs/MODULES.md, docs/SENSORS.md).
 * The drivers are in sensors/ (sens_int.h says how they plug in).
 *
 * Platform-free: everything goes through aos_io_*, so the same code reads a
 * real BME280 on the board and sim/sensors_sim.c's in the simulator.
 *
 * One thread, one round a second:
 *   1. the sensors modules.txt declares, merged with the ones autodetect
 *      found, make the table (one slot per port + address, or per 1-Wire
 *      ROM);
 *   2. each 1-Wire bus starts one conversion for all its sensors;
 *   3. per I2C port with something to do: open it (claiming its pins as
 *      "Sensores"), autodetect when it is due, read every slot on it, and
 *      close it again - the Bus app can have the port between two rounds;
 *   4. the SPI, GPIO and UART sensors, each port opened for its read;
 *   5. the 1-Wire buses are read, their conversions done by now;
 *   6. the values go to the table under the lock, with a five-minute
 *      history per value.
 *
 * Autodetect only reads registers and sends the read-only commands a chip
 * answers with its id or a CRC; it never configures a chip it has not
 * identified first - a declared one, the user has said is there - and it
 * never asks a sensor that is already reading who it is again.
 */
#include "sensors/sens_int.h"

#define scpy        sens_scpy
#define OWNER       AOS_IO_OWNER_SENSORS
#define ROUND_MS    1000
#define SCAN_FAST   5000        /* autodetect while somebody looks */
#define SCAN_SLOW   60000
#define KEEP_MS     10000
#define SLOW_HZ     100000      /* SF_SLOW chips */
#define SLOW_WAIT   200000      /* and the clock stretching they may hold, µs */

typedef sens_slot_t slot_t;

const char *aos_sensor_unit(int q)
{
    static const char *const U[AOS_SQ_COUNT] = {
        "°C", "%", "hPa", "lx", "V", "A", "W",
        "ppm", "ppb", "", "µg/m³", "UVI", "mm", "g", "°/s", "µT", "°", "%", "Ω", "", "Hz",
        "L/min", "L", "m/s", "mm", "µSv/h", "Wh", "g", "", "bpm",
    };
    return q >= 0 && q < AOS_SQ_COUNT ? U[q] : "";
}

/* -------------------------------------------------------------------------- */
/* The table                                                                   */
/* -------------------------------------------------------------------------- */

typedef aos_sensor_port_t port_st_t;

/* ~30 KB with 32 slots: PSRAM (nothing here runs from an interrupt) */
static AOS_BSS_PSRAM slot_t s_slot[AOS_SENSOR_MAX];
static AOS_BSS_PSRAM aos_sensor_cand_t s_cand[AOS_SENSOR_CAND_MAX];
static int s_ncand;
static port_st_t s_pst[AOS_IO_PORT_MAX];
static int s_npst;
static void *s_mx;
static int s_started, s_ready;
static volatile bool s_rescan;
static volatile uint32_t s_seq, s_keep_ms;

static void lock(void) { aos_hal_mutex_lock(s_mx); }
static void unlock(void) { aos_hal_mutex_unlock(s_mx); }

static const sens_drv_t *drv_of(int chip) { return chip >= 0 && chip < SENS_NDRV ? SENS_DRV[chip] : NULL; }
static uint8_t bus_of(const slot_t *s) { const sens_drv_t *d = drv_of(s->chip); return d ? d->pub.bus : 0; }

static slot_t *slot_find(const char *port, uint8_t addr)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++)
        if (s_slot[i].used && !s_slot[i].rom && s_slot[i].pub.addr == addr && !strcmp(s_slot[i].pub.port, port)) return &s_slot[i];
    return NULL;
}

/* A slot's driver lets go of what it holds, with the lock held: a counter,
 * an interrupt (fini), and a GPIO sensor's pins that no other slot reads */
static void slot_release(slot_t *s)
{
    const sens_drv_t *d = s->chip >= 0 && s->chip < SENS_NDRV ? SENS_DRV[s->chip] : NULL;
    if (d && d->fini && s->inited) d->fini(s);
    s->inited = false;
    for (int k = 0; k < 2; k++) {
        int g = s->gpio[k];
        if (g < 0) continue;
        bool other = false;
        for (int i = 0; i < AOS_SENSOR_MAX; i++)
            other |= &s_slot[i] != s && s_slot[i].used && (s_slot[i].gpio[0] == g || s_slot[i].gpio[1] == g);
        const char *o = aos_io_owner(g);
        if (!other && o && !strcmp(o, OWNER)) aos_io_release(g, OWNER);
        s->gpio[k] = -1;
    }
}

static void slot_drop(slot_t *s)
{
    slot_release(s);
    s->used = false;
}

/* A new slot, called with the lock held */
static slot_t *slot_add(const char *port, uint8_t addr, int chip)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (s->used) continue;
        float *h = s->hist;
        memset(s, 0, sizeof *s);
        /* 14 KB: malloc puts it in PSRAM (anything over 1 KB goes there) */
        s->hist = h ? h : malloc(sizeof(float) * AOS_SENSOR_XVALUES * AOS_SENSOR_HIST);
        if (!s->hist) return NULL;
        s->used = true;
        s->chip = chip;
        s->gpio[0] = s->gpio[1] = -1;
        s->gain_mv = 4096;
        s->shunt_mohm = 100;
        scpy(s->pub.port, sizeof s->pub.port, port);
        s->pub.addr = addr;
        scpy(s->pub.chip, sizeof s->pub.chip, SENS_DRV[chip]->pub.name);
        return s;
    }
    return NULL;
}

/* The first four values are what aos_sensor_t carries */
static void publish(slot_t *s)
{
    s->pub.n = s->xn < AOS_SENSOR_VALUES ? s->xn : AOS_SENSOR_VALUES;
    memcpy(s->pub.v, s->xv, sizeof s->pub.v);
}

static void hist_push(slot_t *s, bool ok)
{
    for (int k = 0; k < AOS_SENSOR_XVALUES; k++)
        s->hist[k * AOS_SENSOR_HIST + s->h_pos] = ok && k < s->xn ? s->xv[k].v : NAN;
    s->h_pos = (s->h_pos + 1) % AOS_SENSOR_HIST;
    if (s->h_n < AOS_SENSOR_HIST) s->h_n++;
}

static void arg_str(const char *args, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = args; p && *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t l = strcspn(p, " \t#");
            if (l >= n) l = n - 1;
            memcpy(out, p, l);
            out[l] = 0;
            return;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
}

static int module_chip(const aos_io_module_t *mod)
{
    char cs[24];
    arg_str(mod->args, "chip", cs, sizeof cs);
    return sens_drv_find(cs[0] ? cs : mod->name);
}

/* ---- 1-Wire buses ----
 * A GPIO port with a 1-Wire sensor's module line on it. Found again at every
 * sync from modules.txt; its open handle lives for one round only. */
#define OW_BUSES 4
#define OW_DEVS  16

typedef struct {
    char         port[AOS_IO_PORT_NAME_MAX];
    int          gpio;
    char         base[24];          /* the name of the sensors nobody named */
    bool         seen;              /* still in modules.txt at this sync */
    bool         scanned;
    uint32_t     scan_ms;
    aos_io_ow_t *b;                 /* open from ow_begin() to ow_finish() */
    uint32_t     conv_at;
    int          conv_ms;
} ow_bus_t;

static ow_bus_t s_ow[OW_BUSES];
static int s_now;
static int s_ds_chip = -1;          /* the DS18B20 driver's index */

/* The low 56 bits: the family and the serial, without the CRC byte */
#define ROM56(r) ((r) & 0x00FFFFFFFFFFFFFFull)

static slot_t *slot_find_rom(const char *port, uint64_t rom)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++)
        if (s_slot[i].used && s_slot[i].rom && ROM56(s_slot[i].rom) == ROM56(rom) && !strcmp(s_slot[i].pub.port, port))
            return &s_slot[i];
    return NULL;
}

/* "28-0123456789AB", as the Bus app shows them: the family, then the serial
 * most significant byte first. The CRC byte is computed. 0: not one. */
static uint64_t rom_parse(const char *t)
{
    if (!t || !*t) return 0;
    char *e;
    unsigned long fam = strtoul(t, &e, 16);
    if (*e != '-' || fam > 0xFF) return 0;
    const char *sp = e + 1;
    unsigned long long ser = strtoull(sp, &e, 16);
    if (e - sp != 12 || *e) return 0;
    uint8_t b[8];
    b[0] = (uint8_t)fam;
    for (int i = 0; i < 6; i++) b[1 + i] = (uint8_t)(ser >> (8 * i));
    b[7] = aos_io_ow_crc8(b, 7);
    uint64_t rom = 0;
    for (int i = 7; i >= 0; i--) rom = rom << 8 | b[i];
    return rom;
}

static bool ow_temp_family(uint8_t f) { return f == 0x28 || f == 0x10 || f == 0x22 || f == 0x3B; }

static ow_bus_t *ow_bus_of(const char *port, int gpio)
{
    for (int i = 0; i < s_now; i++)
        if (!strcmp(s_ow[i].port, port)) {
            if (s_ow[i].gpio != gpio) { s_ow[i].gpio = gpio; s_ow[i].scanned = false; }
            s_ow[i].seen = true;
            return &s_ow[i];
        }
    if (s_now >= OW_BUSES) return NULL;
    ow_bus_t *b = &s_ow[s_now++];
    memset(b, 0, sizeof *b);
    scpy(b->port, sizeof b->port, port);
    b->gpio = gpio;
    scpy(b->base, sizeof b->base, "ds18b20");
    b->seen = true;
    return b;
}

/* The board's own chips on i2c.board: the touch (0x14/0x5D), the codecs
 * (0x18 ES8311, 0x40 ES7210), and 0x36 */
static bool board_chip(uint8_t a) { return a == 0x14 || a == 0x5D || a == 0x18 || a == 0x40 || a == 0x36; }

/* A declared module's settings into its slot */
static void slot_settings(slot_t *s, const aos_io_module_t *mod)
{
    scpy(s->args, sizeof s->args, mod->args);
    int32_t sh = aos_io_arg_int(mod->args, "shunt", 100), g = aos_io_arg_int(mod->args, "gain", 4096);
    if (sh != s->shunt_mohm || g != s->gain_mv) { s->shunt_mohm = sh; s->gain_mv = g; s->inited = false; }
}

/* modules.txt into the table: declared slots, with their settings */
static void sync_declared(void)
{
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX; i++) s_slot[i].declared_now = false;
    for (int i = 0; i < s_now; i++) { s_ow[i].seen = false; scpy(s_ow[i].base, sizeof s_ow[i].base, "ds18b20"); }
    for (int m = 0; m < aos_io_module_count(); m++) {
        const aos_io_module_t *mod = aos_io_module_at(m);
        int chip = module_chip(mod);
        if (chip < 0) continue;
        const sens_drv_t *d = SENS_DRV[chip];
        const aos_io_port_t *pp = aos_io_port_find(mod->port);
        if (!pp) continue;
        uint8_t addr;
        switch (d->pub.bus) {
        case AOS_SENSOR_BUS_ONEWIRE: {
            /* the line makes its GPIO port a bus; with rom= it names one sensor */
            if (pp->kind != AOS_PORT_GPIO || pp->pins[0] < 0) continue;
            ow_bus_t *bus = ow_bus_of(mod->port, pp->pins[0]);
            char rs[24];
            arg_str(mod->args, "rom", rs, sizeof rs);
            uint64_t rom = rom_parse(rs);
            if (!rom) {
                if (bus) scpy(bus->base, sizeof bus->base, mod->name);
                continue;
            }
            slot_t *s = slot_find_rom(mod->port, rom);
            if (!s) s = slot_add(mod->port, (uint8_t)pp->pins[0], chip);
            if (!s) continue;
            if (!s->rom) s->rom = rom;
            s->declared_now = true;
            s->pub.declared = true;
            scpy(s->pub.name, sizeof s->pub.name, mod->name);
            const char *fam = aos_io_ow_family((uint8_t)rom);
            if (fam) scpy(s->pub.chip, sizeof s->pub.chip, fam);
            continue;
        }
        case AOS_SENSOR_BUS_SPI: {
            /* the chip select tells two of them apart on the same wires */
            if (pp->kind != AOS_PORT_SPI) continue;
            int cs = aos_io_module_gpio(mod->name, "cs");
            if (cs < 0) cs = pp->pins[3];
            if (cs < 0) continue;
            addr = (uint8_t)cs;
            break;
        }
        case AOS_SENSOR_BUS_GPIO:
            if (pp->kind != AOS_PORT_GPIO || pp->pins[0] < 0) continue;
            addr = (uint8_t)pp->pins[0];
            break;
        case AOS_SENSOR_BUS_UART:
            if (pp->kind != AOS_PORT_UART) continue;
            addr = 0;
            break;
        default:
            if (pp->kind != AOS_PORT_I2C) continue;
            addr = (uint8_t)aos_io_arg_int(mod->args, "addr", d->pub.addrs[0]);
            break;
        }
        slot_t *s = slot_find(mod->port, addr);
        if (s && s->chip != chip && strcmp(SENS_DRV[s->chip]->pub.id, d->pub.id)) {
            /* the user says it is something else than autodetect thought */
            slot_release(s);
            s->chip = chip;
            s->inited = false;
            s->xn = s->pub.n = 0;
            scpy(s->pub.chip, sizeof s->pub.chip, d->pub.name);
        }
        if (!s) s = slot_add(mod->port, addr, chip);
        if (!s) continue;
        s->declared_now = true;
        s->pub.declared = true;
        scpy(s->pub.name, sizeof s->pub.name, mod->name);
        slot_settings(s, mod);
    }
    /* a bus that left modules.txt: what was only found on it goes */
    int w = 0;
    for (int i = 0; i < s_now; i++) {
        if (s_ow[i].seen) { if (w != i) s_ow[w] = s_ow[i]; w++; continue; }
        for (int k = 0; k < AOS_SENSOR_MAX; k++)
            if (s_slot[k].used && s_slot[k].rom && !strcmp(s_slot[k].pub.port, s_ow[i].port)) s_slot[k].detected = false;
    }
    s_now = w;
    /* what is neither declared nor detected any more goes */
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (!s->used) continue;
        if (!s->declared_now) s->pub.declared = false;
        if (!s->declared_now && !s->detected) slot_drop(s);
    }
    unlock();
}

/* A detected chip's name: the chip id, or id_<addr> when there are two */
static void detected_name(slot_t *s)
{
    const char *id = SENS_DRV[s->chip]->pub.id;
    bool dup = false;
    for (int i = 0; i < AOS_SENSOR_MAX; i++)
        if (s_slot[i].used && &s_slot[i] != s && !strcmp(s_slot[i].pub.name, id)) dup = true;
    if (dup) snprintf(s->pub.name, sizeof s->pub.name, "%s_%02x", id, s->pub.addr);
    else scpy(s->pub.name, sizeof s->pub.name, id);
}

/* ---- autodetect ---- */

static bool drv_probes(const sens_drv_t *d, uint8_t a)
{
    if (d->pub.bus != AOS_SENSOR_BUS_I2C || (d->flags & SF_NOPROBE) || !d->identify) return false;
    const uint8_t *list = d->nprobe ? d->probe : d->pub.addrs;
    int n = d->nprobe ? d->nprobe : d->pub.naddr;
    for (int i = 0; i < n; i++) if (list[i] == a) return true;
    return false;
}

/* Every address some driver looks at, once, in order */
static int probe_list(uint8_t *out, int max)
{
    bool want[128] = { 0 };
    for (int k = 0; k < SENS_NDRV; k++)
        for (int a = 0x08; a < 0x78; a++) if (drv_probes(SENS_DRV[k], (uint8_t)a)) want[a] = true;
    int n = 0;
    for (int a = 0; a < 128 && n < max; a++) if (want[a]) out[n++] = (uint8_t)a;
    return n;
}

/* What is at this address: a driver index, or -2 - index for a candidate,
 * -1 nothing known. The table's order decides: the first that proves
 * itself wins; the first candidate counts only if nobody proves anything. */
#define CAND(c) (-2 - (c))
static int identify(aos_io_i2c_t *b, uint8_t a)
{
    int cand = -1;
    for (int k = 0; k < SENS_NDRV; k++) {
        const sens_drv_t *d = SENS_DRV[k];
        if (!drv_probes(d, a)) continue;
        if (d->flags & SF_SLOW) aos_io_i2c_dev_cfg(b, a, SLOW_HZ, SLOW_WAIT);
        int r = d->identify(b, a);
        if (r == ID_YES) return k;
        if (r == ID_MAYBE && cand < 0) cand = k;
    }
    return cand >= 0 ? CAND(cand) : -1;
}

static void detect(aos_io_i2c_t *b, const char *port)
{
    uint8_t addrs[96];
    int na = probe_list(addrs, (int)sizeof addrs);
    int found[96];
    for (int i = 0; i < na; i++) {
        found[i] = -1;
        uint8_t a = addrs[i];
        lock();
        slot_t *s = slot_find(port, a);
        bool declared = s && s->pub.declared;
        bool reading = s && s->detected && s->pub.state == AOS_SENSOR_OK;
        int chip = s ? s->chip : -1;
        unlock();
        if (declared) { found[i] = -100; continue; }            /* the user's: leave it be */
        /* an AM2320 asleep (3 s after its last word) NACKs the call that
         * wakes it: at its address, a second call 2 ms later */
        if (!aos_io_i2c_probe(b, a) && (a != 0x5C || (aos_hal_sleep_ms(2), !aos_io_i2c_probe(b, a)))) continue;
        if (reading) { found[i] = chip; continue; }             /* known and reading: not asked again */
        /* known but failing (out of range, a bad CRC): its own driver asks
         * first, so the other drivers' probes do not poke a chip that is
         * set up (a VL53L1X probe would undo a VL53L0X's sequence) */
        const sens_drv_t *own = drv_of(chip);
        if (own && own->identify && own->identify(b, a) == ID_YES) { found[i] = chip; continue; }
        found[i] = identify(b, a);
    }
    lock();
    /* candidates of this port are replaced by this pass's */
    int w = 0;
    for (int i = 0; i < s_ncand; i++) if (strcmp(s_cand[i].port, port)) s_cand[w++] = s_cand[i];
    s_ncand = w;
    for (int i = 0; i < na; i++) {
        uint8_t a = addrs[i];
        slot_t *s = slot_find(port, a);
        if (found[i] == -100) continue;
        if (found[i] >= 0) {
            if (s && s->chip != found[i]) { slot_drop(s); s = NULL; }
            if (!s) {
                s = slot_add(port, a, found[i]);
                if (s) detected_name(s);
            }
            if (s) s->detected = true;
            continue;
        }
        if (s && !s->pub.declared) slot_drop(s);                      /* gone */
        if (found[i] <= -2 && s_ncand < AOS_SENSOR_CAND_MAX) {
            aos_sensor_cand_t *c = &s_cand[s_ncand++];
            scpy(c->port, sizeof c->port, port);
            c->addr = a;
            scpy(c->chip, sizeof c->chip, SENS_DRV[-2 - found[i]]->pub.id);
            scpy(c->guess, sizeof c->guess, aos_io_i2c_guess(a, false));
        }
    }
    unlock();
}

/* -------------------------------------------------------------------------- */
/* Reading                                                                     */
/* -------------------------------------------------------------------------- */

static port_st_t *pst_of(const char *port)
{
    for (int i = 0; i < s_npst; i++) if (!strcmp(s_pst[i].port, port)) return &s_pst[i];
    if (s_npst >= AOS_IO_PORT_MAX) return NULL;
    port_st_t *p = &s_pst[s_npst++];
    memset(p, 0, sizeof *p);
    scpy(p->port, sizeof p->port, port);
    return p;
}

/* A driver's result into the table: the values whole, or why not */
static void commit(slot_t *s, const slot_t *tmpp, int r)
{
    lock();
    if (!s->used || strcmp(s->pub.port, tmpp->pub.port) || s->pub.addr != tmpp->pub.addr || s->rom != tmpp->rom) { unlock(); return; }
    s->inited = tmpp->inited;
    s->init_ms = tmpp->init_ms;
    s->read_ms = tmpp->read_ms;
    memcpy(&s->priv, &tmpp->priv, sizeof s->priv);
    memcpy(s->pub.chip, tmpp->pub.chip, sizeof s->pub.chip);
    s->pub.reads++;
    if (r == R_OK) {
        s->pub.state = AOS_SENSOR_OK;
        s->pub.err[0] = 0;
        s->xn = tmpp->xn;
        memcpy(s->xv, tmpp->xv, sizeof s->xv);
        publish(s);
        s->pub.t_ms = sens_now();
        hist_push(s, true);
    } else {
        static const char *const WHY[] = { "", "nack", "crc", "id", "notready", "open", "range", "cfg" };
        s->pub.state = r == R_NOTREADY && s->pub.state != AOS_SENSOR_OK ? AOS_SENSOR_WAIT : AOS_SENSOR_ERR;
        /* err is "<why>" or "<why>:<detail>" (the id the chip gave) */
        if (tmpp->pub.err[0]) snprintf(s->pub.err, sizeof s->pub.err, "%s:%.36s", WHY[r], tmpp->pub.err);
        else scpy(s->pub.err, sizeof s->pub.err, WHY[r]);
        if (r != R_NOTREADY) s->pub.errors++;
        /* a chip that stopped answering may have lost power: start it again */
        if (r == R_NACK || r == R_ID) s->inited = false;
        hist_push(s, false);
    }
    unlock();
}

/* Whether this round reads the slot, or keeps its last values (min_ms) */
static bool due(const slot_t *s, const sens_drv_t *d)
{
    if (!d->min_ms || !s->inited || (d->flags & SF_KEEPREAD)) return true;
    return sens_now() - s->read_ms >= d->min_ms;
}

/* Not due: the history goes on with the values it has */
static void keep(slot_t *s)
{
    lock();
    if (s->used) hist_push(s, s->pub.state == AOS_SENSOR_OK);
    unlock();
}

/* init() once, then read(), on a private copy; commit() takes the result */
static void run_driver(slot_t *s, sens_io_t *io)
{
    lock();
    bool mine = s->used;
    slot_t tmp = *s;
    unlock();
    if (!mine) return;
    const sens_drv_t *d = drv_of(tmp.chip);
    if (!d || !d->read) return;
    if (!due(&tmp, d)) { keep(s); return; }
    tmp.pub.err[0] = 0;
    tmp.xn = 0;
    int r = R_OK;
    if (!tmp.inited) {
        if (d->init) r = d->init(io, &tmp);
        if (r == R_OK) { tmp.inited = true; tmp.init_ms = sens_now(); }
    }
    if (r == R_OK) {
        tmp.read_ms = sens_now();
        r = d->read(io, &tmp);
    }
    commit(s, &tmp, r);
}

static void read_slot_i2c(aos_io_i2c_t *b, slot_t *s)
{
    /* a declared chip on the board's bus where the board has one of its own */
    if (!strcmp(s->pub.port, "i2c.board") && board_chip(s->pub.addr)) {
        lock();
        s->pub.state = AOS_SENSOR_ERR;
        const char *g = aos_io_i2c_guess(s->pub.addr, true);
        snprintf(s->pub.err, sizeof s->pub.err, "board:%.*s", (int)strcspn(g, " ("), g);
        s->pub.errors++;
        hist_push(s, false);
        unlock();
        return;
    }
    const sens_drv_t *d = drv_of(s->chip);
    if (d && (d->flags & SF_SLOW)) aos_io_i2c_dev_cfg(b, s->pub.addr, SLOW_HZ, SLOW_WAIT);
    sens_io_t io = { .i2c = b, .pin = { -1, -1 } };
    run_driver(s, &io);
}

/* Every slot of a port that could not be opened, in the same state */
static void port_failed(const char *port, const char *owner)
{
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (!s->used || strcmp(s->pub.port, port)) continue;
        s->pub.state = owner ? AOS_SENSOR_BUSY : AOS_SENSOR_ERR;
        snprintf(s->pub.err, sizeof s->pub.err, owner ? "busy:%s" : "port", owner ? owner : "");
        hist_push(s, false);
    }
    unlock();
}

static const char *port_owner(const aos_io_port_t *p, int extra)
{
    const char *o = NULL;
    for (int k = 0; p && !o && k < 4; k++) if (p->pins[k] >= 0) o = aos_io_owner(p->pins[k]);
    if (!o && extra >= 0) o = aos_io_owner(extra);
    return o && strcmp(o, OWNER) ? o : NULL;
}

/* The SPI chips: each opens its port with its own chip select, for the
 * length of one read */
static void read_spi_slots(void)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        lock();
        slot_t *s = &s_slot[i];
        bool mine = s->used && bus_of(s) == AOS_SENSOR_BUS_SPI;
        char port[AOS_IO_PORT_NAME_MAX];
        scpy(port, sizeof port, s->pub.port);
        uint8_t cs = s->pub.addr;
        int chip = s->chip;
        unlock();
        if (!mine) continue;
        const sens_drv_t *d = SENS_DRV[chip];
        /* the chip select as sync_declared() resolved it: the module's cs=, else the port's */
        aos_io_spi_cfg_t cfg = { .clock_hz = d->spi_hz ? d->spi_hz : 1000000, .mode = d->spi_mode, .cs = (int8_t)cs };
        aos_io_spi_t *h = aos_io_spi_open(port, OWNER, &cfg);
        if (!h) {
            const char *o = port_owner(aos_io_port_find(port), cs);
            lock();
            if (s->used) {
                s->pub.state = o ? AOS_SENSOR_BUSY : AOS_SENSOR_ERR;
                snprintf(s->pub.err, sizeof s->pub.err, o ? "busy:%s" : "port", o ? o : "");
                hist_push(s, false);
            }
            unlock();
            continue;
        }
        sens_io_t io = { .spi = h, .pin = { -1, -1 } };
        run_driver(s, &io);
        aos_io_spi_close(h);
    }
}

/* The GPIO sensors: the driver works the pins (it claims them as Sensores
 * and keeps them while it exists) */
static void read_gpio_slots(void)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        lock();
        slot_t *s = &s_slot[i];
        bool mine = s->used && bus_of(s) == AOS_SENSOR_BUS_GPIO;
        char name[24];
        scpy(name, sizeof name, s->pub.name);
        int pin = s->pub.addr;
        unlock();
        if (!mine) continue;
        const char *o = aos_io_owner(pin);
        if (o && strcmp(o, OWNER)) {
            lock();
            if (s->used) {
                s->pub.state = AOS_SENSOR_BUSY;
                snprintf(s->pub.err, sizeof s->pub.err, "busy:%s", o);
                hist_push(s, false);
            }
            unlock();
            continue;
        }
        sens_io_t io = { .pin = { pin, aos_io_module_gpio(name, "pin2") } };
        /* the pins it reads now, freed when it goes (another pin2= frees the old one) */
        lock();
        if (s->used) {
            int old2 = s->gpio[1];
            s->gpio[0] = (int8_t)pin;
            s->gpio[1] = (int8_t)io.pin[1];
            const char *o2 = old2 >= 0 && old2 != io.pin[1] ? aos_io_owner(old2) : NULL;
            if (o2 && !strcmp(o2, OWNER)) { aos_io_release(old2, OWNER); s->inited = false; }
        }
        unlock();
        run_driver(s, &io);
    }
}

/* The UART sensors: the port is opened for the read and closed again */
static void read_uart_slots(void)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        lock();
        slot_t *s = &s_slot[i];
        bool mine = s->used && bus_of(s) == AOS_SENSOR_BUS_UART;
        char port[AOS_IO_PORT_NAME_MAX];
        scpy(port, sizeof port, s->pub.port);
        int chip = s->chip;
        unlock();
        if (!mine) continue;
        const sens_drv_t *d = SENS_DRV[chip];
        if (!due(s, d)) { keep(s); continue; }
        aos_io_uart_t *u = aos_io_uart_open(port, d->baud ? d->baud : 9600, false, OWNER);
        if (!u) { port_failed(port, port_owner(aos_io_port_find(port), -1)); continue; }
        sens_io_t io = { .uart = u, .pin = { -1, -1 } };
        run_driver(s, &io);
        aos_io_uart_close(u);
    }
}

/* ---- 1-Wire ---- */

/* Every sensor of a bus, in the same state: the bus could not be had */
static void ow_mark(const char *port, int state, const char *err)
{
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (!s->used || !s->rom || strcmp(s->pub.port, port)) continue;
        s->pub.state = (uint8_t)state;
        scpy(s->pub.err, sizeof s->pub.err, err);
        if (state == AOS_SENSOR_ERR) s->pub.errors++;
        hist_push(s, false);
    }
    unlock();
}

/* The round's start on each bus: a search when one is due, then one
 * conversion for every sensor at once; read in ow_finish() */
static void ow_begin(uint32_t t0, bool rescan, bool viewed)
{
    for (int i = 0; i < s_now; i++) {
        ow_bus_t *bus = &s_ow[i];
        bus->b = NULL;
        const char *o = aos_io_owner(bus->gpio);
        if (o && strcmp(o, OWNER)) {
            char e[48];
            snprintf(e, sizeof e, "busy:%s", o);
            ow_mark(bus->port, AOS_SENSOR_BUSY, e);
            continue;
        }
        aos_io_ow_t *b = aos_io_ow_open(bus->gpio, true, OWNER);
        if (!b) { ow_mark(bus->port, AOS_SENSOR_ERR, "port"); continue; }
        if (rescan || !bus->scanned || t0 - bus->scan_ms >= (viewed ? SCAN_FAST : SCAN_SLOW)) {
            uint64_t roms[OW_DEVS];
            int n = aos_io_ow_search(b, roms, OW_DEVS);
            bus->scanned = true;
            bus->scan_ms = sens_now();
            if (n >= 0 && s_ds_chip >= 0) {
                lock();
                bool found[AOS_SENSOR_MAX] = { 0 };
                for (int k = 0; k < n; k++) {
                    uint8_t fam = (uint8_t)roms[k];
                    if (!ow_temp_family(fam)) continue;
                    slot_t *s = slot_find_rom(bus->port, roms[k]);
                    if (!s) {
                        s = slot_add(bus->port, (uint8_t)bus->gpio, s_ds_chip);
                        if (!s) continue;
                        snprintf(s->pub.name, sizeof s->pub.name, "%.15s_%06X", bus->base,
                                 (unsigned)((roms[k] >> 8) & 0xFFFFFF));
                    }
                    s->rom = roms[k];
                    s->detected = true;
                    s->ow_miss = 0;
                    const char *fn = aos_io_ow_family(fam);
                    if (fn) scpy(s->pub.chip, sizeof s->pub.chip, fn);
                    found[s - s_slot] = true;
                }
                /* gone from the bus: forgotten unless modules.txt names it - but
                 * only once three searches in a row missed it AND it stopped
                 * answering its own ROM. A search can lose a sensor to one bad
                 * bit; dropped at the first miss, one that read fine 18 hours
                 * on end came back with its counters at zero (2026-10-19). */
                for (int k = 0; k < AOS_SENSOR_MAX; k++) {
                    slot_t *s = &s_slot[k];
                    if (!s->used || !s->rom || found[k] || strcmp(s->pub.port, bus->port)) continue;
                    if (s->ow_miss < 255) s->ow_miss++;
                    if (s->ow_miss < 3 || s->pub.state == AOS_SENSOR_OK) continue;
                    s->detected = false;
                    if (!s->pub.declared) slot_drop(s);
                }
                unlock();
            }
        }
        int ms = aos_io_ds18b20_convert_all(b);
        if (ms < 0) {
            aos_io_ow_close(b);
            ow_mark(bus->port, AOS_SENSOR_ERR, "nack:ow");    /* no presence pulse */
            continue;
        }
        bus->b = b;
        bus->conv_at = sens_now();
        bus->conv_ms = ms;
    }
}

static void ow_finish(void)
{
    for (int i = 0; i < s_now; i++) {
        ow_bus_t *bus = &s_ow[i];
        if (!bus->b) continue;
        uint32_t el = sens_now() - bus->conv_at;
        if (el < (uint32_t)bus->conv_ms) aos_hal_sleep_ms((uint32_t)bus->conv_ms - el + 5);
        for (int k = 0; k < AOS_SENSOR_MAX; k++) {
            lock();
            slot_t *s = &s_slot[k];
            bool mine = s->used && s->rom && !strcmp(s->pub.port, bus->port);
            slot_t tmp = *s;
            unlock();
            if (!mine) continue;
            tmp.pub.err[0] = 0;
            tmp.xn = 0;
            tmp.inited = true;
            float t;
            int bits, r = R_OK;
            if (aos_io_ds18b20_read(bus->b, tmp.rom, &t, &bits)) sens_setv(&tmp, 0, "temp", AOS_SQ_TEMP, t);
            else { r = R_NACK; scpy(tmp.pub.err, sizeof tmp.pub.err, "ow"); }
            commit(s, &tmp, r);
        }
        aos_io_ow_close(bus->b);
        bus->b = NULL;
    }
}

/* ---- the I2C ports ---- */

static void i2c_round(uint32_t t0, bool rescan, bool viewed)
{
    static uint32_t last_scan[AOS_IO_PORT_MAX];
    static char scan_port[AOS_IO_PORT_MAX][AOS_IO_PORT_NAME_MAX];
    char ports[AOS_IO_PORT_MAX][AOS_IO_PORT_NAME_MAX];
    int np = 0;
    for (int i = 0; i < aos_io_port_count() && np < AOS_IO_PORT_MAX; i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind == AOS_PORT_I2C) scpy(ports[np++], AOS_IO_PORT_NAME_MAX, p->name);
    }
    for (int pi = 0; pi < np; pi++) {
        const char *port = ports[pi];
        bool board = !strcmp(port, "i2c.board");
        /* when is this port's next autodetect */
        int si = -1;
        for (int k = 0; k < AOS_IO_PORT_MAX; k++) if (!strcmp(scan_port[k], port)) { si = k; break; }
        if (si < 0) for (int k = 0; k < AOS_IO_PORT_MAX; k++) if (!scan_port[k][0]) {
            si = k;
            scpy(scan_port[k], sizeof scan_port[k], port);
            last_scan[k] = t0 - SCAN_SLOW;
            break;
        }
        bool scan = !board && si >= 0 && (rescan || t0 - last_scan[si] >= (viewed ? SCAN_FAST : SCAN_SLOW));
        int nslots = 0;
        lock();
        for (int i = 0; i < AOS_SENSOR_MAX; i++) if (s_slot[i].used && !strcmp(s_slot[i].pub.port, port)) nslots++;
        unlock();
        if (!scan && !nslots) continue;

        /* pins somebody else holds (the Bus app driving GPIO21, say):
         * skip the round quietly instead of having the arbiter refuse
         * and log it every second */
        const aos_io_port_t *pp = aos_io_port_find(port);
        bool held = !board && port_owner(pp, -1);
        aos_io_i2c_t *b = held ? NULL : aos_io_i2c_open(port, OWNER);
        if (!b && !held) {
            aos_hal_sleep_ms(40);                  /* the Bus app in the middle of a job: once more */
            b = aos_io_i2c_open(port, OWNER);
        }
        port_st_t *ps;
        if (!b) {
            const char *o = pp ? port_owner(pp, -1) : NULL;
            lock();
            ps = pst_of(port);
            if (ps) {
                ps->state = o ? AOS_SENSOR_BUSY : AOS_SENSOR_ERR;
                scpy(ps->owner, sizeof ps->owner, o ? o : "");
            }
            unlock();
            port_failed(port, o);
            continue;
        }
        if (scan) {
            detect(b, port);
            last_scan[si] = sens_now();
        }
        for (int i = 0; i < AOS_SENSOR_MAX; i++) {
            lock();
            bool mine = s_slot[i].used && !strcmp(s_slot[i].pub.port, port);
            unlock();
            if (mine) read_slot_i2c(b, &s_slot[i]);
        }
        aos_io_i2c_close(b);
        lock();
        ps = pst_of(port);
        if (ps) {
            ps->state = AOS_SENSOR_OK;
            ps->owner[0] = 0;
            ps->found = 0;
            for (int i = 0; i < AOS_SENSOR_MAX; i++) if (s_slot[i].used && !strcmp(s_slot[i].pub.port, port)) ps->found++;
            if (scan) { ps->scan_ms = sens_now(); ps->scanned = true; }
        }
        unlock();
    }
}

static void service(void *arg)
{
    (void)arg;
    s_ds_chip = sens_drv_find("ds18b20");
    for (;;) {
        uint32_t t0 = sens_now();
        bool rescan = s_rescan;
        s_rescan = false;
        if (rescan) {
            /* every chip is started again and every port probed now; what
             * is not there any more goes in detect(), the rest keeps its
             * history */
            lock();
            for (int i = 0; i < AOS_SENSOR_MAX; i++) s_slot[i].inited = false;
            unlock();
        }
        sync_declared();
        bool viewed = t0 - s_keep_ms < KEEP_MS;
        ow_begin(t0, rescan, viewed);        /* the conversions run while the rest is read */
        i2c_round(t0, rescan, viewed);
        read_spi_slots();
        read_gpio_slots();
        read_uart_slots();
        ow_finish();
        s_seq++;
        uint32_t spent = sens_now() - t0;
        aos_hal_sleep_ms(spent < ROUND_MS - 200 ? ROUND_MS - spent : 200);
    }
}

/* -------------------------------------------------------------------------- */
/* The API                                                                     */
/* -------------------------------------------------------------------------- */

void aos_sensors_start(void)
{
    /* the first caller creates the lock and the thread; one that comes at
     * the same moment waits for the lock to exist */
    if (__atomic_exchange_n(&s_started, 1, __ATOMIC_ACQ_REL)) {
        while (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) aos_hal_sleep_ms(1);
        return;
    }
    sens_table_init();
    s_mx = aos_hal_mutex_create();
    s_keep_ms = sens_now();
    __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
    /* the slot copies the drivers work on live on this stack: 8 KB, PSRAM */
    if (!aos_hal_thread_start("sensors", service, NULL, 8192, 3)) aos_hal_log("sensors", "no thread");
}

void aos_sensors_autostart(void)
{
    static bool tried;
    if (tried || __atomic_load_n(&s_started, __ATOMIC_ACQUIRE)) return;
    tried = true;
    for (int m = 0; m < aos_io_module_count(); m++)
        if (module_chip(aos_io_module_at(m)) >= 0) { aos_sensors_start(); return; }
}

void aos_sensors_keep(void) { aos_sensors_start(); s_keep_ms = sens_now(); }
void aos_sensors_rescan(void) { aos_sensors_start(); s_rescan = true; }
uint32_t aos_sensors_seq(void) { return s_seq; }

int aos_sensors_ports(aos_sensor_port_t *out, int max)
{
    aos_sensors_start();
    lock();
    int n = s_npst < max ? s_npst : max;
    memcpy(out, s_pst, sizeof *out * (size_t)n);
    unlock();
    return n;
}

/* Visible slots in a stable order: declared first, then detected. */
static int visible(int *idx)
{
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < AOS_SENSOR_MAX; i++)
            if (s_slot[i].used && s_slot[i].pub.declared == (pass == 0)) idx[n++] = i;
    return n;
}

int aos_sensor_count(void)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX];
    lock();
    int n = visible(idx);
    unlock();
    return n;
}

bool aos_sensor_at(int i, aos_sensor_t *out)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX];
    lock();
    int n = visible(idx);
    bool ok = i >= 0 && i < n;
    if (ok) *out = s_slot[idx[i]].pub;
    unlock();
    return ok;
}

int aos_sensor_values(int i, aos_sensor_value_t *out, int max)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX], got = 0;
    lock();
    int n = visible(idx);
    if (i >= 0 && i < n) {
        const slot_t *s = &s_slot[idx[i]];
        got = s->xn < max ? s->xn : max;
        memcpy(out, s->xv, sizeof *out * (size_t)got);
    }
    unlock();
    return got;
}

bool aos_sensor_value(const char *name, const char *key, float *v)
{
    aos_sensors_start();
    bool ok = false;
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX && !ok; i++) {
        const slot_t *s = &s_slot[i];
        if (!s->used || s->pub.state != AOS_SENSOR_OK || strcmp(s->pub.name, name)) continue;
        for (int k = 0; k < s->xn; k++)
            if (!strcmp(s->xv[k].key, key)) { *v = s->xv[k].v; ok = true; break; }
    }
    unlock();
    return ok;
}

int aos_sensor_history(int i, int k, float *out, int max)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX], got = 0;
    if (k < 0 || k >= AOS_SENSOR_XVALUES) return 0;
    lock();
    int n = visible(idx);
    if (i >= 0 && i < n) {
        const slot_t *s = &s_slot[idx[i]];
        int cnt = s->h_n < max ? s->h_n : max;
        int start = (s->h_pos - cnt + AOS_SENSOR_HIST) % AOS_SENSOR_HIST;
        for (int j = 0; j < cnt; j++) out[j] = s->hist[k * AOS_SENSOR_HIST + (start + j) % AOS_SENSOR_HIST];
        got = cnt;
    }
    unlock();
    return got;
}

int aos_sensor_candidates(aos_sensor_cand_t *out, int max)
{
    aos_sensors_start();
    lock();
    int n = s_ncand < max ? s_ncand : max;
    memcpy(out, s_cand, sizeof *out * (size_t)n);
    unlock();
    return n;
}
