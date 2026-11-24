/* Small bounded string helpers for freestanding code. */
#ifndef CATO_GUI_TEXT_H
#define CATO_GUI_TEXT_H

#include <stdint.h>

static inline int text_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void text_copy(char *dst, int capacity, const char *src)
{
    int i = 0;
    if (capacity <= 0)
        return;
    for (; src[i] && i < capacity - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static inline void text_append(char *dst, int capacity, const char *src)
{
    int n = text_len(dst);
    if (n < capacity)
        text_copy(dst + n, capacity - n, src);
}

static inline void text_append_u64(char *dst, int capacity, uint64_t value)
{
    char digits[21];
    int i = 20;
    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    text_append(dst, capacity, digits + i);
}

static inline void text_append_2d(char *dst, int capacity, unsigned value)
{
    char digits[3] = { (char)('0' + value / 10 % 10), (char)('0' + value % 10), 0 };
    text_append(dst, capacity, digits);
}

static inline int text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline int text_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

#endif
