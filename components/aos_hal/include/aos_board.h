#pragma once

/* lcd5 remains the default for existing build commands. */
#ifndef AOS_BOARD_ID
#define AOS_BOARD_ID 5
#endif

#if AOS_BOARD_ID == 5
#define AOS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-5"
#define AOS_NATIVE_W 720
#define AOS_DSI_MBPS 700
#elif AOS_BOARD_ID == 7
#define AOS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-7"
#define AOS_NATIVE_W 720
#define AOS_DSI_MBPS 1000
#elif AOS_BOARD_ID == 8 || AOS_BOARD_ID == 101
#if AOS_BOARD_ID == 8
#define AOS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-8"
#else
#define AOS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1"
#endif
#define AOS_NATIVE_W 800
#define AOS_DSI_MBPS 1500
#else
#error Unsupported AOS_BOARD_ID
#endif
#define AOS_NATIVE_H 1280

/* Physical header pins in D-/D+ order; protocol port identifiers stay stable. */
#if AOS_BOARD_ID == 5
#define AOS_USB_FS_PINS "21/23"
#define AOS_USB_HS_PINS "25/27"
#else
#define AOS_USB_FS_PINS "19/17"
#define AOS_USB_HS_PINS "15/13"
#endif

#if AOS_BOARD_ID == 5
#define AOS_HEADER_PIN(lcd5_pin) (lcd5_pin)
#else
#define AOS_HEADER_PIN(lcd5_pin) ((lcd5_pin) % 2 ? 40 - (lcd5_pin) : 42 - (lcd5_pin))
#endif
