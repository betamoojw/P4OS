/*
 * BLE - activating a Xiaomi sensor: the "register" of Xiaomi's standard
 * auth, as pvvx's TelinkMiFlasher does it ("Do Activation"), so the board
 * makes the bind key itself instead of a browser with Web Bluetooth or the
 * Mi Home account.
 *
 * Two characteristics of service 0xFE95 carry it: 0x0010 takes 4-byte
 * commands and notifies 4-byte verdicts, 0x0019 carries "parcels" both ways.
 * A parcel is a header (00 00 00 <cmd> <frames, 2 bytes LE>), the other side
 * acks it ready (00 00 01 01), the frames follow (<index, 2 bytes LE, from
 * 1> <up to 18 bytes>) and the other side acks the lot (00 00 01 00). A
 * short one comes in a single notification instead (00 00 02 <cmd> <data>).
 *
 *   1. 0x0010 <- A2 00 00 00: the sensor sends its id, 4 bytes and the DID.
 *   2. 0x0010 <- 15 00 00 00, then our P-256 public key as parcel 3; the
 *      sensor sends its own the same way.
 *   3. ECDH, and HKDF-SHA256 of the secret (no salt, "mible-setup-info")
 *      into 64 bytes: the token (12), the bind key (16) and a key A (16).
 *   4. The DID, AES-CCM with A (nonce 10 11 .. 1B, "devID" as associated
 *      data, 4-byte tag), goes back as parcel 0, which proves we hold the
 *      same secret.
 *   5. 0x0010 <- 13 00 00 00, and the sensor answers 11 00 00 00 when it
 *      has taken the keys, 12 00 00 00 when not.
 *
 * The sensor forgets its previous bind key: whoever read it with the old
 * one (Home Assistant, Mi Home) needs the new one. The crypto that is not
 * ours (P-256, HKDF, the random numbers) is the HAL's (aos_hal.h, Crypto);
 * CCM is bl_crypt.c's.
 */
#include "bl_mi.h"
#include "bl_crypt.h"

#include <string.h>

#define FRAME      18           /* payload per frame: 20-byte writes fit the default MTU */
#define TIMEOUT_MS 30000        /* the whole exchange; ECDH is slow on a sensor's chip */

static struct {
    bl_mi_state_t st;
    uint16_t h10, h19;
    bool rsp10, rsp19;
    int subs;
    uint32_t t0, wait_until;
    bool pending;               /* the register, after a short pause */
    /* what goes out */
    uint8_t tx[96];
    int tx_len;
    bool tx_on;
    /* what comes in */
    uint8_t rx[160];
    int rx_len, rx_count;
    uint8_t rx_cmd;
    uint8_t did[64];
    int did_len;
    uint8_t priv[32], pub[64];
    uint8_t key[16];
    const char *why;
    int code;
    char id[40];
} M;

static bool is16(const aos_ble_attr_t *a, uint16_t v)
{
    if (a->uuid_len == 2) return (a->uuid[0] | (a->uuid[1] << 8)) == v;
    /* a 16-bit UUID written out on Bluetooth's base, 0000xxxx-0000-1000-8000-00805F9B34FB */
    static const uint8_t BASE[12] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
    return a->uuid_len == 16 && !memcmp(a->uuid, BASE, 12) && (a->uuid[12] | (a->uuid[13] << 8)) == v &&
           !a->uuid[14] && !a->uuid[15];
}

typedef struct {
    uint16_t h10, h19;
    bool rsp10, rsp19;
} chars_t;

static bool find(const aos_ble_attr_t *at, int n, chars_t *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < n; i++) {
        if (at[i].kind != AOS_BLE_ATTR_SERVICE || !is16(&at[i], 0xFE95)) continue;
        for (int k = i + 1; k < n && at[k].kind != AOS_BLE_ATTR_SERVICE; k++) {
            const aos_ble_attr_t *a = &at[k];
            if (a->kind != AOS_BLE_ATTR_CHAR || !(a->props & 0x0C) || !(a->props & 0x10)) continue;
            if (is16(a, 0x0010)) {
                c->h10 = a->handle;
                c->rsp10 = (a->props & 0x08) != 0;
            } else if (is16(a, 0x0019)) {
                c->h19 = a->handle;
                c->rsp19 = !(a->props & 0x04);
            }
        }
    }
    return c->h10 && c->h19;
}

bool bl_mi_can(const aos_ble_attr_t *at, int n)
{
    chars_t c;
    return find(at, n, &c);
}

static void wipe(void)
{
    memset(M.priv, 0, sizeof M.priv);
    memset(M.tx, 0, sizeof M.tx);
    memset(M.rx, 0, sizeof M.rx);
}

static void fail(const char *why, int code)
{
    M.st = BL_MI_FAILED;
    M.why = why;
    M.code = code;
    M.pending = false;
    wipe();
}

static void w10(uint8_t cmd)
{
    uint8_t c[4] = { cmd, 0, 0, 0 };
    if (!aos_hal_ble_gatt_write(M.h10, c, 4, M.rsp10)) fail(N_("se cortó la conexión"), 0);
}

static void w19(const uint8_t *d, int n)
{
    if (M.st != BL_MI_FAILED && !aos_hal_ble_gatt_write(M.h19, d, (size_t)n, M.rsp19))
        fail(N_("se cortó la conexión"), 0);
}

static void ack(uint8_t code)
{
    uint8_t a[4] = { 0, 0, 1, code };
    w19(a, 4);
}

static void send_parcel(uint8_t cmd, const uint8_t *d, int n)
{
    memcpy(M.tx, d, n);
    M.tx_len = n;
    M.tx_on = true;
    int frames = (n + FRAME - 1) / FRAME;
    uint8_t h[6] = { 0, 0, 0, cmd, (uint8_t)frames, 0 };
    w19(h, 6);
}

static void send_frames(void)
{
    uint8_t f[2 + FRAME];
    for (int off = 0, i = 1; off < M.tx_len && M.st != BL_MI_FAILED; off += FRAME, i++) {
        int n = M.tx_len - off < FRAME ? M.tx_len - off : FRAME;
        f[0] = (uint8_t)i;
        f[1] = 0;
        memcpy(f + 2, M.tx + off, n);
        w19(f, 2 + n);
    }
}

static void start_register(void)
{
    w10(0x15);
    if (M.st == BL_MI_FAILED) return;
    if (!aos_hal_p256_keypair(M.priv, M.pub)) {
        fail(N_("no se pudo generar la clave"), 0);
        return;
    }
    M.st = BL_MI_KEYS;
    send_parcel(3, M.pub, 64);
}

static void derive(const uint8_t *peer)
{
    uint8_t secret[32], out[64], ct[64 + 4];
    static const uint8_t NONCE[12] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };
    static const char INFO[] = "mible-setup-info";
    bool ok = aos_hal_p256_ecdh(M.priv, peer, secret);
    memset(M.priv, 0, sizeof M.priv);
    if (!ok) {
        fail(N_("la clave del sensor no es válida"), 0);
        return;
    }
    ok = aos_hal_hkdf_sha256(NULL, 0, secret, sizeof secret, INFO, sizeof INFO - 1, out, sizeof out);
    memset(secret, 0, sizeof secret);
    /* out: the token (0..11), the bind key (12..27), A (28..43) */
    if (ok) memcpy(M.key, out + 12, 16);
    ok = ok && bl_ccm_encrypt(out + 28, NONCE, sizeof NONCE, (const uint8_t *)"devID", 5, M.did, M.did_len, 4, ct);
    memset(out, 0, sizeof out);
    if (!ok) {
        fail(N_("no se pudo derivar la clave"), 0);
        return;
    }
    M.st = BL_MI_DID;
    send_parcel(0, ct, M.did_len + 4);
}

static void got_parcel(uint8_t cmd, const uint8_t *d, int n, uint32_t now)
{
    if (M.st == BL_MI_INFO) {
        /* 4 bytes first, then the DID */
        if (n <= 4) {
            fail(N_("el sensor no dio su identificación"), 0);
            return;
        }
        M.did_len = n - 4 > (int)sizeof M.did ? (int)sizeof M.did : n - 4;
        memcpy(M.did, d + 4, M.did_len);
        int k = 0;
        for (int i = 1; i < M.did_len && k < (int)sizeof M.id - 1; i++)
            if (M.did[i] >= 0x20 && M.did[i] < 0x7F) M.id[k++] = (char)M.did[i];
        M.id[k] = 0;
        /* as the flasher: a moment before the register */
        M.pending = true;
        M.wait_until = now + 250;
    } else if (M.st == BL_MI_KEYS && cmd == 3 && n == 64) {
        derive(d);
    }
}

static void tx_done(void)
{
    M.tx_on = false;
    if (M.st == BL_MI_DID) {
        M.st = BL_MI_CONFIRM;
        w10(0x13);
    }
}

static void on19(const uint8_t *d, int n, uint32_t now)
{
    if (n < 2) return;
    int idx = d[0] | (d[1] << 8);
    if (idx == 0) {
        if (n == 4 && d[2] == 1) {
            /* the sensor's ack: ready for our frames, or got them all */
            if (d[3] == 1 && M.tx_on) send_frames();
            else if (d[3] == 0 && M.tx_on) tx_done();
            else if (d[3] > 1) fail(d[3] == 5 ? N_("el sensor se cansó de esperar") : N_("el sensor cortó el intercambio"), d[3]);
        } else if (n == 6 && d[2] == 0) {
            M.rx_cmd = d[3];
            M.rx_count = d[4] | (d[5] << 8);
            M.rx_len = 0;
            if (!M.rx_count || M.rx_count > 8) {
                fail(N_("el sensor mandó algo inesperado"), M.rx_count);
                return;
            }
            ack(1);
        } else if (n >= 4 && d[2] == 2) {
            ack(0);
            got_parcel(d[3], d + 4, n - 4, now);
        }
        return;
    }
    if (!M.rx_count) return;
    int k = n - 2;
    if (M.rx_len + k > (int)sizeof M.rx) k = (int)sizeof M.rx - M.rx_len;
    memcpy(M.rx + M.rx_len, d + 2, k);
    M.rx_len += k;
    if (idx >= M.rx_count) {
        M.rx_count = 0;
        ack(0);
        got_parcel(M.rx_cmd, M.rx, M.rx_len, now);
    }
}

static void on10(const uint8_t *d, int n)
{
    if (n < 1) return;
    switch (d[0]) {
    case 0x11:
        if (M.st == BL_MI_CONFIRM) {
            M.st = BL_MI_DONE;
            wipe();
        }
        break;
    case 0x12: fail(N_("el sensor rechazó el registro"), 0x12); break;
    case 0x14: fail(N_("el sensor no pudo verificar el registro"), 0x14); break;
    case 0xE1: fail(N_("el sensor dice que ya está registrado"), 0xE1); break;
    case 0xE3: fail(N_("el sensor pide que se apriete su botón antes"), 0xE3); break;
    default: break;
    }
}

bool bl_mi_start(const aos_ble_attr_t *at, int n, uint32_t now_ms)
{
    chars_t c;
    if (!find(at, n, &c)) return false;
    memset(&M, 0, sizeof M);
    M.h10 = c.h10;
    M.h19 = c.h19;
    M.rsp10 = c.rsp10;
    M.rsp19 = c.rsp19;
    M.st = BL_MI_SUB;
    M.t0 = now_ms;
    aos_hal_ble_gatt_subscribe(M.h10, 1);
    aos_hal_ble_gatt_subscribe(M.h19, 1);
    return true;
}

void bl_mi_event(const aos_ble_gatt_ev_t *e, uint32_t now_ms)
{
    if (!bl_mi_busy() || (e->handle != M.h10 && e->handle != M.h19)) return;
    switch (e->type) {
    case AOS_BLE_EV_SUBSCRIBE:
        if (e->status) fail(N_("no deja escuchar sus avisos"), e->status);
        else if (M.st == BL_MI_SUB && ++M.subs == 2) {
            M.st = BL_MI_INFO;
            w10(0xA2);
        }
        break;
    case AOS_BLE_EV_WRITE:
        if (e->status) fail(N_("no aceptó lo que se le escribió"), e->status);
        break;
    case AOS_BLE_EV_NOTIFY:
    case AOS_BLE_EV_INDICATE:
        if (e->handle == M.h19) on19(e->data, e->len, now_ms);
        else on10(e->data, e->len);
        break;
    default: break;
    }
}

void bl_mi_tick(uint32_t now_ms)
{
    if (!bl_mi_busy()) return;
    if (now_ms - M.t0 > TIMEOUT_MS) {
        fail(N_("el sensor no contestó a tiempo"), 0);
        return;
    }
    if (M.pending && (int32_t)(now_ms - M.wait_until) >= 0) {
        M.pending = false;
        start_register();
    }
}

void bl_mi_reset(void)
{
    memset(&M, 0, sizeof M);
}

bl_mi_state_t bl_mi_state(void) { return M.st; }
bool bl_mi_busy(void) { return M.st != BL_MI_IDLE && M.st != BL_MI_DONE && M.st != BL_MI_FAILED; }
const char *bl_mi_why(void) { return M.st == BL_MI_FAILED && M.why ? M.why : ""; }
int bl_mi_code(void) { return M.code; }
const char *bl_mi_id(void) { return M.id; }

bool bl_mi_key(uint8_t key[16])
{
    if (M.st != BL_MI_DONE) return false;
    memcpy(key, M.key, 16);
    return true;
}
