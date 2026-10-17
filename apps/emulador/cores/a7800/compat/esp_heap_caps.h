/*
 * P4OS - Emulador, Atari 7800 core: stands in for ESP-IDF's esp_heap_caps.h
 * (ProSystem asks for its buffers in PSRAM). On the board the two functions
 * are the firmware's (aos_symbols.c exports them); in the simulator they are
 * malloc and free.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT   (1u << 2)

#ifdef AOS_SIM
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(p) free(p)
#else
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
#endif
