/*
 * P4OS - The sensor drivers, in the order autodetect tries them at an
 * address (sens_int.h): the ones a plain register read proves first, those
 * that need a command next, the candidates last.
 */
#include "sens_int.h"

#define D(x) extern const sens_drv_t x;
D(DRV_BME280) D(DRV_BMP280) D(DRV_INA226) D(DRV_INA219) D(DRV_ADS1115)
D(DRV_SHT3X) D(DRV_SHT4X) D(DRV_AHT20) D(DRV_SHT3X_CAND) D(DRV_BH1750)
D(DRV_MAX6675) D(DRV_DS18B20)
#undef D

const sens_drv_t *const SENS_DRV[] = {
    /* I2C, proved by a register */
    &DRV_BME280, &DRV_BMP280,
    &DRV_INA226, &DRV_INA219,
    &DRV_ADS1115,
    /* I2C, proved by a command */
    &DRV_SHT3X, &DRV_SHT4X, &DRV_AHT20,
    /* I2C, candidates */
    &DRV_SHT3X_CAND, &DRV_BH1750,
    /* SPI, 1-Wire: declared */
    &DRV_MAX6675, &DRV_DS18B20,
};
const int SENS_NDRV = (int)(sizeof SENS_DRV / sizeof SENS_DRV[0]);

/* Other names people give them in modules.txt */
static const struct { const char *alias, *id; } ALIAS[] = {
    { "sht30", "sht3x" }, { "sht31", "sht3x" }, { "sht35", "sht3x" }, { "sht85", "sht3x" }, { "gxht30", "sht3x" },
    { "sht40", "sht4x" }, { "sht41", "sht4x" }, { "sht43", "sht4x" }, { "sht45", "sht4x" },
    { "aht10", "aht20" }, { "aht21", "aht20" }, { "aht2x", "aht20" }, { "aht25", "aht20" }, { "aht30", "aht20" },
    { "dht20", "aht20" }, { "am2301b", "aht20" }, { "am2315c", "aht20" },
    { "gy302", "bh1750" }, { "gy30", "bh1750" },
    { "ds18s20", "ds18b20" }, { "ds1822", "ds18b20" }, { "max31850", "ds18b20" }, { "onewire", "ds18b20" },
};

int sens_drv_find(const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < SENS_NDRV; i++) if (!strcmp(SENS_DRV[i]->pub.id, id)) return i;
    for (size_t i = 0; i < sizeof ALIAS / sizeof ALIAS[0]; i++)
        if (!strcmp(ALIAS[i].alias, id)) return sens_drv_find(ALIAS[i].id);
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
    int n = 0;
    for (int i = 0; i < SENS_NDRV; i++) if (!(SENS_DRV[i]->flags & SF_HIDDEN)) n++;
    return n;
}

const aos_sensor_chip_t *aos_sensor_chip_at(int i)
{
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
