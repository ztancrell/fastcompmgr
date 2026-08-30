#include <stdbool.h>
#include <stdio.h>

#include "../comp_rect.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                           \
            ++failures;                                                       \
        }                                                                      \
    } while (0)

static bool
rect_equal(const CompRect *a, const CompRect *b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 &&
           a->x2 == b->x2 && a->y2 == b->y2 &&
           a->w == b->w && a->h == b->h;
}

static void
test_fully_occluded(void)
{
    CompRect ignore = { .x1 = 10, .y1 = 20, .x2 = 110, .y2 = 100,
                        .w = 100, .h = 80 };
    const CompRect original = ignore;
    CompRect window = { .x1 = 20, .y1 = 30, .x2 = 60, .y2 = 70,
                        .w = 40, .h = 40 };

    CHECK(!rect_paint_needed(&ignore, &window));
    CHECK(rect_equal(&ignore, &original));
}

static void
test_disjoint_regions(void)
{
    CompRect ignore = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10,
                        .w = 10, .h = 10 };
    CompRect smaller = { .x1 = 20, .y1 = 20, .x2 = 25, .y2 = 25,
                         .w = 5, .h = 5 };
    const CompRect original = ignore;

    CHECK(rect_paint_needed(&ignore, &smaller));
    CHECK(rect_equal(&ignore, &original));

    CompRect larger = { .x1 = 30, .y1 = 30, .x2 = 50, .y2 = 50,
                        .w = 20, .h = 20 };
    CHECK(rect_paint_needed(&ignore, &larger));
    CHECK(rect_equal(&ignore, &larger));
}

static void
test_partial_overlap(void)
{
    CompRect ignore = { .x1 = 0, .y1 = 0, .x2 = 20, .y2 = 20,
                        .w = 20, .h = 20 };
    const CompRect original = ignore;
    CompRect window = { .x1 = 10, .y1 = 5, .x2 = 35, .y2 = 15,
                        .w = 25, .h = 10 };

    CHECK(rect_paint_needed(&ignore, &window));
    CHECK(rect_equal(&ignore, &original));
}

static void
test_intersection_coordinates(void)
{
    /* Zero area metadata forces selection of the computed intersection. */
    CompRect ignore = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10,
                        .w = 0, .h = 0 };
    CompRect window = { .x1 = 5, .y1 = 4, .x2 = 15, .y2 = 12,
                        .w = 0, .h = 0 };
    const CompRect expected = { .x1 = 5, .y1 = 4, .x2 = 10, .y2 = 10,
                                .w = 5, .h = 6 };

    CHECK(rect_paint_needed(&ignore, &window));
    CHECK(rect_equal(&ignore, &expected));
}

static void
test_touching_edges(void)
{
    CompRect ignore = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10,
                        .w = 10, .h = 10 };
    const CompRect original = ignore;
    CompRect window = { .x1 = 10, .y1 = 2, .x2 = 15, .y2 = 8,
                        .w = 5, .h = 6 };

    CHECK(rect_paint_needed(&ignore, &window));
    CHECK(rect_equal(&ignore, &original));
}

static void
test_large_area_comparison(void)
{
    CompRect ignore = { .x1 = 0, .y1 = 0, .x2 = 40000, .y2 = 50000,
                        .w = 40000, .h = 50000 };
    CompRect larger = { .x1 = 100000, .y1 = 0,
                        .x2 = 150000, .y2 = 50000,
                        .w = 50000, .h = 50000 };

    CHECK(rect_paint_needed(&ignore, &larger));
    CHECK(rect_equal(&ignore, &larger));
}

int
main(void)
{
    test_fully_occluded();
    test_disjoint_regions();
    test_partial_overlap();
    test_intersection_coordinates();
    test_touching_edges();
    test_large_area_comparison();

    if (failures != 0) {
        fprintf(stderr, "comp_rect: %d test(s) failed\n", failures);
        return 1;
    }

    puts("comp_rect: all tests passed");
    return 0;
}
