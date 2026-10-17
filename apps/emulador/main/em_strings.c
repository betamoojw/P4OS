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
    /* Super Nintendo (cores/snes/snes_core.c) */
    N_("Calidad del sonido"), N_("Alta"), N_("Normal"), N_("Rápida"), N_("Sin sonido"),
    N_("No se pudo leer el archivo"), N_("No es un juego de Super Nintendo"),
    N_("Este juego usa el chip Super FX, que el emulador no tiene"),
    N_("Este juego usa el chip SA-1, que el emulador no tiene"),
    N_("Este juego usa el chip S-DD1, que el emulador no tiene"),
    N_("Este juego usa el chip SPC7110, que el emulador no tiene"),
    /* Master System, Game Gear, ColecoVision, PC Engine (cores/sms, cores/pce) */
    N_("Región"), N_("Estados Unidos"), N_("Europa"), N_("Japón"),
    N_("Modos anchos"), N_("Proporción de TV"), N_("Píxeles enteros"), N_("Izq"), N_("Der"),
    N_("No es un juego de Master System"), N_("No es un juego de Game Gear"),
    N_("No es un juego de ColecoVision"), N_("No es un juego de PC Engine"),
    N_("El juego es más grande que un cartucho de ColecoVision"),
    N_("Falta la BIOS de ColecoVision (coleco.rom)"),
    /* Mega Drive (cores/md/md_core.c) */
    N_("Mando"), N_("3 botones"), N_("6 botones"), N_("América"), N_("Completo"), N_("Rápido"),
    N_("Sin FM"), N_("No es un juego de Mega Drive"), N_("Los juegos de 32X no se pueden jugar"),
    /* Neo Geo (cores/neogeo/ng_core.c) */
    N_("Sistema"), N_("MVS (arcade)"), N_("AES (consola)"), N_("EE. UU."), N_("Asia"),
    N_("Calidad de sonido"), N_("Alta (44 kHz)"), N_("Media (22 kHz)"), N_("Baja (11 kHz)"),
    N_("Caché de sprites"),
    N_("No es un juego de Neo Geo conocido (el zip tiene que llamarse como en MAME)"),
    N_("No se pudo abrir el zip del juego"), N_("No se pudo leer el zip del juego"),
    N_("El zip del juego está dañado"), N_("Falta un archivo del juego en el zip"),
    N_("Falta el zip del juego original (este es una variante)"),
    N_("Falta la BIOS de Neo Geo: neogeo.zip de MAME (sp-s2.sp1, sfix.sfix) en la carpeta bios"),
    N_("Falta sfix.sfix (la BIOS de Neo Geo está incompleta)"),
    N_("Este juego tiene los gráficos cifrados: hace falta su caché hecha en una computadora"),
    N_("No hay lugar en la tarjeta para la caché del juego"),
    N_("No se pudo escribir la caché del juego en la tarjeta"),
    N_("No se pudo leer la caché del juego"),
};
