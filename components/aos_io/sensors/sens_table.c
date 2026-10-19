/*
 * P4OS - The sensor drivers, in the order autodetect tries them at an
 * address (sens_int.h): the ones a plain register read proves first, those
 * that need a command next, the candidates last. Each family's file brings
 * its own list; the table is put together once, at the first use.
 */
#include "sens_int.h"

/* The families, in the order their drivers break ties of prio */
#define FAMILIES(X) X(BASIC) X(ENV) X(TEMP) X(AIR) X(LIGHT) X(MOTION) X(POWER) X(DIST) X(MISC) X(SPI) X(GPIO) X(UART)
#define DECL(f) extern const sens_drv_t *const SENS_G_##f[]; extern const sens_alias_t SENS_A_##f[];
FAMILIES(DECL)
#undef DECL

#define MAX_DRV 192
const sens_drv_t *SENS_DRV[MAX_DRV];
int SENS_NDRV;
static uint8_t s_cat[MAX_DRV];          /* each driver's form group, in SENS_DRV's order */

/* The form group of each family's drivers (BASIC and the bus files: each
 * driver says its own) */
static const uint8_t FAM_CAT[] = {
    [0] = AOS_SENSOR_CAT_OTHER,  [1] = AOS_SENSOR_CAT_ENV,    [2] = AOS_SENSOR_CAT_TEMP,
    [3] = AOS_SENSOR_CAT_AIR,    [4] = AOS_SENSOR_CAT_LIGHT,  [5] = AOS_SENSOR_CAT_MOTION,
    [6] = AOS_SENSOR_CAT_POWER,  [7] = AOS_SENSOR_CAT_DIST,   [8] = AOS_SENSOR_CAT_OTHER,
    [9] = AOS_SENSOR_CAT_OTHER,  [10] = AOS_SENSOR_CAT_OTHER, [11] = AOS_SENSOR_CAT_OTHER,
};

static const sens_drv_t *const *const GROUPS[] = {
#define G(f) SENS_G_##f,
    FAMILIES(G)
#undef G
};
static const sens_alias_t *const ALIASES[] = {
#define A(f) SENS_A_##f,
    FAMILIES(A)
#undef A
};

static int s_init, s_ready;

void sens_table_init(void)
{
    if (__atomic_exchange_n(&s_init, 1, __ATOMIC_ACQ_REL)) {
        while (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) aos_hal_sleep_ms(1);
        return;
    }
    _Static_assert(sizeof FAM_CAT == sizeof GROUPS / sizeof GROUPS[0], "a family without its group");
    int n = 0;
    for (size_t g = 0; g < sizeof GROUPS / sizeof GROUPS[0]; g++)
        for (int i = 0; GROUPS[g][i] && n < MAX_DRV; i++) {
            const sens_drv_t *d = GROUPS[g][i];
            s_cat[n] = d->cat ? (uint8_t)(d->cat - 1) : FAM_CAT[g];
            SENS_DRV[n++] = d;
        }
    /* stable insertion sort by prio: the families' order breaks the ties */
    for (int i = 1; i < n; i++) {
        const sens_drv_t *d = SENS_DRV[i];
        uint8_t c = s_cat[i];
        int j = i;
        for (; j > 0 && SENS_DRV[j - 1]->prio > d->prio; j--) { SENS_DRV[j] = SENS_DRV[j - 1]; s_cat[j] = s_cat[j - 1]; }
        SENS_DRV[j] = d;
        s_cat[j] = c;
    }
    SENS_NDRV = n;
    __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
}

int sens_drv_find(const char *id)
{
    sens_table_init();
    if (!id || !*id) return -1;
    /* the first of an id that is not a hidden twin */
    for (int i = 0; i < SENS_NDRV; i++) if (!(SENS_DRV[i]->flags & SF_HIDDEN) && !strcmp(SENS_DRV[i]->pub.id, id)) return i;
    for (size_t g = 0; g < sizeof ALIASES / sizeof ALIASES[0]; g++)
        for (int i = 0; ALIASES[g][i].alias; i++)
            if (!strcmp(ALIASES[g][i].alias, id)) return sens_drv_find(ALIASES[g][i].id);
    /* "bme280_b", "ina219-banco": the part before the separator */
    char base[24];
    size_t n = strcspn(id, "_-.");
    if (n == strlen(id) || n >= sizeof base) return -1;
    memcpy(base, id, n);
    base[n] = 0;
    return sens_drv_find(base);
}

const sens_drv_t *sens_drv(const sens_slot_t *s)
{
    return s->chip >= 0 && s->chip < SENS_NDRV ? SENS_DRV[s->chip] : NULL;
}

/* ---- the chips the forms offer: every driver but the hidden twins ---- */

int aos_sensor_chip_count(void)
{
    sens_table_init();
    int n = 0;
    for (int i = 0; i < SENS_NDRV; i++) if (!(SENS_DRV[i]->flags & SF_HIDDEN)) n++;
    return n;
}

const aos_sensor_chip_t *aos_sensor_chip_at(int i)
{
    sens_table_init();
    for (int k = 0; k < SENS_NDRV; k++) {
        if (SENS_DRV[k]->flags & SF_HIDDEN) continue;
        if (i-- == 0) return &SENS_DRV[k]->pub;
    }
    return NULL;
}

const aos_sensor_chip_t *aos_sensor_chip_find(const char *id)
{
    int c = sens_drv_find(id);
    return c >= 0 ? &SENS_DRV[c]->pub : NULL;
}

int aos_sensor_chip_cat(const aos_sensor_chip_t *c)
{
    sens_table_init();
    for (int k = 0; k < SENS_NDRV; k++) if (&SENS_DRV[k]->pub == c) return s_cat[k];
    return AOS_SENSOR_CAT_OTHER;
}
