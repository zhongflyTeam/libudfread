/*
 * Minimal test framework for libudfread tests
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef TEST_UTIL_H_
#define TEST_UTIL_H_

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond) \
    do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); test_failures++; } } while (0)

#define CHECK_NOT_NULL(p) CHECK((p) != NULL)

#define CHECK_EQ_UINT(exp, got) \
    do { uint32_t _e = (uint32_t)(exp), _g = (uint32_t)(got); \
         if (_e != _g) { printf("FAIL %s:%d: %s = %lu, expected %lu\n", __FILE__, __LINE__, #got, (unsigned long)_g, (unsigned long)_e); test_failures++; } } while (0)

#define CHECK_EQ_INT(exp, got) \
    do { int _e = (int)(exp), _g = (int)(got); \
         if (_e != _g) { printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #got, _g, _e); test_failures++; } } while (0)

#define CHECK_EQ_STR(exp, got) \
    do { const char *_e = (exp), *_g = (got); \
         if (!_e || !_g || strcmp(_e, _g) != 0) { printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #got, _g ? _g : "(null)", _e ? _e : "(null)"); test_failures++; } } while (0)

/* capture logger for ecma_ctx / udfread_set_log */
struct capture_log {
    char   buf[1024];
    size_t len;
    int    calls;
};

static int capture_logger(void *c, const char *fmt, ...)
{
    struct capture_log *cl = (struct capture_log *)c;
    va_list ap;
    int n;
    size_t room = sizeof(cl->buf) - cl->len;

    va_start(ap, fmt);
    n = vsnprintf(cl->buf + cl->len, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        cl->len += (size_t)n;
        if (cl->len > sizeof(cl->buf)) {
            cl->len = sizeof(cl->buf);
        }
    }
    cl->calls++;
    return n;
}

#define TEST_ENTRY(fn) { #fn, fn }

/* little-endian buffer writers (matches ecma167.h _get_u* byte order) */
static inline void set_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void set_u32(uint8_t *p, uint32_t v) { set_u16(p, v); set_u16(p + 2, v >> 16); }
static inline void set_u64(uint8_t *p, uint64_t v) { set_u32(p, (uint32_t)v); set_u32(p + 4, (uint32_t)(v >> 32)); }

/* write a 16-byte descriptor tag with valid checksum */
static inline void set_tag(uint8_t *p, uint16_t id)
{
    int i;
    uint8_t sum = 0;

    memset(p, 0, 16);
    set_u16(p, id);
    for (i = 0; i < 4; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    for (i = 5; i < 16; i++) {
        sum = (uint8_t)(sum + p[i]);
    }

    p[4] = sum;
}


#define RUN_TESTS(...) \
    int main(void) \
    { \
        struct { const char *name; void (*fn)(void); } tests[] = { __VA_ARGS__ }; \
        size_t i; \
        int total = 0; \
        for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) { \
            test_failures = 0; \
            printf("test: %s\n", tests[i].name); \
            tests[i].fn(); \
            if (test_failures) { \
                printf("  %d failure(s)\n", test_failures); \
                total += test_failures; \
            } \
        } \
        return total ? 1 : 0; \
    }

#endif /* TEST_UTIL_H_ */