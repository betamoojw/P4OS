/*
 * P4OS - The sensor service's values and errors in words (aos_sensor_text.c),
 * so that Módulos, the portal and the apps (Datalog) say the same thing.
 * Everything comes back in the language the board is in; the values use
 * the decimal comma.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "temp" -> "Temperatura", "pm25" -> "PM2,5", "ax" -> "Aceleración X";
 * an unknown key comes back as it is */
const char *aos_sensor_key_name(const char *key);
/* A glyph of aos_sym_28/44/72 for a value (aos_sensor_q_t, and its key) */
const char *aos_sensor_glyph(int q, const char *key);
/* A value with its unit, scaled the way a bench wants it ("12,3 mA",
 * "1,250 V", "412 ppm"); NAN gives "--" */
void        aos_sensor_fmt(int q, float v, char *out, size_t n);
/* The decimals aos_sensor_fmt() would show, for apps that build their own */
int         aos_sensor_decimals(int q, float v);
/* The service's "why:detail" (aos_sensor_t.err) in words */
void        aos_sensor_err_text(const char *err, char *out, size_t n);
/* What a chip measures, in a line ("Temperatura, presión y humedad"); ""
 * for one it does not know */
const char *aos_sensor_chip_what(const char *id);

#ifdef __cplusplus
}
#endif
