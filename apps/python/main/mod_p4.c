/*
 * P4OS - Python: the builtins the port lends (mpconfigport.h,
 * MICROPY_PORT_BUILTINS and MICROPY_PY_BUILTINS_HELP_TEXT).
 *
 *   input([prompt])   a line from the app's console (p4py_rt.c waits for it)
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

#include <stdlib.h>
#include <string.h>

const char *aos_tr(const char *s);         /* aos_i18n.h */
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

static const char *help_text(void)
{
    /* one literal: the catalogue's key is the whole text */
    return _("El Python de P4OS es MicroPython 1.29.\n\nEscribí una expresión y Enter:\n  2**100, sum(range(10)), 'hola'.upper()\ndef, for, if... siguen con '...'; una\n  línea vacía los corre.\nMódulos: math, cmath, random, json, re,\n  time, collections, struct, array,\n  heapq, gc. help('modules') los lista;\n  help(math) muestra uno.\nArchivos: open() e import usan la\n  carpeta /python de la tarjeta.\nDetener corta lo que está corriendo.\n");
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
    mp_obj_list_sort(1, &list, (mp_map_t *)&mp_const_empty_map);
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(list, &len, &items);
    for (size_t i = 0; i < len; i++) {
        /* three columns: 48 of the console's 52 upright */
        mp_printf(MP_PYTHON_PRINTER, "%-16s", mp_obj_str_get_str(items[i]));
        if (i % 3 == 2 || i == len - 1) mp_print_str(MP_PYTHON_PRINTER, "\n");
    }
    mp_print_str(MP_PYTHON_PRINTER, _("Y los .py de la carpeta /python de la tarjeta.\n"));
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
