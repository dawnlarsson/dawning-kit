/*
        tar -- ustar, GNU and pax archives.

        The kernel does not extract archives. The header checksum is
        memory_sum_bytes; a seekable uncompressed regular member is
        copy_file_range then sendfile, the same floors cp uses. Create
        walks with openat and statx on the directory fd. Extract restores
        setuid only for -p or root, the same rule GNU uses. gzip, xz and
        zstd run in-process: -z, -J, --zstd, -a, and extract looks at the
        magic so a .tar.gz needs no extra flag. A packed stream is not
        seekable. bzip2 and compress are named-refused. GNU sparse type S
        is reconstructed from the old GNU map; holes become zeros.
*/

#define TAR_BLOCK 512
#define TAR_PATH 4096
#define TAR_NAME 100
#define TAR_PREFIX 155
#define TAR_CHKSUM 148
#define TAR_CHKSUM_WIDTH 8
#define TAR_SPACE_SUM ((p32)' ' * TAR_CHKSUM_WIDTH)

static p32 tar_header_sum(p8 address_to block)
{
        return memory_sum_bytes(block, TAR_BLOCK) -
               memory_sum_bytes(block + TAR_CHKSUM, TAR_CHKSUM_WIDTH) +
               TAR_SPACE_SUM;
}

static bool tar_header_zero(p8 address_to block)
{
        return memory_sum_bytes(block, TAR_BLOCK) == 0;
}

static bool tar_base256(p8 address_to field, positive width,
                        p64 address_to value)
{
        p64 held = 0;
        positive at;

        if (!width || (field[0] & 0x40))
                return false;

        held = (p64)(field[0] & 0x7f);
        for (at = 1; at < width; at++)
        {
                if (held > (positive_max >> 8))
                        return false;

                held = (held << 8) | field[at];
        }

        address_to value = held;
        return true;
}

static bool tar_field_value(p8 address_to field, positive width,
                            p64 address_to value)
{
        p8 digits[32];
        positive at = 0;
        positive keep;
        string_address cursor;

        if (width && (field[0] & 0x80))
                return tar_base256(field, width, value);

        if (width >= sizeof(digits))
                return false;

        memory_copy(digits, field, width);
        digits[width] = end;

        while (at < width && digits[at] == ' ')
                at++;

        if (at >= width || !digits[at] || digits[at] == ' ')
        {
                address_to value = 0;
                return true;
        }

        cursor = digits + at;
        if (!string_digits_checked(address_of cursor, 8, address_of keep))
                return false;

        at = (positive)(cursor - digits);
        while (at < width && (digits[at] == ' ' || !digits[at]))
                at++;

        if (at < width && digits[at])
                return false;

        address_to value = keep;
        return true;
}

static fn tar_field_put_octal(p8 address_to field, positive width, p64 value)
{
        p8 digits[32];
        positive length;

        memory_fill(field, '0', width);
        if (!width)
                return;

        field[width - 1] = end;
        length = positive_into_base(digits, (positive)value, 8, false);
        if (length >= width)
                length = width - 1;

        memory_copy(field + (width - 1 - length), digits, length);
}

static fn tar_field_put_base256(p8 address_to field, positive width, p64 value)
{
        positive at;

        memory_fill(field, 0, width);
        for (at = width; at; at--)
        {
                field[at - 1] = (p8)value;
                value >>= 8;
        }

        field[0] |= 0x80;
}

static fn tar_field_put(p8 address_to field, positive width, p64 value)
{
        positive bits = width > 1 ? 3 * (width - 1) : 0;

        if (bits && bits < 64 && value <= (((p64)1 << bits) - 1))
                tar_field_put_octal(field, width, value);
        else
                tar_field_put_base256(field, width, value);
}

static fn tar_header_put_checksum(p8 address_to block)
{
        tar_field_put_octal(block + TAR_CHKSUM, 7, tar_header_sum(block));
        block[TAR_CHKSUM + 6] = end;
        block[TAR_CHKSUM + 7] = ' ';
}

static bool tar_header_ok(p8 address_to block)
{
        p64 stored;

        if (!tar_field_value(block + TAR_CHKSUM, TAR_CHKSUM_WIDTH,
                             address_of stored))
                return false;

        return stored == tar_header_sum(block);
}

static positive tar_padded(p64 size)
{
        return (positive)((size + (TAR_BLOCK - 1)) & ~(p64)(TAR_BLOCK - 1));
}

static bool tar_size_fits(p64 size)
{
        return size <= (p64)bipolar_max - (TAR_BLOCK - 1);
}

static fn tar_field_text(p8 address_to field, positive width,
                         p8 address_to into, positive room)
{
        positive keep = string_length_max((string_address)field, width);

        if (keep >= room)
                keep = room ? room - 1 : 0;

        if (keep)
                memory_copy(into, field, keep);

        if (room)
                into[keep] = end;
}

static bool tar_join_name(p8 address_to prefix, p8 address_to name,
                          p8 address_to into, positive room)
{
        p8 head[TAR_PREFIX + 1];
        p8 leaf[TAR_NAME + 1];

        if (!room)
                return false;

        tar_field_text(name, TAR_NAME, leaf, sizeof(leaf));
        tar_field_text(prefix, TAR_PREFIX, head, sizeof(head));
        if (!head[0])
        {
                string_copy_max_end(into, leaf, room ? room - 1 : 0);
                return into[0] != end;
        }

        return path_join(into, room, head, leaf) && into[0];
}

/*
        Drop leading slashes unless the caller asked to keep them, drop a
        leading run of components, and refuse a path that would walk out of
        the destination with `..`.
*/
static bool tar_safe_path(string_address path, positive strip, bool absolute,
                          p8 address_to into, positive room,
                          bool address_to escaped)
{
        p8 work[TAR_PATH];
        positive used = 0;
        string_address at = path;

        if (!path || !room)
                return false;

        if (room > sizeof(work))
                room = sizeof(work);

        into[0] = end;
        if (escaped)
                address_to escaped = false;

        if (!absolute)
                while (string_is(at, '/'))
                        at++;
        else if (string_is(path, '/'))
        {
                work[0] = '/';
                used = 1;
        }

        while (*at)
        {
                string_address slash = string_first_of(at, '/');
                positive piece = slash ? (positive)(slash - at)
                                       : string_length(at);

                if (!piece)
                {
                        at++;
                        continue;
                }

                if (piece == 1 && string_is(at, '.'))
                {
                        at += slash ? piece + 1 : piece;
                        continue;
                }

                if (piece == 2 && string_is(at, '.') && string_is(at + 1, '.'))
                {
                        if (escaped)
                                address_to escaped = true;

                        return false;
                }

                if (strip)
                {
                        strip--;
                        at += slash ? piece + 1 : piece;
                        continue;
                }

                if (used && used + 1 < room && !(used == 1 && work[0] == '/'))
                        work[used++] = '/';

                if (used + piece >= room)
                        return false;

                memory_copy(work + used, at, piece);
                used += piece;
                at += slash ? piece + 1 : piece;
        }

        if (!used || strip)
                return false;

        work[used] = end;
        string_copy_max_end(into, work, room - 1);
        return true;
}

#define TAR_PACK_NONE 0
#define TAR_PACK_GZIP 1
#define TAR_PACK_XZ 2
#define TAR_PACK_ZSTD 3
#define TAR_PACK_AUTO 4
#define TAR_PACK_BZIP2 5
#define TAR_PACK_COMPRESS 6

typedef struct
{
        p8 path[TAR_PATH];
        p8 link[TAR_PATH];
        p64 size;
        bool has_path;
        bool has_link;
        bool has_size;
} tar_pax_state;

static tar_pax_state tar_pax_global;
static tar_pax_state tar_pax_local;

static fn tar_pax_clear(tar_pax_state address_to state)
{
        state->has_path = false;
        state->has_link = false;
        state->has_size = false;
        state->path[0] = end;
        state->link[0] = end;
}

static bool tar_pax_apply(tar_pax_state address_to state,
                          p8 address_to body, positive length)
{
        positive at = 0;

        while (at < length)
        {
                p8 digits[32];
                positive record;
                string_address cursor = digits;
                p8 address_to space = memory_first_of(body + at, ' ',
                                                       length - at);
                positive used = space ? (positive)(space - (body + at)) : 0;
                string_address mark;
                string_address equal;
                positive key;
                positive value;
                positive rest;

                if (!used || used >= sizeof(digits))
                        return false;

                memory_copy_end(digits, body + at, used);
                if (!string_digits_checked(address_of cursor, 10,
                                            address_of record) || *cursor ||
                    record > length - at || record < used + 4)
                        return false;

                mark = body + at + used;
                if (body[at + record - 1] != '\n')
                        return false;

                equal = memory_first_of(mark + 1, '=', record - used - 2);
                if (!equal || equal == mark + 1)
                        return false;

                key = (positive)(equal - (mark + 1));
                value = (positive)((body + at + record - 1) - (equal + 1));
                rest = record;

                if (body[at + rest - 1] != '\n')
                        return false;

                if (key == 4 && !memory_compare(mark + 1, "path", 4))
                {
                        if (value >= TAR_PATH)
                                return false;

                        memory_copy(state->path, equal + 1, value);
                        state->path[value] = end;
                        state->has_path = true;
                }
                else if (key == 8 && !memory_compare(mark + 1, "linkpath", 8))
                {
                        if (value >= TAR_PATH)
                                return false;

                        memory_copy(state->link, equal + 1, value);
                        state->link[value] = end;
                        state->has_link = true;
                }
                else if (key == 4 && !memory_compare(mark + 1, "size", 4))
                {
                        p8 keep[32];
                        positive parsed;
                        string_address cursor = keep;

                        if (value >= sizeof(keep))
                                return false;

                        memory_copy(keep, equal + 1, value);
                        keep[value] = end;
                        if (!string_digits_checked(address_of cursor, 10,
                                                    address_of parsed) ||
                            *cursor || !tar_size_fits(parsed))
                                return false;
                        state->size = parsed;
                        state->has_size = true;
                }

                at += rest;
        }

        return true;
}

static p8 tar_pack_from_name(string_address name)
{
        static const struct { string_address suffix; p8 pack; } suffixes[] = {
            {".gz", TAR_PACK_GZIP}, {".tgz", TAR_PACK_GZIP},
            {".xz", TAR_PACK_XZ}, {".txz", TAR_PACK_XZ},
            {".zst", TAR_PACK_ZSTD}, {".tzst", TAR_PACK_ZSTD},
            {".bz2", TAR_PACK_BZIP2}, {".tbz2", TAR_PACK_BZIP2},
            {".tbz", TAR_PACK_BZIP2}, {".Z", TAR_PACK_COMPRESS}};
        positive n = name ? string_length(name) : 0;

        for (positive at = 0; at < array_count(suffixes); at++)
        {
                positive length = string_length(suffixes[at].suffix);

                if (n >= length &&
                    !memory_compare(name + n - length, suffixes[at].suffix, length))
                        return suffixes[at].pack;
        }
        return TAR_PACK_NONE;
}

static p8 tar_pack_from_magic(p8 address_to magic, positive n)
{
        if (n >= 2 && memory_is_2(magic, 0x1f, 0x8b))
                return TAR_PACK_GZIP;
        if (n >= 6 && memory_is_4(magic, 0xfd, 0x37, 0x7a, 0x58) &&
            memory_is_2(magic + 4, 0x5a, 0))
                return TAR_PACK_XZ;
        if (n >= 4 && memory_is_4(magic, 0x28, 0xb5, 0x2f, 0xfd))
                return TAR_PACK_ZSTD;
        if (n >= 3 && memory_is_2(magic, 'B', 'Z') && magic[2] == 'h')
                return TAR_PACK_BZIP2;
        if (n >= 2 && memory_is_2(magic, 0x1f, 0x9d))
                return TAR_PACK_COMPRESS;
        return TAR_PACK_NONE;
}

#ifndef TAR_PARSE_ONLY

#define TAR_CREATE 'c'
#define TAR_LIST 't'
#define TAR_EXTRACT 'x'

struct tar_options
{
        p8 mode;
        p8 pack;
        string_address archive;
        string_address directory;
        positive strip;
        bool verbose;
        bool absolute;
        bipolar permissions;
        positive first;
};

/* The selected lifecycle is independent of the archive framing/parser. */
typedef struct
{
        p8 level;
        bool (*read_begin)(bipolar, p8 address_to, positive);
        bipolar (*read)(p8 address_to, positive);
        bool (*read_end)(void);
        bool (*write_begin)(bipolar, p8);
        bool (*write)(p8 address_to, positive);
        bool (*write_end)(void);
        string_address address_to why;
} tar_codec;

static const tar_codec tar_codecs[] = {
    [TAR_PACK_GZIP] = {6, gzip_decode_begin_prefix, gzip_decode_read,
        gzip_decode_end, gzip_encode_begin, gzip_encode_write,
        gzip_encode_end, address_of gzip_why},
    [TAR_PACK_XZ] = {6, xz_decode_begin_prefix, xz_decode_read,
        xz_decode_end, xz_encode_begin, xz_encode_write,
        xz_encode_end, address_of xz_why},
    [TAR_PACK_ZSTD] = {3, zstd_decode_begin_prefix, zstd_decode_read,
        zstd_decode_end, zstd_encode_begin, zstd_encode_write,
        zstd_encode_end, address_of zstd_why},
};

/* All zero and never written: the source of padding and the end marker. */
static p8 tar_block[TAR_BLOCK];
/* Extended headers carry more than names: GNU stores xattrs and ACLs there. */
#define TAR_PAX_BODY (1024 * 1024)
static p8 tar_pax_body[TAR_PAX_BODY];
static p8 tar_name[TAR_PATH];
static p8 tar_link[TAR_PATH];
static b32 tar_status;
static bool tar_preserve;

/* Path-keyed records: an arena of spellings behind a hashed index kept below
   half full, whose stored indexes survive growth of both. Extraction keeps
   two. Directories wait for the last member to get their final mode, deepest
   first. A hard-link header names another archive member, not whatever
   exists below the root, so each non-directory member this run materialized
   keeps its identity, replaced when a later member overwrites the path. */
typedef struct
{
        positive path_at;
        positive path_hash;
        positive depth;
        positive mode;
        file_facts facts;
} tar_path_record;

typedef struct
{
        tar_path_record address_to records;
        positive count, room;
        p8 address_to paths;
        positive paths_used, paths_room;
        positive address_to index;
        positive slots, index_room;
} tar_path_store;

static tar_path_store tar_directories;
static tar_path_store tar_materialized;
static positive address_to tar_directory_order;
static positive tar_directory_order_room;
static positive address_to tar_directory_spare;
static positive tar_directory_spare_room;

/*
        GNU default blocking is twenty 512-byte blocks (10 KiB). One 64 KiB
        record is fewer writes on a file of many small members, and copy_file
        range still takes a member that fills a record on its own. The array
        is BSS: it is not resident until the first archive byte moves.
*/
#define TAR_RECORD (TAR_BLOCK * 128)
#define TAR_TRAILING_LIMIT (TAR_RECORD * 16)
#define TAR_ADVISE_SEQUENTIAL 2

static p8 tar_record[TAR_RECORD];
static positive tar_have;
static positive tar_at;
static p8 tar_pack;
static p64 tar_archive_end;
static const tar_codec address_to tar_decoder;
static const tar_codec address_to tar_encoder;
static file_facts tar_output_facts;
static bool tar_output_known;
static file_facts tar_output_target_facts;
static bool tar_output_target_known;
static file_facts tar_output_stage_facts;
static bool tar_output_stage_known;

/*
        Only files with nlink > 1 enter the table. Sixty-four names at
        ustar's 100-byte link limit is enough for a tree of duplicated
        inodes without a megabyte of BSS, and a miss still writes a
        second copy rather than refusing the archive.
*/
#define TAR_SEEN_CAP 64

typedef struct
{
        p64 inode;
        p64 device;
        p32 name_at;
} tar_seen_file;

static tar_seen_file tar_seen[TAR_SEEN_CAP];
static p8 tar_seen_names[TAR_SEEN_CAP * TAR_NAME];
static positive tar_seen_used;
static positive tar_seen_fill;

static fn tar_name_line(writer output, string_address name)
{
        writer_terminal_name(output, name);
        output("\n", 1);
}

static fn tar_fail(string_address what, bipolar failed)
{
        string_format(log_error, "tar: %w: %s\n", writer_terminal_name, what, file_reason(failed));
        tar_status = 2;
}

static fn tar_refuse(string_address message)
{
        string_format(log_error, "tar: %s\n", message);
        tar_status = 2;
}

static bool tar_refuse_pack(p8 pack)
{
        if (pack == TAR_PACK_BZIP2)
        {
                tar_refuse("bzip2 is not this tar");
                return true;
        }

        if (pack == TAR_PACK_COMPRESS)
        {
                tar_refuse("compress is not this tar");
                return true;
        }

        return false;
}

static bool tar_store_index_prepare(tar_path_store address_to store,
                                    positive wanted)
{
        if (store->slots && wanted <= store->slots / 2)
                return true;

        positive larger = store->slots ? store->slots : 64;
        while (wanted > larger / 2)
        {
                if (larger > positive_max / 2)
                        return false;
                larger *= 2;
        }
        if (!shell_array_room(store->index, store->index_room, larger))
                return false;

        memory_fill(store->index, 0, larger * sizeof(store->index[0]));
        for (positive at = 0; at < store->count; at++)
        {
                positive slot = store->records[at].path_hash & (larger - 1);
                while (store->index[slot])
                        slot = (slot + 1) & (larger - 1);
                store->index[slot] = at + 1;
        }
        store->slots = larger;
        return true;
}

static tar_path_record address_to tar_store_find(tar_path_store address_to store,
                                                 string_address path,
                                                 positive hash)
{
        if (!store->slots)
                return null;

        positive slot = hash & (store->slots - 1);
        while (store->index[slot])
        {
                tar_path_record address_to kept =
                    store->records + store->index[slot] - 1;
                if (kept->path_hash == hash &&
                    string_equals(store->paths + kept->path_at, path))
                        return kept;
                slot = (slot + 1) & (store->slots - 1);
        }
        return null;
}

/* The record for path, added when absent; null when memory runs out. */
static tar_path_record address_to tar_store_remember(
    tar_path_store address_to store, string_address path)
{
        positive2 named = string_hash_33_length(path);
        tar_path_record address_to kept = tar_store_find(store, path, named.x);
        positive length = named.y + 1;

        if (kept)
                return kept;
        if (!tar_store_index_prepare(store, store->count + 1) ||
            named.y == positive_max ||
            length > positive_max - store->paths_used ||
            !shell_array_room(store->records, store->room, store->count + 1) ||
            !shell_array_room(store->paths, store->paths_room,
                              store->paths_used + length))
                return null;

        kept = store->records + store->count;
        kept->path_at = store->paths_used;
        kept->path_hash = named.x;
        memory_copy(store->paths + store->paths_used, path, length);
        store->paths_used += length;
        positive slot = named.x & (store->slots - 1);
        while (store->index[slot])
                slot = (slot + 1) & (store->slots - 1);
        store->index[slot] = ++store->count;
        return kept;
}

/* Forget every record; the next index prepare zeroes the slots. */
static fn tar_store_clear(tar_path_store address_to store)
{
        store->count = 0;
        store->paths_used = 0;
        store->slots = 0;
}

static file_facts address_to tar_materialized_find(string_address path)
{
        tar_path_record address_to kept = tar_store_find(
            address_of tar_materialized, path, string_hash_33_length(path).x);
        return kept ? address_of kept->facts : null;
}

static bipolar tar_materialized_remember(string_address path,
                                         file_facts address_to facts)
{
        if ((facts->mask & STATX_BASIC) != STATX_BASIC)
                return -ERROR_INPUT_OUTPUT;

        tar_path_record address_to kept =
            tar_store_remember(address_of tar_materialized, path);
        if (!kept)
                return -ERROR_NO_MEMORY;
        kept->facts = address_to facts;
        return 0;
}

static bool tar_directory_remember(string_address path, positive mode,
                                   file_facts address_to facts)
{
        tar_path_record address_to kept =
            tar_store_remember(address_of tar_directories, path);

        if (!kept)
        {
                tar_refuse("out of memory while retaining directory metadata");
                return false;
        }
        /* A safe path joins its components with single slashes. */
        kept->depth = path[0] == '/' && !path[1]
                          ? 0
                          : memory_count(path, string_length(path), '/') +
                                (path[0] != '/');
        kept->mode = mode;
        kept->facts = address_to facts;
        return true;
}

static bipolar tar_directory_index_order(positive left, positive right)
{
        positive one = tar_directories.records[left].depth;
        positive two = tar_directories.records[right].depth;

        if (one != two)
                return one > two ? -1 : 1;
        return left > right ? -1 : left < right;
}

static fn tar_directories_finish(void)
{
        positive count = tar_directories.count;

        if (!count)
                return;

        if (!shell_array_room(tar_directory_order, tar_directory_order_room,
                              count) ||
            !shell_array_room(tar_directory_spare, tar_directory_spare_room,
                              count))
        {
                tar_refuse("out of memory while restoring directory metadata");
                tar_store_clear(address_of tar_directories);
                return;
        }

        for (positive at = 0; at < count; at++)
                tar_directory_order[at] = at;
        positive address_to order = array_merge_sort(
            tar_directory_order, tar_directory_spare, count,
            tar_directory_index_order);

        for (positive at = 0; at < count; at++)
        {
                tar_path_record address_to kept =
                    tar_directories.records + order[at];
                string_address path = tar_directories.paths + kept->path_at;
                p8 leaf[TAR_PATH];
                bipolar parent = system_open_parent_nofollow(
                    AT_FDCWD, path, false, 0, leaf, sizeof(leaf));
                bipolar opened = parent < 0
                    ? parent
                    : file_open_same(parent, leaf, address_of kept->facts,
                                     FILE_READ | O_DIRECTORY | O_NOFOLLOW);
                bipolar changed = opened < 0
                    ? opened
                    : system_call_2(syscall(fchmod), (positive)opened,
                                    kept->mode);

                if (opened >= 0)
                        system_close(opened);
                if (parent >= 0)
                        system_close(parent);
                if (changed < 0)
                        tar_fail(path, changed);
        }

        tar_store_clear(address_of tar_directories);
}

static fn tar_reset(void)
{
        tar_have = 0;
        tar_at = 0;
        tar_seen_used = 0;
        tar_seen_fill = 0;
        tar_decoder = null;
        tar_encoder = null;
        tar_output_known = false;
        tar_output_target_known = false;
        tar_output_stage_known = false;
        tar_store_clear(address_of tar_directories);
        tar_store_clear(address_of tar_materialized);
}

static bipolar tar_read_bytes(bipolar handle, p8 address_to into, positive n)
{
        return tar_decoder ? tar_decoder->read(into, n)
                           : system_read_retry((positive)handle, into, n);
}

static bool tar_write_bytes(bipolar handle, p8 address_to bytes, positive n)
{
        if (!n)
                return true;
        return tar_encoder ? tar_encoder->write(bytes, n)
                           : system_write_all((positive)handle, bytes, n) == n;
}

static bool tar_packed(void)
{
        return tar_pack > TAR_PACK_NONE && tar_pack < array_count(tar_codecs);
}

static bool tar_codec_begin_read(bipolar handle, p8 address_to magic, positive n)
{
        if (!tar_packed())
                return true;
        const tar_codec address_to codec = tar_codecs + tar_pack;
        if (!codec->read_begin(handle, magic, n))
        {
                tar_refuse("cannot decode archive");
                return false;
        }
        tar_decoder = codec;
        return true;
}

static fn tar_codec_end_read(bipolar handle)
{
        bool ok = true;
        bool excess = false;
        bool nonzero = false;

        if (!tar_decoder)
                return;

        /* Finish the codec so truncation and checksum failures cannot hide
           behind the tar end marker. Padding is permitted, but it must be
           zero and bounded: otherwise a tiny compressed suffix can demand
           unbounded expansion solely to reach the codec trailer. */
        if (tar_status != 2)
        {
                positive trailing = tar_have - tar_at;
                bipolar got = 0;

                if (trailing > TAR_TRAILING_LIMIT)
                        excess = true;
                else if (memory_span_byte(tar_record + tar_at, 0, trailing) != trailing)
                        nonzero = true;

                while (!excess && !nonzero)
                {
                        positive remaining = TAR_TRAILING_LIMIT - trailing;
                        positive ask = remaining < sizeof(tar_record)
                                           ? remaining + 1
                                           : sizeof(tar_record);

                        got = tar_read_bytes(handle, tar_record, ask);
                        if (got <= 0)
                                break;
                        if (memory_span_byte(tar_record, 0, (positive)got) != (positive)got)
                        {
                                nonzero = true;
                                break;
                        }
                        trailing += (positive)got;
                        if (trailing > TAR_TRAILING_LIMIT)
                                excess = true;
                }
                if (got < 0)
                        ok = false;
        }

        ok = tar_decoder->read_end() && ok;
        if (excess)
                tar_refuse("compressed archive padding exceeds limit");
        else if (nonzero)
                tar_refuse("nonzero data follows archive end marker");
        else if (!ok)
                tar_refuse(*tar_decoder->why ? *tar_decoder->why
                                             : (string_address)"cannot decode archive");
        tar_decoder = null;
}

static bool tar_codec_begin_write(bipolar handle)
{
        if (!tar_packed())
                return true;
        const tar_codec address_to codec = tar_codecs + tar_pack;
        if (!codec->write_begin(handle, codec->level))
        {
                tar_refuse("cannot encode archive");
                return false;
        }
        tar_encoder = codec;
        return true;
}

static bool tar_codec_end_write(void)
{
        if (!tar_encoder)
                return true;
        bool ok = tar_encoder->write_end();
        tar_encoder = null;
        if (!ok)
                tar_refuse("cannot finish compressed archive");
        return ok;
}

static fn tar_advise(bipolar handle)
{
        system_call_4(syscall(fadvise64), (positive)handle, 0, 0,
                      TAR_ADVISE_SEQUENTIAL);
}

static bool tar_fill(bipolar handle)
{
        bipolar got;

        if (tar_at && tar_at < tar_have)
                memory_copy_apart(tar_record, tar_record + tar_at,
                                  tar_have - tar_at);

        tar_have -= tar_at;
        tar_at = 0;
        got = tar_read_bytes(handle, tar_record + tar_have,
                             TAR_RECORD - tar_have);
        if (got < 0)
        {
                tar_refuse("cannot read archive");
                return false;
        }

        if (!got)
                return tar_have > 0;

        tar_have += (positive)got;
        return true;
}

static p8 address_to tar_next_block(bipolar handle)
{
        p8 address_to block;

        if (tar_at + TAR_BLOCK > tar_have && !tar_fill(handle))
                return null;

        if (tar_at + TAR_BLOCK > tar_have)
        {
                if (tar_at >= tar_have)
                        return null;

                tar_refuse("unexpected EOF in archive");
                return null;
        }

        block = tar_record + tar_at;
        tar_at += TAR_BLOCK;
        return block;
}

static bool tar_skip(bipolar handle, p64 bytes, bool seekable)
{
        positive have;

        if (!bytes)
                return true;

        have = tar_have - tar_at;
        if (bytes <= have)
        {
                tar_at += (positive)bytes;
                return true;
        }

        bytes -= have;
        tar_at = 0;
        tar_have = 0;
        bipolar reached = seekable && !tar_packed()
                              ? system_seek(handle, (bipolar)bytes, FILE_SEEK_CUR)
                              : -1;
        if (reached >= 0)
        {
                if ((p64)reached <= tar_archive_end)
                        return true;
                tar_refuse("unexpected EOF in archive");
                return false;
        }

        while (bytes)
        {
                positive ask = bytes > TAR_RECORD ? TAR_RECORD : (positive)bytes;
                bipolar got = tar_read_bytes(handle, tar_record, ask);

                if (got <= 0)
                {
                        tar_refuse("unexpected EOF in archive");
                        return false;
                }

                bytes -= (positive)got;
        }

        return true;
}

static bool tar_copy_out(bipolar in, bipolar out, p64 size)
{
        bool range_copy = true;
        bool send_copy = true;

        if (!size)
                return true;

        return file_copy_stream(in, out, size, true, address_of range_copy,
                                address_of send_copy, null);
}

static bool tar_rewind_unread(bipolar handle)
{
        positive unread = tar_have - tar_at;

        if (!unread)
                return true;

        if (system_seek(handle, -(bipolar)unread, FILE_SEEK_CUR) < 0)
                return false;

        tar_at = 0;
        tar_have = 0;
        return true;
}

/* Member data to a descriptor, into memory, or (out < 0, no into) nowhere. */
static bool tar_copy_n(bipolar archive, bipolar out, p64 size, bool seekable,
                       p8 address_to into)
{
        p64 left = size;

        while (left)
        {
                positive have;
                positive take;

                if (!into && left >= TAR_RECORD && !tar_packed() &&
                    tar_rewind_unread(archive))
                {
                        if (out >= 0 && !tar_copy_out(archive, out, left))
                                return false;

                        if (out < 0 && !tar_skip(archive, left, seekable))
                                return false;

                        return true;
                }

                if (tar_at >= tar_have && !tar_fill(archive))
                {
                        tar_refuse("unexpected EOF in archive");
                        return false;
                }

                have = tar_have - tar_at;
                if (!have)
                {
                        tar_refuse("unexpected EOF in archive");
                        return false;
                }

                take = have > left ? (positive)left : have;
                if (into)
                {
                        memory_copy(into, tar_record + tar_at, take);
                        into += take;
                }
                else if (out >= 0 &&
                         system_write_all((positive)out, tar_record + tar_at,
                                          take) != take)
                        return false;

                tar_at += take;
                left -= take;
        }

        return true;
}

static bool tar_deliver(bipolar archive, bipolar out, p64 size, bool seekable)
{
        return tar_copy_n(archive, out, size, seekable, null) &&
               tar_skip(archive, tar_padded(size) - size, seekable);
}

static bool tar_write_hole(bipolar out, p64 size)
{
        return out < 0 || system_seek(out, (bipolar)size, FILE_SEEK_CUR) >= 0;
}

#define TAR_SPARSE_HEADER 4
#define TAR_SPARSE_EXTRA 21
#define TAR_SPARSE_MAX 256

typedef struct
{
        p64 offset;
        p64 bytes;
} tar_sparse_span;

static tar_sparse_span tar_sparse[TAR_SPARSE_MAX];
static positive tar_sparse_used;
static p64 tar_sparse_real;
static bool tar_sparse_active;

static fn tar_sparse_clear(void)
{
        tar_sparse_used = 0;
        tar_sparse_real = 0;
        tar_sparse_active = false;
}

static bool tar_sparse_add(p64 offset, p64 bytes)
{
        if (!bytes)
                return true;

        if (offset > (p64)-1 - bytes)
                return false;

        if (tar_sparse_used >= TAR_SPARSE_MAX)
                return false;

        if (tar_sparse_used &&
            offset < tar_sparse[tar_sparse_used - 1].offset +
                         tar_sparse[tar_sparse_used - 1].bytes)
                return false;

        tar_sparse[tar_sparse_used].offset = offset;
        tar_sparse[tar_sparse_used].bytes = bytes;
        tar_sparse_used++;
        return true;
}

static bool tar_sparse_entry(p8 address_to field)
{
        p64 offset;
        p64 bytes;

        if (!tar_field_value(field, 12, address_of offset) ||
            !tar_field_value(field + 12, 12, address_of bytes))
                return false;

        return tar_sparse_add(offset, bytes);
}

static bool tar_sparse_load(bipolar archive, p8 address_to header)
{
        positive at;
        bool extended;

        tar_sparse_clear();
        for (at = 0; at < TAR_SPARSE_HEADER; at++)
                if (!tar_sparse_entry(header + 386 + at * 24))
                        return false;

        extended = header[482] != 0;
        if (!tar_field_value(header + 483, 12, address_of tar_sparse_real))
                return false;

        while (extended)
        {
                p8 address_to extra = tar_next_block(archive);

                if (!extra)
                        return false;

                for (at = 0; at < TAR_SPARSE_EXTRA; at++)
                        if (!tar_sparse_entry(extra + at * 24))
                                return false;

                extended = extra[504] != 0;
        }

        for (at = 0; at < tar_sparse_used; at++)
                if (tar_sparse[at].offset + tar_sparse[at].bytes >
                    tar_sparse_real)
                        return false;

        tar_sparse_active = true;
        return true;
}

static p64 tar_sparse_payload(void)
{
        p64 held = 0;
        positive at;

        for (at = 0; at < tar_sparse_used; at++)
                held += tar_sparse[at].bytes;

        return held;
}

static bool tar_deliver_sparse(bipolar archive, bipolar out, p64 size,
                               bool seekable)
{
        p64 cursor = 0;
        positive at;

        if (tar_sparse_payload() != size)
        {
                tar_refuse("invalid sparse archive");
                return false;
        }

        for (at = 0; at < tar_sparse_used; at++)
        {
                if (!tar_write_hole(out, tar_sparse[at].offset - cursor) ||
                    !tar_copy_n(archive, out, tar_sparse[at].bytes, seekable, null))
                        return false;

                cursor = tar_sparse[at].offset + tar_sparse[at].bytes;
        }

        return (out < 0 ||
                system_call_2(syscall(ftruncate), (positive)out,
                              tar_sparse_real) >= 0) &&
               tar_skip(archive, tar_padded(size) - size, seekable);
}

static bool tar_flush(bipolar handle)
{
        if (!tar_at)
                return true;

        if (!tar_write_bytes(handle, tar_record, tar_at))
        {
                tar_refuse("cannot write archive");
                return false;
        }

        tar_at = 0;
        return true;
}

static bool tar_put(bipolar handle, p8 address_to bytes, positive length)
{
        while (length)
        {
                positive room;
                positive take;

                if (tar_at == TAR_RECORD && !tar_flush(handle))
                        return false;

                room = TAR_RECORD - tar_at;
                take = length > room ? room : length;
                memory_copy(tar_record + tar_at, bytes, take);
                tar_at += take;
                bytes += take;
                length -= take;
        }

        return true;
}

static bool tar_write_block(bipolar handle, p8 address_to block)
{
        return tar_put(handle, block, TAR_BLOCK);
}

static bool tar_write_padding(bipolar handle, p64 size)
{
        return tar_put(handle, tar_block, tar_padded(size) - (positive)size);
}

static bool tar_put_file(bipolar archive, bipolar in, p64 size)
{
        p64 left = size;

        if (size >= TAR_RECORD && !tar_packed())
        {
                tar_advise(in);
                if (!tar_flush(archive) || !tar_copy_out(in, archive, size))
                        return false;

                return tar_write_padding(archive, size);
        }

        while (left)
        {
                positive room;
                bipolar got;

                if (tar_at == TAR_RECORD && !tar_flush(archive))
                        return false;

                room = TAR_RECORD - tar_at;
                got = system_read_retry((positive)in, tar_record + tar_at,
                                        left > room ? room : (positive)left);
                if (got <= 0)
                        return false;

                tar_at += (positive)got;
                left -= (positive)got;
        }

        return tar_write_padding(archive, size);
}

static bool tar_read_payload(bipolar handle, p64 size, p8 address_to into,
                             positive room, bool seekable)
{
        if (size >= room)
        {
                tar_refuse("member name is too long");
                tar_skip(handle, tar_padded(size), seekable);
                return false;
        }
        if (!tar_copy_n(handle, -1, size, seekable, into))
                return false;
        into[size] = end;
        return tar_skip(handle, tar_padded(size) - size, seekable);
}

static bool tar_name_matches(string_address name, string_address wanted)
{
        positive keep = string_length(wanted);

        if (string_equals(name, wanted))
                return true;

        return keep && string_length(name) > keep &&
               !memory_compare(name, wanted, keep) && name[keep] == '/';
}

static bool tar_wanted(string_address name, positive first, positive count)
{
        positive at;

        if (first >= count)
                return true;

        for (at = first; at < count; at++)
                if (tar_name_matches(name, program_argument((b32)at)))
                        return true;

        return false;
}

/* Extraction publishes a complete staged inode over the destination only if
   that name still identifies the object observed before staging began.  A
   missing name remains a no-clobber decision, and a non-directory member
   never removes a directory tree. */
static bipolar tar_extract_destination(
    bipolar directory, string_address leaf,
    file_facts address_to replaced, bool address_to replaced_known)
{
        address_to replaced_known = false;
        bipolar looked = file_look_code(
            directory, leaf, AT_SYMLINK_NOFOLLOW, replaced);

        if (looked == -ERROR_NO_ENTRY)
                return 0;
        if (looked < 0)
                return looked;
        if ((replaced->mask & STATX_BASIC) != STATX_BASIC)
                return -ERROR_INPUT_OUTPUT;
        if ((replaced->mode & MODE_FORMAT) == MODE_DIRECTORY)
                return -ERROR_IS_DIRECTORY;

        address_to replaced_known = true;
        return 0;
}

/* A parent used by a later member must remain outside another principal's
   rename control.  A trusted sticky directory is acceptable because the
   next opened entry is independently required to have a trusted owner. */
static bool tar_extract_directory_trusted(bipolar handle)
{
        file_facts facts;
        bipolar looked = file_look_code(
            handle, (string_address)"", AT_EMPTY_PATH, address_of facts);
        p32 effective = (p32)system_call(syscall(geteuid));

        return looked >= 0 && (facts.mask & STATX_BASIC) == STATX_BASIC &&
               (facts.mode & MODE_FORMAT) == MODE_DIRECTORY &&
               (facts.owner == effective || facts.owner == 0) &&
               (!(facts.mode & 0002) || (facts.mode & MODE_STICKY));
}

/* -C establishes the root for every later AT_FDCWD member operation.  Open
   and pin the complete directory walk first so a writable ancestor cannot
   exchange one component for a symlink during privileged extraction. */
static bipolar tar_change_directory(string_address path)
{
        bipolar directory = system_open_directory_nofollow(AT_FDCWD, path);
        if (directory < 0)
                return directory;

        bipolar changed = system_call_1(syscall(fchdir),
                                        (positive)directory);
        (void)system_close(directory);
        return changed;
}

/* Intermediate components are private extraction workspaces too.  Clear the
   process mask only around the contained walk so a restrictive caller umask
   cannot turn requested 0700 into an untraversable directory. */
static bipolar tar_extract_parent(string_address path, bool create,
                                  p8 address_to leaf, positive room)
{
        bipolar mask = system_call_1(syscall(umask), 0);
        if (mask < 0)
                return mask;

        bipolar parent = system_open_parent_nofollow_checked(
            AT_FDCWD, path, create, 0700, leaf, room,
            tar_extract_directory_trusted);
        bipolar restored = system_call_1(syscall(umask), (positive)mask);
        if (restored < 0 && parent >= 0)
        {
                system_close(parent);
                parent = restored;
        }
        return parent < 0 ? parent : restored < 0 ? restored : parent;
}

/* Resolve each parent once and keep its descriptor until the member is
   finished. Name checks alone cannot stop a directory from being replaced
   by a symlink between stat and open, and hard-link sources need the same
   resolution as destinations. */
static bool tar_extract_regular(bipolar archive, bipolar directory,
                                string_address leaf, string_address path,
                                p64 size, positive mode, bool seekable)
{
        system_path_stage protected;
        file_facts materialized;
        file_facts replaced;
        bool replaced_known;
        bipolar looked = tar_extract_destination(
            directory, leaf, address_of replaced, address_of replaced_known);
        if (looked < 0)
        {
                tar_fail(path, looked);
                return tar_skip(archive, tar_padded(size), seekable);
        }

        bipolar made = file_stage_file_open_at(
            address_of protected, directory, leaf, 0600);
        if (made < 0)
        {
                tar_fail(path, made);
                return tar_skip(archive, tar_padded(size), seekable);
        }

        if ((tar_sparse_active &&
             !tar_deliver_sparse(archive, made, size, seekable)) ||
            (!tar_sparse_active &&
             !tar_deliver(archive, made, size, seekable)))
        {
                (void)file_stage_publish_protected_at(
                    address_of protected, directory, leaf, made,
                    -ERROR_INPUT_OUTPUT, false,
                    replaced_known ? address_of replaced : null, 0);
                tar_fail(path, -ERROR_INPUT_OUTPUT);
                return false;
        }

        bipolar changed = system_call_2(syscall(fchmod), (positive)made, mode);
        bipolar published_handle = -1;
        bipolar published = file_stage_publish_protected_keep_at(
            address_of protected, directory, leaf, made, changed, false,
            replaced_known ? address_of replaced : null, 0,
            address_of published_handle, false);
        if (published >= 0)
                published = file_look_code(
                    published_handle, (string_address)"", AT_EMPTY_PATH,
                    address_of materialized);
        if (published >= 0 &&
            ((materialized.mask & STATX_BASIC) != STATX_BASIC ||
             (materialized.mode & MODE_FORMAT) != MODE_FILE))
                published = -ERROR_INPUT_OUTPUT;
        if (published_handle >= 0)
                /* This is an O_PATH identity pin.  Closing it cannot undo a
                   member already published into the extraction namespace. */
                (void)system_close(published_handle);
        if (published >= 0)
                published = tar_materialized_remember(
                    path, address_of materialized);
        if (published < 0)
                tar_fail(path, published);
        return published >= 0;
}

static fn tar_extract_member(bipolar archive, p8 type, string_address path,
                             string_address link, p64 size, p64 mode,
                             p64 major, p64 minor, bool seekable, bool verbose)
{
        positive final_mode = (positive)mode & (tar_preserve ? 07777 : 0777);
        bipolar made = 0;
        bipolar parent;
        p8 leaf[TAR_PATH];
        string_address slash = string_last_of(path, '/');
        bool directory = type == '5' || (slash && !slash[1]);
        bool regular = !directory &&
                       (!type || type == '0' || type == '7' || type == 'S');

        if (!tar_preserve && (directory || regular ||
                              type == '3' || type == '4' || type == '6'))
                final_mode &= ~file_umask();

        if (verbose)
                tar_name_line(log_error, path);

        if (!directory && !regular && type != '1' && type != '2' &&
            type != '3' && type != '4' && type != '6')
        {
                p8 shown[2] = {type, end};

                string_format(log_error, "tar: %w: unknown file type '%s'\n", writer_terminal_name,
                              path, shown);
                tar_status = tar_status ? tar_status : 1;
                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        parent = tar_extract_parent(path, true, leaf, sizeof(leaf));
        if (parent < 0)
        {
                tar_fail(path, parent);
                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (regular)
        {
                tar_extract_regular(archive, parent, leaf, path, size,
                                     final_mode, seekable);
                system_close(parent);
                return;
        }

        if (directory)
        {
                made = system_make_directory_exact_at(parent, leaf, 0700);
                if (made == -ERROR_EXISTS)
                        made = 0;
                if (!made)
                {
                        bipolar exact = system_open_at(
                            parent, leaf,
                            O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

                        if (exact < 0)
                                made = exact;
                        else
                        {
                                file_facts facts;
                                made = file_look_code(
                                    exact, (string_address)"", AT_EMPTY_PATH,
                                    address_of facts);
                                /* Explicit directory entries remain private
                                   but traversable until all descendants have
                                   been created. The final archived mode is
                                   restored through the same inode later. */
                                positive working = 0700;
                                p32 effective =
                                    (p32)system_call(syscall(geteuid));
                                if (made >= 0 &&
                                    ((facts.owner != effective &&
                                      facts.owner != 0) ||
                                     !file_name_stable(parent,
                                                       address_of facts)))
                                        made = -ERROR_ACCESS;
                                if (made >= 0 &&
                                    !tar_directory_remember(
                                        path, final_mode, address_of facts))
                                        made = -ERROR_NO_MEMORY;
                                bool changed;
                                positive old_mode;
                                bipolar opened = made < 0
                                    ? made
                                    : file_directory_real(
                                          exact, parent, leaf,
                                          address_of changed,
                                          address_of old_mode);
                                if (made >= 0)
                                        made = opened;
                                if (made >= 0 &&
                                    (facts.mode & 07777) != working)
                                        made = system_call_2(
                                            syscall(fchmod),
                                            (positive)opened, working);
                                if (opened >= 0)
                                        system_close(opened);
                                system_close(exact);
                        }
                }
        }
        else
        {
                system_path_stage protected;
                file_facts materialized;
                file_facts source_facts;
                file_facts replaced;
                bool replaced_known;
                bool materialized_known = false;
                bool destination_satisfied = false;
                bipolar source_handle = -1;
                file_stage_expectation expected = {
                    .kind = type == '2' ? MODE_LINK : type == '6' ? MODE_PIPE :
                            type == '3' ? MODE_CHARACTER : MODE_BLOCK,
                    .link = type == '2' ? link : null,
                    .device_major = type == '6' ? 0 : (p32)major,
                    .device_minor = type == '6' ? 0 : (p32)minor,
                };
                made = tar_extract_destination(
                    parent, leaf, address_of replaced,
                    address_of replaced_known);
                if (made >= 0 && type == '1')
                {
                        p8 source[TAR_PATH];
                        file_facts address_to authorized =
                            tar_materialized_find(link);
                        made = authorized ? 0 : -ERROR_ACCESS;
                        bipolar source_parent = made < 0
                            ? made
                            : tar_extract_parent(
                                  link, false, source, sizeof(source));
                        if (made >= 0)
                                made = source_parent;
                        if (made >= 0)
                        {
                                source_handle = system_open_at(source_parent,
                                    source, O_PATH | O_NOFOLLOW | O_CLOEXEC);
                                system_close(source_parent);
                                made = source_handle < 0 ? source_handle :
                                    file_look_code(source_handle,
                                        (string_address)"", AT_EMPTY_PATH,
                                        address_of source_facts);
                                if (made >= 0 &&
                                    (source_facts.mask & STATX_BASIC) !=
                                        STATX_BASIC)
                                        made = -ERROR_INPUT_OUTPUT;
                                if (made >= 0 &&
                                    (!file_same_identity(
                                         address_of source_facts, authorized) ||
                                     (source_facts.mode & MODE_FORMAT) !=
                                         (authorized->mode & MODE_FORMAT)))
                                        made = -ERROR_ACCESS;
                                if (made >= 0)
                                {
                                        expected.kind = source_facts.mode & MODE_FORMAT;
                                        expected.identity = address_of source_facts;
                                        destination_satisfied =
                                            replaced_known &&
                                            file_same_identity(
                                                address_of source_facts,
                                                address_of replaced);
                                        if (destination_satisfied)
                                                made =
                                                    system_path_same_opened_at(
                                                        source_handle, parent,
                                                        leaf);
                                        if (made >= 0 && destination_satisfied)
                                        {
                                                materialized = source_facts;
                                                materialized_known = true;
                                        }
                                }
                        }
                }
                if (made >= 0 && !destination_satisfied)
                {
                        bipolar handle = file_stage_claim_at(
                            address_of protected, parent, leaf, 0600,
                            source_handle, address_of expected);
                        made = handle;
                        if (handle >= 0)
                        {
                                bipolar published_handle = -1;
                                if (type != '1' && type != '2')
                                        made = system_change_mode_at(
                                            protected.directory,
                                            SYSTEM_PATH_STAGE_LEAF,
                                            final_mode);
                                made = file_stage_publish_protected_keep_at(
                                    address_of protected, parent, leaf,
                                    handle, made, false,
                                    replaced_known ? address_of replaced
                                                   : null,
                                    0, address_of published_handle, false);
                                if (made >= 0)
                                        made = file_look_code(
                                            published_handle,
                                            (string_address)"",
                                            AT_EMPTY_PATH,
                                            address_of materialized);
                                if (made >= 0 &&
                                    ((materialized.mask & STATX_BASIC) !=
                                         STATX_BASIC ||
                                     (materialized.mode & MODE_FORMAT) !=
                                         expected.kind))
                                        made = -ERROR_INPUT_OUTPUT;
                                if (published_handle >= 0)
                                        /* Publication is already committed;
                                           closing its O_PATH identity pin is
                                           not part of member success. */
                                        (void)system_close(published_handle);
                                if (made >= 0)
                                        materialized_known = true;
                        }
                }
                if (made >= 0 && materialized_known)
                        made = tar_materialized_remember(
                            path, address_of materialized);
                if (source_handle >= 0)
                        system_close(source_handle);
        }

        system_close(parent);
        if (made < 0)
                tar_fail(path, made);
        tar_skip(archive, tar_padded(size), seekable);
}

static bool tar_header_gnu_old(p8 address_to block)
{
        return !memory_compare(block + 257, "ustar  ", 8);
}

static bool tar_member_name(p8 address_to block, p8 address_to into)
{
        tar_pax_state address_to state =
            tar_pax_local.has_path ? address_of tar_pax_local
                                   : address_of tar_pax_global;
        if (state->has_path)
        {
                string_copy_max_end(into, state->path, TAR_PATH - 1);
                return into[0] != end;
        }

        /* GNU old format stores atime/sparse maps where ustar keeps prefix. */
        if (tar_header_gnu_old(block))
        {
                tar_field_text(block, TAR_NAME, into, TAR_PATH);
                return into[0] != end;
        }

        return tar_join_name(block + 345, block, into, TAR_PATH);
}

static bool tar_member_link(p8 address_to block, p8 address_to into)
{
        tar_pax_state address_to state =
            tar_pax_local.has_link ? address_of tar_pax_local
                                   : address_of tar_pax_global;
        if (state->has_link)
        {
                string_copy_max_end(into, state->link, TAR_PATH - 1);
                return true;
        }

        tar_field_text(block + 157, TAR_NAME, into, TAR_PATH);
        return true;
}

static b32 tar_read_archive(struct tar_options address_to options)
{
        bipolar handle;
        p8 address_to block;
        bool seekable;
        bool listed = false;
        p8 long_name[TAR_PATH];
        p8 long_link[TAR_PATH];
        bool have_long_name = false;
        bool have_long_link = false;
        positive count = (positive)program_argument_count();

        tar_pax_clear(address_of tar_pax_global);
        tar_pax_clear(address_of tar_pax_local);
        tar_sparse_clear();
        tar_reset();
        long_name[0] = end;
        long_link[0] = end;
        tar_pack = options->pack;

        if (!options->archive || string_equals(options->archive, "-"))
                handle = 0;
        else
        {
                handle = system_open_at(AT_FDCWD, options->archive,
                                        FILE_READ | O_CLOEXEC);
                if (handle < 0)
                {
                        tar_fail(options->archive, handle);
                        return tar_status;
                }
        }

        seekable = system_seek(handle, 0, FILE_SEEK_CUR) >= 0;
        {
                /* A seek skips member data without reading it, so only the
                   size of a regular archive can say the data is not there. */
                file_facts archive;

                tar_archive_end =
                    file_look(handle, (string_address)"", AT_EMPTY_PATH,
                              address_of archive) &&
                            (archive.mode & MODE_FORMAT) == MODE_FILE
                        ? archive.size
                        : (p64)-1;
        }
        tar_advise(handle);
        {
                p8 magic[6];
                bipolar got = system_read_retry((positive)handle, magic,
                                                sizeof(magic));

                if (got < 0)
                {
                        tar_refuse("cannot read archive");
                        goto abandoned;
                }
                if (tar_pack == TAR_PACK_NONE || tar_pack == TAR_PACK_AUTO)
                {
                        p8 sniffed = tar_pack_from_magic(magic, (positive)got);

                        tar_pack = sniffed;
                }
                if (tar_refuse_pack(tar_pack))
                        goto abandoned;
                if (tar_packed())
                {
                        seekable = false;
                        if (!tar_codec_begin_read(handle, magic, (positive)got))
                                goto abandoned;
                }
                else if (got > 0 && (positive)got <= sizeof(magic))
                {
                        memory_copy(tar_record, magic, (positive)got);
                        tar_have = (positive)got;
                        tar_at = 0;
                }
        }
        /* GNU: -p / root restores MODE_ALL; otherwise only 0777 & ~umask. */
        tar_preserve = options->permissions < 0
                           ? false
                           : (options->permissions > 0 ||
                              system_call_1(syscall(geteuid), 0) == 0);

        if (options->directory)
        {
                bipolar moved = tar_change_directory(options->directory);

                if (moved < 0)
                {
                        tar_fail(options->directory, moved);
                        tar_codec_end_read(handle);
                        goto abandoned;
                }
        }

        while ((block = tar_next_block(handle)))
        {
                p8 type;
                p64 size = 0;
                p64 mode = 0;
                p64 major = 0;
                p64 minor = 0;
                p8 kept[TAR_PATH];
                p8 kept_link[TAR_PATH];
                bool escaped;

                if (tar_header_zero(block))
                {
                        p8 address_to second = tar_next_block(handle);

                        if (!second)
                        {
                                if (tar_status != 2)
                                        tar_refuse("archive end marker is incomplete");
                        }
                        else if (!tar_header_zero(second))
                                tar_refuse("archive end marker is followed by data");
                        break;
                }

                if (!tar_header_ok(block))
                {
                        tar_refuse("invalid header checksum");
                        break;
                }

                type = block[156];
                if (!tar_field_value(block + 124, 12, address_of size) ||
                    !tar_field_value(block + 100, 8, address_of mode) ||
                    !tar_field_value(block + 329, 8, address_of major) ||
                    !tar_field_value(block + 337, 8, address_of minor))
                {
                        tar_refuse("invalid header");
                        break;
                }

                if (!tar_size_fits(size))
                {
                        tar_refuse("member size is too large");
                        break;
                }

                tar_sparse_clear();
                if (type == 'S' && !tar_sparse_load(handle, block))
                {
                        tar_refuse("invalid sparse archive");
                        break;
                }
                if (tar_sparse_active && tar_sparse_payload() != size)
                {
                        tar_refuse("invalid sparse archive");
                        break;
                }

                if (type == 'L')
                {
                        have_long_name = tar_read_payload(handle, size,
                                                          long_name, TAR_PATH,
                                                          seekable);
                        continue;
                }

                if (type == 'K')
                {
                        have_long_link = tar_read_payload(handle, size,
                                                          long_link, TAR_PATH,
                                                          seekable);
                        continue;
                }

                if (type == 'x' || type == 'g')
                {
                        if (!tar_read_payload(handle, size, tar_pax_body,
                                              TAR_PAX_BODY, seekable) ||
                            !tar_pax_apply(
                                type == 'g'
                                    ? address_of tar_pax_global
                                    : address_of tar_pax_local,
                                tar_pax_body, (positive)size))
                        {
                                tar_refuse("invalid extended header");
                                break;
                        }

                        continue;
                }

                if (tar_pax_local.has_size)
                        size = tar_pax_local.size;
                else if (tar_pax_global.has_size)
                        size = tar_pax_global.size;
                if (!tar_size_fits(size))
                {
                        tar_refuse("member size is too large");
                        break;
                }

                if (have_long_name)
                        string_copy_max_end(tar_name, long_name, TAR_PATH - 1);
                else if (!tar_member_name(block, tar_name))
                {
                        tar_refuse("member name is too long");
                        break;
                }

                if (have_long_link)
                        string_copy_max_end(tar_link, long_link, TAR_PATH - 1);
                else
                        tar_member_link(block, tar_link);

                have_long_name = false;
                have_long_link = false;
                long_name[0] = end;
                long_link[0] = end;

                if (!tar_wanted(tar_name, options->first, count))
                {
                        tar_skip(handle, tar_padded(size), seekable);
                        tar_pax_clear(address_of tar_pax_local);
                        continue;
                }

                if (!tar_safe_path(tar_name, options->strip, options->absolute,
                                   kept, TAR_PATH, address_of escaped))
                {
                        /*      A name that reduces to nothing is the
                                archive's own root -- "./" is in every
                                archive written from a directory -- or a
                                member wholly removed by --strip-components.
                                The reference passes over both without a
                                word; only a name that tried to climb out
                                is worth saying anything about. */
                        if (escaped)
                        {
                                string_format(log_error, "tar: %w: member name is unsafe\n",
                                              writer_terminal_name, tar_name);
                                tar_status = 2;
                        }

                        tar_skip(handle, tar_padded(size), seekable);
                        tar_pax_clear(address_of tar_pax_local);
                        continue;
                }

                if (type == '1')
                {
                        if (!tar_safe_path(tar_link, options->strip,
                                           options->absolute, kept_link,
                                           TAR_PATH, null))
                        {
                                tar_refuse("hard link target is unsafe");
                                tar_skip(handle, tar_padded(size), seekable);
                                tar_pax_clear(address_of tar_pax_local);
                                continue;
                        }
                }
                else
                        string_copy_max_end(kept_link, tar_link, TAR_PATH - 1);

                listed = true;
                if (options->mode == TAR_LIST)
                {
                        tar_name_line(log, kept);
                        tar_skip(handle, tar_padded(size), seekable);
                }
                else
                        tar_extract_member(handle, type, kept, kept_link, size,
                                           mode, major, minor, seekable,
                                           options->verbose);

                tar_pax_clear(address_of tar_pax_local);
        }

        tar_codec_end_read(handle);
        if (handle > 0)
                system_close(handle);

        if (options->mode == TAR_EXTRACT)
                tar_directories_finish();

        if (options->first < count && !listed && !tar_status)
                tar_refuse("the requested members were not in the archive");

        log_flush();
        return tar_status;

abandoned:
        if (handle > 0)
                system_close(handle);
        return tar_status;
}

static fn tar_header_ustar(p8 address_to block, string_address name,
                           p8 type, p64 size, p64 mode, p64 mtime,
                           string_address link)
{
        p8 prefix[TAR_PREFIX + 1];
        p8 leaf[TAR_NAME + 1];
        positive length = string_length(name);
        string_address slash;

        memory_fill(block, 0, TAR_BLOCK);
        prefix[0] = end;
        leaf[0] = end;

        if (length < TAR_NAME)
                string_copy_max_end(leaf, name, TAR_NAME);
        else
        {
                slash = string_last_of(name, '/');
                if (!slash || (positive)(slash - name) >= TAR_PREFIX ||
                    string_length(slash + 1) >= TAR_NAME)
                {
                        tar_refuse("member name is too long for ustar");
                        return;
                }

                memory_copy(prefix, name, (positive)(slash - name));
                prefix[slash - name] = end;
                string_copy_max_end(leaf, slash + 1, TAR_NAME);
        }

        memory_copy(block, leaf, string_length(leaf));
        if (prefix[0])
                memory_copy(block + 345, prefix, string_length(prefix));

        tar_field_put_octal(block + 100, 8, mode);
        tar_field_put_octal(block + 108, 8, 0);
        tar_field_put_octal(block + 116, 8, 0);
        tar_field_put(block + 124, 12, size);
        tar_field_put(block + 136, 12, mtime);
        block[156] = type;
        if (link)
                memory_copy(block + 157, link,
                            min(string_length(link), TAR_NAME - 1));

        memory_copy(block + 257, "ustar", 6);
        block[263] = '0';
        block[264] = '0';
        tar_header_put_checksum(block);
}

static bool tar_put_header(bipolar handle, string_address name, p8 type,
                           p64 size, p64 mode, p64 mtime, string_address link)
{
        positive link_length = link ? string_length(link) : 0;

        /* A target the ustar field cannot hold travels whole in a GNU K
           member ahead of its header, as GNU tar writes it. */
        if (link_length >= TAR_NAME &&
            (!tar_put_header(handle, "././@LongLink", 'K', link_length + 1,
                             0644, 0, null) ||
             !tar_put(handle, (p8 address_to)link, link_length + 1) ||
             !tar_write_padding(handle, link_length + 1)))
                return false;

        if (tar_at + TAR_BLOCK > TAR_RECORD && !tar_flush(handle))
                return false;

        tar_header_ustar(tar_record + tar_at, name, type, size, mode, mtime,
                         link);
        if (tar_status == 2)
                return false;

        tar_at += TAR_BLOCK;
        return true;
}

static p64 tar_identity(file_facts address_to facts)
{
        return ((p64)facts->device_major << 32) | facts->device_minor;
}

static string_address tar_seen_name(file_facts address_to facts)
{
        positive at;

        if (facts->hard_links < 2)
                return null;

        for (at = 0; at < tar_seen_used; at++)
                if (tar_seen[at].inode == facts->inode &&
                    tar_seen[at].device == tar_identity(facts))
                        return tar_seen_names + tar_seen[at].name_at;

        return null;
}

static fn tar_seen_store(file_facts address_to facts, string_address member)
{
        positive length;

        if (facts->hard_links < 2 || tar_seen_used >= TAR_SEEN_CAP)
                return;

        length = string_length(member);
        if (length >= TAR_NAME ||
            tar_seen_fill + length + 1 > sizeof(tar_seen_names))
                return;

        tar_seen[tar_seen_used].inode = facts->inode;
        tar_seen[tar_seen_used].device = tar_identity(facts);
        tar_seen[tar_seen_used].name_at = (p32)tar_seen_fill;
        memory_copy(tar_seen_names + tar_seen_fill, member, length + 1);
        tar_seen_fill += length + 1;
        tar_seen_used += 1;
}

static b32 tar_add_named(bipolar archive, bipolar directory,
                         string_address name, string_address member,
                         bool verbose, bool selected);

static b32 tar_add_directory(bipolar archive, bipolar directory,
                             string_address name, string_address member,
                             file_facts address_to facts, bool verbose)
{
        file_walk walk;

        if (!file_walk_open_found_same(address_of walk, directory, name,
                                       facts))
                return tar_fail(member, walk.error), tar_status;

        if (!tar_put_header(archive, member, '5', 0, facts->mode & 07777,
                            (p64)facts->modified.seconds, null))
        {
                file_walk_close(address_of walk);
                return tar_status;
        }

        for (;;)
        {
                struct linux_dirent64 address_to entry = file_walk_next(
                    address_of walk);
                p8 child[TAR_PATH];

                if (!entry)
                        break;

                if (file_is_dot(entry->d_name))
                        continue;

                if (!file_path_join(child, member, entry->d_name))
                {
                        tar_refuse("member name is too long");
                        break;
                }

                tar_add_named(archive, walk.handle, entry->d_name, child,
                              verbose, false);
                if (tar_status == 2)
                        break;
        }

        file_walk_close(address_of walk);
        return tar_status;
}

static b32 tar_add_named(bipolar archive, bipolar directory,
                         string_address name, string_address member,
                         bool verbose, bool selected)
{
        file_facts facts;
        p8 link[TAR_PATH];
        bipolar handle;
        bipolar looked;
        string_address prior;
        p8 type;

        looked = system_stat_at(directory, name,
                                AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                                STATX_BASIC, address_of facts);
        if (looked < 0)
        {
                tar_fail(member, looked);
                return tar_status;
        }

        bool active_output =
            tar_output_known &&
            file_same_identity(address_of facts, address_of tar_output_facts);
        bool replaced_output =
            tar_output_target_known &&
            file_same_identity(address_of facts,
                               address_of tar_output_target_facts);
        if (tar_output_stage_known &&
            file_same_identity(address_of facts,
                               address_of tar_output_stage_facts))
                return tar_status;
        if (active_output || replaced_output)
        {
                log_error("tar: ", 5);
                writer_terminal_name(log_error, member);
                log_error(": archive cannot contain itself; not dumped\n", 44);
                if (selected && replaced_output)
                        tar_status = 2;
                return tar_status;
        }

        if (verbose)
                tar_name_line(log_error, member);

        if ((facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                return tar_add_directory(archive, directory, name, member,
                                         address_of facts, verbose);

        if ((facts.mode & MODE_FORMAT) == MODE_SOCKET)
        {
                string_format(log_error, "tar: %w: socket ignored\n", writer_terminal_name, member);
                tar_status = tar_status ? tar_status : 1;
                return tar_status;
        }

        if ((facts.mode & MODE_FORMAT) == MODE_LINK)
        {
                handle = file_open_same(directory, name, address_of facts,
                                        O_PATH | O_NOFOLLOW);
                bipolar got = handle < 0
                                  ? handle
                                  : system_read_link_at(
                                        handle, (string_address)"", link,
                                        sizeof(link));

                if (got < 0 || (positive)got >= sizeof(link))
                {
                        if (handle >= 0)
                                system_close(handle);
                        tar_fail(member, got < 0 ? got
                                                 : -ERROR_NAME_TOO_LONG);
                        return tar_status;
                }

                link[got] = end;
                tar_put_header(archive, member, '2', 0, facts.mode & 07777,
                               (p64)facts.modified.seconds, link);
                system_close(handle);
                return tar_status;
        }

        prior = tar_seen_name(address_of facts);
        if (prior)
        {
                tar_put_header(archive, member, '1', 0, facts.mode & 07777,
                               (p64)facts.modified.seconds, prior);
                return tar_status;
        }

        if ((facts.mode & MODE_FORMAT) == MODE_PIPE)
                type = '6';
        else if ((facts.mode & MODE_FORMAT) == MODE_CHARACTER)
                type = '3';
        else if ((facts.mode & MODE_FORMAT) == MODE_BLOCK)
                type = '4';
        else
                type = '0';

        if (type != '0')
        {
                p8 address_to header;

                if (tar_at + TAR_BLOCK > TAR_RECORD && !tar_flush(archive))
                        return tar_status;

                header = tar_record + tar_at;
                tar_header_ustar(header, member, type, 0, facts.mode & 07777,
                                 (p64)facts.modified.seconds, null);
                if (tar_status == 2)
                        return tar_status;

                if (type == '3' || type == '4')
                {
                        tar_field_put_octal(header + 329, 8, facts.rdev_major);
                        tar_field_put_octal(header + 337, 8, facts.rdev_minor);
                        tar_header_put_checksum(header);
                }

                tar_at += TAR_BLOCK;
                return tar_status;
        }

        handle = file_open_same(
            directory, name, address_of facts,
            FILE_READ | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (handle < 0)
        {
                tar_fail(member, handle);
                return tar_status;
        }

        if (!tar_put_header(archive, member, '0', facts.size,
                            facts.mode & 07777, (p64)facts.modified.seconds,
                            null))
        {
                system_close(handle);
                return tar_status;
        }

        if (!tar_put_file(archive, handle, (p64)facts.size))
                tar_fail(member, -ERROR_INPUT_OUTPUT);
        else
                tar_seen_store(address_of facts, member);

        system_close(handle);
        return tar_status;
}

static b32 tar_add_path(bipolar archive, string_address path, bool verbose)
{
        return tar_add_named(archive, AT_FDCWD, path, path, verbose, true);
}

static b32 tar_write_archive(struct tar_options address_to options)
{
        bipolar handle;
        bipolar looked;
        file_staged_name output_stage;
        bool managed_output = false;
        positive count = (positive)program_argument_count();
        positive at;

        if (options->first >= count)
        {
                tar_refuse("cowardly refusing to create an empty archive");
                return tar_status;
        }

        tar_pack = options->pack;
        if (tar_pack == TAR_PACK_AUTO)
                tar_pack = tar_pack_from_name(options->archive);
        if (tar_refuse_pack(tar_pack))
                return tar_status;

        tar_reset();
        output_stage.directory = -1;
        output_stage.handle = -1;
        if (!options->archive || string_equals(options->archive, "-"))
                handle = 1;
        else
        {
                handle = file_staged_name_open(
                    address_of output_stage, options->archive,
                    0666 & ~file_umask(), FILE_STAGED_STREAM_SPECIAL);
                if (handle < 0)
                {
                        tar_fail(options->archive, handle);
                        return tar_status;
                }
                managed_output = true;
                tar_output_target_known = output_stage.replaced_known;
                if (tar_output_target_known)
                        tar_output_target_facts = output_stage.replaced;

                if (!output_stage.direct)
                {
                        looked = file_look_code(
                            output_stage.protected.directory,
                            (string_address)"", AT_EMPTY_PATH,
                            address_of tar_output_stage_facts);
                        if (looked >= 0 &&
                            ((tar_output_stage_facts.mask & STATX_BASIC) !=
                                 STATX_BASIC ||
                             (tar_output_stage_facts.mode & MODE_FORMAT) !=
                                 MODE_DIRECTORY))
                                looked = -ERROR_INPUT_OUTPUT;
                        if (looked < 0)
                        {
                                tar_fail(options->archive, looked);
                                goto abandoned;
                        }
                        tar_output_stage_known = true;
                }
        }

        looked = file_look_code(handle, (string_address)"", AT_EMPTY_PATH,
                                address_of tar_output_facts);
        if (looked < 0 ||
            (tar_output_facts.mask & STATX_BASIC) != STATX_BASIC)
        {
                if (looked >= 0)
                        looked = -ERROR_INPUT_OUTPUT;
                tar_fail(options->archive ? options->archive
                                          : (string_address)"-",
                         looked);
                goto abandoned;
        }
        tar_output_known = true;
        tar_advise(handle);
        if (!tar_codec_begin_write(handle))
                goto abandoned;

        if (options->directory)
        {
                bipolar moved = tar_change_directory(options->directory);

                if (moved < 0)
                {
                    tar_fail(options->directory, moved);
                    tar_codec_end_write();
                    goto abandoned;
                }
        }

        for (at = options->first; at < count && tar_status != 2; at++)
                tar_add_path(handle, program_argument((b32)at),
                             options->verbose);

        if (tar_status != 2)
        {
                tar_write_block(handle, tar_block);
                tar_write_block(handle, tar_block);
        }

        tar_flush(handle);
        if (tar_encoder)
                tar_codec_end_write();
        if (managed_output)
        {
                bipolar finished = file_staged_name_finish(
                    address_of output_stage, tar_status != 2, 0);
                if (finished < 0 && tar_status != 2)
                        tar_fail(options->archive, finished);
        }
        else if (handle > 2)
        {
                bipolar closed = system_close(handle);
                if (closed < 0 && tar_status != 2)
                        tar_fail(options->archive, closed);
        }

        log_flush();
        return tar_status;

abandoned:
        if (managed_output)
                file_staged_name_abort(address_of output_stage);
        else if (handle > 2)
                system_close(handle);
        return tar_status;
}

/* Long-only rows borrow letters no short option uses. */
static const argument_option tar_option_table[] = {
    {"extract", 'x'}, {"list", 't'}, {"create", 'c'},
    {"file", 'f', ARGUMENT_REQUIRED}, {"directory", 'C', ARGUMENT_REQUIRED},
    {"verbose", 'v'}, {"absolute-names", 'P'},
    {"preserve-permissions", 'p'}, {"same-permissions", 'p'},
    {"no-same-permissions", 'N', ARGUMENT_LONG_ONLY},
    {"gzip", 'z'}, {"gunzip", 'z'}, {"xz", 'J'},
    {"zstd", 'Y', ARGUMENT_LONG_ONLY}, {"auto-compress", 'a'},
    {"bzip2", 'j'}, {"compress", 'Z'},
    {"use-compress-program", 'I', ARGUMENT_LONG_ONLY | ARGUMENT_OPTIONAL},
    {"strip-components", 'S', ARGUMENT_LONG_ONLY | ARGUMENT_REQUIRED},
    {"help", 'h', ARGUMENT_LONG_ONLY}, {"version", 'V', ARGUMENT_LONG_ONLY},
    {null},
};

static struct tar_options tar_parsed;

/* Apply one option. Help, version and the codecs this tar does not carry
   end parsing. */
static bool tar_option_seen(p8 letter, string_address value)
{
        struct tar_options address_to options = address_of tar_parsed;
        positive used;

        switch (letter)
        {
        case 'x': case 't': case 'c': options->mode = letter; break;
        case 'f': options->archive = value; break;
        case 'C': options->directory = value; break;
        case 'v': options->verbose = true; break;
        case 'P': options->absolute = true; break;
        case 'p': options->permissions = 1; break;
        case 'N': options->permissions = -1; break;
        case 'z': options->pack = TAR_PACK_GZIP; break;
        case 'J': options->pack = TAR_PACK_XZ; break;
        case 'Y': options->pack = TAR_PACK_ZSTD; break;
        case 'a': options->pack = TAR_PACK_AUTO; break;
        case 'j': tar_refuse("bzip2 is not this tar"); return false;
        case 'Z': tar_refuse("compress is not this tar"); return false;
        case 'I': tar_refuse("use-compress-program is not this tar"); return false;
        case 'S':
                options->strip = string_digits_max(value, positive_max,
                                                   address_of used);
                if (!used || value[used])
                        return tar_refuse("invalid number of components"), false;
                break;
        case 'h': case 'V':
                string_format(log, letter == 'h'
                    ? "Usage: tar [-ctxzJa] [-f ARCHIVE] [-C DIR] [-p] "
                      "[--gzip] [--xz] [--zstd] [--strip-components N] "
                      "[FILE...]\n"
                    : "tar from dawning-kit\n");
                log_flush();
                options->mode = 'h';
                return false;
        }
        return true;
}

static bool tar_parse(struct tar_options address_to options)
{
        file_taking taking = {.program = "tar", .options = tar_option_table,
                              .seen = tar_option_seen};
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        string_address cluster = count > 1 ? arguments[1] : null;
        positive at = 1;

        memory_fill(options, 0, sizeof(*options));

        /* The old style: a first word of bare letters, whose values follow
           it in the order the letters name them. */
        if (cluster && *cluster && !string_first_of("-", *cluster))
        {
                string_address letter = cluster;

                while (string_get(letter) &&
                       string_first_of("xtcfvCPpzjJZa", string_get(letter)))
                        letter++;
                if (!string_get(letter))
                        for (at = 2; *cluster; cluster++)
                        {
                                p8 named[3] = {'-', *cluster, end};
                                bool valued = argument_option_mode(tar_option_table,
                                                                   *cluster) &
                                              ARGUMENT_REQUIRED;

                                if (valued && at >= count)
                                        return file_option_needs(address_of taking, named);
                                if (!tar_option_seen(*cluster, valued ? arguments[at++] : null))
                                        return false;
                        }
        }

        if (!file_take_from(address_of taking, at))
                return false;

        options->first = taking.first;
        if (!options->mode)
        {
                tar_refuse("you must specify one of the '-c', '-t', or '-x' options");
                return string_report(log_error, false, "Try 'tar --help' for more information.\n");
        }

        return true;
}

static b32 file_tar(void)
{
        tar_status = 0;
        if (!tar_parse(address_of tar_parsed))
                return tar_parsed.mode == 'h' ? 0 : tar_status ? tar_status : 2;

        if (tar_parsed.mode == 'h')
                return 0;

        if (tar_parsed.mode == TAR_CREATE)
                return tar_write_archive(address_of tar_parsed);

        return tar_read_archive(address_of tar_parsed);
}

#endif /* TAR_PARSE_ONLY */
