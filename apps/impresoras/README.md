# Impresoras 3D (`aos.impresoras`)

Follows and controls every 3D printer in the house from the board. The list
shows each machine at a glance (state, nozzle and bed, progress, time left
and the clock time it ends); a machine opens into its detail, with the tabs
its protocol allows:

| Tab | What it has |
|---|---|
| Estado | a ring with the progress and the layer, the job's picture, time so far, left and the end; pause, resume, cancel (two taps); every heater, tap one to set it (presets and ±5°); preheat by material (PLA, PETG, ABS, ASA, TPU) and cool down; the last twenty minutes of nozzle and bed; a Bambu's AMS (colour, material, what is left, the one in use); speed, flow, fan, position, firmware |
| Control | the head by 0.1 / 1 / 10 / 50 mm, home (all, X, Y, Z), extrude and retract (refused with a cold nozzle), fan, speed and flow sliders (Bambu's four speed levels instead), light, motors off, OctoPrint's serial link, emergency stop (two taps). Moving by hand is refused while it prints |
| Archivos | the machine's files, newest first, with size and the slicer's estimate: print one, delete one |
| Cámara | the machine's camera, only while the tab is shown |
| Consola | G-code by hand and quick ones (G28, M114, M503...), with the answers where the protocol gives them |

The app lives in the background (`AOS_APP_FLAG_BACKGROUND`) and keeps asking
every 15 s, so a print that ends, fails, is cancelled or pauses on its own
(a filament runout) says so wherever the user is: a notice, the
notification sound, a number on the icon, and an hourglass in the status
bar while something prints.

## Protocols

One driver each (`main/drv_*.c`), all on the LAN and all open or published:

| Type | Machines | How | Key |
|---|---|---|---|
| `octoprint` | anything with OctoPrint (the user's MK3S) | REST, `X-Api-Key` | OctoPrint, Settings, API |
| `prusalink` | Prusa MK4/MK4S, MK3.9/3.5, MINI, XL, CORE One, MK3S with PrusaLink | `/api/v1`, HTTP Digest (user `maker`); the first PrusaLink (0.7) has OctoPrint's API and the driver switches to it | the printer's Settings, Network, PrusaLink |
| `bambu` | X1, X1E, P1P, P1S, A1, A1 mini, H2D | MQTT over TLS on 8883 (`device/<serial>/report` and `/request`), the P1/A1 camera on port 6000 | the access code, and the serial number |
| `moonraker` | Klipper: Voron, Sovol, Elegoo Neptune 4, Qidi, RatRig, Creality with Moonraker | Moonraker's REST on 7125 (or 80) | only with `[authorization]` |
| `duet` | Duet 2 / 3 with RepRapFirmware 3, standalone (`rr_*`) or with an SBC (`/machine`) | HTTP | the M551 password, if any |
| `elegoo` | Centauri Carbon; Saturn and Mars with Wi-Fi | SDCP 3, WebSocket on 3030 | none (the mainboard id is learnt) |
| `flashforge` | Adventurer 5M / 5M Pro / AD5X (HTTP on 8898), and every networked FlashForge by G-code on 8899 | HTTP + `~M…` lines | the check code and the serial (the old ones: none) |
| `creality` | K1, K1C, K1 Max, K2 Plus, Ender-3 V3, Hi, not rooted | Creality Print's WebSocket on 9999 | none |

**Prusa Connect** is Prusa's cloud and its API is not published; the same
printers are reached on the LAN through PrusaLink, which is what Connect
itself talks to.

**Bambu Lab:** since the firmware of early 2025 a printer only takes
commands on the LAN in Developer mode (it reports without it). Its
certificate is signed by Bambu's own authority and issued to its serial
number, not its address, so MQTT and the camera go through
`aos_tls_open_trust()` (firmware after 0.22.0): the chain is verified
against Bambu's public authorities only (`main/p3d_trust.c`: BBL CA, the
BBL CA2 ones, the H2C/P2S/X2C device authorities, as Bambu Studio and
ha-bambulab trust them) and the name against the serial typed in the
editor. Anything else is refused and the screen says which (another
serial, not Bambu's). An authority Bambu adds later goes in the card's
`/data/impresoras_ca.pem` without a new app. ha-bambulab notes that the
P2S on firmware 01.02.00.00 does not answer a TLS 1.3 hello; if one hangs,
that is the first suspect. The X1's camera is RTSP with H.264 at a profile
the board does not decode.

**The firmware:** the app calls `aos_tls_open_trust()`, which the board has
after 0.22.0. On 0.22.0 or older the `.so` does not load at all (a symbol
the table lacks): the board needs the firmware of 2026-10-25 first.

**Elegoo's 401-403 and Creality's WebSocket** follow what Creality Print,
OpenCentauri and the community integrations send; both are tried only
against the fake so far.

Not here: AnkerMake and Anycubic's own (closed clouds), Snapmaker, Repetier
Server and Ultimaker, which could be a driver each.

## Measured on the board

2026-10-25, three MK3S on OctoPrint over Wi-Fi:

| Camera | Arrives | Shown | Decode |
|---|---|---|---|
| a USB webcam, MJPEG 640x480 | 450-490 KB/s, 15 whole frames a second | 14.6-16.5 fps | 6 ms |
| a Raspberry Pi camera, MJPEG 1920x1080 | 510-560 KB/s, 1 whole frame a second | 1.0-1.4 fps | 17 ms |

The board shows every frame that arrives whole. With the 1080p camera what
limits the rate is the network: half a megabyte a frame. Whether the Pi's
Wi-Fi or the board's sets the ~500 KB/s is not known yet: both cameras
landed near it. The camera tab shows the camera's own size and its KB/s,
and suggests 640x480 when a big picture goes under 4 fps.

## Files

- `/data/impresoras.txt`: the machines (`main/p3d_cfg.c` has the format),
  written by the app's editor and by the portal's page. The keys are kept
  in the clear, and both say so.
- `/data/imp3d_<n>.img`: the job's picture, only while it is decoded.

## The portal's page

`web/impresoras.js` (`#impresoras`): every machine live (state, heaters,
progress, time left, pause / resume / cancel, preheat, light, G-code, and
the camera straight from the printer to the browser), and the list,
edited by fields with what each type needs and where to find it.
Live data goes through `aos_hal_live_*`: the app puts a JSON `status` every
second while the page looks and takes lines like `pause 0`, `temp 0 2 60`,
`gcode 0 G28` (`impresoras.c`, "the portal's page").

## Inside

- One thread per machine (`p3d_core.c`), 12 KB of stack in PSRAM; a machine
  that does not answer only slows itself, and backs off from 2 to 30 s.
- HTTP is the app's own over the HAL's connection (`p3d_net.c`): keep-alive,
  chunked, Basic and Digest. `aos_hal_http_*` has three slots for the
  system and does not give back the headers Digest needs.
- `destroy()` waits for every thread (up to 6 s): the loader unloads the
  `.so` on the next tick, and a thread still in its code would take the
  board down.
- Everything big is PSRAM: bodies, files, frames, the MQTT buffer (up to
  128 KB for an X1's report).

## Trying it

```
apps/impresoras/tools/fake_printers.py --certs <scratch>/certs --config sim/sim_fs/data/impresoras.txt
cp <scratch>/certs/ca.pem sim/sim_fs/data/impresoras_ca.pem     # the fake Bambu's authority
cd sim && cmake -B build-impresoras -DP4OS_SIM_APPS="hello_app;impresoras" && cmake --build build-impresoras -j8
P4_SIM_PORTAL_PORT=80xx P4_SIM_PREFS=<scratch>/prefs.txt ./build-impresoras/p4os_sim
```

The fake has one printer of each type on 127.0.0.1 (`--base`, 8301-8308,
plus FlashForge's 8899 and Bambu's camera on 6000), printing faster than
real (`--speed`), with cameras and job pictures. Its Bambu has a
certificate issued to its serial by a CA of its own, as a real one: with
another serial in the editor, or without `impresoras_ca.pem`, it is
refused. `P3D_CONFIG=<file>` points
the app at another list; `P3D_OPEN=<n>` opens the n-th machine and
`P3D_TAB=<0-4>` a tab, for screenshots.
