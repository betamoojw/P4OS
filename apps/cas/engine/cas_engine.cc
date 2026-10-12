/*
 * P4OS - CAS: the bridge between the app (C) and Giac (C++).
 *
 * Everything here runs on the engine's own thread (cas_rt.c), never on
 * LVGL's: a computation can take seconds and Giac recurses deeply.
 *
 * The evaluation follows what Giac's own caseval() does on the calculators
 * (gen.cc): parse, turn 'a=1' into an assignment, evaluate protected, print.
 */
#include "config.h"
#include "giacPCH.h"

#include <string.h>

using namespace giac;

extern int lang;    /* cas_kshim.cc: the language of KhiCAS's messages */
extern "C" void cas_console_clear(void);
extern "C" const char *cas_console_text(void);

static context     *s_ctx;
static std::string *s_out;

extern "C" int cas_engine_boot(unsigned stack_bytes)
{
    /* Giac's own guard against running out of stack: it stops a recursion
     * with "Stack overflow" this many bytes below the current frame. A
     * margin is left for what runs above eval() and below Giac's checks. */
    stack_check_init(stack_bytes > 32768 ? stack_bytes - 24576 : stack_bytes / 2);
    lang = 0;                       /* English messages until the app's own */
    s_ctx = new context;
    s_out = new std::string;
    language(0, s_ctx);
    angle_radian(true, s_ctx);
    /* KhiCAS starts in its Python-like syntax (x**2, list[...], True);
     * the app reads and writes Xcas's: x^2, [1,2], true */
    python_compat(0, s_ctx);
    caseval_maxtime = 60;           /* seconds; ctrl_c stops it earlier */
    return 0;
}

extern "C" const char *cas_engine_eval(const char *in, int *is_error)
{
    ctrl_c = interrupted = false;
    caseval_begin = time(0);
    cas_console_clear();
    gen g(in, s_ctx);
    g = equaltosto(g, s_ctx);
    gen r = protecteval(g, 1, s_ctx);
    *is_error = (r.type == _STRNG && r.subtype == -1) || ctrl_c || interrupted;
    /* what print() wrote comes first, as on the calculators' console */
    *s_out = cas_console_text();
    *s_out += r.print(s_ctx);
    ctrl_c = interrupted = false;
    return s_out->c_str();
}

extern "C" void cas_engine_interrupt(void)
{
    ctrl_c = interrupted = true;
}
