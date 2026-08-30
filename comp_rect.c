
#include "comp_rect.h"


static uint64_t rect_area(const CompRect* rect){
    if(rect->w <= 0 || rect->h <= 0){
        return 0;
    }

    return (uint64_t)(uint32_t)rect->w * (uint32_t)rect->h;
}



/// Returns true, if r1 fully contains r2
static bool rect_contains(CompRect* r1, CompRect* r2){
    return r1->x1 <= r2->x1 && r1->y1 <= r2->y1 &&
            r1->x2 >= r2->x2 && r1->y2 >=r2->y2;
}


static bool rects_are_intersecting(CompRect* r1, CompRect* r2)
{
    // if the left point of one rect is greater
    // than the right one of the other, nothing intersects.
    if(r1->x1 > r2->x2 || r2->x1 > r1->x2){
        return false;
    }
    if(r1->y1 > r2->y2 || r2->y1 > r1->y2){
        return false;
    }
    return true;
}

/// Check if we can omit painting a window (rect). E.g., a window
/// completely occluded by another one, does not need to be
/// painted. Further, we try to select the largest possible ignore region
/// Based on window and intersection areas.
bool rect_paint_needed(CompRect* ignore_reg, CompRect* reg){
    if(rect_contains(ignore_reg, reg)){
        // the ignore-region completely occludes the window.
        return false;
    }
    if(! rects_are_intersecting(ignore_reg, reg)){
        // KISS and just use the greater rect as new ignore region.
        if(rect_area(reg) > rect_area(ignore_reg)){
            *ignore_reg = *reg;
        }
        return true;
    }

    // calculate the intersection rect.
    int32_t x1 = (ignore_reg->x1 > reg->x1) ? ignore_reg->x1 : reg->x1;
    int32_t x2 = (ignore_reg->x2 < reg->x2) ? ignore_reg->x2 : reg->x2;
    int32_t y1 = (ignore_reg->y1 > reg->y1) ? ignore_reg->y1 : reg->y1;
    int32_t y2 = (ignore_reg->y2 < reg->y2) ? ignore_reg->y2 : reg->y2;
    int32_t w = x2 - x1;
    int32_t h = y2 - y1;

    // KISS and just use the biggest rect as new ignore rect
    if(rect_area(reg) > rect_area(ignore_reg)){
        *ignore_reg = *reg;
    }
    CompRect r_intersect = {.x1 = x1, .y1 = y1,
              .x2 = x2, .y2 = y2,
              .w = w, .h = h };
    if(rect_area(&r_intersect) > rect_area(ignore_reg)){
        *ignore_reg = r_intersect;
    }
    return true;
}
