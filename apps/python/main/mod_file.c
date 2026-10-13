/*
 * P4OS - Python: open(), on the card.
 *
 * MicroPython leaves open() to the port. Here a file is a stdio FILE on the
 * microSD: a relative name is under /python (where the scripts are), an
 * absolute one is from the card's root ("/data/log.csv"), and ".." is not
 * taken. Text and binary modes, read, write, append, "+", seek, tell,
 * iteration by lines and "with". After MicroPython's
 * extmod/vfs_posix_file.c (MIT), on FILE instead of a descriptor.
 *
 * Named mod_*.c so tools/gen_upy.sh scans it for its names.
 */
#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* from aos_hal.h: not included, tools/gen_upy.sh scans this file with the
 * Mac's compiler and none of the firmware's headers */
const char *aos_hal_path_sd_root(void);

typedef struct {
    mp_obj_base_t base;
    FILE         *f;
} p4py_file_t;

extern const mp_obj_type_t p4py_fileio_type;
extern const mp_obj_type_t p4py_textio_type;

/* a script's name for a file -> the card's */
bool p4py_card_path(const char *name, char *out, size_t n)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd || !*name || strstr(name, "..")) return false;
    if (name[0] == '/') snprintf(out, n, "%s%s", sd, name);
    else snprintf(out, n, "%s/python/%s", sd, name);
    return true;
}

static void check_open(const p4py_file_t *o)
{
    if (!o->f) mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
}

static void file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, "<io.%s>", mp_obj_get_type_str(self_in));
}

static mp_uint_t file_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode)
{
    p4py_file_t *o = MP_OBJ_TO_PTR(o_in);
    check_open(o);
    size_t r = fread(buf, 1, size, o->f);
    if (r == 0 && ferror(o->f)) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return r;
}

static mp_uint_t file_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode)
{
    p4py_file_t *o = MP_OBJ_TO_PTR(o_in);
    check_open(o);
    size_t r = fwrite(buf, 1, size, o->f);
    if (r < size && ferror(o->f)) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return r;
}

static mp_uint_t file_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    p4py_file_t *o = MP_OBJ_TO_PTR(o_in);
    switch (request) {
    case MP_STREAM_FLUSH:
        check_open(o);
        fflush(o->f);
        return 0;
    case MP_STREAM_SEEK: {
        check_open(o);
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        if (fseek(o->f, (long)s->offset, s->whence) != 0) {
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
        }
        s->offset = ftell(o->f);
        return 0;
    }
    case MP_STREAM_CLOSE:
        if (o->f) fclose(o->f);
        o->f = NULL;
        return 0;
    default:
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals, file_locals_table);

static const mp_stream_p_t fileio_stream = { .read = file_read, .write = file_write, .ioctl = file_ioctl };
static const mp_stream_p_t textio_stream = { .read = file_read, .write = file_write, .ioctl = file_ioctl, .is_text = true };

MP_DEFINE_CONST_OBJ_TYPE(p4py_fileio_type, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
                         print, file_print, protocol, &fileio_stream, locals_dict, &file_locals);
MP_DEFINE_CONST_OBJ_TYPE(p4py_textio_type, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
                         print, file_print, protocol, &textio_stream, locals_dict, &file_locals);

static mp_obj_t p4py_open(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_file, ARG_mode, ARG_encoding };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_r) } },
        { MP_QSTR_encoding, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    const char *name = mp_obj_str_get_str(args[ARG_file].u_obj);
    const char *m = mp_obj_str_get_str(args[ARG_mode].u_obj);

    /* Python's modes to stdio's: always binary underneath, the type does text */
    char rw = 'r';
    bool plus = false, bin = false;
    for (const char *c = m; *c; c++) {
        if (*c == 'r' || *c == 'w' || *c == 'a' || *c == 'x') rw = *c;
        else if (*c == '+') plus = true;
        else if (*c == 'b') bin = true;
    }
    char path[300];
    if (!p4py_card_path(name, path, sizeof path)) mp_raise_OSError(MP_ENOENT);
    if (rw == 'x') {
        FILE *t = fopen(path, "rb");
        if (t) {
            fclose(t);
            mp_raise_OSError(MP_EEXIST);
        }
        rw = 'w';
    }
    char mode[4] = { rw, 'b', plus ? '+' : 0, 0 };
    p4py_file_t *o = mp_obj_malloc_with_finaliser(p4py_file_t, bin ? &p4py_fileio_type : &p4py_textio_type);
    o->f = fopen(path, mode);
    if (!o->f) mp_raise_OSError(errno ? errno : MP_ENOENT);
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, p4py_open);
