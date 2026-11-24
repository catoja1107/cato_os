/*
 * Minimal <string.h> for building BearSSL without a C library. The functions
 * are implemented in boot/mem.c.
 */
#ifndef CATO_SHIM_STRING_H
#define CATO_SHIM_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *a, const void *b, size_t count);
size_t strlen(const char *text);

#endif
