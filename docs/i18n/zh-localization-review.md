# Chinese localization quality review

Review date: 2026-10-27

## Result

The Simplified Chinese (`zh-Hans`) and Traditional Chinese (`zh-Hant`) catalogs now cover every string extracted by the repository localization tooling. The review covered system UI, settings, networking, Bluetooth, storage, OTA/recovery, errors, onboarding, native applications, dynamic web applications, IoT protocols, and representative empty/offline/disabled states.

| Measure | zh-Hans | zh-Hant |
| --- | ---: | ---: |
| Catalog files | 80 | 80 |
| Required strings | 10,621 | 10,621 |
| Present strings | 10,621 | 10,621 |
| Missing / extra / empty | 0 / 0 / 0 | 0 / 0 / 0 |
| Existing translations changed in this pass | 1,071 | 1,086 |
| New localized keys added | 25 | 25 |
| Catalogs changed | 14 | 15 |
| Reviewed regeneration overrides | 1,110 in 34 catalogs | 725 in 15 catalogs |

Sixty values in each locale remain identical to their Spanish source keys. They are an explicit, test-enforced allowlist of brands, protocol names, units, formatted values, and machine-facing notation such as `BLE`, `CAN`, `MQTT`, `USB`, `Wi-Fi`, `CAS`, `Cartero`, `Home Assistant`, and printf-based measurements. They are not unreviewed fallbacks.

## Method

1. Used `tools/gen_lang.py collect()` as the source-of-truth inventory and `read_pack()` for exact catalog parsing.
2. Compared every required key with both Chinese catalogs and validated empty values, printf arguments, and literal `\n` counts.
3. Searched first-party C and JavaScript sources for visible text outside localization calls and corrected confirmed user-facing cases.
4. Reviewed terminology independently for Mainland Simplified Chinese and Taiwan Traditional Chinese; Traditional Chinese was not produced by mechanical character conversion.
5. Preserved API fields, JSON keys, protocol identifiers, enum values, filesystem paths, commands, placeholders, Xcas expressions, and MQTT topics byte-for-byte.
6. Regenerated the embedded native catalogs and checked their decompressed contents against the source catalogs.
7. Exercised representative portal and dynamic-app flows in Chromium at desktop and 390 x 844 mobile viewports.

The durable regional vocabulary and writing rules are recorded in [zh-terminology.md](zh-terminology.md).

## High-severity fixes

- Corrected safety-critical CAN, EEPROM write-protect, PWM/servo power, LED current, RS-485/Modbus, MQTT retained-message, and BLE bind-key instructions.
- Rewrote portal access, trusted-network, password, token, HTTPS certificate, OTA trial/rollback, recovery, and crash-dump language.
- Replaced an unlocalized closed-network HTML response with localized UTF-8 plain text, avoiding unsafe insertion of SD-overridable translations into HTML.
- Localized visible macro-pad defaults while leaving key chords, scripts, topics, payloads, and commands unchanged.
- Added presentation-only translations for OTA machine states without altering the API contract.
- Preserved all CAS/Xcas expressions and repaired translated function names and mathematical notation.

## Product-language fixes

- Fully reviewed Cartero and 3D-printer native/web catalogs, including setup, authentication, headers, bodies, tests, temperatures, layers, cameras, serial/LAN states, validation, errors, and empty states.
- Corrected Cartero wording such as on-device execution, imports, storage-card actions, request collections, and response terminology.
- Scoped Cartero and Printer web translators to their own catalogs. This prevents identical Spanish keys from being overwritten by another dynamically loaded application.
- Replaced hardcoded portal time suffixes with locale-aware `Intl.NumberFormat` units.
- Standardized Taiwan usage including `主畫面`, `設定`, `網路`, `存取`, `憑證`, `記錄`, `匯入`, `權杖`, `標頭`, and `重新啟動`.
- Corrected periodic-table names. International element symbols are used for regionally ambiguous or unsupported rare glyphs.

## Visual review

The current portal was checked in Simplified and Traditional Chinese on desktop and mobile for settings/security, firmware/recovery, Cartero, 3D printers, and CAS. Setup forms and representative empty/offline states were included.

Observed results:

- No missing-glyph boxes in the reviewed flows.
- No body or main-content horizontal overflow at 390 x 844.
- The mobile bottom navigation remains intentionally horizontally scrollable.
- Long security, recovery, validation, and printer setup text remained readable.
- Xcas expressions remained intact.
- Dynamic pages consistently used their own scoped catalogs after the loader fix.

Native LVGL screenshots could not be produced in this environment because no C/C++ simulator toolchain or existing simulator build was available. Native coverage was instead checked through extraction, format contracts, embedded-catalog round trips, CJK font coverage, and source review. Physical-device review remains advisable for panel-specific truncation and line wrapping.

## Automated verification

The final verification set covers:

- all four catalog checks (`en`, `de`, `zh-Hans`, `zh-Hant`);
- Chinese coverage, placeholders, newlines, fallback allowlist, reviewed overrides, CJK resource integrity, glyph coverage, and embedded-catalog round trips;
- portal locale behavior, locale-aware units, and dynamic-page catalog scoping;
- localized closed-network response and generated web assets;
- repository whitespace validation with `git diff --check`.

Complete machine coverage does not imply that every long narrative sentence has been certified by a professional translator. The highest-risk operational, security, recovery, protocol, and primary application flows received direct contextual review in this pass.
