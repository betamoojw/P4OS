# Hardware profiles

P4OS keeps the original LCD-5 as the default and adds explicit profiles for
Waveshare's ESP32-P4-WIFI6-Touch-LCD-7, -8 and -10.1 (the LCD-X family).
These are not the LCD-7B or LCD-XC boards.

| Build profile | Panel | Native pixels | Driver | DSI lane rate |
| --- | --- | --- | --- | --- |
| `lcd5` (default) | 5 inch | 720 × 1280 | HX8394 | 700 Mbit/s |
| `lcd7` | 7 inch | 720 × 1280 | ILI9881C | 1000 Mbit/s |
| `lcd8` | 8 inch | 800 × 1280 | JD9365, 8-inch sequence | 1500 Mbit/s |
| `lcd10_1` | 10.1 inch | 800 × 1280 | JD9365, 10.1-inch sequence | 1500 Mbit/s |

All use two DSI lanes, RGB565 framebuffers and software rotation. Landscape
is 1280 × 720 or 1280 × 800. The 8-inch and 10.1-inch command tables differ;
select the actual panel even though their resolutions match.

## Building and installing

Display model and P4 silicon revision are independent. Use the revision
reported by the chip; screen size does not identify the silicon revision.
In an ESP-IDF 5.5 environment:

```sh
# Existing commands keep building LCD-5.
tools/build_fw.sh rev3_x

# Pick lcd7, lcd8 or lcd10_1; use rev1_3 for older silicon.
AOS_BOARD=lcd8 tools/build_fw.sh rev3_x
AOS_BOARD=lcd8 PROFILE=rev3_x tools/build_apps.sh
AOS_BOARD=lcd8 tools/build_fw.sh rev3_x -p PORT flash monitor
AOS_BOARD=lcd8 tools/ota.sh p4os.local rev3_x
AOS_BOARD=lcd8 PROFILE=rev3_x tools/heap_owners.py p4os.local
```

LCD-5 keeps `build/rev3_x` and `build/rev1_3`. Other profiles use
`build/<board>/<revision>`, including their own sdkconfig, firmware and ELF.
Never upload an image built for a different display. Existing release images
named only by silicon revision remain LCD-5 images.
Crash decoding searches all board/revision build directories and selects an
ELF only when its hash matches the crashed firmware.

On Windows, or without the shell helpers, run the equivalent from the project
root in an activated ESP-IDF terminal (quote the semicolon-separated defaults):

```sh
idf.py -B build/lcd8/rev3_x -D AOS_BOARD=lcd8 -D SDKCONFIG=build/lcd8/rev3_x/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev3_x" build
```

Use a fresh build directory when changing board or silicon revision. CMake
rejects unknown board names. C6 firmware and partitions do not change with
screen size; follow [C6.md](C6.md) for the radio firmware.

## Apps, simulator and bench

Rebuild and reinstall dynamic apps with this firmware: ABI **101** rejects
older apps whose framebuffer stride was fixed at 720 pixels. Rebuilt apps
obtain native width from the host's logical resolution and rotation, so the
same app binaries work with both panel widths **within one silicon profile**.
Native height remains 1280. The two silicon profiles select different instruction
sets, so keep their app packages separate and match the running firmware.
Fixed-resolution game artwork stays at its existing size and uses the current
scaling/letterboxing paths; no replacement art packs are required.
Golf allocates its full-screen buffers from the host geometry; Lua and Python
accept script canvases up to the host panel's pixel count, including 800 × 1280.

After building all eight firmware variants, rebuild and check the complete
dynamic-app matrix from an activated ESP-IDF terminal:

```sh
python tools/build_apps.py --all-profiles --all-boards
# Recheck existing outputs without recompiling:
python tools/build_apps.py --all-profiles --all-boards --check-only
```

App outputs are isolated in `apps/<app>/build/<revision>/`; emulator modules
are in its `modules/` subdirectory. Reports and successful complete packages
are written to `build/apps-abi101/<revision>/`. Validation checks ABI entry
points, LVGL configuration, silicon compiler flags, loader relocations and
imports against each firmware ELF's actual resolver tables. This is static
compatibility validation, not an on-device execution test.

Build the CAS engine first with `apps/cas/engine/build_engine.sh` (see its
prerequisites); it uses the common baseline ISA for both profiles. Installation
uses `PROFILE=rev3_x tools/install_apps.sh HOST` or `PROFILE=rev1_3` as appropriate.
Release packages are `apps-rev1_3.zip` and `apps-rev3_x.zip`; the legacy
`apps.zip` name is an alias for **rev1_3 only**.

Simulator selection uses the same CMake option:

```sh
cmake -S sim -B sim/build-lcd8 -DAOS_BOARD=lcd8
cmake --build sim/build-lcd8
```

The hardware benchmark also shares the panel driver and touch adapter:

```sh
cd bench/p4bench
idf.py -B build/lcd8-rev3_x -D AOS_BOARD=lcd8 -D SDKCONFIG=build/lcd8-rev3_x/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev3_x" build
```

The bench's `touch int` and `touch rst` GPIO probes are skipped on LCD-X:
its touch reset and interrupt are not connected.

## Implementation and limits

`components/aos_board` owns panel timings, reset polarity, vendor sequences
and touch creation. LCD-X sequences come from Waveshare LCD-X BSP **2.0.3**,
commit `0694fdb9fa7751cc2719a9d1927de70547b89ba0`, under Apache-2.0.
The common I2C/audio/backlight services continue to use the existing LCD-5
BSP: I2C SDA/SCL 7/8, I2S DOUT/LRCK/DIN/BCLK/MCLK 9/10/11/12/13,
amplifier 53, backlight 26, LCD reset 27 and SDMMC 39–44 are shared.
LCD-X touch is created separately with the correct resolution, no reset/INT
GPIO, and probing of both Goodix addresses (0x5D and 0x14).

**The physical header numbering differs.** LCD-X numbers the rows from the
opposite end. Its pin 1 is GPIO48, not 5 V. The firmware's header view,
USB settings and bench map follow the selected board. `modules.txt` continues
to use GPIO numbers, so its GPIO assignments do not need conversion.

| Connection | LCD-5 physical pins | LCD-X physical pins |
| --- | --- | --- |
| 5 V | 1, 3 | 39, 37 |
| 3V3 | 2, 18 | 40, 24 |
| GND examples | 5, 19 | 35, 21 |
| Board I2C SDA/SCL (GPIO7/8) | 4/6 | 38/36 |
| External I2C SDA/SCL (GPIO21/22) | 15/17 | 25/23 |
| USB Full Speed D−/D+ (GPIO24/25) | 21/23 | 19/17 |
| USB High Speed D−/D+ (OTG) | 25/27 | 15/13 |

Use the [official pinout](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-X#pinout-definition)
to orient the connector. Older wiring examples refer to LCD-5 physical pins.
The USB API accepts `fs`, `hs`, `both`, and the selected board's physical
pairs. Legacy `21/23` and `25/27` tokens remain controller identifiers for
compatibility; `physical_pins` in the response gives the actual wiring pair.

The GT9271 panel advertises ten contacts; the current P4OS/GT911-compatible
touch path exposes **up to five**, matching the existing HAL contract. Camera
and battery monitoring are not newly enabled: physical hardware presence does
not mean those HAL capabilities are implemented.

LCD-5 retains its measured 80 MHz/front-porch tuning and runtime refresh
requests. LCD-X uses the vendor timing; `pnl_mhz`/`pnl_vfp` preferences and
runtime refresh changes do not override it. Framebuffer count, brightness,
rotation, screenshots, video, retro rendering and memory sizing use the
selected geometry. Performance numbers in older documents describe LCD-5.

The new profiles require physical bring-up before being treated as validated
hardware support. Check each model for cold boot, colors, brightness, touch
at all edges in four rotations, SD read/write, Wi-Fi/BLE, audio input/output,
USB and header peripherals. Stress video and games while observing panel
underruns and free memory. Also repeat these checks on LCD-5 after upgrading.
Do not infer a successful board test from a successful compilation.

## Validation recorded on 2026-10-21

- Regression review corrected diagnostic ELF selection across board/revision
  directories, removed an LCD-5-only touch annotation from the LCD-X bench,
  and replaced ignored firmware configuration entries with the current SDIO
  transport setting. The bench retains its older esp_hosted configuration.
- Maps caches the runtime frame size before clearing its pixel buffer, avoiding
  repeated host calls per pixel, and reports the actual benchmark dimensions.
- Build-helper selection and heap-diagnostic ELF selection passed for all
  eight combinations. Crash lookup passed matching 9/16-character hash-prefix
  checks and rejected a nonmatching hash.
- ESP-IDF 5.5.5: all eight firmware variants compiled and linked successfully.
  The LCD-5 / rev1_3 configuration was also regenerated and rebuilt after the
  defaults cleanup.
- Image headers, board compile definitions and PSRAM settings were checked:
  rev1_3 uses minimum revision 100 and 200 MHz PSRAM; rev3_x uses minimum
  revision 300 and 250 MHz PSRAM. All images fit the 8,323,072-byte app partition.
- Public geometry and physical-header regression tests: five tests passed.
- Display adapter and HAL compile checks: all four board selections passed.
- Native-framebuffer app sources compiled with runtime stride enabled.
- Changed bench sources passed compile checks for all four profiles; the
  complete bench firmware build is covered by CI, not locally linked here.
- Simulator rendering and physical boards were not exercised. CI includes
  the three LCD-X models with both silicon profiles and the hardware bench.

| Board | rev1_3 image bytes | rev3_x image bytes | Build result |
| --- | ---: | ---: | --- |
| LCD-5 | 7,852,208 | 7,854,832 | Both passed |
| LCD-7 | 7,862,960 | 7,865,584 | Both passed |
| LCD-8 | 7,858,016 | 7,860,576 | Both passed |
| LCD-10.1 | 7,858,560 | 7,861,184 | Both passed |

These local review builds use `build/<board>/<revision>/` for all four boards,
including `lcd5`; the shell helper's LCD-5 paths remain unchanged. Each folder
contains `p4os.bin`, `p4os.elf`, bootloader, partition table, sdkconfig and its
`build-review.log`. `build/board-build-results.json` records image hashes and
verification results. Remaining compiler warnings are unused functions in
LVGL's bundled `stb_image.h`; no project-source compiler warnings were found.

### Dynamic applications, ABI 101

All 50 dynamic apps were rebuilt separately for each silicon profile (100 app
builds), together with the emulator's 11 core modules per profile. Final checks
passed against the actual linked firmware for all four boards in each profile:

| Silicon profile | ABI 101 apps | Emulator core modules (core ABI 3) | Firmware checks |
| --- | ---: | ---: | --- |
| rev1_3 | 50 passed | 11 passed | LCD-5, LCD-7, LCD-8, LCD-10.1 passed |
| rev3_x | 50 passed | 11 passed | LCD-5, LCD-7, LCD-8, LCD-10.1 passed |

Checks include ELF format, float ABI, ABI entry points, LVGL 9.5.0 and its
configuration, silicon compiler flags, dynamic relocations, resolver imports
and embedded local paths. Five geometry/header tests and five compatibility
checker tests passed, including rejection of old ABI and missing host symbols.

Review fixed shared build-directory reuse, missing header dependencies,
Windows command-length limits, emulator tool selection with `.exe` suffixes,
and CAS source extraction/link-helper issues. Golf and Lua/Python now use the
host panel's pixel capacity. CI and installation use separate silicon packages.

Complete local packages are
`build/apps-abi101/<revision>/apps-abi101-<revision>.zip`.
The adjacent `report.json` records firmware and app hashes and the checked
board matrix; a copy is included in each package. Existing artwork packs are
unchanged and are not included. No applications were installed and no physical
boards were exercised by this validation.

### LCD-10.1 hardware bring-up (2026-10-21)

The connected LCD-10.1 on COM24 identifies as ESP32-P4 revision **v3.2**,
with 32 MB flash and 32 MB PSRAM. The current `lcd10_1` / `rev3_x` build
passed and was flashed over serial at 460800 baud. Esptool verified the
written bootloader, partition table, OTA selection data and application.
No whole-chip erase was performed.

The initial application was 7,861,184 bytes, SHA-256
`7a4cb47ef3a292700ae66941a382bb9135e66461b8b7b9b5ea7610b8aea72023`.
Its running ELF hash matched the built ELF (`b8a4d278d43fa30d...`).

Serial startup confirmed:

- PSRAM at 250 MHz, memory test OK; CPU at 400 MHz.
- JD9365 LCD ID `93 65 04`, panel task and LVGL initialized; measured refresh
  68.6 Hz. This is not a visual inspection of the panel.
- Goodix touch ID bytes `39 32 37`, configuration version 130, touch initialized.
  Physical accuracy and gestures still require an operator check.
- ES8311 and ES7210 initialized, I2S at 48 kHz / 16-bit stereo. Audible output
  and microphone capture were not verified.
- `P4OS up` and boot stage `up`; HTTP service started on port 80.
- C6 SDIO initialized and radio came up with ESP-Hosted 2.12.13. The host
  3.0.9 reports a major-version mismatch, then enables compatible streaming
  mode. The bundled ESP-Hosted migration guide supports this combination;
  Wi-Fi association and HTTP status/screenshot transfers subsequently passed.
  BLE was disabled and not tested.

The HTTP status reported the correct board and a 1280 x 800 landscape UI.
Both native-framebuffer and logical-screen BMP downloads succeeded. At 172 s
uptime the panel reported 11,761 frames, zero underruns, zero late frames and
a 14.901 ms maximum frame gap. No panic or unexpected restart was seen during
the initial 120-second serial capture. Screenshot inspection verifies rendered
content, not physical panel appearance or touch accuracy.

This run exposed repeated first-home-network registration: `first_home()` used
an eight-byte buffer to test for a saved trusted-network list. NVS rejects a
buffer too small for the stored string, causing repeated writes and logs for
longer SSIDs. The probe now has the full trusted-list capacity. The corrected
application is 7,861,200 bytes, SHA-256
`33378fea2bacfcd40b8136cdcf33b98b8fae15822540cc8804e12f36669b45fe`;
ELF SHA-256 `9540b0507d9db48335aafd74fc50673b980bfdae7ecbbaea5680047ba8da3354`.
All ten geometry/ABI regression tests passed, and the 50 rev3_x apps plus
11 emulator modules passed binary compatibility checks against the updated
LCD-10.1 firmware and the other three existing rev3_x builds.

The first attempt to install this correction lost the USB serial connection;
the automatic retry also failed. A complete retry at 230400 baud succeeded,
with all written regions hash-verified. The corrected firmware booted with
ELF hash `9540b0507d9db483...`, restored Wi-Fi automatically at about 5.6 s,
and stopped the repeated first-home-network messages. Four successive portal
rotation requests and screenshot downloads passed (800 x 1280, 1280 x 800,
800 x 1280, 1280 x 800), restoring the initial landscape orientation.
At 56 s after this boot, 3,794 frames had zero underruns and zero late frames;
Wi-Fi reported zero drops. No C6 firmware update was needed for these tests.

microSD mounting timed out on all three attempts. Card presence is not yet
confirmed, so this does not establish a card-driver defect. No dynamic apps
were loaded from the card. The earlier ABI 101 matrix remains a build and
binary-compatibility result, not a physical execution test.

Local evidence: `hardware-build.log`, `hardware-flash.log` and
`hardware-boot.log` in `build/lcd10_1/rev3_x/`; the final corrected image is
covered by `hardware-flash-recovery.log`, `hardware-boot-fixed.log`,
`hardware-sysmon-fixed.json` and `hardware-rotations.json` in the same folder.

## Sources

- [Waveshare product page](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7-8-10.1.htm)
- [LCD-X hardware documentation](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-X)
- [Official examples and schematics](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-X)
- [Pinned BSP source](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/0694fdb9fa7751cc2719a9d1927de70547b89ba0/bsp/esp32_p4_wifi6_touch_lcd_x)
