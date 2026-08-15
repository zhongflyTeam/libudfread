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

#include "udfread.h"

#ifdef HAVE_UDFREAD_VERSION_H
#include "udfread-version.h"
#endif

#include "blockinput.h"
#include "default_blockinput.h"
#include "ecma167.h"
#include "udf_volume.h"
#include "udf_atomic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strtok_r strtok_s
#endif


/*
 * Logging
 */

#include <stdio.h>

static int enable_log   = 0;
static int enable_trace = 0;

#define udf_error(...)   do {                   fprintf(stderr, "udfread ERROR: " __VA_ARGS__); } while (0)
#define udf_log(...)     do { if (enable_log)   fprintf(stderr, "udfread LOG  : " __VA_ARGS__); } while (0)
#define udf_trace(...)   do { if (enable_trace) fprintf(stderr, "udfread TRACE: " __VA_ARGS__); } while (0)

/*
 * utils
 */

static char *_str_dup(const char *s)
{
    size_t len = strlen(s);
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len + 1);
    } else {
        udf_error("out of memory\n");
    }
    return p;
}

static void *_safe_realloc(void *p, size_t s)
{
    void *result = realloc(p, s);
    if (!result) {
        udf_error("out of memory\n");
        free(p);
    }
    return result;
}


/*
 * Decoding
 */

/*
 * outputs Modified UTF-8 (MUTF-8).
 * The null character (U+0000) uses the two-byte overlong encoding 11000000 10000000 (hexadecimal C0 80), instead of 00000000 (hexadecimal 00).
 *
 * - not strictly UTF-8 compilant, but works with C str*() functions and Java, while \0 bytes in middle of strings won't.
 */

#define utf16lo_to_mutf8(out, out_pos, out_size, ch)    \
  do {                                              \
    if (ch != 0 && ch < 0x80) {                     \
      out[out_pos++] = (uint8_t)ch;                 \
    } else {                                        \
      out_size++;                                   \
      out = (uint8_t *)_safe_realloc(out, out_size);\
      if (!out) return NULL;                        \
                                                    \
      out[out_pos++] = 0xc0 | (ch >> 6);            \
      out[out_pos++] = 0x80 | (ch & 0x3f);          \
    }                                               \
  } while (0)

#define utf16_to_mutf8(out, out_pos, out_size, ch)      \
  do {                                                  \
    if (ch < 0x7ff) {                                   \
      utf16lo_to_mutf8(out, out_pos, out_size, ch);     \
    } else {                                            \
      out_size += 2;                                    \
      out = (uint8_t *)_safe_realloc(out, out_size);    \
      if (!out) return NULL;                            \
                                                        \
      out[out_pos++] = 0xe0 | (ch >> 12);               \
      out[out_pos++] = 0x80 | ((ch >> 6) & 0x3f);       \
      out[out_pos++] = 0x80 | (ch & 0x3f);              \
                                                        \
    }                                                   \
  } while (0)

/* Strings, CS0 (UDF 2.1.1) */
static char *_cs0_to_mutf8(const uint8_t *cs0, size_t size)
{
    size_t   out_pos = 0;
    size_t   out_size = size;
    size_t   i;
    uint8_t *out;

    if (size < 1) {
        /* empty string */
        return calloc(1, 1);
    }

    out = (uint8_t *)malloc(size);
    if (!out) {
        udf_error("out of memory\n");
        return NULL;
    }

    switch (cs0[0]) {
    case 8:
        /*udf_trace("string in utf-8\n");*/
        for (i = 1; i < size; i++) {
            utf16lo_to_mutf8(out, out_pos, out_size, cs0[i]);
        }
        break;
    case 16:
        for (i = 1; i < size - 1; i+=2) {
            uint16_t ch = cs0[i + 1] | (cs0[i] << 8);
            utf16_to_mutf8(out, out_pos, out_size, ch);
        }
        break;
    default:
        udf_error("unregonized string encoding %u\n", cs0[0]);
        free(out);
        return NULL;
    }

    out[out_pos] = 0;
    return (char*)out;
}

/*
 * Block access
 *
 * read block(s) from absolute lba
 */

static uint32_t _read_blocks(udfread_block_input *input,
                             uint32_t lba, void *buf, uint32_t nblocks,
                             int flags)
{
    int result;

    if (!input || (int)nblocks < 1) {
        return 0;
    }

    result = input->read(input, lba, buf, nblocks, flags);

    return result < 0 ? 0 : (uint32_t)result;
}

static int _read_descriptor_block(udfread_block_input *input, uint32_t lba, uint8_t *buf)
{
    if (_read_blocks(input, lba, buf, 1, 0) == 1) {
        return decode_descriptor_tag(buf);
    }

    return -1;
}

/*
 * Cached directory data
 */

struct udf_file_identifier {
    char           *filename;       /* MUTF-8 */
    struct long_ad  icb;            /* location of file entry */
    uint8_t         characteristic; /* CHAR_FLAG_* */
};

struct udf_dir {
    uint32_t                     num_entries;
    struct udf_file_identifier  *files;
    struct udf_dir             **subdirs;
};

static void _free_dir(struct udf_dir **pp)
{
    if (pp && *pp) {
        struct udf_dir *p = *pp;
        uint32_t i;

        if (p->subdirs) {
            for (i = 0; i < p->num_entries; i++) {
                _free_dir(&(p->subdirs[i]));
            }
            free(p->subdirs);
        }

        if (p->files) {
            for (i = 0; i < p->num_entries; i++) {
                free(p->files[i].filename);
            }
            free(p->files);
        }

        free(p);

        *pp = NULL;
    }
}


/*
 *
 */

struct udfread {

    udfread_block_input *input;

    ecma_ctx ecma;
    udf_log  lc_storage;

    /* Volume partitions */
    struct udf_partitions part;

    /* cached directory tree */
    struct udf_dir *root_dir;

    char *volume_identifier;
    char volume_set_identifier[128];

    int partition_to_open;
};

udfread *udfread_init(void)
{
    udfread *udf;

    /* set up logging */
    if (getenv("UDFREAD_LOG")) {
        enable_log = 1;
    }
    if (getenv("UDFREAD_TRACE")) {
        enable_trace = 1;
        enable_log = 1;
    }

#ifdef HAVE_UDFREAD_VERSION_H
    udf_log("libudfread " UDFREAD_VERSION_STRING "\n");
#endif

    udf = (udfread *)calloc(1, sizeof(udfread));
    if (!udf) {
        return NULL;
    }

    udf->partition_to_open = UDFREAD_PARTITION_FIRST;

    if (getenv("UDFREAD_TRACE")) {
        udf->lc_storage.level  = UDFREAD_LOG_TRACE;
    } else if (getenv("UDFREAD_LOG")) {
        udf->lc_storage.level  = UDFREAD_LOG_INFO;
    } else {
        udf->lc_storage.level  = UDFREAD_LOG_ERROR;
    }
    udf->lc_storage.ctx    = stderr;
    udf->lc_storage.logger = (udf_logger)fprintf;
    udf->ecma.lc = &udf->lc_storage;

    return udf;
}

int udfread_set_log (udfread *udf, void *ctx, int (*logger)(void *ctx, const char *fmt, ...) )
{
    if (!udf || udf->input) {
        /*
         * Changing the handler requires updating both ctx and logger
         * function atomically; with the image open this would need
         * locking, so the handler can only be changed before open.
         * After open only the level may be changed: it is stored in
         * lc_storage, whose ctx/logger fields are immutable by then.
         */
        return -1;
    }

    if (logger) {
        udf->lc_storage.ctx = ctx;
        udf->lc_storage.logger = logger;
    } else {
        /* NULL logger: reset to the default handler (fprintf to stderr) */
        udf->lc_storage.ctx = stderr;
        udf->lc_storage.logger = (udf_logger)fprintf;
    }

#ifdef HAVE_UDFREAD_VERSION_H
    udf->lc_storage.logger(udf->lc_storage.ctx, "libudfread " UDFREAD_VERSION_STRING "\n");
#endif

    return 0;
}

int udfread_set_log_level (udfread *udf, int level)
{
    if (!udf || level < UDFREAD_LOG_NONE || level > UDFREAD_LOG_TRACE) {
        return -1;
    }

    if (level <= UDFREAD_LOG_NONE) {
        udf->ecma.lc = NULL;
    } else {
        udf->lc_storage.level = level;
        /*
         * Enabling/disabling logging is a single pointer update
         * (ecma.lc) and therefore safe to do while the image is open;
         * the published struct is always fully initialized because its
         * ctx/logger fields cannot change after open.
         */
        udf->ecma.lc = &udf->lc_storage;
    }

    return 0;
}

int udfread_select_partition (udfread *udf, int partition)
{
    if (!udf) {
        return -1;
    }
    if (udf->input) {
        /* already open */
        return -1;
    }
    if (partition < UDFREAD_PARTITION_LAST) {
        return -1;
    }

    udf->partition_to_open = partition;
    return 0;
}

/*
 * Metadata
 */

static int _partition_index(udfread *udf, uint16_t partition_number)
{
    if (partition_number == udf->part.p[0].number) {
        return 0;
    } else if (udf->part.num_partition > 1 && partition_number == udf->part.p[1].number) {
        return 1;
    }

    udf_error("unknown partition %u\n", partition_number);
    return -1;
}

/* read metadata blocks. If read fails, try from mirror (if available). */
static int _read_metadata_blocks(udfread *udf, uint8_t *buf,
                                const struct long_ad *loc)
{
    int      tag_id;
    uint32_t lba, i, got;
    int      part_idx;

    udf_trace("reading metadata from part %u lba %u\n", loc->partition, loc->lba);

    part_idx = _partition_index(udf, loc->partition);
    if (part_idx < 0) {
        return -1;
    }

    /* read first block. Parse and check tag. */

    lba    = udf->part.p[part_idx].lba + loc->lba;
    tag_id = _read_descriptor_block(udf->input, lba, buf);

    if (tag_id < 0) {

        /* try mirror */
        if (udf->part.p[part_idx].mirror_lba) {
            udf_log("read metadata from lba %u failed, trying mirror\n", lba);
            lba    = udf->part.p[part_idx].mirror_lba + loc->lba;
            tag_id = _read_descriptor_block(udf->input, lba, buf);
        }

        if (tag_id < 0) {
            udf_error("read metadata from lba %u failed\n", lba);
            return -1;
        }
    }

    /* read following blocks without tag parsing and checksum validation */

    for (i = 1; i <= (loc->length - 1) / UDF_BLOCK_SIZE; i++) {

        lba  = udf->part.p[part_idx].lba + loc->lba + i;
        buf += UDF_BLOCK_SIZE;

        got = _read_blocks(udf->input, lba, buf, 1, 0);
        if (got != 1) {
            if (udf->part.p[part_idx].mirror_lba) {
                udf_log("read metadata from lba %u failed, trying mirror\n", lba);
                lba = udf->part.p[part_idx].mirror_lba + loc->lba + i;
                got = _read_blocks(udf->input, lba, buf, 1, 0);
            }
            if (got != 1) {
                udf_error("read metadata from lba %u failed\n", lba);
                return -1;
            }
        }
    }

    return tag_id;
}

static uint8_t *_read_metadata(udfread *udf, const struct long_ad *icb, int *tag_id)
{
    uint32_t  num_blocks = (icb->length + UDF_BLOCK_SIZE - 1) / UDF_BLOCK_SIZE;
    uint8_t  *buf;

    if (num_blocks < 1) {
        return NULL;
    }

    buf = (uint8_t *)malloc(num_blocks * UDF_BLOCK_SIZE);
    if (!buf) {
        udf_error("out of memory\n");
        return NULL;
    }

    *tag_id = _read_metadata_blocks(udf, buf, icb);
    if (*tag_id < 0) {
        udf_log("reading icb blocks failed\n");
        free(buf);
        return NULL;
    }

    return buf;
}

static struct file_entry *_read_file_entry(udfread *udf,
                                           const struct long_ad *icb)
{
    struct file_entry *fe = NULL;
    uint8_t  *buf;
    int       tag_id;

    udf_trace("file entry size %u bytes\n", icb->length);

    buf = _read_metadata(udf, icb, &tag_id);
    if (!buf) {
        udf_error("reading file entry failed\n");
        return NULL;
    }

    switch (tag_id) {
        case ECMA_FileEntry:
            fe = decode_file_entry(&udf->ecma, buf, UDF_BLOCK_SIZE, icb->partition);
            break;
        case ECMA_ExtendedFileEntry:
            fe = decode_ext_file_entry(&udf->ecma, buf, UDF_BLOCK_SIZE, icb->partition);
            break;
        default:
            udf_error("_read_file_entry: unknown tag %d\n", tag_id);
            break;
    }

    free(buf);

    /* read possible additional allocation extents */
    if (fe && !fe->content_inline) {
        while (fe->u.ads.num_ad > 0 &&
               fe->u.ads.ad[fe->u.ads.num_ad - 1].extent_type == ECMA_AD_EXTENT_AD) {

            /* drop pointer to this extent from the end of AD list */
            fe->u.ads.num_ad--;

            icb = &fe->u.ads.ad[fe->u.ads.num_ad];
            udf_log("_read_file_entry: reading allocation extent @%u\n", icb->lba);

            buf = _read_metadata(udf, icb, &tag_id);
            if (!buf) {
                udf_error("_read_file_entry: reading allocation extent @%u failed\n", icb->lba);
                break;
            }

            if (tag_id != ECMA_AllocationExtentDescriptor) {
                free(buf);
                udf_error("_read_file_entry: unexpected tag %d (expected ECMA_AllocationExtentDescriptor)\n", tag_id);
                break;
            }

            if (decode_allocation_extent(&udf->ecma, &fe, buf, icb->length, icb->partition) < 0) {
                free(buf);
                udf_error("_read_file_entry: decode_allocation_extent() failed\n");
                break;
            }

            /* failure before this point will cause an error when reading the file past extent point.
               (extent ad is left in file ad list). */

            free(buf);
        }
    }

    return fe;
}

static int _parse_dir(ecma_ctx *ecma, const uint8_t *data, uint32_t length, struct udf_dir *dir)
{
    struct file_identifier fid;
    const uint8_t *p   = data;
    const uint8_t *end = data + length;
    int            tag_id;

    if (length < 16) {
        return 0;
    }

    while (p < end - 16) {
        size_t used;

        if (dir->num_entries == UINT32_MAX) {
            return 0;
        }

        tag_id = decode_descriptor_tag(p);
        if (tag_id != ECMA_FileIdentifierDescriptor) {
            udf_error("unexpected tag %d in directory file\n", tag_id);
            return -1;
        }

        dir->files = (struct udf_file_identifier *)_safe_realloc(dir->files, sizeof(dir->files[0]) * (dir->num_entries + 1));
        if (!dir->files) {
            return -1;
        }

        used = decode_file_identifier(ecma, p, (size_t)(end - p), &fid);
        if (used == 0) {
            /* not enough data. keep the entries we already have. */
            break;
        }
        p += used;

        if (fid.characteristic & CHAR_FLAG_PARENT) {
            continue;
        }
        if (fid.filename_len < 1) {
            continue;
        }

        dir->files[dir->num_entries].characteristic = fid.characteristic;
        dir->files[dir->num_entries].icb = fid.icb;
        dir->files[dir->num_entries].filename = _cs0_to_mutf8(fid.filename, fid.filename_len);

        if (!dir->files[dir->num_entries].filename) {
            continue;
        }

        /* Skip empty file identifiers.
         * Not strictly compilant (?), \0 is allowed in
         * ECMA167 file identifier.
         */
        if (!dir->files[dir->num_entries].filename[0]) {
            udf_error("skipping empty file identifier\n");
            free(dir->files[dir->num_entries].filename);
            continue;
        }

        dir->num_entries++;
    }

    return 0;
}

static int _read_dir_file(udfread *udf, const struct long_ad *loc, struct udf_dir *dir)
{
    int             result;
    uint8_t        *data;
    int             tag_id;

    udf_trace("directory size %u bytes\n", loc->length);

    data = _read_metadata(udf, loc, &tag_id);
    if (!data) {
        udf_error("reading directory file failed\n");
        return -1;
    }

    result = _parse_dir(&udf->ecma, data, loc->length, dir);

    free(data);
    return result;
}

static int _read_dir(udfread *udf, const struct long_ad *icb, struct udf_dir *dir)
{
    struct file_entry *fe;
    int                result;

    fe = _read_file_entry(udf, icb);
    if (!fe) {
        udf_error("error reading directory file entry\n");
        return -1;
    }

    if (fe->file_type != ECMA_FT_DIR) {
        udf_error("directory file type is not directory\n");
        free_file_entry(&fe);
        return -1;
    }

    if (fe->content_inline) {
        result = _parse_dir(&udf->ecma, &fe->u.data.content[0], fe->u.data.information_length, dir);
        if (result < 0 ) {
                udf_error("failed parsing inline directory file\n");
        }
    } else if (fe->u.ads.num_ad == 0) {
        udf_error("empty directory file");
        result = -1;
    } else {
        if (fe->u.ads.num_ad > 1) {
            udf_error("unsupported fragmented directory file\n");
        }
        result = _read_dir_file(udf, &fe->u.ads.ad[0], dir);
    }

    free_file_entry(&fe);
    return result;
}

static int _find_root_dir(udfread *udf, const struct long_ad *fsd_loc,
                          struct file_set_descriptor *fsd)
{
    uint8_t             buf[UDF_BLOCK_SIZE];
    int                 tag_id = -1;
    struct long_ad      loc = *fsd_loc;

    udf_trace("reading root directory fsd from part %u lba %u\n", fsd_loc->partition, fsd_loc->lba);

    /* search for File Set Descriptor from the area described by fsd_loc */

    loc.length = UDF_BLOCK_SIZE;
    for (; loc.lba <= fsd_loc->lba + (fsd_loc->length - 1) / UDF_BLOCK_SIZE; loc.lba++) {

        tag_id = _read_metadata_blocks(udf, buf, &loc);
        if (tag_id == ECMA_FileSetDescriptor) {
            break;
        }
        if (tag_id == ECMA_TerminatingDescriptor) {
            break;
        }
        udf_error("unhandled tag %d in File Set Descriptor area\n", tag_id);
    }
    if (tag_id != ECMA_FileSetDescriptor) {
        udf_error("didn't find File Set Descriptor\n");
        return -1;
    }

    decode_file_set_descriptor(buf, fsd);
    udf_log("root directory in part %u lba %u\n", fsd->root_icb.partition, fsd->root_icb.lba);

    return 0;
}

static struct udf_dir *_read_subdir(udfread *udf, struct udf_dir *dir, uint32_t index)
{
    if (!(dir->files[index].characteristic & CHAR_FLAG_DIR)) {
        return NULL;
    }

    if (!dir->subdirs) {
        struct udf_dir **subdirs = (struct udf_dir **)calloc(dir->num_entries, sizeof(struct udf_dir *));
        if (!subdirs) {
            udf_error("out of memory\n");
            return NULL;
        }
        if (!atomic_pointer_compare_and_exchange(&dir->subdirs, NULL, subdirs)) {
            free(subdirs);
        }
    }

    if (!dir->subdirs[index]) {
        struct udf_dir *subdir = (struct udf_dir *)calloc(1, sizeof(struct udf_dir));
        if (!subdir) {
            udf_error("out of memory\n");
            return NULL;
        }
        if (_read_dir(udf, &dir->files[index].icb, subdir) < 0) {
            _free_dir(&subdir);
            return NULL;
        }
        if (!atomic_pointer_compare_and_exchange(&dir->subdirs[index], NULL, subdir)) {
            _free_dir(&subdir);
        }
    }

    return dir->subdirs[index];
}

static int _scan_dir(const struct udf_dir *dir, const char *filename, uint32_t *index)
{
    uint32_t i;

    for (i = 0; i < dir->num_entries; i++) {
        if (!strcmp(filename, dir->files[i].filename)) {
            *index = i;
            return 0;
        }
    }

    udf_log("file %s not found\n", filename);
    return -1;
}

static int _find_file(udfread *udf, const char *path,
                      struct udf_dir **p_dir,
                      const struct udf_file_identifier **p_fid)
{
    const struct udf_file_identifier *fid = NULL;
    struct udf_dir *current_dir;
    char *tmp_path, *save_ptr, *token;

    current_dir = udf->root_dir;
    if (!current_dir) {
        return -1;
    }

    tmp_path = _str_dup(path);
    if (!tmp_path) {
        return -1;
    }

    token = strtok_r(tmp_path, "/\\", &save_ptr);
    if (token == NULL) {
        udf_trace("_find_file: requested root dir\n");
    }

    while (token) {
        uint32_t index;
        if (_scan_dir(current_dir, token, &index) < 0) {
            udf_log("_find_file: entry %s not found\n", token);
            goto error;
        }
        fid = &current_dir->files[index];

        token = strtok_r(NULL, "/\\", &save_ptr);

        if (fid->characteristic & CHAR_FLAG_DIR) {
            current_dir = _read_subdir(udf, current_dir, index);
            if (!current_dir) {
                goto error;
            }
        } else if (token) {
            udf_log("_find_file: entry %s not found (parent is file, not directory)\n", token);
            goto error;
        } else {
            // found a file, make sure we won't return directory data
            current_dir = NULL;
        }
    }

    if (p_fid) {
        if (!fid) {
            udf_log("no file identifier found for %s\n", path);
            goto error;
        }
        *p_fid = fid;
    }
    if (p_dir) {
        *p_dir = current_dir;
    }

    free(tmp_path);
    return 0;

error:
    free(tmp_path);
    return -1;
}


/*
 * Volume access API
 */

int udfread_open_input(udfread *udf, udfread_block_input *input/*, int partition*/)
{
    struct volume_descriptor_set vds;
    struct long_ad fsd_location;
    struct file_set_descriptor fsd;

    if (!udf || !input || !input->read) {
        return -1;
    }

    if (udf_probe_volume(&udf->ecma, input) < 0) {
        return -1;
    }

    /* read Volume Descriptor Sequence */
    if (udf_read_vds(&udf->ecma, input, udf->partition_to_open, &vds) < 0) {
        return -1;
    }

    /* validate logical volume structure */
    if (udf_validate_logical_volume(&udf->ecma, &vds.lvd, &fsd_location) < 0) {
        return -1;
    }

    /* Volume Identifier. CS0, UDF 2.1.1 */
    udf->volume_identifier = _cs0_to_mutf8(vds.pvd.volume_identifier, vds.pvd.volume_identifier_length);
    if (udf->volume_identifier) {
        udf_log("Volume Identifier: %s\n", udf->volume_identifier);
    }

    memcpy(udf->volume_set_identifier, vds.pvd.volume_set_identifier, 128);

    /* map partitions */
    if (udf_parse_partition_maps(&udf->ecma, input, &vds, &udf->part) < 0) {
        return -1;
    }

    /* Search for root directory from location given in File Set Descriptor */
    udf->input = input;
    if (_find_root_dir(udf, &fsd_location, &fsd) < 0) {
        udf->input = NULL;
        return -1;
    }

    /* Read root directory */
    udf->root_dir = (struct udf_dir *)calloc(1, sizeof(struct udf_dir));
    if (!udf->root_dir) {
        udf_error("out of memory\n");
        udf->input = NULL;
        return -1;
    }
    if (_read_dir(udf, &fsd.root_icb, udf->root_dir) < 0) {
        _free_dir(&udf->root_dir);
        udf->input = NULL;
        return -1;
    }

    return 0;
}

int udfread_open(udfread *udf, const char *path)
{
    udfread_block_input *input;
    int result;

    if (!udf || !path) {
        return -1;
    }

    input = block_input_new(path);
    if (!input) {
        return -1;
    }

    result = udfread_open_input(udf, input);
    if (result < 0) {
        if (input->close) {
            input->close(input);
        }
    }

    return result;
}

void udfread_close(udfread *udf)
{
    if (udf) {
        if (udf->input) {
            if (udf->input->close) {
                udf->input->close(udf->input);
            }
            udf->input = NULL;
        }

        _free_dir(&udf->root_dir);
        free(udf->volume_identifier);
        free(udf);
    }
}

const char *udfread_get_volume_id(udfread *udf)
{
    if (udf) {
        return udf->volume_identifier;
    }
    return NULL;
}

size_t udfread_get_volume_set_id (udfread *udf, void *buffer, size_t size)
{
    if (udf) {
        if (size > sizeof(udf->volume_set_identifier)) {
            size = sizeof(udf->volume_set_identifier);
        }
        memcpy(buffer, udf->volume_set_identifier, size);
        return sizeof(udf->volume_set_identifier);
    }
    return 0;
}

/*
 * Directory access API
 */

struct udfread_dir {
    udfread              *udf;
    struct udf_dir       *dir;
    uint32_t              current_file;
};

static UDFDIR *_new_udfdir(udfread *udf, struct udf_dir *dir)
{
    UDFDIR *result;

    if (!dir) {
        return NULL;
    }

    result = (UDFDIR *)calloc(1, sizeof(UDFDIR));
    if (result) {
        result->dir = dir;
        result->udf = udf;
    }

    return result;
}

UDFDIR *udfread_opendir(udfread *udf, const char *path)
{
    struct udf_dir *dir = NULL;

    if (!udf || !udf->input || !path) {
        return NULL;
    }

    if (_find_file(udf, path, &dir, NULL) < 0) {
        return NULL;
    }

    return _new_udfdir(udf, dir);
}

UDFDIR *udfread_opendir_at(UDFDIR *p, const char *name)
{
    struct udf_dir *dir = NULL;
    uint32_t index;

    if (!p || !name) {
        return NULL;
    }

    if (_scan_dir(p->dir, name, &index) < 0) {
        udf_log("udfread_opendir_at: entry %s not found\n", name);
        return NULL;
    }

    dir = _read_subdir(p->udf, p->dir, index);

    return _new_udfdir(p->udf, dir);
}

struct udfread_dirent *udfread_readdir(UDFDIR *p, struct udfread_dirent *entry)
{
    const struct udf_file_identifier *fi;

    if (!p || !entry || !p->dir) {
        return NULL;
    }

    if (p->current_file >= p->dir->num_entries) {
        return NULL;
    }

    fi = &p->dir->files[p->current_file];

    entry->d_name = fi->filename;

    if (fi->characteristic & CHAR_FLAG_PARENT) {
        entry->d_type = UDF_DT_DIR;
        entry->d_name = "..";
    } else if (fi->characteristic & CHAR_FLAG_DIR) {
        entry->d_type = UDF_DT_DIR;
    } else {
        entry->d_type = UDF_DT_REG;
    }

    p->current_file++;

    return entry;
}

void udfread_rewinddir(UDFDIR *p)
{
    if (p) {
        p->current_file = 0;
    }
}

void udfread_closedir(UDFDIR *p)
{
    free(p);
}


/*
 * File access API
 */

struct udfread_file {
    udfread           *udf;
    struct file_entry *fe;

    /* byte stream access */
    uint64_t    pos;
    uint8_t    *block;
    int         block_valid;

    void       *block_mem;
};

static UDFFILE *_file_open(udfread *udf, const char *path, const struct udf_file_identifier *fi)
{
    struct file_entry *fe;
    UDFFILE *result;

    if (fi->characteristic & CHAR_FLAG_DIR) {
        udf_log("error opening file %s (is directory)\n", path);
        return NULL;
    }

    fe = _read_file_entry(udf, &fi->icb);
    if (!fe) {
        udf_error("error reading file entry for %s\n", path);
        return NULL;
    }

    result = (UDFFILE *)calloc(1, sizeof(UDFFILE));
    if (!result) {
        free_file_entry(&fe);
        return NULL;
    }

    result->udf = udf;
    result->fe  = fe;

    return result;
}

UDFFILE *udfread_file_open(udfread *udf, const char *path)
{
    const struct udf_file_identifier *fi = NULL;

    if (!udf || !udf->input || !path) {
        return NULL;
    }

    if (_find_file(udf, path, NULL, &fi) < 0) {
        return NULL;
    }

    return _file_open(udf, path, fi);
}

UDFFILE *udfread_file_openat(UDFDIR *dir, const char *name)
{
    uint32_t index;

    if (!dir || !name) {
        return NULL;
    }

    if (_scan_dir(dir->dir, name, &index) < 0) {
        udf_log("udfread_file_openat: entry %s not found\n", name);
        return NULL;
    }

    return _file_open(dir->udf, name, &dir->dir->files[index]);
}

int64_t udfread_file_size(UDFFILE *p)
{
    if (p) {
        return (int64_t)p->fe->length;
    }
    return -1;
}

void udfread_file_close(UDFFILE *p)
{
    if (p) {
        free_file_entry(&p->fe);
        free(p->block_mem);
        free(p);
    }
}

/*
 * block access
 */

static uint32_t _file_lba(UDFFILE *p, uint32_t file_block, uint32_t *extent_length)
{
    const struct file_entry *fe;
    unsigned int i;
    uint32_t     ad_size;

    fe = p->fe;

    for (i = 0; i < fe->u.ads.num_ad; i++) {
        const struct long_ad *ad = &fe->u.ads.ad[0];
        ad_size = (ad[i].length + UDF_BLOCK_SIZE - 1) / UDF_BLOCK_SIZE;
        if (file_block < ad_size) {

            if (ad[i].extent_type != ECMA_AD_EXTENT_NORMAL) {
                if (ad[i].extent_type == ECMA_AD_EXTENT_AD) {
                    udf_error("unsupported allocation descriptor: extent type %u\n", ad[i].extent_type);
                }
                return 0;
            }

            if (!ad[i].lba) {
                /* empty file / no allocated space */
                return 0;
            }

            if (ad[i].partition != p->udf->part.p[0].number) {
                udf_error("file partition %u != %u\n", ad[i].partition, p->udf->part.p[0].number);
            }

            if (extent_length) {
                *extent_length = ad_size - file_block;
            }
            return p->udf->part.p[0].lba + ad[i].lba + file_block;
        }

        file_block -= ad_size;
    }

    return 0;
}

static int _file_lba_exists(UDFFILE *p)
{
    if (!p) {
        return 0;
    }

    if (p->fe->content_inline) {
        udf_error("can't map lba for inline file\n");
        return 0;
    }

    return 1;
}

uint32_t udfread_file_lba(UDFFILE *p, uint32_t file_block)
{
    if (!_file_lba_exists(p)) {
        return 0;
    }

    return _file_lba(p, file_block, NULL);
}

uint32_t udfread_read_blocks(UDFFILE *p, void *buf, uint32_t file_block, uint32_t num_blocks, int flags)
{
    uint32_t i;

    if (!num_blocks || !buf) {
        return 0;
    }

    if (!_file_lba_exists(p)) {
        return 0;
    }

    for (i = 0; i < num_blocks; ) {
        uint32_t extent_length = 0;
        uint32_t lba;
        uint8_t *block = (uint8_t *)buf + UDF_BLOCK_SIZE * i;

        lba = _file_lba(p, file_block + i, &extent_length);
        udf_trace("map block %u to lba %u\n", file_block + i, lba);

        if (!lba) {
            /* unallocated/unwritten block or EOF */
            uint32_t file_blocks = (udfread_file_size(p) + UDF_BLOCK_SIZE - 1) / UDF_BLOCK_SIZE;
            if (file_block + i < file_blocks) {
                udf_trace("zero-fill unallocated / unwritten block %u\n", file_block + i);
                memset(block, 0, UDF_BLOCK_SIZE);
                i++;
                continue;
            }
            udf_error("block %u outside of file (size %u blocks)\n", file_block + i, file_blocks);
            break;
        }

        if (extent_length > num_blocks - i) {
            extent_length = num_blocks - i;
        }

        extent_length = _read_blocks(p->udf->input, lba, block, extent_length, flags);
        if (extent_length < 1) {
            break;
        }
        i += extent_length;
    }
    return i;
}

/*
 * byte stream
 */

static ssize_t _read(UDFFILE *p, void *buf, size_t bytes)
{
    /* start from middle of block ?
     * maximal file size, i.e. position, is 2^32 * block size
     */

    size_t pos_off = p->pos % UDF_BLOCK_SIZE;
    uint32_t file_block = (uint32_t)(p->pos / UDF_BLOCK_SIZE);
    if (pos_off) {
        size_t chunk_size = UDF_BLOCK_SIZE - pos_off;
        if (!p->block_valid) {
            if (udfread_read_blocks(p, p->block, file_block, 1, 0) != 1) {
                return -1;
            }
            p->block_valid = 1;
        }
        if (chunk_size > bytes) {
            chunk_size = bytes;
        }
        memcpy(buf, p->block + pos_off, chunk_size);
        p->pos += (uint64_t)chunk_size;
        return (ssize_t)chunk_size;
    }

    /* read full block(s) ? */
    if (bytes >= UDF_BLOCK_SIZE) {
        uint32_t num_blocks = bytes / UDF_BLOCK_SIZE;
        num_blocks = udfread_read_blocks(p, buf, file_block, num_blocks, 0);
        if (num_blocks < 1) {
            return -1;
        }
        p->pos += num_blocks * UDF_BLOCK_SIZE;
        return num_blocks * UDF_BLOCK_SIZE;
    }

    /* read beginning of a block */
    if (udfread_read_blocks(p, p->block, file_block, 1, 0) != 1) {
        return -1;
    }
    p->block_valid = 1;
    memcpy(buf, p->block, bytes);
    p->pos += bytes;
    return (ssize_t)bytes;
}

static ssize_t _read_inline(UDFFILE *p, void *buf, size_t bytes)
{
    uint64_t information_length = p->fe->u.data.information_length;
    size_t   pad_size = 0;

    if (p->pos + bytes > information_length) {
        udf_log("read hits padding in inline file\n");
        if (p->pos > information_length) {
            pad_size = bytes;
        } else {
            pad_size = (size_t)(p->pos + bytes - information_length);
        }
        memset((char*)buf + bytes - pad_size, 0, pad_size);
    }

    if (pad_size < bytes) {
        memcpy(buf, &p->fe->u.data.content[p->pos], bytes - pad_size);
    }

    p->pos = p->pos + bytes;
    return (ssize_t)bytes;
}

#define ALIGN(p, align) \
  (uint8_t *)( ((uintptr_t)(p) + ((align)-1)) & ~((uintptr_t)((align)-1)))

ssize_t udfread_file_read(UDFFILE *p, void *buf, size_t bytes)
{
    uint8_t *bufpt = (uint8_t *)buf;

    /* sanity checks */
    if (!p || !buf) {
        return -1;
    }
    if ((ssize_t)bytes < 0 || (int64_t)bytes < 0) {
        return -1;
    }

    if (p->pos >= p->fe->length) {
        return 0;
    }

    /* limit range to file size */
    if (p->pos + bytes > p->fe->length) {
        bytes = (size_t)(p->fe->length - p->pos);
    }

    /* small files may be stored inline in file entry */
    if (p->fe->content_inline) {
        return _read_inline(p, buf, bytes);
    }

    /* allocate temp storage for input block */
    if (!p->block) {
        p->block_mem = malloc(2 * UDF_BLOCK_SIZE);
        if (!p->block_mem) {
            return -1;
        }
        p->block = ALIGN(p->block_mem, UDF_BLOCK_SIZE);
    }

    /* read chunks */
    while (bytes > 0) {
        ssize_t r = _read(p, bufpt, bytes);
        if (r < 0) {
            if (bufpt != buf) {
                /* got some bytes */
                break;
            }
            /* got nothing */
            return -1;
        }
        bufpt += r;
        bytes -= (size_t)r;
    }

    return (intptr_t)bufpt - (intptr_t)buf;
}

int64_t udfread_file_tell(UDFFILE *p)
{
    if (p) {
        return (int64_t)p->pos;
    }
    return -1;
}

int64_t udfread_file_seek(UDFFILE *p, int64_t pos, int whence)
{
    if (!p) {
        return -1;
    }

    switch (whence) {
        case UDF_SEEK_CUR:
            pos = udfread_file_tell(p) + pos;
            break;
        case UDF_SEEK_END:
            pos = udfread_file_size(p) + pos;
            break;
        case UDF_SEEK_SET:
            break;
        default:
            return -1;
    }

    if (pos >= 0 && pos <= udfread_file_size(p)) {
        p->pos = (uint64_t)pos;
        p->block_valid = 0;
        return udfread_file_tell(p);
    }

    return -1;
}
