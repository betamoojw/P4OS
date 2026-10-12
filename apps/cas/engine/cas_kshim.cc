/*
 * P4OS - CAS: what Giac needs from KhiCAS's user interface, without it.
 *
 * KhiCAS's interface for the NumWorks is kdisplay.cc: its console, editor,
 * menus, spreadsheet and interactive geometry, ~680 KB of code the app does
 * not use (it draws its own). Giac's core still calls into it here and
 * there, and its command table points at the turtle's commands, which live
 * there too. So:
 *
 *  - the turtle (logo) commands are cut out of kdisplay.cc by
 *    build_engine.sh, as they are, into cas_turtle.cc: they only keep state,
 *    and the app will draw that state;
 *  - everything else is answered here with "nothing on screen, nothing
 *    chosen, cancelled";
 *  - dConsolePut(), where print() in a Giac program writes, keeps the text
 *    for the app (cas_engine.cc hands it over with the result).
 */
#include "config.h"
#include "giacPCH.h"
#include "kdisplay.h"

#include <string.h>

using namespace giac;

/* --- the console --------------------------------------------------------- */

static std::string *s_console;

extern "C" void cas_console_clear(void)
{
    if (s_console) {
        s_console->clear();
    }
}

extern "C" const char *cas_console_text(void)
{
    return s_console ? s_console->c_str() : "";
}

/* --- globals kdisplay.cc defined ------------------------------------------ */

int lang = 0;
short int nspirelua = 0;
bool warn_nr = false;
int xcas_python_eval = 0;
char *python_heap = 0;
const char *flash_buf = 0;

const char *gettext(const char *s) { return s; }

int python_init(int stack_size, int heap_size) { (void)stack_size; (void)heap_size; return 0; }
int select_item(const char **ptr, const char *title, bool askfor1) { (void)ptr; (void)title; (void)askfor1; return -1; }

#define KHI_KEY_EXIT 5

namespace giac {
    bool do_confirm(const char *s) { (void)s; return false; }
    int confirm(const char *m1, const char *m2, bool acexit, int y) { (void)m1; (void)m2; (void)acexit; (void)y; return KHI_KEY_EXIT; }
    gen select_var(const context *contextptr) { (void)contextptr; return undef; }
    std::string help_insert(const char *cmdline, int &back, int exec, const context *, bool warn)
    {
        (void)cmdline; (void)exec; (void)warn;
        back = 0;
        return "";
    }
    bool inputdouble(const char *msg1, double &d, const context *contextptr) { (void)msg1; (void)d; (void)contextptr; return false; }
    int inputline(const char *msg1, const char *msg2, std::string &s, bool numeric, int ypos, const context *contextptr)
    {
        (void)msg1; (void)msg2; (void)s; (void)numeric; (void)ypos; (void)contextptr;
        return KHI_KEY_EXIT;
    }
    const char *keytostring(int key, int keyflag, bool py, const context *contextptr) { (void)key; (void)keyflag; (void)py; (void)contextptr; return 0; }
    int showCatalog(char *insertText, int preselect, int menupos, const context *contextptr) { (void)insertText; (void)preselect; (void)menupos; (void)contextptr; return 0; }
    void copy_clipboard(const std::string &s, bool status) { (void)s; (void)status; }
    int doMenu(Menu *menu, MenuItemIcon *icontable) { (void)menu; (void)icontable; return 0; }
    void insert(std::string &s, int pos, const char *add)
    {
        if (pos < 0 || pos > (int)s.size()) {
            pos = (int)s.size();
        }
        s.insert(pos, add);
    }
    int chartab() { return -1; }
}

namespace xcas {
    int dconsole_mode = 0;
    Graph2d *geoptr = 0;
    tableur *sheetptr = 0;

    void dConsolePut(const char *s)
    {
        if (!s_console) {
            s_console = new std::string;
        }
        if (s_console->size() < 16384) {
            *s_console += s;
        }
    }
    int doTextArea(textArea *text, const context *contextptr) { (void)text; (void)contextptr; return 0; }
    void add(textArea *edptr, const std::string &s) { (void)edptr; (void)s; }
    int displaylogo() { return 0; }
    int print_color(int print_x, int print_y, const char *s, int color, bool invert, bool minimini, const context *contextptr)
    {
        (void)print_y; (void)s; (void)color; (void)invert; (void)minimini; (void)contextptr;
        return print_x;
    }
    int Console_Disp(int redraw_mode, const context *ptr) { (void)redraw_mode; (void)ptr; return 0; }
    int console_main(const context *contextptr, const char *sessionname) { (void)contextptr; (void)sessionname; return 0; }
    const char *console_menu(int key, char *cfg, int active_app) { (void)key; (void)cfg; (void)active_app; return 0; }
    int displaygraph(const gen &ge, const gen &gs, const context *contextptr) { (void)ge; (void)gs; (void)contextptr; return 0; }
    int check_do_graph(gen &ge, const gen &gs, int do_logo_graph_eqw, const context *contextptr)
    {
        (void)ge; (void)gs; (void)do_logo_graph_eqw; (void)contextptr;
        return 0;
    }
    gen eqw(const gen &ge, bool editable, const context *contextptr) { (void)editable; (void)contextptr; return ge; }
    int get_filename(char *filename, const char *extension) { (void)filename; (void)extension; return 0; }
    int giac_filebrowser(char *filename, const char *extension, const char *title, int storage)
    {
        (void)filename; (void)extension; (void)title; (void)storage;
        return 0;
    }
    void save_session(const context *contextptr) { (void)contextptr; }
    std::string print_tableur(const tableur &t, const context *contextptr) { (void)t; (void)contextptr; return ""; }
    void fix_sheet(tableur &t, const context *contextptr) { (void)t; (void)contextptr; }
    int periodic_table(const char *&name, const char *&symbol, char *protons, char *nucleons, char *mass, char *electroneg)
    {
        (void)name; (void)symbol; (void)protons; (void)nucleons; (void)mass; (void)electroneg;
        return 0;
    }
    void draw_filled_circle(int xc, int yc, int r, int color, bool left, bool right)
    {
        (void)xc; (void)yc; (void)r; (void)color; (void)left; (void)right;
    }
    void draw_line(int x0, int y0, int x1, int y1, int c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
    bool tooltip(int x, int y, int pos, const char *editline, const context *contextptr)
    {
        (void)x; (void)y; (void)pos; (void)editline; (void)contextptr;
        return false;
    }
    bool textedit(char *s, int bufsize, bool OKparse, const context *contextptr, const char *filename)
    {
        (void)s; (void)bufsize; (void)OKparse; (void)contextptr; (void)filename;
        return false;
    }
    bool textedit(char *s, int bufsize, const context *contextptr) { (void)s; (void)bufsize; (void)contextptr; return false; }
    int newgeo(const context *contextptr) { (void)contextptr; return 0; }
    int geoloop(Graph2d *geoptr) { (void)geoptr; return 0; }
    bool geoparse(textArea *text, const context *contextptr) { (void)text; (void)contextptr; return false; }
    void Graph2d::orthonormalize(bool doupdate) { (void)doupdate; }
    void Graph2d::update_rotation() { }
}
