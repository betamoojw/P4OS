/*
 * P4OS - Python: the builtins the port lends (mpconfigport.h,
 * MICROPY_PORT_BUILTINS and MICROPY_PY_BUILTINS_HELP_TEXT).
 *
 *   input([prompt])   a line from the app's console (p4py_rt.c waits for it)
 *   sys.stdin/stdout/stderr   the console as streams: what goes to stderr
 *                     is painted red, stdin reads the lines input() would
 *   help([obj])       what this Python is, in the interface's language;
 *                     help('modules') and help(obj) as MicroPython's own
 *                     (py/builtinhelp.c, MIT), whose text is an array the
 *                     catalogue could not translate
 *
 * Named mod_*.c so tools/gen_upy.sh scans it for its names; it includes
 * none of the firmware's headers for the same reason (the scan runs with
 * the Mac's compiler).
 */
#include "py/builtin.h"
#include "py/objlist.h"
#include "py/objmodule.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"

#include <stdlib.h>
#include <string.h>

const char *aos_tr(const char *s);         /* aos_i18n.h */
extern const char mp_frozen_names[];       /* upy/frozen_content.c */
#define _(s) aos_tr(s)

char *p4py_input_wait(void);

static mp_obj_t p4py_input(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 1) {
        mp_obj_print(args[0], PRINT_STR);
    }
    char *line = p4py_input_wait();
    mp_obj_t s = mp_obj_new_str(line, strlen(line));
    free(line);
    return s;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(p4py_input_obj, 0, 1, p4py_input);

/* --- sys.stdin, sys.stdout, sys.stderr -------------------------------------------- */

typedef struct {
    mp_obj_base_t base;
    int           fd;
} p4py_stdio_t;

/* what is left of the last line stdin read, for reads smaller than a line */
static char  *s_in;
static size_t s_in_len, s_in_pos;

static mp_uint_t stdio_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    p4py_stdio_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd != 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (size == 0) return 0;
    if (s_in_pos >= s_in_len) {
        free(s_in);
        s_in = NULL;
        s_in_len = s_in_pos = 0;
        char *line = p4py_input_wait();
        size_t n = strlen(line);
        s_in = realloc(line, n + 2);
        if (!s_in) {
            free(line);
            *errcode = MP_ENOMEM;
            return MP_STREAM_ERROR;
        }
        s_in[n] = '\n';
        s_in[n + 1] = 0;
        s_in_len = n + 1;
    }
    size_t n = s_in_len - s_in_pos;
    if (n > size) n = size;
    memcpy(buf, s_in + s_in_pos, n);
    s_in_pos += n;
    return n;
}

static mp_uint_t stdio_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    p4py_stdio_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd == 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    /* the console paints between these bytes red, as it does errors */
    if (self->fd == 2) mp_hal_stdout_tx_strn("\x01", 1);
    mp_hal_stdout_tx_strn(buf, size);
    if (self->fd == 2) mp_hal_stdout_tx_strn("\x02", 1);
    return size;
}

static mp_uint_t stdio_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    (void)self_in;
    (void)arg;
    if (request == MP_STREAM_FLUSH || request == MP_STREAM_CLOSE) return 0;
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_stream_p_t stdio_stream_p = {
    .read = stdio_read,
    .write = stdio_write,
    .ioctl = stdio_ioctl,
    .is_text = true,
};

static const mp_rom_map_elem_t stdio_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
};
static MP_DEFINE_CONST_DICT(stdio_locals, stdio_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    p4py_stdio_type,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &stdio_stream_p,
    locals_dict, &stdio_locals
    );

/* py/modsys.c asks for these names */
const p4py_stdio_t mp_sys_stdin_obj = { { &p4py_stdio_type }, 0 };
const p4py_stdio_t mp_sys_stdout_obj = { { &p4py_stdio_type }, 1 };
const p4py_stdio_t mp_sys_stderr_obj = { { &p4py_stdio_type }, 2 };

/* --- help() ---------------------------------------------------------------------- */

static const char *help_text(void)
{
    /* one literal: the catalogue's key is the whole text */
    return _("El Python de P4OS es MicroPython 1.29.\n\nEscribí una expresión y Enter:\n  2**100, sum(range(10)), 'hola'.upper()\ndef, for, if... siguen con '...'; una\n  línea vacía los corre.\nMódulos: math, random, json, re, time,\n  os, struct, collections, gc...\n  help('modules') los lista;\n  help(math) muestra uno.\nArchivos: la tarjeta es '/' y se\n  empieza en /python. open(), os e\n  import como en cualquier Python.\nRed: requests.get(url), socket.\nBibliotecas: import mip, y\n  mip.install('nombre') la baja de\n  micropython-lib a /python/lib.\nDetener corta lo que está corriendo.\n");
}

static void help_item(mp_obj_t name, mp_obj_t value)
{
    mp_print_str(MP_PYTHON_PRINTER, "  ");
    mp_obj_print(name, PRINT_STR);
    mp_print_str(MP_PYTHON_PRINTER, " -- ");
    mp_obj_print(value, PRINT_STR);
    mp_print_str(MP_PYTHON_PRINTER, "\n");
}

static void help_modules(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    /* two tables: the extensible ones (random, json, re, time...) apart */
    const mp_map_t *maps[] = {
        &mp_builtin_module_map,
        #if MICROPY_HAVE_REGISTERED_EXTENSIBLE_MODULES
        &mp_builtin_extensible_module_map,
        #endif
    };
    for (size_t m = 0; m < MP_ARRAY_SIZE(maps); m++) {
        for (size_t i = 0; i < maps[m]->alloc; i++) {
            mp_obj_t k = maps[m]->table[i].key;
            if (k != MP_OBJ_NULL) mp_obj_list_append(list, k);
        }
    }
    /* and the ones in Python inside the app (frozen/: requests, mip...):
     * "mip/__init__.py" is mip, "tls.py" is tls */
    for (const char *n = mp_frozen_names; *n; n += strlen(n) + 1) {
        const char *end = strchr(n, '/');
        if (!end) end = strstr(n, ".py");
        if (end) mp_obj_list_append(list, mp_obj_new_str(n, end - n));
    }
    mp_obj_list_sort(1, &list, (mp_map_t *)&mp_const_empty_map);
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(list, &len, &items);
    for (size_t i = 0; i < len; i++) {
        /* three columns: 48 of the console's 52 upright */
        mp_printf(MP_PYTHON_PRINTER, "%-16s", mp_obj_str_get_str(items[i]));
        if (i % 3 == 2 || i == len - 1) mp_print_str(MP_PYTHON_PRINTER, "\n");
    }
    mp_print_str(MP_PYTHON_PRINTER, _("Y los .py y .mpy de /python y /python/lib.\n"));
}

static mp_obj_t p4py_help(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) {
        mp_print_str(MP_PYTHON_PRINTER, help_text());
        return mp_const_none;
    }
    mp_obj_t obj = args[0];
    if (mp_obj_is_str(obj) && !strcmp(mp_obj_str_get_str(obj), "modules")) {
        help_modules();
        return mp_const_none;
    }
    const mp_obj_type_t *type = mp_obj_get_type(obj);
    mp_print_str(MP_PYTHON_PRINTER, "object ");
    mp_obj_print(obj, PRINT_STR);
    mp_printf(MP_PYTHON_PRINTER, " is of type %q\n", type->name);
    mp_map_t *map = NULL;
    if (type == &mp_type_module) {
        map = &mp_obj_module_get_globals(obj)->map;
    } else {
        if (type == &mp_type_type) type = MP_OBJ_TO_PTR(obj);
        if (MP_OBJ_TYPE_HAS_SLOT(type, locals_dict)) map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
    }
    if (map) {
        for (size_t i = 0; i < map->alloc; i++) {
            if (map->table[i].key != MP_OBJ_NULL) help_item(map->table[i].key, map->table[i].value);
        }
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(p4py_help_obj, 0, 1, p4py_help);
