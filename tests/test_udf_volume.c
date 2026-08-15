/*
 * Unit tests for udf_volume.c
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Includes the library source to reach its static functions.
 * Must NOT be linked against libudfread (duplicate decode_* symbols).
 */

#include "test_ecma.h"
#include "test_blockinput.h"

#include "../src/ecma167.c"
#include "../src/udf_volume.c"

#define IMG_BLOCKS 512
static uint8_t image[IMG_BLOCKS * UDF_BLOCK_SIZE];

static uint8_t *img_block(uint32_t lba)
{
    return image + (size_t)lba * UDF_BLOCK_SIZE;
}

/* Volume Recognition patterns (ECMA 167 2/8) */
static const char bea01[] = {'\0', 'B', 'E', 'A', '0', '1', '\1'};
static const char nsr02[] = {'\0', 'N', 'S', 'R', '0', '2', '\1'};
static const char nsr03[] = {'\0', 'N', 'S', 'R', '0', '3', '\1'};
static const char tea01[] = {'\0', 'T', 'E', 'A', '0', '1', '\1'};

static void set_probe_pattern(uint32_t lba, const char *pat)
{
    memcpy(img_block(lba), pat, 7);
}

/* PVD + PD + LVD + Terminating descriptor at lba..lba+3 */
static void make_vds_sequence(uint32_t lba, uint16_t pd_number)
{
    make_vds_block(img_block(lba), ECMA_PrimaryVolumeDescriptor);
    make_partition_block(img_block(lba + 1), pd_number, 100, 1000);
    make_lvd_block(img_block(lba + 2), UDF_BLOCK_SIZE);
    make_vds_block(img_block(lba + 3), ECMA_TerminatingDescriptor);
}

static void test_probe_volume(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    static const uint32_t trap16[] = { 16 };
    static const uint32_t trap1617[] = { 16, 17 };

    log_init(&cap, &log, &ctx);

    /* BEA01 + NSR02 */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    set_probe_pattern(16, bea01);
    set_probe_pattern(17, nsr02);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap1617, 2);
    CHECK_EQ_INT(0, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "ECMA 167 Volume, NSR02") != NULL);

    /* BEA01 + NSR03 */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    set_probe_pattern(16, bea01);
    set_probe_pattern(17, nsr03);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap1617, 2);
    CHECK_EQ_INT(0, udf_probe_volume(&ctx, &m.bi));

    /* NSR02 without BEA01: not accepted, zeroed block breaks the loop */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    set_probe_pattern(16, nsr02);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap1617, 2);
    CHECK_EQ_INT(-1, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "Volume Recognition failed") != NULL);

    /* TEA01 */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    set_probe_pattern(16, tea01);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap16, 1);
    CHECK_EQ_INT(-1, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "no NSR descriptor") != NULL);

    /* BEA01 + TEA01 */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    set_probe_pattern(16, bea01);
    set_probe_pattern(17, tea01);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap1617, 2);
    CHECK_EQ_INT(-1, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "no NSR descriptor") != NULL);

    /* all reads fail */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    mock_init(&m, image, 0);
    CHECK_EQ_INT(-1, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "Volume Recognition failed") != NULL);

    /* zeroed image: nul pattern breaks the loop */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    mock_init(&m, image, 32);
    mock_expect(&m, trap16, 1);
    CHECK_EQ_INT(-1, udf_probe_volume(&ctx, &m.bi));
    CHECK(strstr(cap.buf, "Volume Recognition failed") != NULL);
}

static void test_read_vds(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    static const uint32_t trap[] = { 256, 257, 258, 259 };

    log_init(&cap, &log, &ctx);

    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 4 * UDF_BLOCK_SIZE, 300, 4 * UDF_BLOCK_SIZE);
    make_vds_sequence(257, 0);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 4);

    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK_EQ_UINT(0, vds.pd.number);
    CHECK_EQ_UINT(100, vds.pd.start_block);
    CHECK_EQ_UINT(2048, vds.lvd.block_size);
    CHECK_EQ_UINT(0, vds.pvd.volume_identifier_length);
    CHECK_EQ_UINT(4, m.read_calls);
    CHECK(strstr(cap.buf, "Found Anchor Volume Descriptor Pointer from lba 256") != NULL);
}

static void test_read_vds_avdp_locations(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    static const uint32_t trap_last[] = { 256, 299, 100, 101, 102 };
    static const uint32_t trap_last256[] = { 256, 299, 43, 100, 101, 102 };
    static const uint32_t trap_none[] = { 256, 299, 43 };
    static const uint32_t trap_256[] = { 256 };
    static const uint32_t trap_256ok[] = { 256, 257, 258, 259 };

    log_init(&cap, &log, &ctx);

    /* AVDP in last block (299) of a 300-block image */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(299), 100, 3 * UDF_BLOCK_SIZE, 200, 4 * UDF_BLOCK_SIZE);
    make_vds_sequence(100, 0);
    mock_init(&m, image, 300);
    mock_expect(&m, trap_last, 5);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK_EQ_UINT(2048, vds.lvd.block_size);

    /* AVDP in block 43 (last - 256) */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(43), 100, 3 * UDF_BLOCK_SIZE, 200, 4 * UDF_BLOCK_SIZE);
    make_vds_sequence(100, 0);
    mock_init(&m, image, 300);
    mock_expect(&m, trap_last256, 6);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));

    /* no AVDP */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    mock_init(&m, image, 300);
    mock_expect(&m, trap_none, 3);
    CHECK_EQ_INT(-1, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK(strstr(cap.buf, "Can't find Anchor Volume Descriptor Pointer") != NULL);

    /* no size callback, block 256 invalid */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    mock_init(&m, image, IMG_BLOCKS);
    m.bi.size = NULL;
    mock_expect(&m, trap_256, 1);
    CHECK_EQ_INT(-1, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK(strstr(cap.buf, "Can't find Anchor Volume Descriptor Pointer") != NULL);

    /* no size callback, block 256 valid */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 4 * UDF_BLOCK_SIZE, 300, 4 * UDF_BLOCK_SIZE);
    make_vds_sequence(257, 0);
    mock_init(&m, image, IMG_BLOCKS);
    m.bi.size = NULL;
    mock_expect(&m, trap_256ok, 4);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));
}

static void test_read_vds_fallback(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    static const uint32_t trap_ok[] = { 256, 257, 258, 259, 300, 301, 302 };
    static const uint32_t trap_fail[] = { 256, 257, 300 };

    log_init(&cap, &log, &ctx);

    /* MVDS incomplete (no LVD), RVDS complete */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 3 * UDF_BLOCK_SIZE, 300, 4 * UDF_BLOCK_SIZE);
    make_vds_block(img_block(257), ECMA_PrimaryVolumeDescriptor);
    make_partition_block(img_block(258), 0, 100, 1000);
    make_vds_block(img_block(259), ECMA_TerminatingDescriptor);
    make_vds_sequence(300, 0);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap_ok, 7);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK_EQ_UINT(2048, vds.lvd.block_size);   /* LVD came from RVDS */

    /* both sequences empty */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 2 * UDF_BLOCK_SIZE, 300, UDF_BLOCK_SIZE);
    make_vds_block(img_block(257), ECMA_TerminatingDescriptor);
    make_vds_block(img_block(300), ECMA_TerminatingDescriptor);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap_fail, 3);
    CHECK_EQ_INT(-1, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK(strstr(cap.buf, "failed reading Volume Descriptor Sequence") != NULL);
}

static void test_read_vds_vdp(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    static const uint32_t trap[] = { 256, 257, 300, 301, 302 };

    log_init(&cap, &log, &ctx);

    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 2 * UDF_BLOCK_SIZE, 300, 4 * UDF_BLOCK_SIZE);
    make_vdp_block(img_block(257), 300, UDF_BLOCK_SIZE);
    make_vds_block(img_block(258), ECMA_TerminatingDescriptor);
    make_vds_sequence(300, 0);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 5);

    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 0, &vds));
    CHECK_EQ_UINT(2048, vds.lvd.block_size);
    CHECK_EQ_UINT(6, m.read_calls);
}

static void test_read_vds_part_number(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    static const uint32_t trap_5[] = { 256, 257, 258, 259, 260 };
    static const uint32_t trap_first[] = { 256, 257, 258, 259, 260 };
    static const uint32_t trap_last[] = { 256, 257, 258, 259, 260, 261 };

    log_init(&cap, &log, &ctx);

    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_avdp(img_block(256), 257, 5 * UDF_BLOCK_SIZE, 300, 4 * UDF_BLOCK_SIZE);
    make_vds_block(img_block(257), ECMA_PrimaryVolumeDescriptor);
    make_partition_block(img_block(258), 7, 100, 1000);
    make_partition_block(img_block(259), 5, 100, 1000);
    make_lvd_block(img_block(260), UDF_BLOCK_SIZE);
    make_vds_block(img_block(261), ECMA_TerminatingDescriptor);

    /* specific partition number */
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap_5, 5);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, 5, &vds));
    CHECK_EQ_UINT(5, vds.pd.number);

    /* first partition */
    log_init(&cap, &log, &ctx);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap_first, 5);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, UDFREAD_PARTITION_FIRST, &vds));
    CHECK_EQ_UINT(7, vds.pd.number);

    /* last partition: no early exit, reads to the terminator */
    log_init(&cap, &log, &ctx);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap_last, 6);
    CHECK_EQ_INT(0, udf_read_vds(&ctx, &m.bi, UDFREAD_PARTITION_LAST, &vds));
    CHECK_EQ_UINT(5, vds.pd.number);
    CHECK_EQ_UINT(6, m.read_calls);
}

static void test_validate_logical_volume(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct logical_volume_descriptor lvd;
    struct long_ad fsd_loc;

    log_init(&cap, &log, &ctx);

    /* valid */
    memset(&lvd, 0, sizeof(lvd));
    lvd.block_size = UDF_BLOCK_SIZE;
    memcpy(lvd.domain_id.identifier, "*OSTA UDF Compliant", 19);
    set_u16(lvd.domain_id.identifier_suffix, 0x0260);
    set_long_ad(lvd.contents_use, 2048, 123, 2);

    CHECK_EQ_INT(0, udf_validate_logical_volume(&ctx, &lvd, &fsd_loc));
    CHECK_EQ_UINT(1, fsd_loc.extent_type);
    CHECK_EQ_UINT(2048, fsd_loc.length);
    CHECK_EQ_UINT(123, fsd_loc.lba);
    CHECK_EQ_UINT(2, fsd_loc.partition);
    CHECK(strstr(cap.buf, "Found UDF 2.60 Logical Volume") != NULL);
    CHECK(strstr(cap.buf, "File Set Descriptor location: partition 2 lba 123 (len 2048)") != NULL);

    /* incompatible block size */
    memset(&lvd, 0, sizeof(lvd));
    lvd.block_size = 512;
    CHECK_EQ_INT(-1, udf_validate_logical_volume(&ctx, &lvd, &fsd_loc));
    CHECK(strstr(cap.buf, "incompatible block size") != NULL);

    /* unknown domain id */
    memset(&lvd, 0, sizeof(lvd));
    lvd.block_size = UDF_BLOCK_SIZE;
    memcpy(lvd.domain_id.identifier, "XXXXXXXX", 8);
    CHECK_EQ_INT(-1, udf_validate_logical_volume(&ctx, &lvd, &fsd_loc));
    CHECK(strstr(cap.buf, "unknown Domain ID") != NULL);

    /* prefix match: longer identifier still accepted */
    memset(&lvd, 0, sizeof(lvd));
    lvd.block_size = UDF_BLOCK_SIZE;
    memcpy(lvd.domain_id.identifier, "*OSTA UDF CompliantXYZ", 22);
    CHECK_EQ_INT(0, udf_validate_logical_volume(&ctx, &lvd, &fsd_loc));
}

static void test_parse_partition_maps(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct volume_descriptor_set vds;
    struct udf_partitions part;

    log_init(&cap, &log, &ctx);

    /* empty table */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.pd.start_block = 100;
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK(strstr(cap.buf, "no type 1 partition found") != NULL);

    /* valid type 1 */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.pd.start_block = 100;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 6;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 6;
    set_u16(vds.lvd.partition_map_table + 4, 0);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK_EQ_UINT(0, part.p[0].number);
    CHECK_EQ_UINT(100, part.p[0].lba);
    CHECK_EQ_UINT(0, part.p[0].mirror_lba);

    /* type 1 referring to another physical partition */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.pd.start_block = 100;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 6;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 6;
    set_u16(vds.lvd.partition_map_table + 4, 1);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK(strstr(cap.buf, "refers to another physical partition") != NULL);

    /* two type 1 maps: second one rejected but not fatal */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.pd.start_block = 100;
    vds.lvd.num_partition_maps = 2;
    vds.lvd.partition_map_lable_length = 12;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 6;
    set_u16(vds.lvd.partition_map_table + 4, 0);
    vds.lvd.partition_map_table[6] = 1;
    vds.lvd.partition_map_table[7] = 6;
    set_u16(vds.lvd.partition_map_table + 10, 0);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "more than one type1 partitions not supported") != NULL);

    /* invalid type 1 length */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 6;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 5;
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK(strstr(cap.buf, "invalid type 1 partition map length") != NULL);

    /* length < 2 */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 6;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 1;
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK(strstr(cap.buf, "invalid partition map length") != NULL);

    /* map extends past the table */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 5;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 6;
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK(strstr(cap.buf, "partition map table too short") != NULL);

    /* table length > 2048: clamped */
    log_init(&cap, &log, &ctx);
    memset(&vds, 0, sizeof(vds));
    vds.pd.number = 0;
    vds.pd.start_block = 100;
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 3000;
    vds.lvd.partition_map_table[0] = 1;
    vds.lvd.partition_map_table[1] = 6;
    set_u16(vds.lvd.partition_map_table + 4, 0);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, NULL, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "partition map table too big") != NULL);
}

/* type 1 map (ref 0) + type 2 metadata map (ref, lba, mirror_lba) */
static void vds_meta_init(struct volume_descriptor_set *vds, uint16_t ref,
                          uint32_t lba, uint32_t mirror_lba)
{
    memset(vds, 0, sizeof(*vds));
    vds->pd.number = 0;
    vds->pd.start_block = 100;
    vds->lvd.num_partition_maps = 2;
    vds->lvd.partition_map_lable_length = 70;
    /* type 1 map */
    vds->lvd.partition_map_table[0] = 1;
    vds->lvd.partition_map_table[1] = 6;
    set_u16(vds->lvd.partition_map_table + 4, 0);
    /* type 2 metadata map */
    vds->lvd.partition_map_table[6] = 2;
    vds->lvd.partition_map_table[7] = 64;
    memcpy(vds->lvd.partition_map_table + 11, "*UDF Metadata Partition", 23);
    set_u16(vds->lvd.partition_map_table + 44, ref);
    set_u32(vds->lvd.partition_map_table + 46, lba);
    set_u32(vds->lvd.partition_map_table + 50, mirror_lba);
}

static void test_parse_partition_maps_metadata(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct mock_input m;
    struct volume_descriptor_set vds;
    struct udf_partitions part;
    static const uint32_t trap[] = { 105, 106 };

    log_init(&cap, &log, &ctx);

    /* metadata file found (pd.start_block 100 + lba 5 = 105) */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 250, 0, 8, 0, 2048, 7);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(2, part.num_partition);
    CHECK_EQ_UINT(1, part.p[1].number);
    CHECK_EQ_UINT(107, part.p[1].lba);
    CHECK_EQ_UINT(0, part.p[1].mirror_lba);
    CHECK(strstr(cap.buf, "metadata file at lba 107") != NULL);

    /* primary location invalid, mirror used */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_vds_block(img_block(105), ECMA_PrimaryVolumeDescriptor);
    make_ext_file_entry(img_block(106), 251, 0, 8, 0, 2048, 9);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(2, part.num_partition);
    CHECK_EQ_UINT(109, part.p[1].lba);
    CHECK_EQ_UINT(0, part.p[1].mirror_lba);
    CHECK(strstr(cap.buf, "unexpected tag") != NULL);

    /* inline content */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 250, 3, 4, 0, 0, 0);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "content inline") != NULL);

    /* no allocation descriptors */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 250, 0, 0, 0, 0, 0);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "no allocation descriptors") != NULL);

    /* unknown metadata file type */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 5, 0, 8, 0, 2048, 7);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "unknown metadata file type") != NULL);

    /* both locations invalid */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "unexpected tag") != NULL);

    /* metadata file entry fails to decode (unsupported icb strategy type) */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 250, 0, 8, 0, 2048, 7);
    set_u16(img_block(105) + 20, 5);
    vds_meta_init(&vds, 0, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "parsing metadata file entry 0 failed") != NULL);

    /* unsupported type 2 partition: no reads */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    vds_meta_init(&vds, 0, 5, 6);
    vds.lvd.partition_map_table[11] = 'X';
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, NULL, 0);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(1, part.num_partition);
    CHECK(strstr(cap.buf, "unsupported type 2 partition") != NULL);

    /* metadata partition ref mismatch: still mapped */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    make_ext_file_entry(img_block(105), 250, 0, 8, 0, 2048, 7);
    vds_meta_init(&vds, 1, 5, 6);
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, trap, 2);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(0, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK_EQ_UINT(2, part.num_partition);
    CHECK(strstr(cap.buf, "metadata file partition 1 != 0") != NULL);

    /* invalid type 2 length */
    log_init(&cap, &log, &ctx);
    memset(image, 0, sizeof(image));
    vds_meta_init(&vds, 0, 5, 6);
    vds.lvd.num_partition_maps = 1;
    vds.lvd.partition_map_lable_length = 63;
    vds.lvd.partition_map_table[0] = 2;
    vds.lvd.partition_map_table[1] = 63;
    mock_init(&m, image, IMG_BLOCKS);
    mock_expect(&m, NULL, 0);
    memset(&part, 0, sizeof(part));
    CHECK_EQ_INT(-1, udf_parse_partition_maps(&ctx, &m.bi, &vds, &part));
    CHECK(strstr(cap.buf, "invalid type 2 partition map length") != NULL);
}

RUN_TESTS(
    TEST_ENTRY(test_probe_volume),
    TEST_ENTRY(test_read_vds),
    TEST_ENTRY(test_read_vds_avdp_locations),
    TEST_ENTRY(test_read_vds_fallback),
    TEST_ENTRY(test_read_vds_vdp),
    TEST_ENTRY(test_read_vds_part_number),
    TEST_ENTRY(test_validate_logical_volume),
    TEST_ENTRY(test_parse_partition_maps),
    TEST_ENTRY(test_parse_partition_maps_metadata),
)