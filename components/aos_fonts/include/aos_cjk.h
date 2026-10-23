#pragma once
#include "lvgl.h"
#include <stdbool.h>
/* The immutable subset lives in the board's storage partition, not on SD. */
bool aos_cjk_init(void);
void aos_cjk_language(const char *code);
