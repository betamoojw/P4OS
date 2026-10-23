# P4OS: personalización y "vida" del sistema

Lista de mejoras al sistema que se propusieron el 2026-10-22 para darle más
vida sin gastar RAM interna (lo que vaya a memoria, a PSRAM). Se aplican de
a poco; esta lista dice qué está hecho y qué queda.

## Hechas

| Mejora | Dónde | Cuándo |
|---|---|---|
| Fotos de fondo, una para el inicio y otra para el bloqueo, elegidas desde Fotos (la paleta del visor); oscurecidas según su propio brillo; se sueltan con una app al frente | `aos_wallpaper.c`, Ajustes, Fondo de pantalla | 0.21.5 |
| Color de acento a elección (nueve colores del sistema de iOS); `AOS_C_ACCENT` pasó a ser una llamada | `aos_theme.c`, Ajustes, Fondo de pantalla | después de 0.21.5 (71da541) |
| Fondo según la hora del día: noche, amanecer, día, atardecer, recalculado por minuto, en el inicio y el bloqueo | `aos_ui.c` (`DYN[]`) | después de 0.21.5 (309a8c8) |
| Reloj del bloqueo a elección: tamaño, lugar, color (blanco, acento o el de la foto del bloqueo, aclarado) | `aos_lock.c`, Ajustes, Pantalla de bloqueo | después de 0.21.5 (309a8c8) |
| Sonidos del sistema (bloquear, desbloquear, notificación, llamada, captura, grabación, clic del teclado opcional); se apagan con la campana del Centro de control o en Ajustes, Sonido | `aos_sounds.c` | después de 0.21.5 (309a8c8) |
| Grabación de pantalla a MP4 (PPA + codificador H.264 del P4) | `aos_screenrec_p4.c`, Centro de control | 0.21.5 |

## Pendientes

| # | Mejora | Costo | Notas |
|---|---|---|---|
| 4 | **Widget de sensores en el inicio**: CO₂, temperatura, humedad o lo que haya conectado, desde el servicio de sensores | poco | lo que más le queda a esta placa; los widgets del inicio ya existen (`aos_ui_widget_*`) |
| 6 | **Presentación de fotos en el bloqueo**: un álbum de Fotos que rota cada vez que se bloquea | PSRAM (1,8 MB, el mismo búfer) | reusa `aos_wallpaper.c`: cambiar `lock_img` al bloquear |
| 7 | **Modo reposo**: con la placa acostada (y cargando, si algún día hay batería), un reloj grande con fotos o sensores, como el StandBy del iPhone | PSRAM | una app de sistema o una capa como el bloqueo |
| 8 | **Estilo de íconos**: forma (redondeado, círculo), cuántas columnas, nombres sí o no | nada | `aos_home.c`, `aos_icon.c` |
| 9 | **Animación de arranque** con logo, y **carpetas que se abren animadas** | poco | la pantalla de arranque ya muestra el avance real |

## Lo que se midió y conviene recordar

- Pasarle copias al PPA o al AXI-GDMA **no rinde** en esta placa: el cuello es
  la PSRAM, no la CPU (fotos de fondo y conversión de cámaras, 2026-10-22;
  `docs/CAMERAS.md`). Los aceleradores sirven donde cambian el formato o
  hacen cuentas sin copias de más: el codificador H.264, el JPEG, rotar y
  escalar con el PPA algo que ya está en su formato.
- La RAM interna libre mínima llegó a ~150-170 KB; nada de esto debe bajarla.
