/*
 * P4OS - The sensor drivers, in the order autodetect tries them at an
 * address (sens_int.h): the ones a plain register read proves first, those
 * that need a command next, the candidates last. Each family's file brings
 * its own list; the table is put together once, at the first use.
 */
#include "sens_int.h"

/* The families, in the order their drivers break ties of prio */
#define FAMILIES(X) X(BASIC) X(ENV) X(AIR) X(LIGHT) X(MOTION) X(POWER) X(DIST) X(MISC) X(SPI) X(GPIO) X(UART)
#define DECL(f) extern const sens_drv_t *const SENS_G_##f[]; extern const sens_alias_t SENS_A_##f[];
FAMILIES(DECL)
#undef DECL

#define MAX_DRV 192
const sens_drv_t *SENS_DRV[MAX_DRV];
int SENS_NDRV;

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
    int n = 0;
    for (size_t g = 0; g < sizeof GROUPS / sizeof GROUPS[0]; g++)
        for (int i = 0; GROUPS[g][i] && n < MAX_DRV; i++) SENS_DRV[n++] = GROUPS[g][i];
    /* stable insertion sort by prio: the families' order breaks the ties */
    for (int i = 1; i < n; i++) {
        const sens_drv_t *d = SENS_DRV[i];
        int j = i;
        for (; j > 0 && SENS_DRV[j - 1]->prio > d->prio; j--) SENS_DRV[j] = SENS_DRV[j - 1];
        SENS_DRV[j] = d;
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
