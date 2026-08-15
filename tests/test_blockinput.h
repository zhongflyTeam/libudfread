/*
 * Mock block input for unit tests
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef TEST_BLOCKINPUT_H_
#define TEST_BLOCKINPUT_H_

#include "test_util.h"

#include "blockinput.h"

#include <string.h>

/* mock block input serving blocks from an in-memory image */
struct mock_input {
    udfread_block_input bi;   /* must be first: callbacks cast to this */
    uint8_t            *image;
    uint32_t            nblocks;
    uint32_t            read_calls;
    uint32_t            last_lba;

    /* optional expected-read trap: any read outside the declared set
     * fails the test (and returns 0, like an I/O error) */
    int                 expect_enabled;
    const uint32_t     *expect_lbas;
    uint32_t            expect_count;
};

static int mock_read(udfread_block_input *bi, uint32_t lba, void *buf, uint32_t nblocks, int flags)
{
    struct mock_input *m = (struct mock_input *)bi;

    (void)flags;
    m->read_calls++;
    m->last_lba = lba;

    if (m->expect_enabled) {
        uint32_t i;
        int found = 0;
        for (i = 0; i < m->expect_count; i++) {
            if (m->expect_lbas[i] == lba) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("FAIL %s:%d: unexpected read of lba %u\n", __FILE__, __LINE__, lba);
            test_failures++;
            return 0;
        }
    }

    if (lba > m->nblocks || nblocks > m->nblocks - lba) {
        return 0;
    }
    memcpy(buf, m->image + (size_t)lba * UDF_BLOCK_SIZE, (size_t)nblocks * UDF_BLOCK_SIZE);
    return (int)nblocks;
}

static uint32_t mock_size(udfread_block_input *bi)
{
    return ((struct mock_input *)bi)->nblocks;
}

static void mock_init(struct mock_input *m, uint8_t *image, uint32_t nblocks)
{
    memset(m, 0, sizeof(*m));
    m->image = image;
    m->nblocks = nblocks;
    m->bi.read = mock_read;
    m->bi.size = mock_size;
}

/* declare the set of LBAs the test expects to be read; any other read
 * fails the test. Pass count 0 to forbid all reads. */
static void mock_expect(struct mock_input *m, const uint32_t *lbas, uint32_t count)
{
    m->expect_enabled = 1;
    m->expect_lbas = lbas;
    m->expect_count = count;
}

#endif /* TEST_BLOCKINPUT_H_ */