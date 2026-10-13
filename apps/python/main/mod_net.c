/*
 * P4OS - Python: the socket module, over the firmware's connection.
 *
 * A client socket as MicroPython's own: socket.getaddrinfo(), socket(),
 * connect(), send/recv, read/readinto/readline/write, settimeout(), close().
 * Underneath is the HAL's connection (aos_tls.c): a TCP socket, or TLS over
 * it checked against the system's certificate store, which is what makes
 * https:// work from Python with no certificates of its own.
 *
 * That connection is plain or encrypted from the start, while Python's
 * order is connect() first and wrap in TLS after (tls.SSLContext's
 * wrap_socket(), frozen/tls.py). So connect() only takes the address, and
 * the connection opens at the first write or read, or when wrap_socket()
 * asks for it encrypted. An error to connect comes out there. getaddrinfo()
 * does not resolve either: the name goes as it is, the connection resolves
 * it. No servers (bind, listen, accept) and no UDP.
 *
 * Named mod_*.c so tools/gen_upy.sh scans it for its names.
 */
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* from the HAL's aos_tls.h, which is not included: it pulls in mbedtls's
 * headers, and tools/gen_upy.sh scans this file with the Mac's compiler.
 * The connection is only handled through its pointer. */
typedef struct p4py_conn p4py_conn_t;
p4py_conn_t *aos_tls_new(void);
void aos_tls_free(p4py_conn_t *c);
int  aos_tls_open(p4py_conn_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why, size_t why_n);
bool aos_tls_send(p4py_conn_t *c, const void *buf, int len, int timeout_ms);
int  aos_tls_recv(p4py_conn_t *c, void *buf, int max, int timeout_ms);
#define TLS_ENCRYPT  0x01u
#define TLS_NODELAY  0x02u
#define TLS_TIMEOUT  (-2)
#define ERR_DNS      (-1)
#define ERR_CONNECT  (-2)
#define ERR_MEM      (-6)
#define ERR_NO_TIME  (-7)
#define ERR_TLS      (-8)

#define RBUF         1024           /* read ahead: readline() goes a byte at a time */
#define OPEN_MS      15000          /* to connect, and again to negotiate, without a timeout set */
#define SLICE_MS     200            /* a wait in slices, so Stop is heard */

typedef struct {
    mp_obj_base_t base;
    p4py_conn_t  *c;                /* NULL until it opens */
    char         *host;             /* GC heap */
    int           port;
    int           timeout_ms;       /* -1: wait for ever */
    bool          tls, eof, closed;
    uint16_t      rpos, rlen;
    uint8_t      *rbuf;             /* RBUF, GC heap */
} sock_t;

static void raise_os(int err, const char *msg)
{
    mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(err), mp_obj_new_str_from_cstr(msg) };
    nlr_raise(mp_obj_new_exception_args(&mp_type_OSError, 2, args));
}

static void sock_close_conn(sock_t *s)
{
    if (s->c) {
        aos_tls_free(s->c);         /* closes it too */
        s->c = NULL;
    }
    s->closed = true;
}

static void sock_open(sock_t *s)
{
    if (s->c) return;
    if (s->closed) mp_raise_OSError(MP_EBADF);
    if (!s->host) mp_raise_OSError(MP_ENOTCONN);
    s->c = aos_tls_new();
    if (!s->c) mp_raise_OSError(MP_ENOMEM);
    char why[120] = "";
    int to = s->timeout_ms > 0 ? s->timeout_ms : OPEN_MS;
    int rc = aos_tls_open(s->c, s->host, s->port, (s->tls ? TLS_ENCRYPT : 0) | TLS_NODELAY, to, why, sizeof why);
    if (rc == 0) return;
    aos_tls_free(s->c);
    s->c = NULL;
    s->closed = true;
    char msg[200];
    switch (rc) {
    case ERR_DNS:
        snprintf(msg, sizeof msg, "cannot resolve %s", s->host);
        raise_os(MP_EHOSTUNREACH, msg);
    case ERR_CONNECT:
        snprintf(msg, sizeof msg, "cannot connect to %s:%d", s->host, s->port);
        raise_os(MP_ECONNREFUSED, msg);
    case ERR_NO_TIME:
        raise_os(MP_EAGAIN, "TLS: the clock is not set yet, try again in a few seconds");
    case ERR_TLS:
        snprintf(msg, sizeof msg, "TLS: %s", why[0] ? why : "the server could not be verified");
        raise_os(MP_EIO, msg);
    case ERR_MEM:
        mp_raise_OSError(MP_ENOMEM);
    default:
        snprintf(msg, sizeof msg, "cannot open %s:%d (%d)", s->host, s->port, rc);
        raise_os(MP_EIO, msg);
    }
}

/* the next bytes, up to max, into rbuf: > 0, 0 at the end, -1 with *err */
static int sock_fill(sock_t *s, int *err, mp_uint_t max)
{
    if (s->eof) return 0;
    uint32_t t0 = mp_hal_ticks_ms();
    for (;;) {
        int wait = SLICE_MS;
        if (s->timeout_ms == 0) {
            wait = 1;
        } else if (s->timeout_ms > 0) {
            int left = s->timeout_ms - (int)(mp_hal_ticks_ms() - t0);
            if (left <= 0) {
                *err = MP_ETIMEDOUT;
                return -1;
            }
            if (left < wait) wait = left;
        }
        int n = aos_tls_recv(s->c, s->rbuf, (int)(max > 0x7fffffff ? 0x7fffffff : max), wait);
        if (n > 0) {
            s->rpos = 0;
            s->rlen = (uint16_t)n;
            return n;
        }
        if (n == 0) {
            s->eof = true;
            return 0;
        }
        if (n != TLS_TIMEOUT) {
            *err = MP_ECONNRESET;
            return -1;
        }
        if (s->timeout_ms == 0) {
            *err = MP_EAGAIN;
            return -1;
        }
        mp_handle_pending(true);    /* Stop raises here */
    }
}

/* --- the stream ------------------------------------------------------------------ */

static mp_uint_t sock_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    sock_open(s);
    if (size == 0) return 0;
    if (s->rpos >= s->rlen && size >= RBUF) {
        /* a big read goes straight into the caller's buffer: whatever the
         * connection has, up to its size, and not RBUF at a time */
        uint8_t *keep = s->rbuf;
        s->rbuf = (uint8_t *)buf;
        int n = sock_fill(s, errcode, size);
        s->rbuf = keep;
        s->rpos = s->rlen = 0;
        return n < 0 ? MP_STREAM_ERROR : (mp_uint_t)n;
    }
    if (s->rpos >= s->rlen) {
        int n = sock_fill(s, errcode, RBUF);
        if (n < 0) return MP_STREAM_ERROR;
        if (n == 0) return 0;
    }
    mp_uint_t n = s->rlen - s->rpos;
    if (n > size) n = size;
    memcpy(buf, s->rbuf + s->rpos, n);
    s->rpos += n;
    return n;
}

static mp_uint_t sock_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    sock_open(s);
    if (!aos_tls_send(s->c, buf, (int)size, s->timeout_ms > 0 ? s->timeout_ms : OPEN_MS)) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return size;
}

static mp_uint_t sock_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    (void)arg;
    switch (request) {
    case MP_STREAM_CLOSE:
        sock_close_conn(s);
        return 0;
    case MP_STREAM_FLUSH:
        return 0;
    default:
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

/* --- the methods ----------------------------------------------------------------- */

static mp_obj_t sock_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 0, 3, false);
    if (n_args >= 2 && mp_obj_get_int(args[1]) != 1) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("only SOCK_STREAM"));
    }
    sock_t *s = mp_obj_malloc_with_finaliser(sock_t, type);
    s->c = NULL;
    s->host = NULL;
    s->port = 0;
    s->timeout_ms = -1;
    s->tls = s->eof = s->closed = false;
    s->rpos = s->rlen = 0;
    s->rbuf = m_new(uint8_t, RBUF);
    return MP_OBJ_FROM_PTR(s);
}

static mp_obj_t sock_connect(mp_obj_t self_in, mp_obj_t addr_in)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    if (s->c || s->host) mp_raise_OSError(MP_EISCONN);
    mp_obj_t *addr;
    mp_obj_get_array_fixed_n(addr_in, 2, &addr);
    size_t n;
    const char *host = mp_obj_str_get_data(addr[0], &n);
    s->host = m_new(char, n + 1);
    memcpy(s->host, host, n);
    s->host[n] = 0;
    s->port = mp_obj_get_int(addr[1]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

/* tls.py's wrap_socket(): encrypted, under the name the certificate must have */
static mp_obj_t sock_tls(mp_obj_t self_in, mp_obj_t hostname_in)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    if (s->c) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("wrap the socket before sending or reading"));
    if (!s->host) mp_raise_OSError(MP_ENOTCONN);
    if (hostname_in != mp_const_none) {
        size_t n;
        const char *h = mp_obj_str_get_data(hostname_in, &n);
        s->host = m_new(char, n + 1);
        memcpy(s->host, h, n);
        s->host[n] = 0;
    }
    s->tls = true;
    sock_open(s);
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_tls_obj, sock_tls);

static mp_obj_t sock_settimeout(mp_obj_t self_in, mp_obj_t t_in)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    if (t_in == mp_const_none) {
        s->timeout_ms = -1;
    } else {
        mp_float_t t = mp_obj_get_float(t_in);
        s->timeout_ms = t <= 0 ? 0 : (int)(t * 1000 + 0.5);
        if (t > 0 && s->timeout_ms == 0) s->timeout_ms = 1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_obj_t sock_setblocking(mp_obj_t self_in, mp_obj_t flag)
{
    sock_t *s = MP_OBJ_TO_PTR(self_in);
    s->timeout_ms = mp_obj_is_true(flag) ? -1 : 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_setblocking_obj, sock_setblocking);

/* options: the connection sets what it needs (TCP_NODELAY) */
static mp_obj_t sock_setsockopt(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_setsockopt_obj, 4, 4, sock_setsockopt);

/* makefile(): the socket is a stream already */
static mp_obj_t sock_makefile(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    return args[0];
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_makefile_obj, 1, 3, sock_makefile);

static mp_obj_t sock_unsupported(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("P4OS sockets are clients only"));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_unsupported_obj, 1, 3, sock_unsupported);

static const mp_rom_map_elem_t sock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sock_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&sock_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&sock_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&sock_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&mp_stream_read1_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&sock_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&sock_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&sock_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR__tls), MP_ROM_PTR(&sock_tls_obj) },
};
static MP_DEFINE_CONST_DICT(sock_locals, sock_locals_table);

static const mp_stream_p_t sock_stream_p = {
    .read = sock_read,
    .write = sock_write,
    .ioctl = sock_ioctl,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    sock_type,
    MP_QSTR_socket,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, sock_make_new,
    protocol, &sock_stream_p,
    locals_dict, &sock_locals
    );

/* --- the module ------------------------------------------------------------------ */

/* the name as it is: the connection resolves it when it opens */
static mp_obj_t net_getaddrinfo(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    mp_obj_t addr[2] = { args[0], mp_obj_new_int(mp_obj_get_int(args[1])) };
    mp_obj_t entry[5] = {
        MP_OBJ_NEW_SMALL_INT(2),                /* AF_INET     */
        MP_OBJ_NEW_SMALL_INT(1),                /* SOCK_STREAM */
        MP_OBJ_NEW_SMALL_INT(6),                /* IPPROTO_TCP */
        MP_OBJ_NEW_QSTR(MP_QSTR_),
        mp_obj_new_tuple(2, addr),
    };
    mp_obj_t one = mp_obj_new_tuple(5, entry);
    return mp_obj_new_list(1, &one);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_getaddrinfo_obj, 2, 6, net_getaddrinfo);

static const mp_rom_map_elem_t socket_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&sock_type) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&net_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_TCP), MP_ROM_INT(6) },
    { MP_ROM_QSTR(MP_QSTR_SOL_SOCKET), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_SO_REUSEADDR), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_TCP_NODELAY), MP_ROM_INT(1) },
};
static MP_DEFINE_CONST_DICT(socket_globals, socket_globals_table);

const mp_obj_module_t p4py_socket_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&socket_globals,
};

MP_REGISTER_MODULE(MP_QSTR_socket, p4py_socket_module);
