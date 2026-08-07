/*
 * Unit tests for ecma167.c
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

#include "../src/ecma167.c"

static uint8_t buf[2048];

static void test_tag(void)
{
    static const uint16_t ids[] = { 1, 2, 3, 5, 6, 8, 256, 257, 258, 261, 266 };
    size_t i;

    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        set_tag(buf, ids[i]);
        CHECK_EQ_INT((int)ids[i], (int)decode_descriptor_tag(buf));
    }

    /* corrupted checksum byte */
    set_tag(buf, 1);
    buf[4] = (uint8_t)(buf[4] ^ 0xff);
    CHECK_EQ_INT(ECMA_TAG_INVALID, (int)decode_descriptor_tag(buf));

    /* flipped byte in tag body */
    set_tag(buf, 1);
    buf[7] = (uint8_t)(buf[7] ^ 0xff);
    CHECK_EQ_INT(ECMA_TAG_INVALID, (int)decode_descriptor_tag(buf));

    /* all-zero block: valid checksum, unknown id */
    memset(buf, 0, 16);
    CHECK_EQ_INT(0, (int)decode_descriptor_tag(buf));
}

static void test_entity_id(void)
{
    struct entity_id eid;
    size_t i;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 23; i++) {
        buf[1 + i] = (uint8_t)(0x40 + i);
    }
    for (i = 0; i < 8; i++) {
        buf[24 + i] = (uint8_t)(0x80 + i);
    }

    decode_entity_id(buf, &eid);
    CHECK(memcmp(eid.identifier, buf + 1, 23) == 0);
    CHECK(memcmp(eid.identifier_suffix, buf + 24, 8) == 0);
}

static void test_primary_volume(void)
{
    struct primary_volume_descriptor pvd;
    size_t i;

    memset(buf, 0, sizeof(buf));
    memcpy(buf + 24, "TEST", 4);
    buf[55] = 4; /* dstring length byte (field 32 at offset 24) */
    for (i = 0; i < 128; i++) {
        buf[72 + i] = (uint8_t)(i + 1);
    }

    decode_primary_volume(buf, &pvd);
    CHECK_EQ_UINT(4, pvd.volume_identifier_length);
    CHECK(memcmp(pvd.volume_identifier, "TEST", 4) == 0);
    CHECK(memcmp(pvd.volume_set_identifier, buf + 72, 128) == 0);

    /* stored length > field size -> clamped */
    buf[55] = 200;
    decode_primary_volume(buf, &pvd);
    CHECK_EQ_UINT(31, pvd.volume_identifier_length);

    /* zero-length */
    buf[55] = 0;
    decode_primary_volume(buf, &pvd);
    CHECK_EQ_UINT(0, pvd.volume_identifier_length);

    /* _decode_dstring: zero field length */
    CHECK_EQ_UINT(0, _decode_dstring(buf, (size_t)0, pvd.volume_identifier));
}

static void test_avdp_vdp(void)
{
    struct anchor_volume_descriptor avdp;
    struct volume_descriptor_pointer vdp;

    make_avdp(buf, 257, 2048, 258, 4096);

    decode_avdp(buf, &avdp);
    CHECK_EQ_UINT(2048, avdp.mvds.length);
    CHECK_EQ_UINT(257, avdp.mvds.lba);
    CHECK_EQ_UINT(4096, avdp.rvds.length);
    CHECK_EQ_UINT(258, avdp.rvds.lba);

    make_vdp_block(buf, 300, 8192);
    decode_vdp(buf, &vdp);
    CHECK_EQ_UINT(8192, vdp.next_extent.length);
    CHECK_EQ_UINT(300, vdp.next_extent.lba);
}

static void test_partition_lvd(void)
{
    struct partition_descriptor pd;
    struct logical_volume_descriptor lvd;

    make_partition_block(buf, 7, 1000, 5000);

    decode_partition(buf, &pd);
    CHECK_EQ_UINT(7, pd.number);
    CHECK_EQ_UINT(1000, pd.start_block);
    CHECK_EQ_UINT(5000, pd.num_blocks);

    make_lvd_block(buf, 2048);
    memcpy(buf + 217, "ABCDEF", 6);   /* entity id identifier (starts at p+1) */
    memset(buf + 240, 0xaa, 8);       /* entity id suffix (p+24) */
    memcpy(buf + 248, "CONTENTS_USE", 12);
    set_u32(buf + 264, 3000);   /* map length > table -> clamped */
    set_u32(buf + 268, 2);
    memset(buf + 440, 0xaa, 1608);

    memset(lvd.partition_map_table, 0xff, sizeof(lvd.partition_map_table));
    decode_logical_volume(buf, &lvd);
    CHECK_EQ_UINT(2048, lvd.block_size);
    CHECK(memcmp(lvd.domain_id.identifier, "ABCDEF", 6) == 0);
    CHECK(memcmp(lvd.domain_id.identifier_suffix, buf + 240, 8) == 0);
    CHECK(memcmp(lvd.contents_use, "CONTENTS_USE", 12) == 0);
    CHECK_EQ_UINT(3000, lvd.partition_map_lable_length);
    CHECK_EQ_UINT(2, lvd.num_partition_maps);
    CHECK(memcmp(lvd.partition_map_table, buf + 440, 1608) == 0);
    CHECK_EQ_UINT(0xff, lvd.partition_map_table[1608]);

    /* small map */
    set_u32(buf + 264, 10);
    memset(buf + 440, 0x55, 10);
    memset(lvd.partition_map_table, 0xff, sizeof(lvd.partition_map_table));
    decode_logical_volume(buf, &lvd);
    CHECK(memcmp(lvd.partition_map_table, buf + 440, 10) == 0);
    CHECK_EQ_UINT(0xff, lvd.partition_map_table[10]);
}

static void test_long_ad_fsd(void)
{
    struct long_ad ad;
    struct file_set_descriptor fsd;

    set_long_ad(buf, 5, 5678, 3);

    decode_long_ad(buf, &ad);
    CHECK_EQ_UINT(1, ad.extent_type);
    CHECK_EQ_UINT(5, ad.length);
    CHECK_EQ_UINT(5678, ad.lba);
    CHECK_EQ_UINT(3, ad.partition);

    /* length masking: all bits set -> type 3, length 0x3fffffff */
    set_u32(buf + 0, 0xffffffff);
    decode_long_ad(buf, &ad);
    CHECK_EQ_UINT(3, ad.extent_type);
    CHECK_EQ_UINT(0x3fffffff, ad.length);

    /* short ad: lba at +4, partition from parameter */
    set_u32(buf + 0, 0x80000000 | 100);
    set_u32(buf + 4, 42);
    _decode_short_ad(buf, (uint16_t)9, &ad);
    CHECK_EQ_UINT(2, ad.extent_type);
    CHECK_EQ_UINT(100, ad.length);
    CHECK_EQ_UINT(42, ad.lba);
    CHECK_EQ_UINT(9, ad.partition);

    /* extended ad: lba at +12, partition at +16 */
    set_u32(buf + 0, 0x40000000 | 200);
    set_u32(buf + 12, 77);
    set_u16(buf + 16, 5);
    _decode_extended_ad(buf, &ad);
    CHECK_EQ_UINT(1, ad.extent_type);
    CHECK_EQ_UINT(200, ad.length);
    CHECK_EQ_UINT(77, ad.lba);
    CHECK_EQ_UINT(5, ad.partition);

    /* file set descriptor: root icb at +400 */
    set_long_ad(buf + 400, 300, 123, 0);
    decode_file_set_descriptor(buf, &fsd);
    CHECK_EQ_UINT(300, fsd.root_icb.length);
    CHECK_EQ_UINT(123, fsd.root_icb.lba);
    CHECK_EQ_UINT(0, fsd.root_icb.partition);
}

static void test_file_identifier(void)
{
    struct file_identifier fi;
    ecma_ctx ctx = { NULL };
    size_t r;

    memset(buf, 0, sizeof(buf));

    /* too small */
    r = decode_file_identifier(&ctx, buf, (size_t)37, &fi);
    CHECK_EQ_UINT(0, r);

    /* empty filename */
    r = decode_file_identifier(&ctx, buf, (size_t)38, &fi);
    CHECK_EQ_UINT(40, r);
    CHECK_EQ_UINT(0, fi.filename_len);
    CHECK_EQ_UINT(0, fi.filename[0]);

    /* normal entry */
    buf[18] = 0x02;   /* characteristic: dir */
    buf[19] = 3;      /* filename length */
    memcpy(buf + 38, "abc", 3);
    set_u32(buf + 20, 0x40000000 | 10);   /* icb */
    set_u32(buf + 24, 55);
    set_u16(buf + 28, 1);
    r = decode_file_identifier(&ctx, buf, (size_t)41, &fi);
    CHECK_EQ_UINT(44, r);
    CHECK_EQ_UINT(0x02, fi.characteristic);
    CHECK_EQ_UINT(3, fi.filename_len);
    CHECK(memcmp(fi.filename, "abc", 3) == 0);
    CHECK_EQ_UINT(0, fi.filename[3]);
    CHECK_EQ_UINT(1, fi.icb.extent_type);
    CHECK_EQ_UINT(55, fi.icb.lba);
    CHECK_EQ_UINT(1, fi.icb.partition);

    /* with implementation use field */
    set_u16(buf + 36, 5);
    memcpy(buf + 43, "abc", 3);
    r = decode_file_identifier(&ctx, buf, (size_t)46, &fi);
    CHECK_EQ_UINT(48, r);
    CHECK(memcmp(fi.filename, "abc", 3) == 0);

    /* truncated */
    r = decode_file_identifier(&ctx, buf, (size_t)45, &fi);
    CHECK_EQ_UINT(0, r);
}

static void test_file_entry(void)
{
    struct file_entry *fe;
    ecma_ctx ctx = { NULL };

    memset(buf, 0, sizeof(buf));
    set_u16(buf + 20, 4);      /* icb tag: strategy type */
    buf[16 + 11] = 5;          /* file type */
    set_u16(buf + 16 + 18, 0); /* icb flags: short ads */
    set_u64(buf + 56, 1000);
    set_u32(buf + 168, 0);     /* l_ea */
    set_u32(buf + 172, 16);    /* l_ad: 2 short ads */
    set_u32(buf + 176, 0x40000000 | 10);
    set_u32(buf + 180, 100);
    set_u32(buf + 184, 0x40000000 | 20);
    set_u32(buf + 188, 200);

    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK_NOT_NULL(fe);
    CHECK_EQ_UINT(5, fe->file_type);
    CHECK_EQ_UINT(1000, fe->length);
    CHECK_EQ_UINT(0, fe->ad_type);
    CHECK_EQ_UINT(2, fe->u.ads.num_ad);
    CHECK_EQ_UINT(10, fe->u.ads.ad[0].length);
    CHECK_EQ_UINT(100, fe->u.ads.ad[0].lba);
    CHECK_EQ_UINT(7, fe->u.ads.ad[0].partition);
    CHECK_EQ_UINT(20, fe->u.ads.ad[1].length);
    CHECK_EQ_UINT(200, fe->u.ads.ad[1].lba);
    free_file_entry(&fe);
    CHECK(fe == NULL);

    /* long ads */
    set_u16(buf + 16 + 18, 1);
    set_u32(buf + 172, 16);
    set_u32(buf + 176, 0x40000000 | 30);
    set_u32(buf + 180, 300);
    set_u16(buf + 184, 2);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK_NOT_NULL(fe);
    CHECK_EQ_UINT(1, fe->ad_type);
    CHECK_EQ_UINT(1, fe->u.ads.num_ad);
    CHECK_EQ_UINT(30, fe->u.ads.ad[0].length);
    CHECK_EQ_UINT(300, fe->u.ads.ad[0].lba);
    CHECK_EQ_UINT(2, fe->u.ads.ad[0].partition);
    free_file_entry(&fe);

    /* extended ads */
    set_u16(buf + 16 + 18, 2);
    set_u32(buf + 172, 20);
    set_u32(buf + 176, 0x40000000 | 40);
    set_u32(buf + 188, 400);
    set_u16(buf + 192, 3);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK_NOT_NULL(fe);
    CHECK_EQ_UINT(2, fe->ad_type);
    CHECK_EQ_UINT(1, fe->u.ads.num_ad);
    CHECK_EQ_UINT(40, fe->u.ads.ad[0].length);
    CHECK_EQ_UINT(400, fe->u.ads.ad[0].lba);
    CHECK_EQ_UINT(3, fe->u.ads.ad[0].partition);
    free_file_entry(&fe);

    /* inline content */
    set_u16(buf + 16 + 18, 3);
    set_u32(buf + 172, 4);
    memcpy(buf + 176, "abcd", 4);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK_NOT_NULL(fe);
    CHECK_EQ_UINT(1, fe->content_inline);
    CHECK_EQ_UINT(4, fe->u.data.information_length);
    CHECK(memcmp(fe->u.data.content, "abcd", 4) == 0);
    free_file_entry(&fe);

    /* unsupported strategy type */
    set_u16(buf + 16 + 4, 5);
    set_u16(buf + 16 + 18, 0);
    set_u32(buf + 172, 8);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK(fe == NULL);

    /* unsupported icb flags */
    set_u16(buf + 16 + 4, 4);
    set_u16(buf + 16 + 18, 4);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK(fe == NULL);

    /* integer overflow guard */
    set_u16(buf + 16 + 18, 0);
    set_u32(buf + 168, 0xffffffff);
    set_u32(buf + 172, 0);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK(fe == NULL);

    /* p_ad + l_ad > size */
    set_u32(buf + 168, 100);
    set_u32(buf + 172, 2000);
    fe = decode_file_entry(&ctx, buf, sizeof(buf), (uint16_t)2048);
    CHECK(fe == NULL);
}

static void test_ext_file_entry(void)
{
    struct file_entry *fe;
    ecma_ctx ctx = { NULL };

    make_ext_file_entry(buf, 5, 0, 8, 500, 60, 600);

    fe = decode_ext_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK_NOT_NULL(fe);
    CHECK_EQ_UINT(500, fe->length);
    CHECK_EQ_UINT(1, fe->u.ads.num_ad);
    CHECK_EQ_UINT(60, fe->u.ads.ad[0].length);
    CHECK_EQ_UINT(600, fe->u.ads.ad[0].lba);
    free_file_entry(&fe);

    /* integer overflow guard (216) */
    set_u32(buf + 208, 0xffffffff);
    set_u32(buf + 212, 0);
    fe = decode_ext_file_entry(&ctx, buf, sizeof(buf), (uint16_t)7);
    CHECK(fe == NULL);
}

static void test_allocation_extent(void)
{
    struct file_entry *fe;
    ecma_ctx ctx = { NULL };

    memset(buf, 0, sizeof(buf));
    fe = (struct file_entry *)calloc(1, sizeof(struct file_entry));
    CHECK_NOT_NULL(fe);
    fe->ad_type = 0;
    fe->u.ads.num_ad = 1;

/* valid growth */
    set_u32(buf + 20, 8);   /* l_ad: 1 short ad */
    set_u32(buf + 24, 0x40000000 | 70);
    set_u32(buf + 28, 700);
    CHECK_EQ_INT(0, decode_allocation_extent(&ctx, &fe, buf, (size_t)32, (uint16_t)7));
    CHECK_EQ_UINT(2, fe->u.ads.num_ad);
    CHECK_EQ_UINT(70, fe->u.ads.ad[1].length);
    CHECK_EQ_UINT(700, fe->u.ads.ad[1].lba);
    CHECK_EQ_UINT(7, fe->u.ads.ad[1].partition);

    /* empty extent */
    set_u32(buf + 20, 0);
    CHECK_EQ_INT(0, decode_allocation_extent(&ctx, &fe, buf, (size_t)24, (uint16_t)7));
    CHECK_EQ_UINT(2, fe->u.ads.num_ad);

    /* size < 24 */
    CHECK_EQ_INT(-1, decode_allocation_extent(&ctx, &fe, buf, (size_t)23, (uint16_t)7));

    /* size - 24 < l_ad */
    set_u32(buf + 20, 100);
    CHECK_EQ_INT(-1, decode_allocation_extent(&ctx, &fe, buf, (size_t)123, (uint16_t)7));

    /* unsupported ad_type */
    fe->ad_type = 3;
    set_u32(buf + 20, 8);
    CHECK_EQ_INT(-1, decode_allocation_extent(&ctx, &fe, buf, (size_t)32, (uint16_t)7));

    /* long ads */
    fe->ad_type = 1;
    fe->u.ads.num_ad = 1;
    set_u32(buf + 20, 16);   /* l_ad: 1 long ad */
    set_u32(buf + 24, 0x40000000 | 80);
    set_u32(buf + 28, 800);
    set_u16(buf + 32, 4);
    CHECK_EQ_INT(0, decode_allocation_extent(&ctx, &fe, buf, (size_t)40, (uint16_t)7));
    CHECK_EQ_UINT(2, fe->u.ads.num_ad);
    CHECK_EQ_UINT(80, fe->u.ads.ad[1].length);
    CHECK_EQ_UINT(800, fe->u.ads.ad[1].lba);
    CHECK_EQ_UINT(4, fe->u.ads.ad[1].partition);

    /* extended ads */
    fe->ad_type = 2;
    fe->u.ads.num_ad = 1;
    set_u32(buf + 20, 20);   /* l_ad: 1 extended ad */
    set_u32(buf + 24, 0x40000000 | 90);
    set_u32(buf + 36, 900);
    set_u16(buf + 40, 5);
    CHECK_EQ_INT(0, decode_allocation_extent(&ctx, &fe, buf, (size_t)44, (uint16_t)7));
    CHECK_EQ_UINT(2, fe->u.ads.num_ad);
    CHECK_EQ_UINT(90, fe->u.ads.ad[1].length);
    CHECK_EQ_UINT(900, fe->u.ads.ad[1].lba);
    CHECK_EQ_UINT(5, fe->u.ads.ad[1].partition);

    free_file_entry(&fe);
}

static void test_free_file_entry(void)
{
    struct file_entry *fe;

    free_file_entry(NULL);

    fe = NULL;
    free_file_entry(&fe);
    CHECK(fe == NULL);

    fe = (struct file_entry *)calloc(1, sizeof(struct file_entry));
    CHECK_NOT_NULL(fe);
    free_file_entry(&fe);
    CHECK(fe == NULL);
}

static void test_ecma_logging(void)
{
    struct capture_log cap;
    udf_log log;
    ecma_ctx ctx;
    struct file_identifier fi;

    log_init(&cap, &log, &ctx);

    memset(buf, 0, sizeof(buf));

    /* NULL ctx: no crash */
    CHECK_EQ_UINT(0, decode_file_identifier(NULL, buf, (size_t)37, &fi));

    /* ctx with NULL lc: no crash */
    ctx.lc = NULL;
    CHECK_EQ_UINT(0, decode_file_identifier(&ctx, buf, (size_t)37, &fi));

    /* capture logger: "ecma: " prefix + message */
    ctx.lc = &log;
    CHECK_EQ_UINT(0, decode_file_identifier(&ctx, buf, (size_t)37, &fi));
    CHECK_EQ_INT(1, cap.calls);
    CHECK(strstr(cap.buf, "ecma: decode_file_identifier: not enough data") != NULL);
}

RUN_TESTS(
    TEST_ENTRY(test_tag),
    TEST_ENTRY(test_entity_id),
    TEST_ENTRY(test_primary_volume),
    TEST_ENTRY(test_avdp_vdp),
    TEST_ENTRY(test_partition_lvd),
    TEST_ENTRY(test_long_ad_fsd),
    TEST_ENTRY(test_file_identifier),
    TEST_ENTRY(test_file_entry),
    TEST_ENTRY(test_ext_file_entry),
    TEST_ENTRY(test_allocation_extent),
    TEST_ENTRY(test_free_file_entry),
    TEST_ENTRY(test_ecma_logging),
)
