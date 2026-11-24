/* Pure UTF-16 number formatting, shared by the loader and the host unit tests. */
#ifndef CATO_BOOT_FMT_H
#define CATO_BOOT_FMT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Writes value in the given base (2 to 16) with at least min_digits digits and
 * a NUL terminator. Returns the digit count, or 0 if the buffer is too small
 * or the base is invalid (out[0] is then NUL when capacity allows).
 */
static inline size_t fmt_u64(uint16_t *out, size_t capacity, uint64_t value,
                             unsigned base, unsigned min_digits)
{
    static const char digits[] = "0123456789abcdef";
    uint16_t reversed[64];
    size_t count = 0;

    if (capacity > 0)
        out[0] = 0;
    if (base < 2 || base > 16 || min_digits > 64)
        return 0;

    do {
        reversed[count++] = (uint16_t)digits[value % base];
        value /= base;
    } while (value != 0);

    while (count < min_digits)
        reversed[count++] = '0';

    if (count + 1 > capacity)
        return 0;

    for (size_t i = 0; i < count; i++)
        out[i] = reversed[count - 1 - i];
    out[count] = 0;
    return count;
}

#endif
