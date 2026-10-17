/*
 * P4OS - Emulador: the cores' texts, for the catalogs.
 *
 * The cores (cores/<id>/) hand the app Spanish keys: their options' labels
 * and values, and the errors load() returns. The app translates them where
 * it shows them (em_menu.c, em_run.c), but tools/gen_lang.py only reads
 * main/, so they are listed here as well. Nothing uses this table: it is
 * for the extractor. A new core's texts are added here.
 */
#include "aos_i18n.h"

__attribute__((unused)) static const char *const CORE_TEXTS[] = {
    /* NES (cores/nes/nes_core.c) */
    N_("Paleta"), N_("Compuesta"), N_("Suave"),
    N_("Recortar bordes"), N_("Recortar costados"), N_("Límite de sprites"),
    N_("Sí"), N_("No"),
    N_("No se pudo abrir el archivo"), N_("No es un juego de NES"),
    N_("El archivo es demasiado grande"), N_("No hay memoria"),
    N_("Falta la BIOS del Famicom Disk System (disksys.rom)"),
    /* Game Boy / Game Boy Color (cores/gb/gb_core.c) */
    N_("Paleta (juegos de Game Boy)"), N_("Verde (DMG)"), N_("Gris"), N_("Pocket"),
    N_("Color automático"), N_("No es un juego de Game Boy"),
    N_("Este tipo de cartucho no está soportado"), N_("No se pudo leer el juego"),
    /* Game Boy Advance (cores/gba/gba_core.c) */
    N_("BIOS"), N_("Automática"), N_("Integrada"),
    N_("Corrección de color"), N_("Mezcla de cuadros"),
    N_("El archivo no es un juego de Game Boy Advance"),
    N_("El juego es más grande que un cartucho de Game Boy Advance"),
    N_("No hay memoria suficiente para el juego"),
    N_("No se pudo abrir el juego"), N_("No se pudo cargar el juego"),
};
