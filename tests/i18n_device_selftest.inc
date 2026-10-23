/* On-target test, included inside aos_i18n.c only with AOS_I18N_SELFTEST=ON.
 * Runs before UI objects/apps exist and restores the preference afterwards. */
#include "aos_fonts.h"
void aos_i18n_device_selftest(void)
{
    char original[AOS_LANG_CODE_MAX], preferred[AOS_LANG_CODE_MAX]={0};
    snprintf(original,sizeof(original),"%s",aos_i18n_current());
    bool had_preference=aos_hal_pref_get_str(PREF_KEY,preferred,sizeof(preferred));
    const char *codes[]={"zh-Hans","zh-Hant"}, *titles[]={"设置","設定"};
    bool ok=true;
    int checked=0;
    uint32_t gids[2]={UINT32_MAX,UINT32_MAX};
    for (int variant=0;variant<2;variant++) {
        bool language_ok=aos_i18n_set(codes[variant]);
        if (!language_ok || strcmp(aos_tr("Ajustes"),titles[variant])) ok=false;
        const aos_lang_pack_t *pack=embedded_pack(codes[variant]);
        if (!pack) { ok=false; continue; }
        for (int i=0;i<pack->nfiles;i++) {
            const char *name=pack->files[i].file;
            if (!strcmp(name,SYSTEM_CATALOG)) continue;
            char id[64];
            size_t n=strlen(name);
            if (n<5 || n-5>=sizeof(id)) { ok=false; continue; }
            memcpy(id,name,n-5);id[n-5]=0;
            aos_i18n_app_load(id);
            if (!aos_i18n_app_count()) {
                aos_hal_log("i18n-test","FAIL empty catalogue: %s/%s",codes[variant],name);
                ok=false;
            }
            checked++;
            aos_i18n_app_forget(id);
        }
        lv_font_glyph_dsc_t glyph={0};
        bool found=lv_font_get_glyph_dsc(&aos_inter_20,&glyph,0x4e2d,0);
        if (!found || glyph.is_placeholder || !glyph.resolved_font || glyph.box_w!=20 || glyph.box_h!=20) {
            ok=false;
        } else {
            gids[variant]=glyph.gid.index;
            lv_draw_buf_t *buffer=lv_draw_buf_create(glyph.box_w,glyph.box_h,LV_COLOR_FORMAT_A8,0);
            if (!buffer) ok=false;
            else {
                memset(buffer->data,0,buffer->header.stride*glyph.box_h);
                bool drawn=lv_font_get_glyph_bitmap(&glyph,buffer)!=NULL;
                unsigned ink=0;
                for (unsigned p=0;p<buffer->header.stride*glyph.box_h;p++) ink+=buffer->data[p];
                if (!drawn || !ink) ok=false;
                lv_font_glyph_release_draw_data(&glyph);
                lv_draw_buf_destroy(buffer);
            }
        }
        aos_hal_log("i18n-test","%s system=%d glyph=%lu",codes[variant],aos_i18n_count(),(unsigned long)gids[variant]);
    }
    if (gids[0]==gids[1]) ok=false;
    if (!aos_i18n_set(original)) ok=false;
    if (had_preference) aos_hal_pref_set_str(PREF_KEY,preferred);
    aos_hal_log("i18n-test","%s: %d native catalogues, two regional glyphs; restored %s",
                ok ? "PASS" : "FAIL",checked,original);
}
