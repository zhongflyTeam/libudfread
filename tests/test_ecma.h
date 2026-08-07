/*
 * Shared helpers for ecma167/udf_volume unit tests
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef TEST_ECMA_H_
#define TEST_ECMA_H_

#include "test_util.h"

#include "ecma167.h"
#include "logger.h"
#include "udfread.h"   /* UDFREAD_LOG_TRACE */

/* capture-log + ecma_ctx setup.
 * Level TRACE: captures errors, informational messages and traces. */
static inline void log_init(struct capture_log *cap, udf_log *log, ecma_ctx *ctx)
{
    memset(cap, 0, sizeof(*cap));
    memset(log, 0, sizeof(*log));
    log->ctx = cap;
    log->logger = capture_logger;
    log->level = UDFREAD_LOG_TRACE;
    ctx->lc = log;
}

/* write a 16-byte long allocation descriptor (extent type 1) */
static inline void set_long_ad(uint8_t *p, uint32_t length, uint32_t lba, uint16_t partition)
{
    set_u32(p + 0, 0x40000000 | length);
    set_u32(p + 4, lba);
    set_u16(p + 8, partition);
}

/* descriptor block builders (each writes a full 2048-byte block) */

static inline void make_vds_block(uint8_t *b, uint16_t tag_id)
{
    memset(b, 0, UDF_BLOCK_SIZE);
    set_tag(b, tag_id);
}

static inline void make_avdp(uint8_t *b, uint32_t mvds_lba, uint32_t mvds_len,
                             uint32_t rvds_lba, uint32_t rvds_len)
{
    make_vds_block(b, ECMA_AnchorVolumeDescriptorPointer);
    set_u32(b + 16, mvds_len);
    set_u32(b + 20, mvds_lba);
    set_u32(b + 24, rvds_len);
    set_u32(b + 28, rvds_lba);
}

static inline void make_vdp_block(uint8_t *b, uint32_t lba, uint32_t len)
{
    make_vds_block(b, ECMA_VolumeDescriptorPointer);
    set_u32(b + 20, len);
    set_u32(b + 24, lba);
}

static inline void make_partition_block(uint8_t *b, uint16_t number,
                                        uint32_t start_block, uint32_t num_blocks)
{
    make_vds_block(b, ECMA_PartitionDescriptor);
    set_u16(b + 22, number);
    set_u32(b + 188, start_block);
    set_u32(b + 192, num_blocks);
}

static inline void make_lvd_block(uint8_t *b, uint32_t block_size)
{
    make_vds_block(b, ECMA_LogicalVolumeDescriptor);
    set_u32(b + 212, block_size);
}

/* extended file entry with one short allocation descriptor (or inline content) */
static inline void make_ext_file_entry(uint8_t *b, uint8_t file_type, uint16_t icb_flags,
                                       uint32_t l_ad, uint64_t length,
                                       uint32_t ad_len, uint32_t ad_lba)
{
    make_vds_block(b, ECMA_ExtendedFileEntry);
    set_u16(b + 20, 4);            /* strategy type */
    b[27] = file_type;
    set_u16(b + 34, icb_flags);
    set_u64(b + 56, length);
    set_u32(b + 208, 0);           /* l_ea */
    set_u32(b + 212, l_ad);
    if ((icb_flags & 7) == 3) {
        memcpy(b + 216, "abcd", 4);   /* inline content */
    } else {
        set_u32(b + 216, 0x40000000 | ad_len);  /* extent length */
        set_u32(b + 220, ad_lba);
    }
}

#endif /* TEST_ECMA_H_ */