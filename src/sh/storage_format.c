/*
        New partition tables and filesystems, for a disk Moonwater installs to.

        blkid, findfs and mount beside this read these structures; this is the
        other direction, and only as much of it as an install needs: a GPT, a
        FAT32 system partition the firmware loads the image from, and an ext4
        the kernel keeps /bowls, /root and /home on. Nothing here picks a disk.
        The caller hands over an open descriptor and a byte range, and every
        write is positional, so one descriptor on the whole disk formats both
        partitions and a regular file stands in for a disk in the checks.

        Each layout is the one the reference tools write for the same request,
        so their checkers are the oracle: sfdisk --verify for the table, the
        kernel's vfat and the firmware for the system partition, and e2fsck -fn
        for the ext4.
*/

#define STORAGE_FORMAT_ZERO_CHUNK ((positive)1 << 20)

typedef struct
{
        p8 type[16];
        p8 unique[16];
        p64 first;
        p64 last;
        string_address name;
} storage_format_partition;

/* Everything a format would otherwise invent, handed in so that two formats
   of the same size given the same identity are the same bytes. */
typedef struct
{
        p8 uuid[16];
        p8 hash_seed[16];
        p32 time;
        string_address label;
} storage_format_identity;

/* Every field here is little-endian on disk, as the machine floor is; the
   one big-endian field, ext4's jbd2 header, takes the network store. */
#define storage_put16(at, value) memory_store_unaligned(p16, (at), (value))
#define storage_put32(at, value) memory_store_unaligned(p32, (at), (value))
#define storage_put64(at, value) memory_store_unaligned(p64, (at), (value))
#define storage_put32_be(at, value) network_store_32((at), (p32)(value))

static bipolar storage_format_write(bipolar handle, p8 address_to bytes,
                                    positive length, p64 offset)
{
        bipolar written = file_transfer_exact(syscall(pwrite64), handle, bytes,
                                              length, offset);

        return written < 0 ? written : 0;
}

static bipolar storage_format_zero(bipolar handle, p64 offset, p64 length)
{
        p8 address_to zeros;
        bipolar failed = 0;

        if (!length)
                return 0;

        zeros = memory(STORAGE_FORMAT_ZERO_CHUNK);
        if (!zeros)
                return -ERROR_NO_MEMORY;

        memory_zero(zeros, STORAGE_FORMAT_ZERO_CHUNK);

        while (length && !failed)
        {
                positive part = length < STORAGE_FORMAT_ZERO_CHUNK
                                    ? (positive)length
                                    : STORAGE_FORMAT_ZERO_CHUNK;

                failed = storage_format_write(handle, zeros, part, offset);
                offset += part;
                length -= part;
        }

        memory_free(zeros, STORAGE_FORMAT_ZERO_CHUNK);
        return failed;
}

/*
        CRC-32C the way ext4 and jbd2 chain it: no inversion on the way in or
        out, so a seed of ~0 starts a superblock's and a previous result
        continues one. hash_crc32 is the other polynomial, GPT's, and chains
        the same way, so a GPT sum is its complement from a seed of ~0.
*/
static p32 storage_crc32c_table[256];

static p32 storage_crc32c(p32 crc, p8 address_to bytes, positive length)
{
        if (!storage_crc32c_table[128])
                for (positive at = 0; at < 256; at++)
                {
                        p32 value = (p32)at;

                        for (positive bit = 0; bit < 8; bit++)
                                value = value & 1 ? (value >> 1) ^ 0x82F63B78
                                                  : value >> 1;

                        storage_crc32c_table[at] = value;
                }

        while (length--)
                crc = storage_crc32c_table[(crc ^ *bytes++) & 0xff] ^
                      (crc >> 8);

        return crc;
}

// GPT -----------------------------------------------------------

/* Type GUIDs in their on-disk order: the first three fields little-endian. */
static p8 storage_gpt_system_type[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static p8 storage_gpt_linux_type[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

#define STORAGE_GPT_ENTRIES 128
#define STORAGE_GPT_ENTRY 128
#define STORAGE_GPT_HEADER 92
#define STORAGE_GPT_NAME 36

static p64 storage_gpt_table_sectors(p32 sector_size)
{
        return (STORAGE_GPT_ENTRIES * STORAGE_GPT_ENTRY + sector_size - 1) /
               sector_size;
}

/* Where partitions may go on a disk this size, so the caller can place them
   before asking for the table. False for a geometry no table fits. */
static bool storage_gpt_span(p64 sectors, p32 sector_size,
                             p64 address_to first, p64 address_to last)
{
        p64 table;

        if (sector_size < 512 || sector_size > 4096 ||
            (sector_size & (sector_size - 1)))
                return false;

        table = storage_gpt_table_sectors(sector_size);
        if (sectors < 2 * (2 + table) + 1)
                return false;

        address_to first = 2 + table;
        address_to last = sectors - 2 - table;
        return true;
}

static fn storage_gpt_header(p8 address_to sector, p64 self, p64 other,
                             p64 first, p64 last, p8 address_to disk,
                             p64 entries, p32 entries_crc)
{
        memory_zero(sector, STORAGE_GPT_HEADER);
        memory_copy(sector, "EFI PART", 8);
        storage_put32(sector + 8, 0x00010000);
        storage_put32(sector + 12, STORAGE_GPT_HEADER);
        storage_put64(sector + 24, self);
        storage_put64(sector + 32, other);
        storage_put64(sector + 40, first);
        storage_put64(sector + 48, last);
        memory_copy(sector + 56, disk, 16);
        storage_put64(sector + 72, entries);
        storage_put32(sector + 80, STORAGE_GPT_ENTRIES);
        storage_put32(sector + 84, STORAGE_GPT_ENTRY);
        storage_put32(sector + 88, entries_crc);
        storage_put32(sector + 16,
                      ~hash_crc32(~(p32)0, sector, STORAGE_GPT_HEADER));
}

/*
        A protective MBR, both headers and both entry arrays.

        The backup goes down first and the protective MBR last, so a write that
        stops part way leaves a disk no tool mistakes for a finished table.
*/
static bipolar storage_format_gpt(bipolar handle, p64 sectors, p32 sector_size,
                                  p8 address_to disk,
                                  storage_format_partition address_to parts,
                                  positive count)
{
        p64 first_usable;
        p64 last_usable;
        p64 table_sectors;
        positive table_bytes;
        p8 address_to table;
        p8 address_to sector;
        p32 entries_crc;
        bipolar failed;

        if (!storage_gpt_span(sectors, sector_size, address_of first_usable,
                              address_of last_usable) ||
            count > STORAGE_GPT_ENTRIES)
                return -ERROR_INVALID;

        for (positive at = 0; at < count; at++)
        {
                if (parts[at].first < first_usable ||
                    parts[at].last > last_usable ||
                    parts[at].first > parts[at].last)
                        return -ERROR_INVALID;

                for (positive other = 0; other < at; other++)
                        if (parts[at].first <= parts[other].last &&
                            parts[other].first <= parts[at].last)
                                return -ERROR_INVALID;
        }

        table_sectors = storage_gpt_table_sectors(sector_size);
        table_bytes = (positive)(table_sectors * sector_size);
        table = memory(table_bytes + sector_size);
        if (!table)
                return -ERROR_NO_MEMORY;

        sector = table + table_bytes;
        memory_zero(table, table_bytes + sector_size);

        for (positive at = 0; at < count; at++)
        {
                p8 address_to entry = table + at * STORAGE_GPT_ENTRY;
                string_address name = parts[at].name;

                memory_copy(entry, parts[at].type, 16);
                memory_copy(entry + 16, parts[at].unique, 16);
                storage_put64(entry + 32, parts[at].first);
                storage_put64(entry + 40, parts[at].last);

                for (positive letter = 0;
                     name && letter < STORAGE_GPT_NAME && name[letter]; letter++)
                        storage_put16(entry + 56 + 2 * letter, (p8)name[letter]);
        }

        entries_crc = ~hash_crc32(~(p32)0, table,
                                  STORAGE_GPT_ENTRIES * STORAGE_GPT_ENTRY);

        failed = storage_format_write(handle, table, table_bytes,
                                      (sectors - 1 - table_sectors) *
                                          sector_size);

        if (!failed)
        {
                storage_gpt_header(sector, sectors - 1, 1, first_usable,
                                   last_usable, disk,
                                   sectors - 1 - table_sectors, entries_crc);
                failed = storage_format_write(handle, sector, sector_size,
                                              (sectors - 1) * sector_size);
        }

        if (!failed)
                failed = storage_format_write(handle, table, table_bytes,
                                              2 * (p64)sector_size);

        if (!failed)
        {
                memory_zero(sector, sector_size);
                storage_gpt_header(sector, 1, sectors - 1, first_usable,
                                   last_usable, disk, 2, entries_crc);
                failed = storage_format_write(handle, sector, sector_size,
                                              sector_size);
        }

        if (!failed)
        {
                p8 address_to slot = sector + 446;

                memory_zero(sector, sector_size);
                slot[2] = 0x02;
                slot[4] = 0xee;
                slot[5] = slot[6] = slot[7] = 0xff;
                storage_put32(slot + 8, 1);
                storage_put32(slot + 12, sectors - 1 > 0xffffffff
                                             ? 0xffffffff
                                             : sectors - 1);
                sector[510] = 0x55;
                sector[511] = 0xaa;
                failed = storage_format_write(handle, sector, sector_size, 0);
        }

        memory_free(table, table_bytes + sector_size);
        return failed;
}

// FAT32 ---------------------------------------------------------

#define STORAGE_FAT_RESERVED 32
#define STORAGE_FAT_CLUSTER 4096
#define STORAGE_FAT32_FEWEST 65525
#define STORAGE_FAT32_MOST 0x0ffffff4

/*
        A FAT32 with nothing in it but its label, the shape firmware expects
        of an EFI system partition.

        Clusters are 4 KiB whatever the sector size. Each FAT is sized from an
        over-estimate of the clusters it has to describe, then the clusters
        are counted from what is left; the estimate only shrinks, so one
        recount is enough and the tables are never short.
*/
static bipolar storage_format_fat32(bipolar handle, p64 offset, p64 bytes,
                                    p32 sector_size, p64 hidden,
                                    storage_format_identity address_to identity)
{
        p64 total = bytes / sector_size;
        p32 per_cluster;
        p64 fat_sectors;
        p64 clusters;
        p64 data;
        p8 address_to sector;
        bipolar failed;

        if (sector_size < 512 || sector_size > STORAGE_FAT_CLUSTER ||
            (sector_size & (sector_size - 1)) || total > 0xffffffff ||
            total <= STORAGE_FAT_RESERVED)
                return -ERROR_INVALID;

        per_cluster = STORAGE_FAT_CLUSTER / sector_size;
        fat_sectors = (((total - STORAGE_FAT_RESERVED) / per_cluster + 2) * 4 +
                       sector_size - 1) /
                      sector_size;

        if (STORAGE_FAT_RESERVED + 2 * fat_sectors >= total)
                return -ERROR_INVALID;

        clusters = (total - STORAGE_FAT_RESERVED - 2 * fat_sectors) / per_cluster;
        if (clusters < STORAGE_FAT32_FEWEST || clusters > STORAGE_FAT32_MOST)
                return -ERROR_INVALID;

        data = STORAGE_FAT_RESERVED + 2 * fat_sectors;

        failed = storage_format_zero(handle, offset,
                                     (data + per_cluster) * sector_size);
        if (failed)
                return failed;

        sector = memory(sector_size);
        if (!sector)
                return -ERROR_NO_MEMORY;

        //      The boot sector, and its copy at sector 6.
        memory_zero(sector, sector_size);
        sector[0] = 0xeb;
        sector[1] = 0x58;
        sector[2] = 0x90;
        memory_copy(sector + 3, "MSWIN4.1", 8);
        storage_put16(sector + 11, sector_size);
        sector[13] = (p8)per_cluster;
        storage_put16(sector + 14, STORAGE_FAT_RESERVED);
        sector[16] = 2;
        sector[21] = 0xf8;
        storage_put16(sector + 24, 63);
        storage_put16(sector + 26, 255);
        storage_put32(sector + 28, hidden > 0xffffffff ? 0xffffffff : hidden);
        storage_put32(sector + 32, total);
        storage_put32(sector + 36, fat_sectors);
        storage_put32(sector + 44, 2);
        storage_put16(sector + 48, 1);
        storage_put16(sector + 50, 6);
        sector[64] = 0x80;
        sector[66] = 0x29;
        memory_copy(sector + 67, identity->uuid, 4);
        memory_fill(sector + 71, ' ', 11);
        for (positive at = 0; identity->label && at < 11 && identity->label[at];
             at++)
                sector[71 + at] = byte_to_upper(identity->label[at]);
        memory_copy(sector + 82, "FAT32   ", 8);
        //      A BIOS that jumps here anyway asks for the next boot device.
        sector[90] = 0xcd;
        sector[91] = 0x18;
        sector[510] = 0x55;
        sector[511] = 0xaa;

        failed = storage_format_write(handle, sector, sector_size, offset);
        if (!failed)
                failed = storage_format_write(handle, sector, sector_size,
                                              offset + 6 * (p64)sector_size);

        //      The label again, as the root directory's first entry.
        if (!failed)
        {
                p8 label[11];

                memory_copy(label, sector + 71, 11);
                memory_zero(sector, sector_size);
                memory_copy(sector, label, 11);
                sector[11] = 0x08;
                failed = storage_format_write(handle, sector, sector_size,
                                              offset + data * sector_size);
        }

        //      FSInfo, and its copy at sector 7.
        if (!failed)
        {
                memory_zero(sector, sector_size);
                storage_put32(sector, 0x41615252);
                storage_put32(sector + 484, 0x61417272);
                storage_put32(sector + 488, clusters - 1);
                storage_put32(sector + 492, 3);
                storage_put32(sector + 508, 0xaa550000);
                failed = storage_format_write(handle, sector, sector_size,
                                              offset + sector_size);
        }
        if (!failed)
                failed = storage_format_write(handle, sector, sector_size,
                                              offset + 7 * (p64)sector_size);

        //      Both FATs: the media byte, a clean volume, and the root's chain.
        if (!failed)
        {
                memory_zero(sector, sector_size);
                storage_put32(sector, 0x0ffffff8);
                storage_put32(sector + 4, 0x0fffffff);
                storage_put32(sector + 8, 0x0fffffff);
                failed = storage_format_write(
                    handle, sector, sector_size,
                    offset + STORAGE_FAT_RESERVED * (p64)sector_size);
        }
        if (!failed)
                failed = storage_format_write(
                    handle, sector, sector_size,
                    offset + (STORAGE_FAT_RESERVED + fat_sectors) * sector_size);

        memory_free(sector, sector_size);
        return failed;
}

// ext4 ----------------------------------------------------------

/*
        An ext4 as `mkfs.ext4 -b 4096 -I 256 -i 16384 -m 0 -O ^flex_bg,
        ^resize_inode,^64bit,^orphan_file,^metadata_csum_seed -E
        lazy_itable_init=1` lays it out: has_journal ext_attr dir_index
        filetype extent sparse_super large_file huge_file dir_nlink
        extra_isize metadata_csum.

        Without flex_bg every group keeps its own bitmaps and inode table at
        its start, so where anything is is arithmetic on the group number and
        nothing needs allocating. Without 64bit the descriptors are 32 bytes
        and the filesystem stops at 2^32 blocks, 16 TiB; the install caps the
        partition there. Every block bitmap is written, but only group 0's
        inode table is: the others are marked uninitialised, which is what
        lets a terabyte format in megabytes of writes, and the kernel's lazy
        init thread zeroes them after the first mount.
*/
#define STORAGE_EXT4_BLOCK 4096
#define STORAGE_EXT4_PER_GROUP 32768
#define STORAGE_EXT4_INODE 256
#define STORAGE_EXT4_INODES_PER_BLOCK (STORAGE_EXT4_BLOCK / STORAGE_EXT4_INODE)
#define STORAGE_EXT4_RATIO 16384
#define STORAGE_EXT4_DESCRIPTOR 32
#define STORAGE_EXT4_FIRST_INODE 11
#define STORAGE_EXT4_ROOT 2
#define STORAGE_EXT4_JOURNAL 8
#define STORAGE_EXT4_LOST 11
#define STORAGE_EXT4_LOST_BLOCKS 4
#define STORAGE_EXT4_JOURNAL_MOST 16384
#define STORAGE_EXT4_FEWEST ((p64)16384)
#define STORAGE_EXT4_MOST ((p64)0xffffffff)
#define STORAGE_EXT4_TAIL (STORAGE_EXT4_BLOCK - 12)

#define STORAGE_EXT4_INODE_UNINIT 0x0001
#define STORAGE_EXT4_ITABLE_ZEROED 0x0004
#define STORAGE_EXT4_EXTENTS_FLAG 0x80000

typedef struct
{
        p64 blocks;
        p32 groups;
        p32 inodes_per_group;
        p32 table_blocks;
        p32 descriptor_blocks;
        p32 journal_blocks;
        p32 journal_group;
        p64 journal_start;
        p64 root_block;
} storage_ext4_plan;

static bool storage_ext4_backup(p64 group)
{
        static const p8 bases[] = {3, 5, 7};

        if (group <= 1)
                return true;

        for (positive at = 0; at < array_count(bases); at++)
        {
                p64 power = bases[at];

                while (power < group)
                        power *= bases[at];

                if (power == group)
                        return true;
        }

        return false;
}

static p64 storage_ext4_group_blocks(storage_ext4_plan address_to plan,
                                     p64 group)
{
        return group + 1 < plan->groups ? STORAGE_EXT4_PER_GROUP
                                        : plan->blocks -
                                              group * STORAGE_EXT4_PER_GROUP;
}

//      Superblock and descriptor copies, where the group carries them.
static p64 storage_ext4_front(storage_ext4_plan address_to plan, p64 group)
{
        return storage_ext4_backup(group) ? 1 + plan->descriptor_blocks : 0;
}

static p64 storage_ext4_overhead(storage_ext4_plan address_to plan,
                                 p64 group)
{
        return storage_ext4_front(plan, group) + 2 + plan->table_blocks;
}

/*
        The numbers mke2fs arrives at: an inode per 16 KiB spread evenly and
        rounded up to whole table blocks, and a last group dropped when it
        could not hold its own metadata and fifty blocks besides. The journal
        follows e2fsprogs' size table up to 64 MiB and goes in the first group
        from the middle of the disk with room for it in one extent.
*/
static bool storage_ext4_layout(storage_ext4_plan address_to plan, p64 bytes)
{
        p64 blocks = bytes / STORAGE_EXT4_BLOCK;

        memory_zero(plan, sizeof(address_to plan));

        if (blocks < STORAGE_EXT4_FEWEST || blocks > STORAGE_EXT4_MOST)
                return false;

        for (;;)
        {
                p64 groups = (blocks + STORAGE_EXT4_PER_GROUP - 1) /
                             STORAGE_EXT4_PER_GROUP;
                p64 inodes = blocks * STORAGE_EXT4_BLOCK / STORAGE_EXT4_RATIO;
                p64 per_group = (inodes + groups - 1) / groups;
                p64 last;

                per_group = (per_group + STORAGE_EXT4_INODES_PER_BLOCK - 1) /
                            STORAGE_EXT4_INODES_PER_BLOCK *
                            STORAGE_EXT4_INODES_PER_BLOCK;
                if (per_group > STORAGE_EXT4_BLOCK * 8)
                        per_group = STORAGE_EXT4_BLOCK * 8;

                plan->blocks = blocks;
                plan->groups = (p32)groups;
                plan->inodes_per_group = (p32)per_group;
                plan->table_blocks = (p32)(per_group /
                                           STORAGE_EXT4_INODES_PER_BLOCK);
                plan->descriptor_blocks =
                    (p32)((groups * STORAGE_EXT4_DESCRIPTOR +
                           STORAGE_EXT4_BLOCK - 1) /
                          STORAGE_EXT4_BLOCK);

                last = storage_ext4_group_blocks(plan, groups - 1);
                if (groups > 1 &&
                    last < storage_ext4_overhead(plan, groups - 1) + 50)
                {
                        blocks -= last;
                        continue;
                }

                break;
        }

        plan->journal_blocks = plan->blocks < 32768           ? 1024
                               : plan->blocks < 256 * 1024     ? 4096
                               : plan->blocks < 512 * 1024     ? 8192
                                                               : STORAGE_EXT4_JOURNAL_MOST;

        plan->root_block = storage_ext4_overhead(plan, 0);

        for (p64 tried = 0; tried < plan->groups; tried++)
        {
                p64 group = (plan->blocks / 2 / STORAGE_EXT4_PER_GROUP + tried) %
                            plan->groups;
                p64 taken = storage_ext4_overhead(plan, group) +
                            (group ? 0 : 1 + STORAGE_EXT4_LOST_BLOCKS);

                if (storage_ext4_group_blocks(plan, group) >=
                    taken + plan->journal_blocks)
                {
                        plan->journal_group = (p32)group;
                        plan->journal_start = group * STORAGE_EXT4_PER_GROUP + taken;
                        return true;
                }
        }

        return false;
}

static p64 storage_ext4_used(storage_ext4_plan address_to plan, p64 group)
{
        return storage_ext4_overhead(plan, group) +
               (group ? 0 : 1 + STORAGE_EXT4_LOST_BLOCKS) +
               (group == plan->journal_group ? plan->journal_blocks : 0);
}

static fn storage_bits_set(p8 address_to bitmap, p64 from, p64 to)
{
        while (from < to && from % 8)
        {
                bitmap[from / 8] |= (p8)(1 << (from % 8));
                from++;
        }

        if (to - from >= 8)
        {
                memory_fill(bitmap + from / 8, (b8)0xff, (positive)((to - from) / 8));
                from += (to - from) / 8 * 8;
        }

        while (from < to)
        {
                bitmap[from / 8] |= (p8)(1 << (from % 8));
                from++;
        }
}

static p32 storage_ext4_inode_seed(p32 filesystem_seed, p32 number)
{
        p8 word[4];

        storage_put32(word, number);
        filesystem_seed = storage_crc32c(filesystem_seed, word, 4);
        storage_put32(word, 0);
        return storage_crc32c(filesystem_seed, word, 4);
}

/*
        One inode. Reserved ones are empty but still carry a checksum, the
        way mke2fs writes them; the bad-block inode alone also carries the
        format's time. A file gets its blocks as one extent held in the inode.
*/
static fn storage_ext4_inode(p8 address_to inode, p32 seed, p32 number,
                             p16 mode, p16 links, p64 size, p64 start,
                             p32 count, p32 time, bool extra)
{
        memory_zero(inode, STORAGE_EXT4_INODE);
        storage_put16(inode, mode);
        storage_put32(inode + 0x04, size);
        storage_put32(inode + 0x08, time);
        storage_put32(inode + 0x0c, time);
        storage_put32(inode + 0x10, time);
        storage_put16(inode + 0x1a, links);
        storage_put32(inode + 0x1c, (p64)count * (STORAGE_EXT4_BLOCK / 512));
        storage_put32(inode + 0x6c, size >> 32);

        if (count)
        {
                storage_put32(inode + 0x20, STORAGE_EXT4_EXTENTS_FLAG);
                storage_put16(inode + 0x28, 0xf30a);
                storage_put16(inode + 0x2a, 1);
                storage_put16(inode + 0x2c, 4);
                storage_put16(inode + 0x38, count);
                storage_put16(inode + 0x3a, start >> 32);
                storage_put32(inode + 0x3c, start);
        }

        if (extra)
        {
                storage_put16(inode + 0x80, 32);
                storage_put32(inode + 0x90, time);
        }

        seed = storage_crc32c(storage_ext4_inode_seed(seed, number), inode,
                              STORAGE_EXT4_INODE);
        storage_put16(inode + 0x7c, seed);
        if (extra)
                storage_put16(inode + 0x82, seed >> 16);
}

static fn storage_ext4_entry(p8 address_to at, p32 inode, p64 length,
                             string_address name)
{
        positive name_length = string_length(name);

        storage_put32(at, inode);
        storage_put16(at + 4, length);
        at[6] = (p8)name_length;
        at[7] = inode ? 2 : 0;
        memory_copy(at + 8, name, name_length);
}

static fn storage_ext4_tail(p8 address_to block, p32 seed, p32 inode)
{
        p8 address_to tail = block + STORAGE_EXT4_TAIL;

        memory_zero(tail, 12);
        storage_put16(tail + 4, 12);
        tail[7] = 0xde;
        storage_put32(tail + 8,
                      storage_crc32c(storage_ext4_inode_seed(seed, inode),
                                     block, STORAGE_EXT4_TAIL));
}

static fn storage_ext4_super(p8 address_to super,
                             storage_ext4_plan address_to plan,
                             storage_format_identity address_to identity,
                             p8 address_to journal_map, p64 free_blocks,
                             p32 group)
{
        p64 inodes = (p64)plan->groups * plan->inodes_per_group;
        p64 journal_bytes = (p64)plan->journal_blocks * STORAGE_EXT4_BLOCK;

        memory_zero(super, 1024);
        storage_put32(super + 0x00, inodes);
        storage_put32(super + 0x04, plan->blocks);
        storage_put32(super + 0x0c, free_blocks);
        storage_put32(super + 0x10, inodes - STORAGE_EXT4_FIRST_INODE);
        storage_put32(super + 0x18, 2);
        storage_put32(super + 0x1c, 2);
        storage_put32(super + 0x20, STORAGE_EXT4_PER_GROUP);
        storage_put32(super + 0x24, STORAGE_EXT4_PER_GROUP);
        storage_put32(super + 0x28, plan->inodes_per_group);
        storage_put32(super + 0x30, identity->time);
        storage_put16(super + 0x36, 0xffff);
        storage_put16(super + 0x38, 0xef53);
        //      A copy is not known to be clean: a check that falls back to
        //      one should look.
        storage_put16(super + 0x3a, group ? 0 : 1);
        storage_put16(super + 0x3c, 1);
        storage_put32(super + 0x40, identity->time);
        storage_put32(super + 0x4c, 1);
        storage_put32(super + 0x54, STORAGE_EXT4_FIRST_INODE);
        storage_put16(super + 0x58, STORAGE_EXT4_INODE);
        storage_put16(super + 0x5a, group);
        storage_put32(super + 0x5c, 0x0004 | 0x0008 | 0x0020);
        storage_put32(super + 0x60, 0x0002 | 0x0040);
        storage_put32(super + 0x64,
                      0x0001 | 0x0002 | 0x0008 | 0x0020 | 0x0040 | 0x0400);
        memory_copy(super + 0x68, identity->uuid, 16);
        for (positive at = 0; identity->label && at < 16 && identity->label[at];
             at++)
                super[0x78 + at] = identity->label[at];
        storage_put32(super + 0xe0, STORAGE_EXT4_JOURNAL);
        memory_copy(super + 0xec, identity->hash_seed, 16);
        super[0xfc] = 1;
        super[0xfd] = 1;
        storage_put32(super + 0x100, 0x0004 | 0x0008);
        storage_put32(super + 0x108, identity->time);
        memory_copy(super + 0x10c, journal_map, 60);
        storage_put32(super + 0x148, journal_bytes >> 32);
        storage_put32(super + 0x14c, journal_bytes);
        storage_put16(super + 0x15c, 32);
        storage_put16(super + 0x15e, 32);
        storage_put32(super + 0x160, 1);
        super[0x175] = 1;
        storage_put32(super + 0x3fc, storage_crc32c(~(p32)0, super, 0x3fc));
}

static bipolar storage_format_ext4(bipolar handle, p64 offset, p64 bytes,
                                   storage_format_identity address_to identity)
{
        storage_ext4_plan plan;
        positive descriptor_bytes;
        p8 address_to descriptors;
        p8 address_to block;
        p32 seed;
        p64 free_blocks = 0;
        bipolar failed = 0;

        if (!storage_ext4_layout(address_of plan, bytes))
                return -ERROR_INVALID;

        seed = storage_crc32c(~(p32)0, identity->uuid, 16);
        descriptor_bytes = (positive)plan.descriptor_blocks * STORAGE_EXT4_BLOCK;
        descriptors = memory(descriptor_bytes + STORAGE_EXT4_BLOCK);
        if (!descriptors)
                return -ERROR_NO_MEMORY;

        block = descriptors + descriptor_bytes;
        memory_zero(descriptors, descriptor_bytes + STORAGE_EXT4_BLOCK);

        //      Nothing of an earlier filesystem left where a probe looks first.
        failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK, offset);

        //      A block bitmap for every group, and every group's descriptor.
        for (p64 group = 0; group < plan.groups && !failed; group++)
        {
                p8 address_to descriptor = descriptors +
                                           group * STORAGE_EXT4_DESCRIPTOR;
                p64 start = group * STORAGE_EXT4_PER_GROUP;
                p64 bitmap = start + storage_ext4_front(address_of plan, group);
                p64 group_blocks = storage_ext4_group_blocks(address_of plan, group);
                p64 used = storage_ext4_used(address_of plan, group);
                p64 free_inodes = plan.inodes_per_group -
                                  (group ? 0 : STORAGE_EXT4_FIRST_INODE);

                memory_zero(block, STORAGE_EXT4_BLOCK);
                storage_bits_set(block, 0, storage_ext4_overhead(address_of plan, group));
                if (!group)
                        storage_bits_set(block, plan.root_block,
                                         plan.root_block + 1 +
                                             STORAGE_EXT4_LOST_BLOCKS);
                if (group == plan.journal_group)
                        storage_bits_set(block, plan.journal_start - start,
                                         plan.journal_start - start +
                                             plan.journal_blocks);
                storage_bits_set(block, group_blocks, STORAGE_EXT4_BLOCK * 8);

                storage_put32(descriptor + 0x00, bitmap);
                storage_put32(descriptor + 0x04, bitmap + 1);
                storage_put32(descriptor + 0x08, bitmap + 2);
                storage_put16(descriptor + 0x0c, group_blocks - used);
                storage_put16(descriptor + 0x0e, free_inodes);
                storage_put16(descriptor + 0x10, group ? 0 : 2);
                storage_put16(descriptor + 0x12, group ? STORAGE_EXT4_INODE_UNINIT
                                                       : STORAGE_EXT4_ITABLE_ZEROED);
                storage_put16(descriptor + 0x18,
                              storage_crc32c(seed, block, STORAGE_EXT4_BLOCK));
                storage_put16(descriptor + 0x1c, free_inodes);
                free_blocks += group_blocks - used;

                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              offset + bitmap * STORAGE_EXT4_BLOCK);
        }

        //      Group 0's inode bitmap: the reserved inodes and lost+found.
        if (!failed)
        {
                memory_zero(block, STORAGE_EXT4_BLOCK);
                storage_bits_set(block, 0, STORAGE_EXT4_FIRST_INODE);
                storage_bits_set(block, plan.inodes_per_group,
                                 STORAGE_EXT4_BLOCK * 8);
                storage_put16(descriptors + 0x1a,
                              storage_crc32c(seed, block,
                                             plan.inodes_per_group / 8));
                failed = storage_format_write(
                    handle, block, STORAGE_EXT4_BLOCK,
                    offset + (storage_ext4_front(address_of plan, 0) + 1) *
                                 STORAGE_EXT4_BLOCK);
        }

        for (p64 group = 0; group < plan.groups; group++)
        {
                p8 address_to descriptor = descriptors +
                                           group * STORAGE_EXT4_DESCRIPTOR;
                p8 word[4];
                p32 sum;

                storage_put32(word, group);
                sum = storage_crc32c(seed, word, 4);
                sum = storage_crc32c(sum, descriptor, STORAGE_EXT4_DESCRIPTOR);
                storage_put16(descriptor + 0x1e, sum);
        }

        //      Group 0's inode table, zeroed whole, then its first block.
        if (!failed)
                failed = storage_format_zero(
                    handle,
                    offset + (storage_ext4_front(address_of plan, 0) + 2) *
                                 STORAGE_EXT4_BLOCK,
                    (p64)plan.table_blocks * STORAGE_EXT4_BLOCK);

        p8 journal_map[60];

        if (!failed)
        {
                p64 table = offset + (storage_ext4_front(address_of plan, 0) + 2) *
                                         STORAGE_EXT4_BLOCK;

                memory_zero(block, STORAGE_EXT4_BLOCK);

                for (p32 number = 1; number < STORAGE_EXT4_FIRST_INODE; number++)
                        storage_ext4_inode(block + (number - 1) * STORAGE_EXT4_INODE,
                                           seed, number, 0, 0, 0, 0, 0,
                                           number == 1 ? identity->time : 0,
                                           false);

                storage_ext4_inode(block + (STORAGE_EXT4_ROOT - 1) * STORAGE_EXT4_INODE,
                                   seed, STORAGE_EXT4_ROOT, 040755, 3,
                                   STORAGE_EXT4_BLOCK, plan.root_block, 1,
                                   identity->time, true);
                storage_ext4_inode(block + (STORAGE_EXT4_JOURNAL - 1) * STORAGE_EXT4_INODE,
                                   seed, STORAGE_EXT4_JOURNAL, 0100600, 1,
                                   (p64)plan.journal_blocks * STORAGE_EXT4_BLOCK,
                                   plan.journal_start, plan.journal_blocks,
                                   identity->time, true);
                storage_ext4_inode(block + (STORAGE_EXT4_LOST - 1) * STORAGE_EXT4_INODE,
                                   seed, STORAGE_EXT4_LOST, 040700, 2,
                                   STORAGE_EXT4_LOST_BLOCKS * STORAGE_EXT4_BLOCK,
                                   plan.root_block + 1, STORAGE_EXT4_LOST_BLOCKS,
                                   identity->time, true);

                memory_copy(journal_map,
                            block + (STORAGE_EXT4_JOURNAL - 1) * STORAGE_EXT4_INODE + 0x28,
                            60);

                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              table);
        }

        //      The root directory, then lost+found's four blocks.
        if (!failed)
        {
                memory_zero(block, STORAGE_EXT4_BLOCK);
                storage_ext4_entry(block, STORAGE_EXT4_ROOT, 12, ".");
                storage_ext4_entry(block + 12, STORAGE_EXT4_ROOT, 12, "..");
                storage_ext4_entry(block + 24, STORAGE_EXT4_LOST,
                                   STORAGE_EXT4_TAIL - 24, "lost+found");
                storage_ext4_tail(block, seed, STORAGE_EXT4_ROOT);
                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              offset + plan.root_block *
                                                           STORAGE_EXT4_BLOCK);
        }

        for (p64 at = 0; at < STORAGE_EXT4_LOST_BLOCKS && !failed; at++)
        {
                memory_zero(block, STORAGE_EXT4_BLOCK);
                if (!at)
                {
                        storage_ext4_entry(block, STORAGE_EXT4_LOST, 12, ".");
                        storage_ext4_entry(block + 12, STORAGE_EXT4_ROOT,
                                           STORAGE_EXT4_TAIL - 12, "..");
                }
                else
                        storage_ext4_entry(block, 0, STORAGE_EXT4_TAIL, "");
                storage_ext4_tail(block, seed, STORAGE_EXT4_LOST);
                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              offset + (plan.root_block + 1 + at) *
                                                           STORAGE_EXT4_BLOCK);
        }

        //      The journal: zeroed, so nothing left there can replay, and clean.
        if (!failed)
                failed = storage_format_zero(handle,
                                             offset + plan.journal_start *
                                                          STORAGE_EXT4_BLOCK,
                                             (p64)plan.journal_blocks *
                                                 STORAGE_EXT4_BLOCK);
        if (!failed)
        {
                memory_zero(block, STORAGE_EXT4_BLOCK);
                storage_put32_be(block + 0x00, 0xc03b3998);
                storage_put32_be(block + 0x04, 4);
                storage_put32_be(block + 0x0c, STORAGE_EXT4_BLOCK);
                storage_put32_be(block + 0x10, plan.journal_blocks);
                storage_put32_be(block + 0x14, 1);
                storage_put32_be(block + 0x18, 1);
                memory_copy(block + 0x30, identity->uuid, 16);
                storage_put32_be(block + 0x40, 1);
                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              offset + plan.journal_start *
                                                           STORAGE_EXT4_BLOCK);
        }

        /*
                Descriptors and superblocks last, backups before the primary:
                until the primary lands there is no filesystem here for
                anything to mount half made.
        */
        for (p64 group = plan.groups; group-- > 0 && !failed;)
        {
                p64 start = group * STORAGE_EXT4_PER_GROUP;

                if (!storage_ext4_backup(group))
                        continue;

                failed = storage_format_write(handle, descriptors, descriptor_bytes,
                                              offset + (start + 1) *
                                                           STORAGE_EXT4_BLOCK);
                if (failed)
                        break;

                memory_zero(block, STORAGE_EXT4_BLOCK);
                storage_ext4_super(block + (group ? 0 : 1024), address_of plan,
                                   identity, journal_map, free_blocks, (p32)group);
                failed = storage_format_write(handle, block, STORAGE_EXT4_BLOCK,
                                              offset + start * STORAGE_EXT4_BLOCK);
        }

        memory_free(descriptors, descriptor_bytes + STORAGE_EXT4_BLOCK);
        return failed;
}
