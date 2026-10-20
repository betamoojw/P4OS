/*
 * P4OS - The sensor service's values and errors in words (aos_sensor_text.h).
 * One table for every UI, in aos_ui so that its strings are in the
 * system's catalogue (_sistema.lang).
 */
#include "aos_sensor_text.h"
#include "aos_sensors.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

static void scpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    if (n) d[i] = 0;
}

const char *aos_sensor_key_name(const char *key)
{
    static const char *const K[][2] = {
        { "temp", N_("Temperatura") }, { "hum", N_("Humedad") }, { "press", N_("Presión") },
        { "lux", N_("Luz") }, { "bus", N_("Tensión") }, { "current", N_("Corriente") },
        { "power", N_("Potencia") }, { "shunt", N_("En el shunt") },
        { "a0", "A0" }, { "a1", "A1" }, { "a2", "A2" }, { "a3", "A3" },
        { "a4", "A4" }, { "a5", "A5" }, { "a6", "A6" }, { "a7", "A7" },
        { "v1", N_("Tensión 1") }, { "v2", N_("Tensión 2") }, { "v3", N_("Tensión 3") },
        { "i1", N_("Corriente 1") }, { "i2", N_("Corriente 2") }, { "i3", N_("Corriente 3") },
        { "co2", "CO₂" }, { "eco2", "eCO₂" }, { "tvoc", "TVOC" }, { "aqi", N_("Calidad del aire") },
        { "voc", N_("Índice VOC") }, { "nox", N_("Índice NOx") }, { "gas", N_("Resistencia del gas") },
        { "h2", "H₂" }, { "ethanol", N_("Etanol") },
        { "pm1", "PM1" }, { "pm25", "PM2,5" }, { "pm4", "PM4" }, { "pm10", "PM10" },
        { "uvi", N_("Índice UV") }, { "uva", "UVA" }, { "uvb", "UVB" }, { "uv", "UV" },
        { "ir", N_("Infrarrojo") }, { "visible", N_("Visible") }, { "full", N_("Espectro completo") },
        { "r", N_("Rojo") }, { "g", N_("Verde") }, { "b", N_("Azul") }, { "clear", N_("Sin filtro") },
        { "cct", N_("Temperatura de color") }, { "prox", N_("Proximidad") },
        { "dist", N_("Distancia") },
        { "ax", N_("Aceleración X") }, { "ay", N_("Aceleración Y") }, { "az", N_("Aceleración Z") },
        { "gx", N_("Giro X") }, { "gy", N_("Giro Y") }, { "gz", N_("Giro Z") },
        { "mx", N_("Campo X") }, { "my", N_("Campo Y") }, { "mz", N_("Campo Z") },
        { "heading", N_("Rumbo") }, { "pitch", N_("Cabeceo") }, { "roll", N_("Alabeo") },
        { "angle", N_("Ángulo") }, { "soc", N_("Carga") }, { "cell", N_("Tensión de la celda") },
        { "weight", N_("Peso") }, { "raw", N_("Crudo") }, { "count", N_("Pulsos") },
        { "freq", N_("Frecuencia") }, { "flow", N_("Caudal") }, { "volume", N_("Volumen") },
        { "speed", N_("Velocidad") }, { "rain", N_("Lluvia") }, { "dose", N_("Dosis") },
        { "cpm", N_("Cuentas por minuto") }, { "energy", N_("Energía") }, { "pf", N_("Factor de potencia") },
        { "motion", N_("Movimiento") }, { "presence", N_("Presencia") }, { "state", N_("Estado") },
        { "touch", N_("Toques") }, { "moist", N_("Humedad del suelo") },
        { "hr", N_("Pulso") }, { "spo2", N_("Oxígeno en sangre") }, { "red", N_("Rojo") },
        { "obj", N_("Objeto") }, { "amb", N_("Ambiente") }, { "cj", N_("Junta fría") },
        { "rtd", N_("Resistencia") }, { "die", N_("Chip") },
        { "move", N_("En movimiento") }, { "still", N_("Quieto") },
        { "white", N_("Blanco") }, { "calib", N_("Calibración") }, { "vocraw", N_("VOC crudo") }, { "noxraw", N_("NOx crudo") },
        { "signal", N_("Señal") }, { "magnet", N_("Imán") },
        { "p0", "P0" }, { "p1", "P1" }, { "p2", "P2" }, { "p3", "P3" },
        { "p4", "P4" }, { "p5", "P5" }, { "p6", "P6" }, { "p7", "P7" },
        { "alarm", N_("Alarma") }, { "people", N_("Personas") },
        { "move_dist", N_("Distancia en movimiento") }, { "still_dist", N_("Distancia quieto") },
        { "move_en", N_("Señal en movimiento") }, { "still_en", N_("Señal quieto") },
        { "x1", "X 1" }, { "x2", "X 2" }, { "x3", "X 3" }, { "y1", "Y 1" }, { "y2", "Y 2" }, { "y3", "Y 3" },
        { "speed1", N_("Velocidad 1") }, { "speed2", N_("Velocidad 2") }, { "speed3", N_("Velocidad 3") }, { "rate", N_("Carga por hora") }, { "rainrate", N_("Lluvia por hora") },
    };
    if (!key) return "";
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (!strcmp(K[i][0], key)) return _(K[i][1]);
    return key;
}

const char *aos_sensor_glyph(int q, const char *key)
{
    switch (q) {
    case AOS_SQ_TEMP: return AOS_SYM_THERMOMETER;
    case AOS_SQ_HUM: return AOS_SYM_WATER_PERCENT;
    case AOS_SQ_PRESS: return AOS_SYM_GAUGE;
    case AOS_SQ_LUX: return AOS_SYM_BRIGHTNESS_6;
    case AOS_SQ_CURR: return AOS_SYM_SINE_WAVE;
    case AOS_SQ_POWER: return AOS_SYM_POWER_PLUG;
    case AOS_SQ_CO2: return AOS_SYM_MOLECULE_CO2;
    case AOS_SQ_TVOC: return AOS_SYM_MOLECULE;
    case AOS_SQ_INDEX: return AOS_SYM_MOLECULE;
    case AOS_SQ_PM: return AOS_SYM_BLUR;
    case AOS_SQ_UVI: return AOS_SYM_WEATHER_SUNNY;
    case AOS_SQ_DIST: return AOS_SYM_RULER;
    case AOS_SQ_ACCEL: return AOS_SYM_AXIS_ARROW;
    case AOS_SQ_GYRO: return AOS_SYM_ROTATE_3D_VARIANT;
    case AOS_SQ_MAG: return key && !strcmp(key, "heading") ? AOS_SYM_COMPASS_OUTLINE : AOS_SYM_MAGNET;
    case AOS_SQ_ANGLE: return key && !strcmp(key, "heading") ? AOS_SYM_COMPASS_OUTLINE : AOS_SYM_ANGLE_ACUTE;
    case AOS_SQ_PERCENT: return AOS_SYM_BATTERY;
    case AOS_SQ_OHM: return AOS_SYM_RESISTOR;
    case AOS_SQ_RAW: return AOS_SYM_COUNTER;
    case AOS_SQ_FREQ: return AOS_SYM_SQUARE_WAVE;
    case AOS_SQ_FLOW: case AOS_SQ_VOLUME: return AOS_SYM_WATER;
    case AOS_SQ_SPEED: return AOS_SYM_WEATHER_WINDY;
    case AOS_SQ_RAIN: return AOS_SYM_WEATHER_RAINY;
    case AOS_SQ_DOSE: return AOS_SYM_RADIOACTIVE;
    case AOS_SQ_ENERGY: return AOS_SYM_LIGHTNING_BOLT;
    case AOS_SQ_MASS: return AOS_SYM_WEIGHT_KILOGRAM;
    case AOS_SQ_STATE: return key && (!strcmp(key, "motion") || !strcmp(key, "presence")) ? AOS_SYM_MOTION_SENSOR : AOS_SYM_TOGGLE_SWITCH;
    case AOS_SQ_BPM: return AOS_SYM_HEART_PULSE;
    default: return key && key[0] == 'a' && key[1] >= '0' && key[1] <= '9' ? AOS_SYM_WAVEFORM : AOS_SYM_LIGHTNING_BOLT;
    }
}

int aos_sensor_decimals(int q, float v)
{
    float a = fabsf(v);
    switch (q) {
    case AOS_SQ_TEMP: case AOS_SQ_HUM: case AOS_SQ_PRESS: case AOS_SQ_PERCENT: case AOS_SQ_FLOW:
    case AOS_SQ_SPEED: case AOS_SQ_RAIN: case AOS_SQ_UVI: case AOS_SQ_ANGLE: case AOS_SQ_MAG: case AOS_SQ_GYRO:
        return 1;
    case AOS_SQ_LUX: return a < 100 ? 1 : 0;
    case AOS_SQ_ACCEL: return 3;
    case AOS_SQ_DOSE: return 3;
    case AOS_SQ_VOLUME: case AOS_SQ_ENERGY: return a < 10 ? 3 : 1;
    case AOS_SQ_MASS: return a < 100 ? 1 : 0;
    case AOS_SQ_VOLT: case AOS_SQ_CURR: return 3;
    case AOS_SQ_POWER: return 2;
    case AOS_SQ_FREQ: return a < 100 ? 1 : 0;
    default: return 0;
    }
}

void aos_sensor_fmt(int q, float v, char *out, size_t n)
{
    if (isnan(v)) { snprintf(out, n, "--"); return; }
    float a = fabsf(v);
    switch (q) {
    case AOS_SQ_VOLT: if (a < 1) snprintf(out, n, "%.1f mV", v * 1000); else snprintf(out, n, "%.3f V", v); break;
    case AOS_SQ_CURR: if (a < 1) snprintf(out, n, "%.1f mA", v * 1000); else snprintf(out, n, "%.3f A", v); break;
    case AOS_SQ_POWER: if (a < 1) snprintf(out, n, "%.0f mW", v * 1000); else snprintf(out, n, "%.2f W", v); break;
    case AOS_SQ_DIST: if (a >= 1000) snprintf(out, n, "%.2f m", v / 1000); else snprintf(out, n, "%.0f mm", v); break;
    case AOS_SQ_MASS: if (a >= 1000) snprintf(out, n, "%.2f kg", v / 1000); else snprintf(out, n, a < 100 ? "%.1f g" : "%.0f g", v); break;
    case AOS_SQ_OHM: if (a >= 1e6f) snprintf(out, n, "%.2f MΩ", v / 1e6f); else if (a >= 1000) snprintf(out, n, "%.1f kΩ", v / 1000); else snprintf(out, n, "%.1f Ω", v); break;
    case AOS_SQ_STATE: snprintf(out, n, "%s", v >= 0.5f ? _("Sí") : _("No")); return;
    default: {
        const char *u = aos_sensor_unit(q);
        int d = aos_sensor_decimals(q, v);
        snprintf(out, n, *u ? "%.*f %s" : "%.*f", d, (double)v, u);
        break;
    }
    }
    comma(out);
}

void aos_sensor_err_text(const char *err, char *out, size_t n)
{
    if (!err || !*err) { if (n) out[0] = 0; return; }
    const char *d = strchr(err, ':');
    d = d ? d + 1 : "";
    if (!strncmp(err, "nack:spi", 8)) scpy(out, n, _("No contesta: MISO quedó en 1. ¿Alimentación, CS, cables?"));
    else if (!strncmp(err, "nack:ow", 7)) scpy(out, n, _("No contesta en el bus 1-Wire. ¿Alimentación, la resistencia de 4,7 kΩ, cables?"));
    else if (!strncmp(err, "nack:gpio", 9)) scpy(out, n, _("No contesta en el pin. ¿Alimentación, cables, el modelo?"));
    else if (!strncmp(err, "nack:uart", 9)) scpy(out, n, _("No contesta por el puerto serie. ¿TX y RX cruzados, alimentación?"));
    else if (!strncmp(err, "nack", 4)) scpy(out, n, _("No contesta en esa dirección"));
    else if (!strncmp(err, "crc:sync", 8)) scpy(out, n, _("No se entiende lo que manda: ¿otra velocidad u otro sensor?"));
    else if (!strncmp(err, "crc", 3)) scpy(out, n, _("Llegó una lectura con el CRC mal"));
    else if (!strncmp(err, "id", 2)) snprintf(out, n, _("En esa dirección hay otro chip (dice %s)"), d);
    else if (!strncmp(err, "notready", 8)) scpy(out, n, _("Todavía no midió"));
    else if (!strncmp(err, "busy", 4)) snprintf(out, n, _("Los pines los tiene %s"), d);
    else if (!strncmp(err, "port", 4)) scpy(out, n, _("No se pudo abrir el puerto"));
    else if (!strncmp(err, "board", 5)) snprintf(out, n, _("Ahí está el %s de la placa"), d);
    else if (!strncmp(err, "open:rtd", 8)) scpy(out, n, _("La RTD está abierta o le falta un cable"));
    else if (!strncmp(err, "open:gnd", 8)) scpy(out, n, _("La termocupla está en corto a GND"));
    else if (!strncmp(err, "open:vcc", 8)) scpy(out, n, _("La termocupla está en corto a VCC"));
    else if (!strncmp(err, "open:ovuv", 9)) scpy(out, n, _("Una punta de la termocupla toca VCC o GND"));
    else if (!strncmp(err, "open", 4)) scpy(out, n, _("La termocupla está abierta (o no está conectada)"));
    else if (!strncmp(err, "range:short", 11)) scpy(out, n, _("La RTD está en corto"));
    else if (!strncmp(err, "range", 5)) scpy(out, n, _("Fuera de rango"));
    else if (!strncmp(err, "cfg", 3)) snprintf(out, n, _("Falta %s= en la línea del módulo"), d);
    else scpy(out, n, err);
}

const char *aos_sensor_chip_what(const char *id)
{
    static const char *const W[][2] = {
        /* I2C, environment */
        { "bme280", N_("Temperatura, presión y humedad") }, { "bmp280", N_("Temperatura y presión") },
        { "bme680", N_("Temperatura, presión, humedad y gas (calidad del aire)") },
        { "bmp180", N_("Temperatura y presión") }, { "bmp3xx", N_("Temperatura y presión, de precisión") },
        { "bmp5xx", N_("Temperatura y presión, de precisión") }, { "dps310", N_("Temperatura y presión") },
        { "lps22", N_("Temperatura y presión") }, { "ms5611", N_("Temperatura y presión (altímetro)") },
        { "qmp6988", N_("Temperatura y presión") },
        { "sht3x", N_("Temperatura y humedad") }, { "sht4x", N_("Temperatura y humedad") },
        { "aht20", N_("Temperatura y humedad") }, { "aht10", N_("Temperatura y humedad") }, { "sht2x", N_("Temperatura y humedad") },
        { "hdc1080", N_("Temperatura y humedad") }, { "hdc2080", N_("Temperatura y humedad") },
        { "hdc302x", N_("Temperatura y humedad") }, { "shtc3", N_("Temperatura y humedad") },
        { "am2320", N_("Temperatura y humedad") }, { "mcp9808", N_("Temperatura, ±0,25 °C") },
        { "tmp117", N_("Temperatura, ±0,1 °C") }, { "tmp102", N_("Temperatura") }, { "lm75", N_("Temperatura") },
        { "mlx90614", N_("Temperatura sin contacto (infrarrojo) y del ambiente") },
        /* air */
        { "scd4x", N_("CO₂ real (fotoacústico), temperatura y humedad") }, { "scd30", N_("CO₂ real (NDIR), temperatura y humedad") },
        { "sgp30", N_("eCO₂ y TVOC") }, { "sgp4x", N_("Índice de VOC y de NOx (algoritmo de Sensirion)") },
        { "ccs811", N_("eCO₂ y TVOC") }, { "ens160", N_("Calidad del aire, eCO₂ y TVOC") },
        { "pmsa003i", N_("Partículas PM1, PM2,5 y PM10") }, { "sen5x", N_("Partículas, temperatura, humedad, VOC y NOx") },
        { "sps30", N_("Partículas PM1 a PM10") }, { "hm3301", N_("Partículas PM1, PM2,5 y PM10") },
        /* light */
        { "bh1750", N_("Luz, en lux") }, { "veml7700", N_("Luz, en lux") }, { "veml6075", N_("UVA, UVB e índice UV") },
        { "tsl2561", N_("Luz, en lux (visible e infrarrojo)") }, { "tsl2591", N_("Luz, en lux, de alto rango") },
        { "tcs34725", N_("Color (rojo, verde, azul), lux y temperatura de color") },
        { "apds9960", N_("Proximidad y color") }, { "ltr390", N_("Índice UV y luz") }, { "opt3001", N_("Luz, en lux") },
        { "ltr329", N_("Luz, en lux (visible e infrarrojo)") }, { "sts3x", N_("Temperatura, de precisión") },
        { "tfluna", N_("Distancia por LiDAR, hasta 8 m") },
        { "si1145", N_("Índice UV, visible e infrarrojo") }, { "max44009", N_("Luz, en lux") },
        /* motion */
        { "mpu6050", N_("Acelerómetro y giróscopo (y brújula en el MPU-9250)") }, { "icm20948", N_("Acelerómetro, giróscopo y brújula") },
        { "icm42688", N_("Acelerómetro y giróscopo") }, { "lsm6ds", N_("Acelerómetro y giróscopo") },
        { "lis3dh", N_("Acelerómetro") }, { "adxl345", N_("Acelerómetro") }, { "bmi160", N_("Acelerómetro y giróscopo") },
        { "qmi8658", N_("Acelerómetro y giróscopo") }, { "bno055", N_("Orientación absoluta (fusión de 9 ejes)") },
        { "qmc5883l", N_("Brújula (campo magnético)") }, { "qmc5883p", N_("Brújula (campo magnético)") },
        { "hmc5883l", N_("Brújula (campo magnético)") }, { "lis3mdl", N_("Brújula (campo magnético)") },
        { "mmc5983", N_("Brújula (campo magnético)") }, { "mmc5603", N_("Brújula (campo magnético)") },
        /* distance */
        { "vl53l0x", N_("Distancia por láser, hasta 2 m") }, { "vl53l1x", N_("Distancia por láser, hasta 4 m") },
        { "vl6180x", N_("Distancia por láser hasta 20 cm, y luz") }, { "rcwl9620", N_("Distancia por ultrasonido") },
        { "urm09", N_("Distancia por ultrasonido y temperatura") },
        /* power, ADC */
        { "ina219", N_("Tensión, corriente y potencia, hasta 26 V") }, { "ina226", N_("Tensión, corriente y potencia, hasta 36 V") },
        { "ina260", N_("Tensión, corriente y potencia, con su shunt adentro (15 A)") },
        { "ina3221", N_("Tres canales de tensión y corriente") }, { "ina228", N_("Tensión, corriente, potencia y energía, hasta 85 V") },
        { "max17048", N_("Batería de litio: tensión y carga") },
        { "ads1115", N_("ADC de 16 bits, 4 entradas") }, { "ads1015", N_("ADC de 12 bits, 4 entradas") },
        { "pcf8591", N_("ADC de 8 bits, 4 entradas") }, { "mcp3421", N_("ADC de 18 bits") },
        { "nau7802", N_("Celda de carga (balanza), 24 bits") }, { "mcp9600", N_("Termocupla por I2C") },
        /* other */
        { "max3010x", N_("Pulso (MAX30102) y la luz roja e infrarroja, en crudo") }, { "mpr121", N_("12 teclas táctiles") },
        { "seesaw", N_("Humedad del suelo y temperatura (Adafruit)") }, { "as5600", N_("Ángulo magnético") },
        { "ds3231", N_("Temperatura del reloj DS3231") }, { "pcf8574", N_("8 entradas digitales") },
        /* SPI */
        { "max6675", N_("Termocupla tipo K, de 0 a 1024 °C, por SPI") }, { "max31855", N_("Termocupla, de −200 a 1350 °C, por SPI") },
        { "max31856", N_("Termocupla de cualquier tipo, por SPI") }, { "max31865", N_("PT100 o PT1000 (RTD), por SPI") },
        { "mcp3008", N_("ADC de 10 bits, 8 entradas, por SPI") }, { "mcp3208", N_("ADC de 12 bits, 8 entradas, por SPI") },
        /* 1-Wire */
        { "ds18b20", N_("Temperatura por 1-Wire: todos los que haya en el bus, cada uno con su número de serie") },
        /* GPIO */
        { "dht11", N_("Temperatura y humedad (DHT11)") }, { "dht22", N_("Temperatura y humedad (DHT22, AM2302)") },
        { "hcsr04", N_("Distancia por ultrasonido (disparo y eco)") }, { "hx711", N_("Celda de carga (balanza)") },
        { "ping", N_("Distancia por ultrasonido con un solo pin (Grove, PING)") },
        { "pulse", N_("Pulsos contados y su frecuencia: un medidor, un contador") },
        { "flow", N_("Caudal de agua (YF-S201 y parecidos): litros por minuto y litros") },
        { "rain", N_("Lluvia, con un pluviómetro de cubeta basculante") },
        { "wind", N_("Velocidad del viento, con un anemómetro de cazoletas") },
        { "geiger", N_("Radiación: cuentas por minuto y dosis (tubo Geiger)") },
        { "input", N_("Una entrada digital: un botón, un contacto, un módulo KY") },
        { "pir", N_("Movimiento: un PIR (HC-SR501, AM312) o un radar RCWL-0516") },
        /* UART */
        { "mhz19", N_("CO₂ real (NDIR) por puerto serie") }, { "senseair_s8", N_("CO₂ real (NDIR), Senseair S8") },
        { "pms5003", N_("Partículas PM1, PM2,5 y PM10 por puerto serie") }, { "sds011", N_("Partículas PM2,5 y PM10 por puerto serie") },
        { "pzem004t", N_("Energía de 220 V: tensión, corriente, potencia y energía") },
        { "ld2410", N_("Presencia humana por radar") },
        { "ld2450", N_("Presencia y posición de hasta tres personas por radar") },
    };
    if (!id) return "";
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) if (!strcmp(W[i][0], id)) return _(W[i][1]);
    return "";
}

const char *aos_sensor_cat_name(int cat)
{
    switch (cat) {
    case AOS_SENSOR_CAT_TEMP: return _("Temperatura y humedad");
    case AOS_SENSOR_CAT_ENV: return _("Presión y clima");
    case AOS_SENSOR_CAT_AIR: return _("Calidad del aire");
    case AOS_SENSOR_CAT_LIGHT: return _("Luz y color");
    case AOS_SENSOR_CAT_MOTION: return _("Movimiento y brújula");
    case AOS_SENSOR_CAT_POWER: return _("Tensión, corriente y ADC");
    case AOS_SENSOR_CAT_DIST: return _("Distancia y presencia");
    default: return _("Otros");
    }
}
