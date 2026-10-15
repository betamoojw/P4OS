/*
 * P4OS - Programmer: the flashing service on top of esp-serial-flasher.
 *
 * One job at a time, in a thread of its own (aos_hal_thread_start), so it
 * outlives the app: the Programador app starts it and draws aos_flasher_status()
 * and the log, the service does the rest with the app open or closed.
 *
 * The port layer (esp_loader_port_ops_t) is aos_io, nothing of ESP-IDF, so
 * the same file runs in the simulator against tools/fake_esp_rom.py:
 *   read/write         aos_io_uart_read/_write, with the library's timeouts;
 *                      a cancel makes them fail, which unwinds any command
 *   enter_bootloader   esptool's classic sequence on aos_io_uart_lines: EN
 *                      low with BOOT high, then BOOT low and EN released in
 *                      one step, then BOOT released. It is the only order
 *                      that also works through the usual two-transistor
 *                      auto-reset circuit (RTS/DTR in the simulator), where
 *                      both lines low at once cannot exist.
 *   reset_target       EN low, then released with BOOT high: the program runs
 *   baud               aos_io_uart_set_baud
 * Without EN/BOOT on the port (no "en=/boot=" in modules.txt) the job asks
 * for the target to be put in download mode by hand and keeps trying SYNC
 * for a few seconds.
 *
 * A flash job: open the port at 115200, connect and upload the flasher stub
 * (the ROM itself if the stub does not come up, or if asked), read the chip
 * (name, revision, MAC, flash size, security), refuse a build for another
 * chip or a flash that is encrypted, go to the fast speed (460800 by
 * default) and check the link with a register read, falling back to 115200
 * with a new reset if it does not answer; then every file: FLASH_BEGIN
 * (which erases), the data in blocks read from the card, and the MD5 the
 * target computes of what it has against the one of what was sent; at the
 * end a hard reset so the new program runs, and the port closes, which lets
 * EN and BOOT go.
 *
 * Blocks: 16 KB with the stub at 460800 and up, 4 KB below (a 16 KB block at
 * 115200 takes 1.4 s on the wire and the library gives each block 1 s), and
 * 1 KB for the ROM, esptool's sizes. Files are padded with 0xFF to a
 * multiple of four, as esptool does; the MD5 covers the padded image.
 *
 * Not here yet: compressed writes (FLASH_DEFL_*, the stub supports them but
 * it needs a deflate that the simulator also has), downloads from a URL, the
 * RFC2217 bridge (APPS.md: `esptool --port rfc2217://p4os.local:4000`), which
 * would be a second kind of job holding the port and piping a TCP socket to
 * it, with aos_io_uart_lines for its SET_CONTROL, and STM32's UART bootloader.
 */
#include "aos_flasher.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("esp_loader.h")
#define HAVE_ESF 1
#include "esp_loader.h"
#include "esp_loader_io.h"
#include "esp_loader_stubs.h"
#else
#define HAVE_ESF 0
#endif

#define LOG_LINES   200
#define BAUD_FAST   460800
#define BLOCK_MAX   16384

/* -------------------------------------------------------------------------- */
/* State and the log                                                           */
/* -------------------------------------------------------------------------- */

static void *s_mutex;
static aos_flasher_status_t s_st;
static volatile bool s_cancel;

static struct {
    char    (*text)[AOS_FLASHER_LOG_LINE];
    uint8_t *level;
    uint32_t count, first;
} s_log;

static void lock(void)
{
    if (!s_mutex) s_mutex = aos_hal_mutex_create();
    aos_hal_mutex_lock(s_mutex);
}
static void unlock(void) { aos_hal_mutex_unlock(s_mutex); }

enum { L_INFO = 0, L_OK, L_WARN, L_ERR };

static void log_v(int level, const char *fmt, va_list ap)
{
    char line[AOS_FLASHER_LOG_LINE];
    vsnprintf(line, sizeof line, fmt, ap);
    aos_hal_log("flasher", "%s", line);
    lock();
    if (!s_log.text) {
        s_log.text = calloc(LOG_LINES, AOS_FLASHER_LOG_LINE);
        s_log.level = calloc(LOG_LINES, 1);
    }
    if (s_log.text) {
        uint32_t i = s_log.count % LOG_LINES;
        memcpy(s_log.text[i], line, sizeof line);
        s_log.level[i] = (uint8_t)level;
        s_log.count++;
        if (s_log.count - s_log.first > LOG_LINES) s_log.first = s_log.count - LOG_LINES;
    }
    unlock();
}

static void logf_(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_v(level, fmt, ap);
    va_end(ap);
}

uint32_t aos_flasher_log_count(void) { return s_log.count; }
uint32_t aos_flasher_log_first(void) { return s_log.first; }

int aos_flasher_log_line(uint32_t index, char *out, int cap)
{
    int level = -1;
    lock();
    if (s_log.text && index >= s_log.first && index < s_log.count && cap > 0) {
        snprintf(out, (size_t)cap, "%s", s_log.text[index % LOG_LINES]);
        level = s_log.level[index % LOG_LINES];
    }
    unlock();
    return level;
}

void aos_flasher_status(aos_flasher_status_t *out)
{
    lock();
    *out = s_st;
    if (s_st.busy && s_st.job) out->elapsed_ms = (uint32_t)aos_hal_uptime_ms() - s_st.elapsed_ms;
    unlock();
}

bool aos_flasher_busy(void) { return s_st.busy; }
void aos_flasher_cancel(void) { if (s_st.busy) s_cancel = true; }

/* The job's own copy of what it works with */
static struct {
    aos_flasher_job_t job;
    char port[AOS_IO_PORT_NAME_MAX];
    aos_flasher_source_t src;
    aos_flasher_opts_t opts;
    uint64_t t0, write_t0, write_ms;
    uint32_t write_bytes;
} J;

static void set_phase(aos_flasher_phase_t ph)
{
    lock();
    s_st.phase = ph;
    unlock();
}

#if HAVE_ESF

/* -------------------------------------------------------------------------- */
/* The port: esp-serial-flasher over aos_io                                    */
/* -------------------------------------------------------------------------- */

#define RESET_HOLD_MS 100
#define BOOT_HOLD_MS  50

typedef struct {
    esp_loader_port_t port;         /* first, for container_of */
    aos_io_uart_t *u;
    uint64_t deadline;
    bool lines;
    bool jtag;                      /* the target's own USB-Serial-JTAG */
} fl_port_t;

static esp_loader_error_t p_read(esp_loader_port_t *port, uint8_t *data, uint16_t size, uint32_t timeout)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t end = aos_hal_uptime_ms() + timeout;
    int got = 0;
    for (;;) {
        if (s_cancel) return ESP_LOADER_ERROR_FAIL;
        uint64_t now = aos_hal_uptime_ms();
        int left = now < end ? (int)(end - now) : 0;
        int n = aos_io_uart_read(p->u, data + got, size - got, left > 50 ? 50 : left);
        if (n < 0) return ESP_LOADER_ERROR_FAIL;
        got += n;
        if (got >= size) return ESP_LOADER_SUCCESS;
        if (!left) return ESP_LOADER_ERROR_TIMEOUT;
    }
}

static esp_loader_error_t p_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t end = aos_hal_uptime_ms() + (timeout < 1000 ? 1000 : timeout);
    int done = 0;
    while (done < size) {
        if (s_cancel) return ESP_LOADER_ERROR_FAIL;
        int n = aos_io_uart_write(p->u, data + done, size - done);
        if (n < 0) return ESP_LOADER_ERROR_FAIL;
        done += n;
        if (done < size) {
            if (aos_hal_uptime_ms() > end) return ESP_LOADER_ERROR_TIMEOUT;
            aos_hal_sleep_ms(2);
        }
    }
    return ESP_LOADER_SUCCESS;
}

static void p_enter_bootloader(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    if (p->jtag) {
        /* esptool's USBJTAGSerialReset, step for step: the chip's USB logic
         * resets it when RTS rises with DTR low, into the ROM if DTR was
         * high just before. It goes through (1,1), never (0,0). */
        aos_io_uart_dtr_rts(p->u, false, false);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, true, false);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, true, true);
        aos_io_uart_dtr_rts(p->u, false, true);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, false, false);
        return;
    }
    if (!p->lines) return;
    aos_io_uart_lines(p->u, 0, 1);      /* held in reset, GPIO0 high */
    aos_hal_sleep_ms(RESET_HOLD_MS);
    aos_io_uart_lines(p->u, 1, 0);      /* GPIO0 low, then EN released: the ROM samples it low */
    aos_hal_sleep_ms(BOOT_HOLD_MS);
    aos_io_uart_lines(p->u, -1, 1);
}

static void p_reset_target(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    if (p->jtag) {
        /* RTS alone: a reset with DTR low, into the application */
        aos_io_uart_dtr_rts(p->u, false, true);
        aos_hal_sleep_ms(200);
        aos_io_uart_dtr_rts(p->u, false, false);
        return;
    }
    if (!p->lines) return;
    aos_io_uart_lines(p->u, 0, 1);
    aos_hal_sleep_ms(RESET_HOLD_MS);
    aos_io_uart_lines(p->u, 1, 1);
}

/* esp-serial-flasher gives an erase 10 s per MB; esptool gives it 30. A
 * 16 MB flash erasing 4 KB sectors at ~45 ms each needs ~11.5 s per MB.
 * Above 3 s a timeout is one of the library's per-MB ones (erase, MD5,
 * loading to RAM), never one that waits for an answer, so those are
 * tripled: esptool's margin. */
static void p_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    if (ms > 3000) ms *= 3;
    container_of(port, fl_port_t, port)->deadline = aos_hal_uptime_ms() + ms;
}

static uint32_t p_remaining_time(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t now = aos_hal_uptime_ms();
    return p->deadline > now ? (uint32_t)(p->deadline - now) : 0;
}

static void p_delay_ms(esp_loader_port_t *port, uint32_t ms) { (void)port; aos_hal_sleep_ms(ms); }

static void p_log(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt, va_list args)
{
    (void)port;
    /* the chip-detection probe: GET_SECURITY_INFO is refused by the ESP32's
     * and the ESP8266's ROMs, which the library reports as an error */
    if (s_st.phase == AOS_FLASHER_CONNECTING && !strncmp(fmt, "Protocol error", 14)) return;
    if (s_cancel) return;           /* a cancel fails the command in flight: its retries are noise */
    log_v(level <= ESP_LOADER_LOG_LEVEL_ERROR ? L_ERR : level == ESP_LOADER_LOG_LEVEL_WARN ? L_WARN : L_INFO, fmt, args);
}

static esp_loader_error_t p_baud(esp_loader_port_t *port, uint32_t rate)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    return aos_io_uart_set_baud(p->u, rate) ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static const esp_loader_port_ops_t PORT_OPS = {
    .enter_bootloader = p_enter_bootloader,
    .reset_target = p_reset_target,
    .start_timer = p_start_timer,
    .remaining_time = p_remaining_time,
    .delay_ms = p_delay_ms,
    .log = p_log,
    .change_transmission_rate = p_baud,
    .write = p_write,
    .read = p_read,
};

/* -------------------------------------------------------------------------- */
/* The job                                                                     */
/* -------------------------------------------------------------------------- */

static const char *const CHIP_IDF[ESP_MAX_CHIP] = {
    [ESP8266_CHIP] = "esp8266", [ESP32_CHIP] = "esp32", [ESP32S2_CHIP] = "esp32s2",
    [ESP32C3_CHIP] = "esp32c3", [ESP32S3_CHIP] = "esp32s3", [ESP32C2_CHIP] = "esp32c2",
    [ESP32C5_CHIP] = "esp32c5", [ESP32H2_CHIP] = "esp32h2", [ESP32C6_CHIP] = "esp32c6",
    [ESP32P4_CHIP] = "esp32p4", [ESP32C61_CHIP] = "esp32c61", [ESP32S31_CHIP] = "esp32s31",
    [ESP32H21_CHIP] = "esp32h21", [ESP32H4_CHIP] = "esp32h4",
};

static const char *err_text(esp_loader_error_t e)
{
    switch (e) {
    case ESP_LOADER_SUCCESS: return _("bien");
    case ESP_LOADER_ERROR_TIMEOUT: return _("no respondió a tiempo");
    case ESP_LOADER_ERROR_IMAGE_SIZE: return _("la imagen no entra en la flash");
    case ESP_LOADER_ERROR_INVALID_MD5: return _("el MD5 no coincide");
    case ESP_LOADER_ERROR_INVALID_PARAM: return _("parámetro inválido");
    case ESP_LOADER_ERROR_INVALID_TARGET: return _("chip desconocido");
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return _("chip no soportado");
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC: return _("el chip no lo soporta");
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return _("respuesta inválida");
    default: return _("falló");
    }
}

/* Flash encryption, as esptool decides it: an odd number of bits set in
 * FLASH_CRYPT_CNT. esp-serial-flasher (2.1.0, and its main branch on
 * 2026-10-15) counts the bits of the key purposes instead, so an ESP32-P4
 * v3.1 with nothing encrypted came out "encrypted" and could not be
 * flashed. Its GET_SECURITY_INFO is asked again here, the reply laid out as
 * the library's protocol.h has it (an ESP32-S2's lacks the last 8 bytes). */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint8_t  flash_crypt_cnt;
    uint8_t  key_purposes[7];
    uint32_t chip_id;
    uint32_t eco_version;
} sec_reply_t;
extern esp_loader_error_t loader_get_security_info_cmd(esp_loader_t *loader, sec_reply_t *reply, uint32_t *got);

typedef struct {
    esp_loader_t L;
    fl_port_t P;
    aos_io_uart_t *u;
    uint8_t *buf;
} ctx_t;

static bool failf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool failf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lock();
    vsnprintf(s_st.error, sizeof s_st.error, fmt, ap);
    unlock();
    va_end(ap);
    logf_(L_ERR, "%s", s_st.error);
    return false;
}

static void progress(uint32_t add)
{
    lock();
    s_st.done += add;
    s_st.percent = s_st.total ? (int)((uint64_t)s_st.done * 100 / s_st.total) : 0;
    if (J.write_ms > 300) {
        s_st.bytes_per_s = (uint32_t)((uint64_t)J.write_bytes * 1000 / J.write_ms);
        s_st.eta_ms = s_st.bytes_per_s ? (uint32_t)((uint64_t)(s_st.total - s_st.done) * 1000 / s_st.bytes_per_s) : 0;
    }
    unlock();
}

/* ESP32-P4 v3.1 and v3.2 (ECO6, ECO7) come out of reset with their flash
 * unpowered: the default went from 1.8 V to 3.3 V, so it stays off until
 * someone powers it at the voltage the eFuses say. esptool does it right
 * after connecting (esptool 5.3.1, targets/esp32p4.py, power_on_flash);
 * esp-serial-flasher does not yet (its issue 184), and an ESP32-P4-Nano
 * (v3.1) answered, read its MAC and then hung on the first erase. The same
 * register writes, through the ROM, before the stub goes up. */
#define P4_EFUSE_RD_REPEAT_DATA1    0x5012D034u
#define P4_EFUSE_DL_XPD_ON          (1u << 16)
#define P4_LP_SYS_XPD_PAD_GROUP     0x5011010Cu
#define P4_PMU_EXT_LDO_P0_0P1A      0x501151B8u
#define P4_PMU_EXT_LDO_P0_0P1A_ANA  0x501151BCu
#define P4_PMU_DATE                 0x501153FCu
#define P4_ANA_0P1A_EN_CUR_LIM_0    (1u << 27)
#define P4_0P1A_TARGET0_0           (0xFFu << 23)
#define P4_0P1A_FORCE_TIEH_SEL_0    (1u << 7)

static esp_loader_error_t reg_or(esp_loader_t *l, uint32_t a, uint32_t set, uint32_t clear)
{
    uint32_t v;
    esp_loader_error_t e = esp_loader_read_register(l, a, &v);
    if (e != ESP_LOADER_SUCCESS) return e;
    return esp_loader_write_register(l, a, (v | set) & ~clear);
}

static void p4_power_on_flash(esp_loader_t *l)
{
    uint16_t rev = 0xFFFF;
    if (esp_loader_get_chip_revision(l, &rev) != ESP_LOADER_SUCCESS || (rev != 301 && rev != 302)) return;
    esp_loader_target_security_info_t si;
    if (esp_loader_get_security_info(l, &si) == ESP_LOADER_SUCCESS && si.secure_download_mode_enabled) return;
    uint32_t v = 0;
    if (rev == 302 && esp_loader_read_register(l, P4_EFUSE_RD_REPEAT_DATA1, &v) == ESP_LOADER_SUCCESS &&
        (v & P4_EFUSE_DL_XPD_ON)) {
        /* the ROM powered it already; only release its force-on */
        esp_loader_write_register(l, P4_PMU_DATE, 0);
        return;
    }
    esp_loader_error_t e = esp_loader_write_register(l, P4_LP_SYS_XPD_PAD_GROUP, 1);
    aos_hal_sleep_ms(10);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A_ANA, P4_ANA_0P1A_EN_CUR_LIM_0, 0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, P4_0P1A_FORCE_TIEH_SEL_0, 0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_DATE, 3, 0);
    aos_hal_sleep_ms(1);                                    /* esptool: 50 us */
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A_ANA, 0, P4_ANA_0P1A_EN_CUR_LIM_0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0, P4_0P1A_TARGET0_0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0x80, 0);     /* the eFuses' voltage */
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0, P4_0P1A_FORCE_TIEH_SEL_0);
    aos_hal_sleep_ms(2);                                    /* esptool: 1.8 ms */
    aos_hal_log("flasher", "ESP32-P4 v%u.%u: flash powered on (%s)", rev / 100, rev % 100,
                e == ESP_LOADER_SUCCESS ? "ok" : "a register write failed");
}

/* The stub the library would pick, with the P4's flash powered first. The
 * library's own provider is static: its table is copied here. */
static const esp_stub_t *stub_for(esp_loader_t *loader, target_chip_t chip, void *ctx)
{
    (void)ctx;
    switch (chip) {
    case ESP8266_CHIP:  return &esp_stub_esp8266;
    case ESP32_CHIP:    return &esp_stub_esp32;
    case ESP32S2_CHIP:  return &esp_stub_esp32s2;
    case ESP32C3_CHIP:  return &esp_stub_esp32c3;
    case ESP32S3_CHIP:  return &esp_stub_esp32s3;
    case ESP32C2_CHIP:  return &esp_stub_esp32c2;
    case ESP32C5_CHIP:  return &esp_stub_esp32c5;
    case ESP32H2_CHIP:  return &esp_stub_esp32h2;
    case ESP32C6_CHIP:  return &esp_stub_esp32c6;
    case ESP32C61_CHIP: return &esp_stub_esp32c61;
    case ESP32S31_CHIP: return &esp_stub_esp32s31;
    case ESP32H21_CHIP: return &esp_stub_esp32h21;
    case ESP32H4_CHIP:  return &esp_stub_esp32h4;
    case ESP32P4_CHIP: {
        p4_power_on_flash(loader);
        esp_loader_target_security_info_t si;
        bool v3 = esp_loader_get_security_info(loader, &si) == ESP_LOADER_SUCCESS && si.eco_version >= 5;
        return v3 ? &esp_stub_esp32p4 : &esp_stub_esp32p4rev1;
    }
    default:            return NULL;
    }
}

static bool connect(ctx_t *c, bool stub)
{
    set_phase(AOS_FLASHER_CONNECTING);
    aos_io_uart_set_baud(c->u, 115200);
    esp_loader_init_serial(&c->L, &c->P.port);
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    if (!c->P.lines) args.trials = 80;      /* about ten seconds to press the buttons */
    esp_loader_error_t e;
    if (stub) {
        e = esp_loader_connect_with_stub_provider(&c->L, &args, stub_for, NULL);
        if (e != ESP_LOADER_SUCCESS && !s_cancel && esp_loader_get_target(&c->L) != ESP_UNKNOWN_CHIP) {
            /* it answered but the stub did not come up: the ROM can do it all, slower */
            logf_(L_WARN, _("El stub no arrancó (%s): sigo con la ROM"), err_text(e));
            esp_loader_init_serial(&c->L, &c->P.port);
            e = esp_loader_connect(&c->L, &args);
            stub = false;
        }
    } else {
        e = esp_loader_connect(&c->L, &args);
    }
    if (e == ESP_LOADER_SUCCESS && !stub && esp_loader_get_target(&c->L) == ESP32P4_CHIP)
        p4_power_on_flash(&c->L);
    if (s_cancel) return false;
    if (e != ESP_LOADER_SUCCESS)
        return failf(e == ESP_LOADER_ERROR_TIMEOUT ? _("La placa no respondió: revisá TX/RX cruzados, GND, EN y BOOT")
                                                   : _("No se pudo conectar: %s"), err_text(e));
    lock();
    s_st.stub = stub;
    s_st.baud = 115200;
    unlock();
    return true;
}

static bool read_info(ctx_t *c)
{
    set_phase(AOS_FLASHER_INFO);
    target_chip_t t = esp_loader_get_target(&c->L);
    const char *idf = t < ESP_MAX_CHIP ? CHIP_IDF[t] : NULL;
    uint8_t mac[6] = { 0 };
    bool mac_ok = t != ESP8266_CHIP && esp_loader_read_mac(&c->L, mac) == ESP_LOADER_SUCCESS;
    uint16_t rev = 0xFFFF;
    if (esp_loader_get_chip_revision(&c->L, &rev) != ESP_LOADER_SUCCESS) rev = 0xFFFF;
    bool secure = false, crypt = false;
    esp_loader_target_security_info_t si;
    if (t != ESP32_CHIP && t != ESP8266_CHIP && esp_loader_get_security_info(&c->L, &si) == ESP_LOADER_SUCCESS) {
        secure = si.secure_boot_enabled || si.secure_download_mode_enabled;
        sec_reply_t r;
        uint32_t got = 0;
        p_start_timer(&c->P.port, 100);
        if (loader_get_security_info_cmd(&c->L, &r, &got) == ESP_LOADER_SUCCESS && got >= 5)
            crypt = __builtin_popcount(r.flash_crypt_cnt) & 1;
    }
    uint32_t flash = 0;
    if (esp_loader_flash_detect_size(&c->L, &flash) != ESP_LOADER_SUCCESS) flash = 0;
    lock();
    s_st.chip_known = true;
    snprintf(s_st.chip, sizeof s_st.chip, "%s", idf ? aos_flasher_chip_label(idf) : "?");
    s_st.revision = rev;
    memcpy(s_st.mac, mac, 6);
    s_st.mac_known = mac_ok;
    s_st.flash_size = flash;
    s_st.secure = secure || crypt;
    unlock();
    char revs[12] = "";
    if (rev != 0xFFFF) snprintf(revs, sizeof revs, " v%u.%u", rev / 100, rev % 100);
    char macs[24] = "";
    if (mac_ok) snprintf(macs, sizeof macs, ", MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char fl[16] = "?";
    if (flash) snprintf(fl, sizeof fl, "%u MB", (unsigned)(flash >> 20));
    logf_(L_OK, "%s%s%s, flash %s%s", s_st.chip, revs, macs, fl, s_st.stub ? ", stub" : ", ROM");
    if (secure) logf_(L_WARN, "%s", _("Secure boot o descarga segura activos"));
    if (crypt && J.job == AOS_FLASHER_JOB_FLASH)
        return failf("%s", _("La flash está cifrada: grabarla en claro la deja sin arrancar"));
    if (J.job == AOS_FLASHER_JOB_FLASH && J.src.chip[0] && idf && strcasecmp(J.src.chip, idf))
        return failf(_("El firmware es para %s y la placa es %s"), aos_flasher_chip_label(J.src.chip), s_st.chip);
    /* the bootloader refuses to start outside its range: say it now, not
     * after a flash that leaves the board without a program */
    if (J.job == AOS_FLASHER_JOB_FLASH && J.src.rev_max && rev != 0xFFFF &&
        (rev < J.src.rev_min || rev > J.src.rev_max))
        return failf(_("El firmware acepta la revisión v%u.%u a v%u.%u y el chip es v%u.%u: no arrancaría"),
                     J.src.rev_min / 100, J.src.rev_min % 100, J.src.rev_max / 100, J.src.rev_max % 100,
                     rev / 100, rev % 100);
    return true;
}

/* The fast speed, checked with a read; if it does not answer, back to 115200
 * through a fresh reset. */
static bool go_fast(ctx_t *c)
{
    uint32_t baud = J.opts.baud ? J.opts.baud : BAUD_FAST;
    target_chip_t t = esp_loader_get_target(&c->L);
    if (baud <= 115200 || t == ESP8266_CHIP) return true;
    set_phase(AOS_FLASHER_BAUD);
    esp_loader_error_t e = esp_loader_change_transmission_rate(&c->L, baud);
    uint8_t mac[6];
    if (e == ESP_LOADER_SUCCESS) e = esp_loader_read_mac(&c->L, mac);
    if (s_cancel) return false;
    if (e == ESP_LOADER_SUCCESS) {
        logf_(L_INFO, _("Velocidad: %u baudios"), (unsigned)baud);
        lock();
        s_st.baud = baud;
        unlock();
        return true;
    }
    logf_(L_WARN, _("A %u baudios no contesta: vuelvo a 115200"), (unsigned)baud);
    if (!c->P.lines) return failf(_("La placa quedó a %u baudios: reseteala y probá de nuevo"), (unsigned)baud);
    return connect(c, s_st.stub);
}

static bool write_file(ctx_t *c, int i)
{
    const aos_flasher_file_t *f = &J.src.files[i];
    char path[330];
    if (J.src.project) snprintf(path, sizeof path, "%s/%s", J.src.path, f->name);
    else snprintf(path, sizeof path, "%s", J.src.path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return failf(_("No se pudo abrir %s"), f->name);

    target_chip_t t = esp_loader_get_target(&c->L);
    bool stub = s_st.stub;
    uint32_t block = !stub ? 1024 : s_st.baud >= 460800 ? BLOCK_MAX : 4096;
    uint32_t padded = (f->size + 3) & ~3u;
    esp_loader_flash_cfg_t cfg = {
        .offset = f->offset, .image_size = padded, .block_size = block,
        /* the ESP8266's ROM has no MD5; its stub has */
        .skip_verify = J.opts.no_verify || (t == ESP8266_CHIP && !stub),
    };
    lock();
    s_st.file_index = i;
    snprintf(s_st.file_name, sizeof s_st.file_name, "%s", f->name);
    s_st.file_offset = f->offset;
    s_st.phase = AOS_FLASHER_ERASING;
    unlock();
    logf_(L_INFO, "0x%05x %s: %u KB", (unsigned)f->offset, f->name, (unsigned)((f->size + 1023) / 1024));
    esp_loader_error_t e = esp_loader_flash_start(&c->L, &cfg);
    if (e != ESP_LOADER_SUCCESS) { fclose(fp); return s_cancel ? false : failf(_("0x%x: no empezó (%s)"), (unsigned)f->offset, err_text(e)); }

    set_phase(AOS_FLASHER_WRITING);
    uint32_t left = padded, file_left = f->size;
    while (left && !s_cancel) {
        uint32_t want = left < block ? left : block;
        size_t n = fread(c->buf, 1, want, fp);
        if (n < want) memset(c->buf + n, 0xFF, want - n);      /* the padding to four */
        if (n == 0 && want > 3) { fclose(fp); return failf(_("%s: no se pudo leer de la tarjeta"), f->name); }
        uint64_t t0 = aos_hal_uptime_ms();
        e = esp_loader_flash_write(&c->L, &cfg, c->buf, want);
        J.write_ms += aos_hal_uptime_ms() - t0;
        J.write_bytes += want;
        if (e != ESP_LOADER_SUCCESS) { fclose(fp); return s_cancel ? false : failf(_("0x%x: falló la escritura (%s)"), (unsigned)(f->offset + padded - left), err_text(e)); }
        left -= want;
        uint32_t add = want < file_left ? want : file_left;     /* the padding is not the file's */
        file_left -= add;
        progress(add);
    }
    fclose(fp);
    if (s_cancel) return false;
    if (!cfg.skip_verify) set_phase(AOS_FLASHER_VERIFYING);
    e = esp_loader_flash_finish(&c->L, &cfg);
    if (e != ESP_LOADER_SUCCESS)
        return s_cancel ? false : failf("0x%x %s: %s", (unsigned)f->offset, f->name, err_text(e));
    if (cfg.skip_verify) {
        lock();
        s_st.verified = false;
        unlock();
    }
    logf_(L_OK, "0x%05x %s%s", (unsigned)f->offset, f->name, cfg.skip_verify ? _(": escrito") : _(": escrito, MD5 bien"));
    return true;
}

static bool run(ctx_t *c)
{
    set_phase(AOS_FLASHER_OPENING);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    bool usb = !strncmp(J.port, "usb", 3);      /* a USB serial port of the host */
    if (!usb && (!pt || pt->kind != AOS_PORT_UART)) return failf(_("No hay un puerto serie %s"), J.port);
    c->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    /* A USB device can be away for a moment: a chip on its own USB that
     * restarts (an ESP32-C3 in a boot loop was gone 0.4 s out of every
     * 2.7, 2026-10-05) comes back as a new device. Up to 3 s for it. */
    for (int i = 0; !c->u && usb && i < 30 && !s_cancel; i++) {
        aos_hal_sleep_ms(100);
        c->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    }
    if (!c->u && usb) return failf(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
    if (!c->u) {
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? failf(_("%s está ocupado: lo tiene %s"), J.port, o) : failf(_("No se pudo abrir %s"), J.port);
    }
    c->P.port.ops = &PORT_OPS;
    c->P.u = c->u;
    c->P.lines = aos_io_uart_lines(c->u, -1, -1);
    c->P.jtag = aos_io_uart_is_usb_jtag(c->u);
    lock();
    s_st.lines = c->P.lines;
    unlock();
    int en = -1, boot = -1;
    aos_io_port_lines(J.port, &en, &boot);
    if (c->P.jtag) logf_(L_INFO, _("%s: la USB-Serial-JTAG del chip, se reinicia sola"), aos_io_uart_desc(c->u));
    else if (usb) logf_(L_INFO, _("%s, EN por RTS y BOOT por DTR"), aos_io_uart_desc(c->u));
    else if (c->P.lines) logf_(L_INFO, _("%s, EN GPIO%d, BOOT GPIO%d"), aos_io_uart_desc(c->u), en, boot);
    else if (en >= 0 || boot >= 0) logf_(L_WARN, _("%s: EN/BOOT ocupados por %s"), aos_io_uart_desc(c->u), aos_io_owner(en >= 0 ? en : boot) ? aos_io_owner(en >= 0 ? en : boot) : _("otro"));
    else logf_(L_WARN, _("%s sin EN/BOOT: poné la placa en modo descarga (BOOT apretado y reset)"), aos_io_uart_desc(c->u));

    bool stub = J.job == AOS_FLASHER_JOB_FLASH && !J.opts.no_stub;
    if (!connect(c, stub)) return false;
    if (!read_info(c)) return false;
    if (J.job == AOS_FLASHER_JOB_DETECT) return true;

    uint32_t end = 0;
    for (int i = 0; i < J.src.nfiles; i++) {
        uint32_t e = J.src.files[i].offset + J.src.files[i].size;
        if (e > end) end = e;
    }
    if (s_st.flash_size && end > s_st.flash_size)
        return failf(_("No entra: llega a %u KB y la flash tiene %u KB"), (unsigned)(end >> 10), (unsigned)(s_st.flash_size >> 10));
    if (!go_fast(c)) return false;
    lock();
    s_st.verified = !J.opts.no_verify;
    unlock();
    for (int i = 0; i < J.src.nfiles; i++) if (!write_file(c, i)) return false;
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    ctx_t *c = calloc(1, sizeof *c);
    bool ok = false;
    if (c) {
        c->buf = malloc(BLOCK_MAX);
        if (c->buf) ok = run(c);
        else failf("%s", _("Sin memoria"));
    } else {
        failf("%s", _("Sin memoria"));
    }
    bool cancelled = s_cancel && !ok;
    if (c && c->u) {
        /* the target back to its own program, unless asked to stay */
        bool reset = !(J.job == AOS_FLASHER_JOB_FLASH && ok && J.opts.no_reset);
        if (reset && c->P.lines) {
            set_phase(AOS_FLASHER_RESETTING);
            s_cancel = false;       /* a cancelled job still resets */
            esp_loader_reset_target(&c->L);
            if (J.job == AOS_FLASHER_JOB_FLASH && ok) logf_(L_INFO, "%s", _("Placa reiniciada"));
        } else if (ok && J.job == AOS_FLASHER_JOB_FLASH && !c->P.lines) {
            logf_(L_WARN, "%s", _("Reseteá la placa para que arranque el firmware nuevo"));
        }
        aos_io_uart_close(c->u);
    }
    if (c) free(c->buf);
    free(c);
    lock();
    s_st.ok = ok;
    if (ok) s_st.percent = 100;
    if (ok && J.job == AOS_FLASHER_JOB_FLASH) { s_st.done = s_st.total; s_st.eta_ms = 0; }
    s_st.phase = ok ? AOS_FLASHER_DONE : cancelled ? AOS_FLASHER_CANCELLED : AOS_FLASHER_FAILED;
    if (cancelled) snprintf(s_st.error, sizeof s_st.error, "%s", _("Cancelado"));
    s_st.elapsed_ms = (uint32_t)(aos_hal_uptime_ms() - J.t0);
    s_st.busy = false;
    s_st.seq++;
    unlock();
    uint32_t ms = s_st.elapsed_ms;
    if (ok && J.job == AOS_FLASHER_JOB_FLASH)
        logf_(L_OK, _("Listo en %u,%u s"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    else if (cancelled)
        logf_(L_WARN, "%s", _("Cancelado"));
    s_cancel = false;
}

static bool start(aos_flasher_job_t job, const char *port)
{
    lock();
    if (s_st.busy) { unlock(); return false; }
    J.job = job;
    snprintf(J.port, sizeof J.port, "%s", port ? port : "uart.b");
    J.t0 = aos_hal_uptime_ms();
    J.write_ms = 0;
    J.write_bytes = 0;
    s_cancel = false;
    /* what the last job knew about the chip goes: the next one may be another */
    aos_flasher_status_t keep = s_st;
    memset(&s_st, 0, sizeof s_st);
    s_st.seq = keep.seq + 1;
    s_st.busy = true;
    s_st.job = job;
    s_st.phase = AOS_FLASHER_OPENING;
    s_st.revision = 0xFFFF;
    snprintf(s_st.port, sizeof s_st.port, "%s", J.port);
    if (job == AOS_FLASHER_JOB_FLASH) {
        snprintf(s_st.source, sizeof s_st.source, "%s", J.src.name);
        s_st.file_count = J.src.nfiles;
        s_st.total = J.src.total;
    }
    s_st.elapsed_ms = (uint32_t)J.t0;      /* the start, while busy (see aos_flasher_status) */
    unlock();
    logf_(L_INFO, _("%s %s en %s"), job == AOS_FLASHER_JOB_DETECT ? _("Detectar") : _("Grabar"),
          job == AOS_FLASHER_JOB_DETECT ? "chip" : J.src.name, J.port);
    if (!aos_hal_thread_start("flasher", worker, NULL, 8192, 5)) {
        lock();
        s_st.busy = false;
        s_st.phase = AOS_FLASHER_FAILED;
        snprintf(s_st.error, sizeof s_st.error, "%s", _("No se pudo lanzar la tarea"));
        unlock();
        return false;
    }
    return true;
}

bool aos_flasher_detect(const char *port)
{
    if (s_st.busy) return false;
    return start(AOS_FLASHER_JOB_DETECT, port);
}

bool aos_flasher_flash(const char *port, const aos_flasher_source_t *src, const aos_flasher_opts_t *opts)
{
    if (s_st.busy || !src || !src->nfiles) return false;
    J.src = *src;
    if (opts) J.opts = *opts;
    else memset(&J.opts, 0, sizeof J.opts);
    return start(AOS_FLASHER_JOB_FLASH, port);
}

#else   /* no esp-serial-flasher: the simulator before any firmware build */

static bool unavailable(const char *port)
{
    (void)port;
    lock();
    s_st.phase = AOS_FLASHER_FAILED;
    s_st.seq++;
    snprintf(s_st.error, sizeof s_st.error, "%s", _("Falta esp-serial-flasher: compilá el firmware una vez (managed_components)"));
    unlock();
    logf_(L_ERR, "%s", s_st.error);
    return false;
}

bool aos_flasher_detect(const char *port) { return unavailable(port); }
bool aos_flasher_flash(const char *port, const aos_flasher_source_t *src, const aos_flasher_opts_t *opts)
{
    (void)src; (void)opts;
    return unavailable(port);
}

#endif
