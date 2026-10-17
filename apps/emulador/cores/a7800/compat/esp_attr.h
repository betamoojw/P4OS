/*
 * P4OS - Emulador, Atari 7800 core: stands in for ESP-IDF's esp_attr.h,
 * which RetroESP32-P4's ProSystem includes for IRAM_ATTR. In an app loaded
 * from the card all code runs from PSRAM: the attribute means nothing here.
 */
#pragma once
#define IRAM_ATTR
