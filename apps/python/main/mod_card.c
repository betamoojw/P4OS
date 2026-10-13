/*
 * P4OS - Python: the microSD as MicroPython's filesystem.
 *
 * A VFS object of the app's own, mounted at "/" when the interpreter starts:
 * open(), the os module (listdir, mkdir, remove, rename, stat, chdir...) and
 * import all reach the card through it, as on any MicroPython board.
 *
 * MicroPython's VfsPosix would do, but it leans on the process's current
 * directory (chdir() and relative paths handed to open()), and the firmware
 * has none. Here the current directory is this object's: every path is
 * made whole against it, "." and ".." resolved (never above the card's
 * root), and the firmware's calls only ever see absolute paths. It starts
 * at /python, where the scripts are. The file objects are VfsPosix's
 * (extmod/vfs_posix_file.c, on descriptors).
 *
 * Named mod_*.c so tools/gen_upy.sh scans it for its names.
 */
#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"

#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* from aos_hal.h: not included, tools/gen_upy.sh scans this file with the
 * Mac's compiler and none of the firmware's headers */
const char *aos_hal_path_sd_root(void);
bool aos_hal_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes);

#define PATH_MAX_P4 MICROPY_ALLOC_PATH_MAX

typedef struct {
    mp_obj_base_t base;
    char root[64];              /* the card's mount point, no slash at the end */
    char cwd[PATH_MAX_P4];      /* within the card: "/" or "/python", no slash at the end */
} card_t;

/* "a/../b", against the current directory, into "/b" (within the card) and
 * the firmware's whole path; false if it does not fit */
static bool card_resolve(const card_t *self, const char *path, char *in_card, char *full)
{
    char joined[2 * PATH_MAX_P4 + 2];
    int n = path[0] == '/' ? snprintf(joined, sizeof joined, "%s", path)
                           : snprintf(joined, sizeof joined, "%s/%s", self->cwd, path);
    if (n < 0 || (size_t)n >= sizeof joined) return false;

    size_t len = 0;
    for (const char *seg = joined; *seg;) {
        size_t s = strcspn(seg, "/");
        if (s == 0 || (s == 1 && seg[0] == '.')) {
            /* "//" or "/./" */
        } else if (s == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 0 && in_card[len - 1] != '/') len--;
            if (len > 0) len--;                 /* the slash before it */
        } else {
            if (len + 1 + s >= PATH_MAX_P4) return false;
            in_card[len++] = '/';
            memcpy(in_card + len, seg, s);
            len += s;
        }
        seg += s;
        if (*seg == '/') seg++;
    }
    if (len == 0) in_card[len++] = '/';
    in_card[len] = 0;

    n = snprintf(full, PATH_MAX_P4 + sizeof self->root, "%s%s", self->root, len == 1 ? "" : in_card);
    return n > 0 && (size_t)n < PATH_MAX_P4 + sizeof self->root;
}

typedef struct {
    char in_card[PATH_MAX_P4];
    char full[PATH_MAX_P4 + 64];
} card_path_t;

static const char *card_full(card_t *self, mp_obj_t path_in, card_path_t *p)
{
    if (!card_resolve(self, mp_obj_str_get_str(path_in), p->in_card, p->full)) mp_raise_OSError(ENAMETOOLONG);
    return p->full;
}

static void raise_errno(void)
{
    mp_raise_OSError(errno);
}

/* --- the VFS protocol ------------------------------------------------------------ */

static mp_import_stat_t card_import_stat(void *self_in, const char *path)
{
    card_t *self = self_in;
    card_path_t p;
    struct stat st;
    if (!card_resolve(self, path, p.in_card, p.full) || stat(p.full, &st) != 0) return MP_IMPORT_STAT_NO_EXIST;
    if (S_ISDIR(st.st_mode)) return MP_IMPORT_STAT_DIR;
    return S_ISREG(st.st_mode) ? MP_IMPORT_STAT_FILE : MP_IMPORT_STAT_NO_EXIST;
}

static mp_obj_t card_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs)
{
    (void)self_in;
    (void)readonly;
    if (mp_obj_is_true(mkfs)) mp_raise_OSError(MP_EPERM);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(card_mount_obj, card_mount);

static mp_obj_t card_umount(mp_obj_t self_in)
{
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(card_umount_obj, card_umount);

/* a file opened to write since the runtime last asked (p4py_rt.c) */
static bool s_opened_to_write;

bool p4py_card_take_written(void)
{
    bool w = s_opened_to_write;
    s_opened_to_write = false;
    return w;
}

static mp_obj_t card_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in)
{
    const char *mode = mp_obj_str_get_str(mode_in);
    if (strpbrk(mode, "wax+")) s_opened_to_write = true;
    card_path_t p;
    if (!mp_obj_is_small_int(path_in)) path_in = mp_obj_new_str_from_cstr(card_full(MP_OBJ_TO_PTR(self_in), path_in, &p));
    return mp_vfs_posix_file_open(&mp_type_vfs_posix_textio, path_in, mode_in);
}
static MP_DEFINE_CONST_FUN_OBJ_3(card_open_obj, card_open);

static mp_obj_t card_chdir(mp_obj_t self_in, mp_obj_t path_in)
{
    card_t *self = MP_OBJ_TO_PTR(self_in);
    card_path_t p;
    struct stat st;
    card_full(self, path_in, &p);
    if (stat(p.full, &st) != 0) raise_errno();
    if (!S_ISDIR(st.st_mode)) mp_raise_OSError(MP_ENOTDIR);
    strcpy(self->cwd, p.in_card);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_chdir_obj, card_chdir);

static mp_obj_t card_getcwd(mp_obj_t self_in)
{
    card_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_str_from_cstr(self->cwd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(card_getcwd_obj, card_getcwd);

/* the whole directory at once, into a list: no directory stays open between
 * one step of the iteration and the next */
static mp_obj_t card_ilistdir(mp_obj_t self_in, mp_obj_t path_in)
{
    card_path_t p;
    card_full(MP_OBJ_TO_PTR(self_in), path_in, &p);
    bool as_bytes = mp_obj_get_type(path_in) == &mp_type_bytes;
    DIR *d = opendir(p.full);
    if (!d) raise_errno();
    mp_obj_t list = mp_obj_new_list(0, NULL);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        size_t base = strlen(p.full);
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            mp_int_t type = 0;
            if (e->d_type == DT_DIR) type = MP_S_IFDIR;
            else if (e->d_type == DT_REG) type = MP_S_IFREG;
            else if (base + 1 + strlen(e->d_name) < sizeof p.full) {
                struct stat st;
                snprintf(p.full + base, sizeof p.full - base, "/%s", e->d_name);
                if (stat(p.full, &st) == 0) type = S_ISDIR(st.st_mode) ? MP_S_IFDIR : MP_S_IFREG;
                p.full[base] = 0;
            }
            mp_obj_t t[3] = {
                as_bytes ? mp_obj_new_bytes((const byte *)e->d_name, strlen(e->d_name))
                         : mp_obj_new_str_from_cstr(e->d_name),
                MP_OBJ_NEW_SMALL_INT(type),
                MP_OBJ_NEW_SMALL_INT(0),
            };
            mp_obj_list_append(list, mp_obj_new_tuple(3, t));
        }
        nlr_pop();
    } else {
        closedir(d);
        nlr_jump(nlr.ret_val);
    }
    closedir(d);
    return mp_getiter(list, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_ilistdir_obj, card_ilistdir);

static mp_obj_t card_mkdir(mp_obj_t self_in, mp_obj_t path_in)
{
    card_path_t p;
    if (mkdir(card_full(MP_OBJ_TO_PTR(self_in), path_in, &p), 0777) != 0) raise_errno();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_mkdir_obj, card_mkdir);

static mp_obj_t card_remove(mp_obj_t self_in, mp_obj_t path_in)
{
    card_path_t p;
    struct stat st;
    card_full(MP_OBJ_TO_PTR(self_in), path_in, &p);
    if (stat(p.full, &st) != 0) raise_errno();
    /* the card's unlink() would take an empty directory too */
    if (S_ISDIR(st.st_mode)) mp_raise_OSError(MP_EISDIR);
    if (unlink(p.full) != 0) raise_errno();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_remove_obj, card_remove);

static mp_obj_t card_rmdir(mp_obj_t self_in, mp_obj_t path_in)
{
    card_path_t p;
    struct stat st;
    card_full(MP_OBJ_TO_PTR(self_in), path_in, &p);
    if (stat(p.full, &st) != 0) raise_errno();
    if (!S_ISDIR(st.st_mode)) mp_raise_OSError(MP_ENOTDIR);
    if (!strcmp(p.in_card, "/")) mp_raise_OSError(MP_EPERM);
    #ifdef ESP_PLATFORM
    /* the firmware lends no rmdir(); on the card's FAT unlink() takes an
     * empty directory (FatFs's f_unlink, which is what rmdir() calls) */
    int r = unlink(p.full);
    #else
    int r = rmdir(p.full);
    #endif
    if (r != 0) raise_errno();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_rmdir_obj, card_rmdir);

/* as Python's: an existing file at the new name is replaced (FAT refuses) */
static mp_obj_t card_rename(mp_obj_t self_in, mp_obj_t old_in, mp_obj_t new_in)
{
    card_t *self = MP_OBJ_TO_PTR(self_in);
    card_path_t a, b;
    card_full(self, old_in, &a);
    card_full(self, new_in, &b);
    if (rename(a.full, b.full) != 0) {
        struct stat st;
        if (stat(b.full, &st) != 0 || S_ISDIR(st.st_mode) || unlink(b.full) != 0 || rename(a.full, b.full) != 0) raise_errno();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(card_rename_obj, card_rename);

static mp_obj_t card_stat(mp_obj_t self_in, mp_obj_t path_in)
{
    card_path_t p;
    struct stat st;
    if (stat(card_full(MP_OBJ_TO_PTR(self_in), path_in, &p), &st) != 0) raise_errno();
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(S_ISDIR(st.st_mode) ? MP_S_IFDIR : MP_S_IFREG);
    t->items[1] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[3] = MP_OBJ_NEW_SMALL_INT(1);
    t->items[4] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[6] = mp_obj_new_int_from_ull((unsigned long long)st.st_size);
    t->items[7] = mp_obj_new_int_from_ll((long long)st.st_atime);
    t->items[8] = mp_obj_new_int_from_ll((long long)st.st_mtime);
    t->items[9] = mp_obj_new_int_from_ll((long long)st.st_ctime);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_stat_obj, card_stat);

/* in 4 KB blocks: the card's size and what is free, as the system counts them */
static mp_obj_t card_statvfs(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    (void)path_in;
    uint64_t total = 0, avail = 0;
    if (!aos_hal_sd_usage(&total, &avail)) mp_raise_OSError(MP_ENODEV);
    const uint64_t bs = 4096;
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(bs);
    t->items[1] = MP_OBJ_NEW_SMALL_INT(bs);
    t->items[2] = mp_obj_new_int_from_ull(total / bs);
    t->items[3] = mp_obj_new_int_from_ull(avail / bs);
    t->items[4] = mp_obj_new_int_from_ull(avail / bs);
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[6] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[8] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[9] = MP_OBJ_NEW_SMALL_INT(255);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(card_statvfs_obj, card_statvfs);

static const mp_rom_map_elem_t card_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&card_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&card_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&card_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&card_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&card_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&card_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&card_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&card_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&card_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&card_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&card_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&card_statvfs_obj) },
};
static MP_DEFINE_CONST_DICT(card_locals, card_locals_table);

static const mp_vfs_proto_t card_proto = {
    .import_stat = card_import_stat,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    card_type,
    MP_QSTR_Card,
    MP_TYPE_FLAG_NONE,
    protocol, &card_proto,
    locals_dict, &card_locals
    );

/* --- at start -------------------------------------------------------------------- */

/* the card at "/", /python made if missing and made the current directory,
 * and sys.path: the current directory, then /python/lib (where mip puts
 * what it installs), then the modules inside the app (a newer one on the
 * card wins). Runs once after mp_init(); false without a card. */
bool p4py_card_mount(void)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd || strlen(sd) >= sizeof ((card_t *)0)->root) return false;
    card_t *card = mp_obj_malloc(card_t, &card_type);
    snprintf(card->root, sizeof card->root, "%s", sd);
    strcpy(card->cwd, "/");

    mp_vfs_mount_t *m = m_new_obj(mp_vfs_mount_t);
    m->str = "/";
    m->len = 1;
    m->obj = MP_OBJ_FROM_PTR(card);
    m->next = NULL;
    MP_STATE_VM(vfs_mount_table) = m;

    char dir[sizeof card->root + 8];
    snprintf(dir, sizeof dir, "%s/python", card->root);
    mkdir(dir, 0777);
    mp_vfs_chdir(MP_OBJ_NEW_QSTR(qstr_from_str("/python")));

    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr("/python/lib"));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__dot_frozen));
    return true;
}
