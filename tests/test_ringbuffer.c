#include <stdio.h>
#include <stdlib.h>

#include "../ringbuffer.h"

ringBuffer_typedef(int, IntBuffer);

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                           \
            ++failures;                                                       \
        }                                                                      \
    } while (0)

static void
check_read(IntBuffer *buffer, int expected)
{
    int actual = 0;

    CHECK(!isBufferEmpty(buffer));
    bufferRead(buffer, actual);
    CHECK(actual == expected);
}

static void
test_fifo_and_wrap(void)
{
    IntBuffer storage;
    IntBuffer *buffer = &storage;

    bufferInit(storage, 3, int);
    CHECK(storage.elems != NULL);
    CHECK(isBufferEmpty(buffer));

    bufferWrite(buffer, 1);
    bufferWrite(buffer, 2);
    bufferWrite(buffer, 3);
    CHECK(isBufferFull(buffer));
    check_read(buffer, 1);
    bufferWrite(buffer, 4);
    CHECK(isBufferFull(buffer));

    check_read(buffer, 2);
    check_read(buffer, 3);
    check_read(buffer, 4);
    CHECK(isBufferEmpty(buffer));
    bufferDestroy(buffer);
}

static void
test_full_buffer_overwrites_oldest(void)
{
    IntBuffer storage;
    IntBuffer *buffer = &storage;

    bufferInit(storage, 2, int);
    bufferWrite(buffer, 10);
    bufferWrite(buffer, 20);
    bufferWrite(buffer, 30);

    check_read(buffer, 20);
    check_read(buffer, 30);
    CHECK(isBufferEmpty(buffer));
    bufferDestroy(buffer);
}

static void
test_growth_preserves_wrapped_order(void)
{
    IntBuffer storage;
    IntBuffer *buffer = &storage;

    bufferInit(storage, 3, int);
    bufferWrite(buffer, 1);
    bufferWrite(buffer, 2);
    bufferWrite(buffer, 3);
    check_read(buffer, 1);
    check_read(buffer, 2);
    bufferWrite(buffer, 4);
    bufferWrite(buffer, 5);
    CHECK(isBufferFull(buffer));

    bufferIncrease(buffer, 8);
    CHECK(buffer->size == 8);
    CHECK(buffer->start == 0);
    CHECK(buffer->end == 3);
    check_read(buffer, 3);
    check_read(buffer, 4);
    check_read(buffer, 5);
    CHECK(isBufferEmpty(buffer));
    bufferDestroy(buffer);
}

static void
test_empty_buffer_growth(void)
{
    IntBuffer storage;
    IntBuffer *buffer = &storage;

    bufferInit(storage, 1, int);
    bufferIncrease(buffer, 4);
    CHECK(buffer->size == 4);
    CHECK(buffer->start == 0);
    CHECK(buffer->end == 0);
    CHECK(isBufferEmpty(buffer));

    bufferWrite(buffer, 99);
    check_read(buffer, 99);
    bufferDestroy(buffer);
}

int
main(void)
{
    test_fifo_and_wrap();
    test_full_buffer_overwrites_oldest();
    test_growth_preserves_wrapped_order();
    test_empty_buffer_growth();

    if (failures != 0) {
        fprintf(stderr, "ringbuffer: %d test(s) failed\n", failures);
        return 1;
    }

    puts("ringbuffer: all tests passed");
    return 0;
}
