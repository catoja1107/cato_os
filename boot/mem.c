/*
 * A freestanding C compiler may emit calls to the memory functions even when
 * the source never names them, so the loader must provide them. BearSSL also
 * uses strlen.
 */
#include <stddef.h>

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = dest;
    while (count--)
        *d++ = (unsigned char)value;
    return dest;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (count--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (count--)
            *d++ = *s++;
    } else if (d > s) {
        while (count--)
            d[count] = s[count];
    }
    return dest;
}

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    return length;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < count; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}
