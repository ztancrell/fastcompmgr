#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    int32_t w;
    int32_t h;
} CompRect;



bool rect_paint_needed(CompRect* ignore_reg, CompRect* reg);
