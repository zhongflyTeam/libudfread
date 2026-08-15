/*
 * This file is part of libudfread
 * Copyright (C) 2014-2026 VLC authors and VideoLAN
 *
 * Authors: Petri Hintukainen <phintuka@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "udf_volume.h"
#include "ecma167.h"

#include "attributes.h"
#include "udfread.h"  /* constants from API */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define udf_error(...)   udf_log_msg(ecma->lc, UDFREAD_LOG_ERROR, "udfread ERROR: ", __VA_ARGS__)
#define udf_log(...)     udf_log_msg(ecma->lc, UDFREAD_LOG_INFO,  "udfread LOG  : ", __VA_ARGS__)
#define udf_trace(...)   udf_log_msg(ecma->lc, UDFREAD_LOG_TRACE, "udfread TRACE: ", __VA_ARGS__)

/* Additional File Types (UDF 2.60, 2.3.5.2) */
enum udf_file_type {
    UDF_FT_METADATA        = 250,
    UDF_FT_METADATA_MIRROR = 251,
};

/*
 * Domain Identifiers, UDF 2.1.5.2
 */

static const char lvd_domain_id[]  = "*OSTA UDF Compliant";
static const char meta_domain_id[] = "*UDF Metadata Partition";

static int _check_domain_identifier(const struct entity_id *eid, const char *value)
{
    return (!memcmp(value, eid->identifier, strlen(value))) ? 0 : -1;
}

/*
 * Disc probing
 */

int udf_probe_volume(ecma_ctx *ecma, udfread_block_input *input)
{
    /* Volume Recognition (ECMA 167 2/8, UDF 2.60 2.1.7) */

    static const char bea[]    = {'\0',  'B',  'E',  'A',  '0',  '1', '\1'};
    static const char nsr_02[] = {'\0',  'N',  'S',  'R',  '0',  '2', '\1'};
    static const char nsr_03[] = {'\0',  'N',  'S',  'R',  '0',  '3', '\1'};
    static const char tea[]    = {'\0',  'T',  'E',  'A',  '0',  '1', '\1'};
    static const char nul[]    = {'\0', '\0', '\0', '\0', '\0', '\0', '\0'};

    uint8_t  buf[UDF_BLOCK_SIZE];
    uint32_t lba;
    int      bea_seen = 0;

    for (lba = 16; lba < 256; lba++) {
        if (input->read(input, lba, buf, 1, 0) == 1) {

            /* Terminating Extended Area Descriptor */
            if (!memcmp(buf, tea, sizeof(tea))) {
                udf_error("ECMA 167 Volume Recognition failed (no NSR descriptor)\n");
                return -1;
            }
            if (!memcmp(buf, nul, sizeof(nul))) {
                break;
            }
            if (!memcmp(buf, bea, sizeof(bea))) {
                udf_trace("ECMA 167 Volume, BEA01\n");
                bea_seen = 1;
            }

            if (bea_seen) {
                if (!memcmp(buf, nsr_02, sizeof(nsr_02))) {
                    udf_trace("ECMA 167 Volume, NSR02\n");
                    return 0;
                }
                if (!memcmp(buf, nsr_03, sizeof(nsr_03))) {
                    udf_trace("ECMA 167 Volume, NSR03\n");
                    return 0;
                }
            }
        }
    }

    udf_error("ECMA 167 Volume Recognition failed\n");
    return -1;
}

/*
 * Volume structure
 */

static int _read_descriptor_block(udfread_block_input *input, uint32_t lba, uint8_t *buf)
{
    if (input->read(input, lba, buf, 1, 0) == 1) {
        return decode_descriptor_tag(buf);
    }

    return -1;
}

static int _read_avdp(ecma_ctx *ecma, udfread_block_input *input,
                      struct anchor_volume_descriptor *avdp)
{
    uint8_t  buf[UDF_BLOCK_SIZE];
    int      tag_id;
    uint32_t lba = 256;

    /*
     * Find Anchor Volume Descriptor Pointer.
     * It is in block 256, last block or (last block - 256)
     * (UDF 2.60, 2.2.3)
     */

    /* try block 256 */
    tag_id = _read_descriptor_block(input, lba, buf);
    if (tag_id != ECMA_AnchorVolumeDescriptorPointer) {

        /* try last block */
        if (!input->size) {
            udf_error("Can't find Anchor Volume Descriptor Pointer\n");
            return -1;
        }

        lba = input->size(input) - 1;
        tag_id = _read_descriptor_block(input, lba, buf);
        if (tag_id != ECMA_AnchorVolumeDescriptorPointer) {

            /* try last block - 256 */
            lba -= 256;
            tag_id = _read_descriptor_block(input, lba, buf);
            if (tag_id != ECMA_AnchorVolumeDescriptorPointer) {
                udf_error("Can't find Anchor Volume Descriptor Pointer\n");
                return -1;
            }
        }
    }
    udf_log("Found Anchor Volume Descriptor Pointer from lba %u\n", lba);

    decode_avdp(buf, avdp);

    return 1;
}

#define VDS_HAVE_PART     (1<<0)
#define VDS_HAVE_PVD      (1<<1)
#define VDS_HAVE_LVD      (1<<2)
#define VDS_HAVE_REQUIRED (VDS_HAVE_PART | VDS_HAVE_PVD)
#define VDS_HAVE_ALL      (VDS_HAVE_PART | VDS_HAVE_PVD | VDS_HAVE_LVD)
/* return value: VDS_HAVE_* mask */
static unsigned _search_vds(ecma_ctx *ecma, udfread_block_input *input,
                            int part_number, const struct extent_ad *loc,
                            struct volume_descriptor_set *vds,
                            unsigned found_mask /* already found descriptors */)
{
    struct volume_descriptor_pointer vdp;
    uint8_t  buf[UDF_BLOCK_SIZE];
    int      tag_id;
    uint32_t lba;
    uint32_t end_lba;

next_extent:
    udf_trace("reading Volume Descriptor Sequence at lba %u, len %u bytes\n", loc->lba, loc->length);

    end_lba = loc->lba + loc->length / UDF_BLOCK_SIZE;

    /* parse Volume Descriptor Sequence */
    for (lba = loc->lba; lba < end_lba; lba++) {

        tag_id = _read_descriptor_block(input, lba, buf);

        switch (tag_id) {

        case ECMA_VolumeDescriptorPointer:
            decode_vdp(buf, &vdp);
            loc = &vdp.next_extent;
            goto next_extent;

        case ECMA_PrimaryVolumeDescriptor:
            udf_log("Primary Volume Descriptor in lba %u\n", lba);
            decode_primary_volume(buf, &vds->pvd);
            found_mask |= VDS_HAVE_PVD;
            break;

        case ECMA_LogicalVolumeDescriptor:
            udf_log("Logical volume descriptor in lba %u\n", lba);
            decode_logical_volume(buf, &vds->lvd);
            found_mask |= VDS_HAVE_LVD;
            break;

        case ECMA_PartitionDescriptor:
          udf_log("Partition Descriptor in lba %u\n", lba);
          if (!(found_mask & VDS_HAVE_PART) ||
              part_number == UDFREAD_PARTITION_LAST) {

              decode_partition(buf, &vds->pd);
              if (part_number < 0 || part_number == vds->pd.number) {
                  found_mask |= VDS_HAVE_PART;
              }
              udf_log("  partition %u at lba %u, %u blocks\n", vds->pd.number, vds->pd.start_block, vds->pd.num_blocks);
          }
          break;

        case ECMA_TerminatingDescriptor:
            udf_trace("Terminating Descriptor in lba %u\n", lba);
            return found_mask;
        }

        if ((found_mask & VDS_HAVE_ALL) == VDS_HAVE_ALL &&
            part_number != UDFREAD_PARTITION_LAST) {
            /* got everything interesting, skip rest blocks */
            return found_mask;
        }
    }

    return found_mask;
}

int udf_read_vds(ecma_ctx *ecma, udfread_block_input *input,
                 int part_number,
                 struct volume_descriptor_set *vds)
{
    struct anchor_volume_descriptor avdp;
    unsigned found_mask;

    /* Find Anchor Volume Descriptor */
    if (_read_avdp(ecma, input, &avdp) < 0) {
        return -1;
    }

    memset(vds, 0, sizeof(*vds));

    /* try to read Main Volume Descriptor Sequence */
    found_mask = _search_vds(ecma, input, part_number, &avdp.mvds, vds, 0);
    if ((found_mask & VDS_HAVE_ALL) == VDS_HAVE_ALL) {
        /* all data found */
        return 0;
    }

    /*
     * Some (or all) descriptors are missing.
     * Try to read missing descriptors from backup area.
     */

    /* try to read Backup Volume Descriptor */
    found_mask = _search_vds(ecma, input, part_number, &avdp.rvds, vds, found_mask);
    if ((found_mask & VDS_HAVE_REQUIRED) == VDS_HAVE_REQUIRED) {
        /* all strictly needed data found (PVD may still be missing) */
        return 0;
    }

    udf_error("failed reading Volume Descriptor Sequence\n");
    return -1;
}

int udf_validate_logical_volume(ecma_ctx *ecma, const struct logical_volume_descriptor *lvd, struct long_ad *fsd_loc)
{
    if (lvd->block_size != UDF_BLOCK_SIZE) {
        udf_error("incompatible block size %u\n", lvd->block_size);
        return -1;
    }

    /* UDF 2.60 2.1.5.2 */
    if (_check_domain_identifier(&lvd->domain_id, lvd_domain_id) < 0) {
        udf_error("unknown Domain ID in Logical Volume Descriptor: %1.22s\n", lvd->domain_id.identifier);
        return -1;

    } else {

        /* UDF 2.60 2.1.5.3 */
        uint16_t rev = _get_u16(lvd->domain_id.identifier_suffix);
        udf_log("Found UDF %x.%02x Logical Volume\n", rev >> 8, rev & 0xff);

        /* UDF 2.60 2.2.4.4 */

        /* location of File Set Descriptors */
        decode_long_ad(lvd->contents_use, fsd_loc);

        udf_log("File Set Descriptor location: partition %u lba %u (len %u)\n",
                fsd_loc->partition, fsd_loc->lba, fsd_loc->length);
    }

    return 0;
}

/*
 * Partitions
 */

static int _map_metadata_partition(udfread_block_input *input,
                                   ecma_ctx *ecma,
                                   struct udf_partitions *part,
                                   uint32_t lba, uint32_t mirror_lba,
                                   const struct partition_descriptor *pd)
{
    struct file_entry *fe;
    uint8_t       buf[UDF_BLOCK_SIZE];
    int           tag_id;
    unsigned int  i;

    /* resolve metadata partition location (it is virtual partition inside another partition) */
    udf_trace("Reading metadata file entry: lba %u, mirror lba %u\n", lba, mirror_lba);

    for (i = 0; i < 2; i++) {

        if (i == 0) {
            tag_id = _read_descriptor_block(input, pd->start_block + lba, buf);
        } else {
            tag_id = _read_descriptor_block(input, pd->start_block + mirror_lba, buf);
        }

        if (tag_id != ECMA_ExtendedFileEntry) {
            udf_error("read metadata file %u: unexpected tag %d\n", i, tag_id);
            continue;
        }

        fe = decode_ext_file_entry(ecma, buf, UDF_BLOCK_SIZE, pd->number);
        if (!fe) {
            udf_error("parsing metadata file entry %u failed\n", i);
            continue;
        }

        if (fe->content_inline) {
            udf_error("invalid metadata file (content inline)\n");
        } else if (!fe->u.ads.num_ad) {
            udf_error("invalid metadata file (no allocation descriptors)\n");
        } else if (fe->file_type == UDF_FT_METADATA) {
            part->p[1].lba = pd->start_block + fe->u.ads.ad[0].lba;
            udf_log("metadata file at lba %u\n", part->p[1].lba);
        } else if (fe->file_type == UDF_FT_METADATA_MIRROR) {
            part->p[1].mirror_lba = pd->start_block + fe->u.ads.ad[0].lba;
            udf_log("metadata mirror file at lba %u\n", part->p[1].mirror_lba);
        } else {
            udf_error("unknown metadata file type %u\n", fe->file_type);
        }

        free_file_entry(&fe);
    }

    if (!part->p[1].lba && part->p[1].mirror_lba) {
        /* failed reading primary location, must use mirror */
        part->p[1].lba        = part->p[1].mirror_lba;
        part->p[1].mirror_lba = 0;
    }

    return part->p[1].lba ? 0 : -1;
}

int udf_parse_partition_maps(ecma_ctx *ecma, udfread_block_input *input,
                             const struct volume_descriptor_set *vds,
                             struct udf_partitions *part)
{
    /* parse partition maps
     * There should be one type1 partition.
     * There may be separate metadata partition.
     * metadata partition is virtual partition that is mapped to metadata file.
     */

    const uint8_t *map = vds->lvd.partition_map_table;
    const uint8_t *end = map + vds->lvd.partition_map_lable_length;
    unsigned int   i;
    int            num_type1_partition = 0;

    udf_log("Partition map count: %u\n", vds->lvd.num_partition_maps);
    if (vds->lvd.partition_map_lable_length > sizeof(vds->lvd.partition_map_table)) {
        udf_error("partition map table too big !\n");
        end -= vds->lvd.partition_map_lable_length - sizeof(vds->lvd.partition_map_table);
    }

    for (i = 0; i < vds->lvd.num_partition_maps && map + 2 < end; i++) {

        /* Partition map, ECMA 167 3/10.7 */
        uint8_t  type = _get_u8(map + 0);
        uint8_t  len  = _get_u8(map + 1);
        uint16_t ref;

        if (len < 2) {
            udf_error("invalid partition map length %d\n", (int)len);
            break;
        }

        udf_trace("map %u: type %u\n", i, type);
        if (map + len > end) {
            udf_error("partition map table too short !\n");
            break;
        }

        if (type == 1) {

            /* ECMA 167 Type 1 partition map */

            if (len != 6) {
                udf_error("invalid type 1 partition map length %d\n", (int)len);
                break;
            }

            ref = _get_u16(map + 4);
            udf_log("partition map: %u: type 1 partition, ref %u\n", i, ref);

            if (num_type1_partition) {
                udf_error("more than one type1 partitions not supported\n");
            } else if (ref != vds->pd.number) {
                udf_error("Logical partition %u refers to another physical partition %u (expected %u)\n", i, ref, vds->pd.number);
            } else {
                part->num_partition   = 1;
                part->p[0].number     = i;
                part->p[0].lba        = vds->pd.start_block;
                part->p[0].mirror_lba = 0; /* no mirror for data partition */

                num_type1_partition++;
            }

        } else if (type == 2) {

            /* Type 2 partition map, UDF 2.60 2.2.18 */

            if (len != 64) {
                udf_error("invalid type 2 partition map length %d\n", (int)len);
                break;
            }

            struct entity_id type_id;
            decode_entity_id(map + 4, &type_id);
            if (!_check_domain_identifier(&type_id, meta_domain_id)) {

                /* Metadata Partition, UDF 2.60 2.2.10 */

                uint32_t lba, mirror_lba;

                ref        = _get_u16(map + 38);
                lba        = _get_u32(map + 40);
                mirror_lba = _get_u32(map + 44);
                if (ref != vds->pd.number) {
                    udf_error("metadata file partition %u != %u\n", ref, vds->pd.number);
                }

                if (!_map_metadata_partition(input, ecma, part, lba, mirror_lba, &vds->pd)) {
                    part->num_partition = 2;
                    part->p[1].number   = i;
                    udf_log("partition map: %u: metadata partition, ref %u. lba %u, mirror %u\n", i, ref, part->p[1].lba, part->p[1].mirror_lba);
                }

            } else {
                udf_log("%u: unsupported type 2 partition\n", i);
            }
        }
        map += len;
    }

    if (!num_type1_partition) {
        udf_error("no type 1 partition found\n");
        return -1;
    }
    return 0;
}
