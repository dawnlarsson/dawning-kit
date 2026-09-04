/*
        Block identity probing shared by blkid, findfs and mount.

        This file deliberately owns no command dispatch.  A shell builtin and
        a multicall name both hand the same argc/argv to the runners at the
        bottom; mount can bypass their presentation entirely and use
        storage_probe_device or storage_resolve_tag.

        Probing is bounded.  The common first 4 KiB, the Btrfs superblock and
        the ISO volume descriptor are the only unconditional reads.  exFAT's
        root directory costs one further sector, and only after its geometry
        has passed overflow and range checks.  A short read never leaves old
        bytes behind and no recogniser examines beyond the returned count.
*/

#define STORAGE_PATH_ROOM 4096
#define STORAGE_TYPE_ROOM 16
#define STORAGE_UUID_ROOM 40
#define STORAGE_LABEL_ROOM 257
#define STORAGE_PARTUUID_ROOM 40
#define STORAGE_PARTLABEL_ROOM 257
#define STORAGE_PROBE_ROOM 4096
#define STORAGE_DEVICE_BLOCK 4096

typedef struct
{
        string_address path;
        p8 type[STORAGE_TYPE_ROOM];
        p8 uuid[STORAGE_UUID_ROOM];
        p8 label[STORAGE_LABEL_ROOM];
        p8 partuuid[STORAGE_PARTUUID_ROOM];
        p8 partlabel[STORAGE_PARTLABEL_ROOM];
        positive type_length;
        positive uuid_length;
        positive label_length;
        positive partuuid_length;
        positive partlabel_length;
} storage_identity;

/* A recogniser may expose its exact magic location without teaching the
   presentation commands how to recognise a filesystem a second time. */
typedef struct
{
        storage_identity identity;
        p64 offset;
        p8 length;
        p8 magic[10];
        string_address usage;
} storage_signature;

typedef bool (*storage_signature_visitor)(
    storage_signature address_to signature, address_any context);

typedef struct
{
        p32 sectors;
        p16 sector_size;
} storage_iso_volume;

typedef struct
{
        string_address tag;
        positive tag_length;
        string_address value;
} storage_selector;

#define STORAGE_SHOW_LABEL     1
#define STORAGE_SHOW_UUID      2
#define STORAGE_SHOW_TYPE      4
#define STORAGE_SHOW_PARTLABEL 8
#define STORAGE_SHOW_PARTUUID  16
#define STORAGE_SHOW_DEVNAME   32

/* One tag schema drives selection and blkid's output mask.  DEVNAME is the
   sole direct pointer; the others name an inline byte array and its length. */
typedef struct
{
        string_address name;
        p8 length;
        p8 show;
        p16 value_offset;
        p16 length_offset;
} storage_tag_descriptor;

#define STORAGE_TAG(name, member, length_member, flag)                      \
        {(string_address)#name, sizeof(#name) - 1, STORAGE_SHOW_##flag,      \
         __builtin_offsetof(storage_identity, member),                      \
         __builtin_offsetof(storage_identity, length_member)}
static const storage_tag_descriptor storage_tags[] = {
    STORAGE_TAG(LABEL, label, label_length, LABEL),
    STORAGE_TAG(UUID, uuid, uuid_length, UUID),
    STORAGE_TAG(TYPE, type, type_length, TYPE),
    STORAGE_TAG(PARTLABEL, partlabel, partlabel_length, PARTLABEL),
    STORAGE_TAG(PARTUUID, partuuid, partuuid_length, PARTUUID),
    {(string_address)"DEVNAME", 7, STORAGE_SHOW_DEVNAME, 0, 0},
};
#undef STORAGE_TAG

#define storage_le16(at) memory_load_unaligned(p16, (at))
#define storage_le32(at) memory_load_unaligned(p32, (at))
#define storage_le64(at) memory_load_unaligned(p64, (at))

#define storage_be16 network_load_16
#define storage_be32 network_load_32

static p64 storage_be64(p8 address_to at)
{
        return ((p64)network_load_32(at) << 32) |
               network_load_32(at + 4);
}

static bool storage_bytes(p8 address_to bytes, positive have,
                          positive at, p8 address_to wanted,
                          positive length)
{
        return at <= have && length <= have - at &&
               !memory_compare(bytes + at, wanted, length);
}

/* pread64 is one 64-bit argument on every architecture Moonwater supports. */
static positive storage_read(bipolar handle, p8 address_to bytes,
                             positive room, p64 offset)
{
        positive used = 0;

        memory_zero(bytes, room);
        while (used < room)
        {
                bipolar got = system_call_4(syscall(pread64), (positive)handle,
                                            (positive)(bytes + used),
                                            room - used,
                                            (positive)(offset + used));

                if (got == -4) /* EINTR */
                        continue;
                if (got <= 0)
                        break;
                used += (positive)got;
        }

        return used;
}

static bipolar storage_write(bipolar handle, p8 address_to bytes,
                             positive length, p64 offset)
{
        positive used = 0;

        while (used < length)
        {
                bipolar wrote = system_call_4(
                    syscall(pwrite64), (positive)handle,
                    (positive)(bytes + used), length - used,
                    (positive)(offset + used));

                if (wrote == -4) /* EINTR */
                        continue;
                if (wrote <= 0)
                        return wrote ? wrote : -5; /* EIO */
                used += (positive)wrote;
        }
        return (bipolar)used;
}

static fn storage_text(p8 address_to into, positive room,
                       positive address_to length, string_address text)
{
        string_address end_at;

        if (!room)
        {
                address_to length = 0;
                return;
        }

        end_at = string_copy_max_end(into, text, room - 1);
        address_to length = (positive)(end_at - into);
}

static fn storage_trimmed(p8 address_to into, positive room,
                          positive address_to length,
                          p8 address_to source, positive size,
                          bool trim_space)
{
        p8 address_to terminator = (p8 address_to)memory_first_of(source, 0, size);
        positive used = terminator ? (positive)(terminator - source) : size;

        while (used && (source[used - 1] == 0 ||
                        (trim_space && source[used - 1] == ' ')))
                used--;

        if (room)
        {
                positive copied = used < room - 1 ? used : room - 1;

                memory_copy(into, source, copied);
                into[copied] = end;
                address_to length = copied;
        }
        else
                address_to length = 0;
}

static p8 storage_hex_digit(p8 value, bool upper)
{
        return value < 10 ? (p8)('0' + value)
                          : (p8)((upper ? 'A' : 'a') + value - 10);
}

static fn storage_uuid_bytes(p8 address_to into,
                             p8 address_to bytes)
{
        positive out = 0;

        for (positive at = 0; at < 16; at++)
        {
                if (at == 4 || at == 6 || at == 8 || at == 10)
                        into[out++] = '-';

                into[out++] = storage_hex_digit(bytes[at] >> 4, false);
                into[out++] = storage_hex_digit(bytes[at] & 15, false);
        }

        into[out] = end;
}

static fn storage_hex_padded(p8 address_to into, positive value,
                             positive width, bool upper)
{
        p8 digits[2 * sizeof(positive)];
        positive count = positive_into_base(digits, value, 16, upper);
        positive padding = width > count ? width - count : 0;

        memory_fill(into, '0', padding);
        memory_copy(into + padding, digits, count);
        into[padding + count] = end;
}

/* Fourteen recognisers name their type and six copy a sixteen-byte UUID;
   each spelled the three-argument store and the length by hand. */
static fn storage_set_type(storage_identity address_to identity,
                           string_address type)
{
        storage_text(identity->type, sizeof(identity->type),
                     address_of identity->type_length, type);
}

static fn storage_set_uuid(storage_identity address_to identity,
                           p8 address_to bytes)
{
        storage_uuid_bytes(identity->uuid, bytes);
        identity->uuid_length = 36;
}

static fn storage_uuid_fat(p8 address_to into, p32 serial)
{
        storage_hex_padded(into, (positive)(serial >> 16), 4, true);
        into[4] = '-';
        storage_hex_padded(into + 5, (positive)(serial & 0xffff), 4, true);
}

static bool storage_power_of_two(positive value)
{
        return value && !(value & (value - 1));
}

static bool storage_log_matches(positive value, positive logarithm)
{
        return logarithm < positive_bits &&
               value == ((positive)1 << logarithm);
}

static bool storage_uuid_present(p8 address_to uuid)
{
        return memory_count(uuid, 16, 0) != 16;
}

static bool storage_header_word(p8 address_to word, positive room)
{
        positive at = 0;

        while (at < room && word[at])
        {
                if (word[at] < 0x21 || word[at] > 0x7e)
                        return false;
                at++;
        }

        return at && at < room;
}

static bool storage_uuid_header(p8 address_to uuid, positive room)
{
        if (room < 37 || uuid[36])
                return false;

        for (positive at = 0; at < 36; at++)
        {
                p8 byte = uuid[at];

                if (at == 8 || at == 13 || at == 18 || at == 23)
                {
                        if (byte != '-')
                                return false;
                }
                else if (digit_known(byte, 16) >= 16)
                        return false;
        }

        return true;
}

static bool storage_utf8_add(p8 address_to into, positive room,
                             positive address_to out, positive codepoint)
{
        positive need = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2
                                       : codepoint < 0x10000 ? 3 : 4;

        if (codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            !room || *out >= room || need > room - 1 - *out)
                return false;

        if (need == 1)
                into[(*out)++] = (p8)codepoint;
        else if (need == 2)
        {
                into[(*out)++] = (p8)(0xc0 | (codepoint >> 6));
                into[(*out)++] = (p8)(0x80 | (codepoint & 0x3f));
        }
        else if (need == 3)
        {
                into[(*out)++] = (p8)(0xe0 | (codepoint >> 12));
                into[(*out)++] = (p8)(0x80 | ((codepoint >> 6) & 0x3f));
                into[(*out)++] = (p8)(0x80 | (codepoint & 0x3f));
        }
        else
        {
                into[(*out)++] = (p8)(0xf0 | (codepoint >> 18));
                into[(*out)++] = (p8)(0x80 | ((codepoint >> 12) & 0x3f));
                into[(*out)++] = (p8)(0x80 | ((codepoint >> 6) & 0x3f));
                into[(*out)++] = (p8)(0x80 | (codepoint & 0x3f));
        }

        return true;
}

static fn storage_utf16_label(p8 address_to into, positive room,
                              positive address_to length,
                              p8 address_to source, positive characters)
{
        positive out = 0;

        for (positive at = 0; at < characters; at++)
        {
                p16 character = storage_le16(source + at * 2);
                positive codepoint = character;

                if (!character)
                        continue;
                if (character >= 0xd800 && character <= 0xdbff)
                {
                        p16 low;

                        if (at + 1 >= characters)
                                continue;
                        low = storage_le16(source + (at + 1) * 2);
                        if (low < 0xdc00 || low > 0xdfff)
                                continue;
                        codepoint = 0x10000 +
                                    (((positive)character - 0xd800) << 10) +
                                    ((positive)low - 0xdc00);
                        at++;
                }
                else if (character >= 0xdc00 && character <= 0xdfff)
                        continue;

                if (!storage_utf8_add(into, room, address_of out, codepoint))
                        break;
        }

        if (room)
                into[out] = end;
        address_to length = out;
}

static fn storage_probe_ext(storage_identity address_to identity,
                            p8 address_to bytes, positive have)
{
        p32 compatible;
        p32 incompatible;

        if (have < 1024 + 256 || storage_le16(bytes + 1024 + 56) != 0xef53)
                return;

        compatible = storage_le32(bytes + 1024 + 92);
        incompatible = storage_le32(bytes + 1024 + 96);

        if (incompatible & (0x40 | 0x80 | 0x200))
                storage_set_type(identity, (string_address)"ext4");
        else if (compatible & 0x4)
                storage_set_type(identity, (string_address)"ext3");
        else
                storage_set_type(identity, (string_address)"ext2");

        storage_set_uuid(identity, bytes + 1024 + 104);
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + 1024 + 120, 16, false);
}

static fn storage_probe_fat(storage_identity address_to identity,
                            p8 address_to bytes, positive have)
{
        positive sector;
        bool fat32;
        positive type_at;
        positive serial_at;
        positive label_at;

        if (have < 512 || bytes[510] != 0x55 || bytes[511] != 0xaa)
                return;

        sector = storage_le16(bytes + 11);
        if (!storage_power_of_two(sector) || sector < 512 || sector > 4096 ||
            !storage_power_of_two(bytes[13]))
                return;

        fat32 = storage_bytes(bytes, have, 82, (p8 address_to)"FAT32   ", 8);
        type_at = fat32 ? 82 : 54;

        if (!fat32 && !storage_bytes(bytes, have, type_at,
                                     (p8 address_to)"FAT", 3))
                return;

        serial_at = fat32 ? 67 : 39;
        label_at = fat32 ? 71 : 43;
        storage_set_type(identity, (string_address)"vfat");
        storage_uuid_fat(identity->uuid, storage_le32(bytes + serial_at));
        identity->uuid_length = 9;
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + label_at, 11, true);

        if (string_equals(identity->label, "NO NAME"))
        {
                identity->label[0] = end;
                identity->label_length = 0;
        }
}

static fn storage_probe_exfat_label(bipolar handle,
                                    storage_identity address_to identity,
                                    p8 address_to boot, positive have)
{
        p8 sector[STORAGE_PROBE_ROOM];
        positive sector_shift;
        positive cluster_shift;
        p64 heap;
        p32 root;
        p64 sector_number;
        p64 offset;
        positive got;

        if (have < 120)
                return;

        sector_shift = boot[108];
        cluster_shift = boot[109];
        heap = storage_le32(boot + 88);
        root = storage_le32(boot + 96);

        if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 ||
            root < 2 || (p64)(root - 2) > ((p64)-1 >> cluster_shift))
                return;

        sector_number = heap + ((p64)(root - 2) << cluster_shift);
        if (sector_number < heap || sector_number > ((p64)bipolar_max >> sector_shift))
                return;

        offset = sector_number << sector_shift;
        got = storage_read(handle, sector, (positive)1 << sector_shift, offset);

        for (positive at = 0; at <= got && 32 <= got - at; at += 32)
        {
                positive letters;

                if (sector[at] == 0)
                        break;
                if (sector[at] != 0x83)
                        continue;

                letters = sector[at + 1];
                if (letters > 15)
                        letters = 15;
                storage_utf16_label(identity->label, sizeof(identity->label),
                                    address_of identity->label_length,
                                    sector + at + 2, letters);
                break;
        }
}

static fn storage_probe_exfat(bipolar handle,
                              storage_identity address_to identity,
                              p8 address_to bytes, positive have)
{
        p32 fat_at;
        p32 fat_length;
        p32 heap_at;
        p32 clusters;
        p32 root;
        positive fats;
        positive sector_shift;
        positive cluster_shift;

        if (have < 512 || !storage_bytes(bytes, have, 0,
                                         (p8 address_to)"\xeb\x76\x90", 3) ||
            !storage_bytes(bytes, have, 3,
                           (p8 address_to)"EXFAT   ", 8) ||
            memory_count(bytes + 11, 53, 0) != 53 ||
            storage_le16(bytes + 510) != 0xaa55)
                return;

        fat_at = storage_le32(bytes + 80);
        fat_length = storage_le32(bytes + 84);
        heap_at = storage_le32(bytes + 88);
        clusters = storage_le32(bytes + 92);
        root = storage_le32(bytes + 96);
        sector_shift = bytes[108];
        cluster_shift = bytes[109];
        fats = bytes[110];

        if (sector_shift < 9 || sector_shift > 12 ||
            cluster_shift > 25 - sector_shift || fats < 1 || fats > 2 ||
            fat_at < 24 || !fat_length || !clusters || root < 2 ||
            (p64)root > (p64)clusters + 1 ||
            (p64)fat_at + (p64)fat_length * fats > heap_at)
                return;

        storage_set_type(identity, (string_address)"exfat");
        storage_uuid_fat(identity->uuid, storage_le32(bytes + 100));
        identity->uuid_length = 9;
        storage_probe_exfat_label(handle, identity, bytes, have);
}

static fn storage_probe_ntfs(storage_identity address_to identity,
                             p8 address_to bytes, positive have)
{
        positive sector_size;
        positive sectors_per_cluster;
        p64 sectors;
        p64 mft_cluster;

        if (have < 512 || !storage_bytes(bytes, have, 3,
                                         (p8 address_to)"NTFS    ", 8) ||
            (bytes[0] != 0xeb && bytes[0] != 0xe9) ||
            storage_le16(bytes + 510) != 0xaa55)
                return;

        sector_size = storage_le16(bytes + 11);
        sectors_per_cluster = bytes[13];
        sectors = storage_le64(bytes + 40);
        mft_cluster = storage_le64(bytes + 48);
        if (!storage_power_of_two(sector_size) || sector_size < 256 ||
            sector_size > STORAGE_PROBE_ROOM ||
            !storage_power_of_two(sectors_per_cluster) ||
            sectors_per_cluster > 128 ||
            !sectors || mft_cluster >= sectors / sectors_per_cluster)
                return;

        storage_set_type(identity, (string_address)"ntfs");
        storage_hex_padded(identity->uuid, (positive)storage_le64(bytes + 72),
                           16, true);
        identity->uuid_length = 16;
}

static fn storage_probe_ntfs_label(bipolar handle,
                                   storage_identity address_to identity,
                                   p8 address_to boot, positive have)
{
        p8 record[STORAGE_PROBE_ROOM];
        positive sector_size;
        positive sectors_per_cluster;
        positive cluster_size;
        p8 record_code;
        positive record_size;
        p64 mft_cluster;
        p64 mft_offset;
        p64 volume_offset;
        positive got;
        positive usa_at;
        positive usa_count;
        p16 usa_value;
        positive attribute_at;
        positive used;

        if (have < 80)
                return;

        sector_size = storage_le16(boot + 11);
        sectors_per_cluster = boot[13];
        if (!storage_power_of_two(sector_size) || sector_size < 256 ||
            sector_size > STORAGE_PROBE_ROOM ||
            !storage_power_of_two(sectors_per_cluster) ||
            sectors_per_cluster > STORAGE_PROBE_ROOM / sector_size)
                return;

        cluster_size = sector_size * sectors_per_cluster;
        record_code = boot[64];
        if (record_code & 0x80)
        {
                positive shift = 256 - record_code;

                if (shift > 12)
                        return;
                record_size = (positive)1 << shift;
        }
        else
        {
                if (!record_code || record_code > STORAGE_PROBE_ROOM / cluster_size)
                        return;
                record_size = cluster_size * record_code;
        }

        if (record_size < 128 || record_size > sizeof(record))
                return;

        mft_cluster = storage_le64(boot + 48);
        if (mft_cluster > (p64)bipolar_max / cluster_size)
                return;
        mft_offset = mft_cluster * cluster_size;
        if (mft_offset > (p64)bipolar_max - 3 * record_size)
                return;
        volume_offset = mft_offset + 3 * record_size;
        got = storage_read(handle, record, record_size, volume_offset);

        if (got < 32 || !storage_bytes(record, got, 0,
                                       (p8 address_to)"FILE", 4))
                return;

        usa_at = storage_le16(record + 4);
        usa_count = storage_le16(record + 6);
        if (usa_count < 2 || usa_count > record_size / sector_size + 1 ||
            usa_at > got || usa_count * 2 > got - usa_at)
                return;

        usa_value = storage_le16(record + usa_at);
        for (positive which = 1; which < usa_count; which++)
        {
                positive end_at = which * sector_size - 2;

                if (end_at + 2 > got || storage_le16(record + end_at) != usa_value)
                        return;
                memory_copy(record + end_at, record + usa_at + which * 2, 2);
        }

        attribute_at = storage_le16(record + 20);
        used = storage_le32(record + 24);
        if (used > got)
                used = got;

        while (attribute_at <= used && 24 <= used - attribute_at)
        {
                p32 type = storage_le32(record + attribute_at);
                positive length = storage_le32(record + attribute_at + 4);

                if (type == 0xffffffff)
                        break;
                if (length < 24 || length > used - attribute_at)
                        break;

                if (type == 0x60 && !record[attribute_at + 8])
                {
                        positive value_length =
                            storage_le32(record + attribute_at + 16);
                        positive value_at =
                            storage_le16(record + attribute_at + 20);

                        if (value_at <= length && value_length <= length - value_at)
                                storage_utf16_label(
                                    identity->label, sizeof(identity->label),
                                    address_of identity->label_length,
                                    record + attribute_at + value_at,
                                    value_length / 2);
                        return;
                }

                attribute_at += length;
        }
}

static fn storage_probe_erofs(storage_identity address_to identity,
                              p8 address_to bytes, positive have)
{
        if (have < 1024 + 80 || storage_le32(bytes + 1024) != 0xe0f5e1e2)
                return;

        storage_set_type(identity, (string_address)"erofs");
        storage_set_uuid(identity, bytes + 1024 + 48);
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + 1024 + 64, 16, false);
}

/* XFS stores its primary superblock in big endian at byte zero.  The geometry
   cross-checks are intentional: XFSB alone is a four-byte file signature, not
   enough authority to choose a filesystem driver for mount. */
static fn storage_probe_xfs(storage_identity address_to identity,
                            p8 address_to bytes, positive have)
{
        positive block_size;
        positive sector_size;
        positive inode_size;
        positive ag_blocks;
        positive ag_count;
        p64 data_blocks;
        positive version;

        if (have < 208 ||
            !storage_bytes(bytes, have, 0, (p8 address_to)"XFSB", 4))
                return;

        block_size = storage_be32(bytes + 4);
        data_blocks = storage_be64(bytes + 8);
        ag_blocks = storage_be32(bytes + 84);
        ag_count = storage_be32(bytes + 88);
        version = storage_be16(bytes + 100) & 15;
        sector_size = storage_be16(bytes + 102);
        inode_size = storage_be16(bytes + 104);

        if (!storage_power_of_two(block_size) || block_size < 512 ||
            block_size > 65536 || !storage_power_of_two(sector_size) ||
            sector_size < 512 || sector_size > block_size ||
            !storage_power_of_two(inode_size) || inode_size < 256 ||
            inode_size > block_size ||
            storage_be16(bytes + 106) != block_size / inode_size ||
            !storage_log_matches(block_size, bytes[120]) ||
            !storage_log_matches(sector_size, bytes[121]) ||
            !storage_log_matches(inode_size, bytes[122]) ||
            !data_blocks || !ag_blocks || !ag_count ||
            (version != 4 && version != 5) ||
            (p64)(ag_count - 1) * ag_blocks >= data_blocks ||
            data_blocks > (p64)ag_count * ag_blocks ||
            !storage_uuid_present(bytes + 32))
                return;

        storage_set_type(identity, (string_address)"xfs");
        storage_set_uuid(identity, bytes + 32);
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + 108, 12, true);
}

/* F2FS keeps two copies; the first begins at 1024 and is wholly inside the
   common probe.  Address ordering and segment accounting reject a magic-only
   block while allowing feature growth after the fixed header. */
static fn storage_probe_f2fs(storage_identity address_to identity,
                             p8 address_to bytes, positive have)
{
        p8 address_to super = bytes + 1024;
        positive sector_log;
        positive sectors_per_block_log;
        positive block_log;
        positive blocks_per_segment_log;
        p64 segment_parts = 0;
        p64 block_count;
        positive segment_count;
        positive main_address;

        if (have < 1024 + 1148 || storage_le32(super) != 0xf2f52010)
                return;

        sector_log = storage_le32(super + 8);
        sectors_per_block_log = storage_le32(super + 12);
        block_log = storage_le32(super + 16);
        blocks_per_segment_log = storage_le32(super + 20);
        block_count = storage_le64(super + 36);
        segment_count = storage_le32(super + 48);
        main_address = storage_le32(super + 92);

        for (positive at = 0; at < 5; at++)
                segment_parts += storage_le32(super + 52 + at * 4);

        if (storage_le16(super + 4) < 1 || sector_log < 9 ||
            sector_log > 12 || sectors_per_block_log > 3 ||
            sector_log + sectors_per_block_log != block_log ||
            block_log != 12 || blocks_per_segment_log != 9 ||
            !storage_le32(super + 24) || !storage_le32(super + 28) ||
            !block_count || !segment_count || segment_parts > segment_count ||
            storage_le32(super + 72) > storage_le32(super + 76) ||
            storage_le32(super + 76) >= storage_le32(super + 80) ||
            storage_le32(super + 80) >= storage_le32(super + 84) ||
            storage_le32(super + 84) >= storage_le32(super + 88) ||
            storage_le32(super + 88) >= main_address ||
            main_address >= block_count ||
            storage_le32(super + 96) != 3 ||
            storage_le32(super + 100) != 1 ||
            storage_le32(super + 104) != 2 ||
            !storage_uuid_present(super + 108))
                return;

        storage_set_type(identity, (string_address)"f2fs");
        storage_set_uuid(identity, super + 108);
        storage_utf16_label(identity->label, sizeof(identity->label),
                            address_of identity->label_length,
                            super + 124, 512);
}

static bool storage_squashfs_table(p64 offset, p64 bytes_used)
{
        return offset == (p64)-1 ||
               (offset >= 96 && offset < bytes_used);
}

static fn storage_probe_squashfs(storage_identity address_to identity,
                                 p8 address_to bytes, positive have)
{
        positive block_size;
        positive block_log;
        p64 bytes_used;

        if (have < 96 || storage_le32(bytes) != 0x73717368)
                return;

        block_size = storage_le32(bytes + 12);
        block_log = storage_le16(bytes + 22);
        bytes_used = storage_le64(bytes + 40);

        if (!storage_le32(bytes + 4) ||
            !storage_power_of_two(block_size) || block_size < 4096 ||
            block_size > (1 << 20) ||
            !storage_log_matches(block_size, block_log) ||
            storage_le16(bytes + 20) < 1 ||
            storage_le16(bytes + 20) > 6 ||
            storage_le16(bytes + 28) != 4 || storage_le16(bytes + 30) != 0 ||
            bytes_used < 96 ||
            !storage_squashfs_table(storage_le64(bytes + 48), bytes_used) ||
            !storage_squashfs_table(storage_le64(bytes + 56), bytes_used) ||
            !storage_squashfs_table(storage_le64(bytes + 64), bytes_used) ||
            !storage_squashfs_table(storage_le64(bytes + 72), bytes_used) ||
            !storage_squashfs_table(storage_le64(bytes + 80), bytes_used) ||
            !storage_squashfs_table(storage_le64(bytes + 88), bytes_used) ||
            storage_le64(bytes + 64) == (p64)-1 ||
            storage_le64(bytes + 72) == (p64)-1)
                return;

        storage_set_type(identity, (string_address)"squashfs");
}

static bool storage_udf_tag(p8 address_to descriptor, positive have,
                            positive identifier, positive location)
{
        positive checksum = 0;

        if (have < 16 || storage_le16(descriptor) != identifier ||
            storage_le16(descriptor + 2) < 2 ||
            storage_le32(descriptor + 12) != location)
                return false;

        // The tag's own checksum byte is left out of the sum over the tag.
        checksum = memory_sum_bytes(descriptor, 16) - descriptor[4];

        return (p8)checksum == descriptor[4];
}

static fn storage_udf_dstring(p8 address_to into, positive room,
                              positive address_to length,
                              p8 address_to text, positive field_size)
{
        positive encoded;
        positive out = 0;

        address_to length = 0;

        if (!room || field_size < 2)
                return;

        encoded = text[field_size - 1];
        if (encoded < 2 || encoded > field_size - 1)
        {
                into[0] = end;
                return;
        }

        if (text[0] == 8)
        {
                for (positive at = 1; at < encoded; at++)
                {
                        if (!text[at])
                                continue;
                        if (!storage_utf8_add(into, room, address_of out,
                                              text[at]))
                                break;
                }
        }
        else if (text[0] == 16)
        {
                for (positive at = 1; at + 1 < encoded; at += 2)
                {
                        positive codepoint = storage_be16(text + at);

                        if (!codepoint)
                                continue;
                        if (!storage_utf8_add(into, room, address_of out,
                                              codepoint))
                                break;
                }
        }

        into[out] = end;
        address_to length = out;
}

static bool storage_udf_metadata(bipolar handle,
                                 storage_identity address_to identity)
{
        p8 descriptor[4096];
        positive block_sizes[] = {512, 1024, 2048, 4096};

        for (positive which = 0;
             which < array_count(block_sizes); which++)
        {
                positive block = block_sizes[which];
                positive have = storage_read(handle, descriptor, block,
                                             (p64)256 * block);
                positive extent_length;
                positive extent_location;

                if (!storage_udf_tag(descriptor, have, 2, 256))
                        continue;

                extent_length = storage_le32(descriptor + 16);
                extent_location = storage_le32(descriptor + 20);

                if (extent_length < block || extent_length % block ||
                    !extent_location ||
                    (p64)extent_location > (p64)bipolar_max / block)
                        continue;

                have = storage_read(handle, descriptor, block,
                                    (p64)extent_location * block);

                if (!storage_udf_tag(descriptor, have, 1,
                                     extent_location) || have < 200)
                        continue;

                storage_udf_dstring(identity->label,
                                    sizeof(identity->label),
                                    address_of identity->label_length,
                                    descriptor + 24, 32);

                {
                        p8 volume_set[STORAGE_LABEL_ROOM];
                        positive volume_set_length = 0;

                        storage_udf_dstring(volume_set, sizeof(volume_set),
                                            address_of volume_set_length,
                                            descriptor + 72, 128);

                        if (volume_set_length >= 16)
                        {
                                bool hexadecimal = true;

                                for (positive at = 0; at < 16; at++)
                                        if (!((volume_set[at] >= '0' &&
                                               volume_set[at] <= '9') ||
                                              (volume_set[at] >= 'a' &&
                                               volume_set[at] <= 'f') ||
                                              (volume_set[at] >= 'A' &&
                                               volume_set[at] <= 'F')))
                                                hexadecimal = false;

                                if (hexadecimal)
                                {
                                        memory_copy(identity->uuid,
                                                    volume_set, 16);
                                        identity->uuid[16] = end;
                                        identity->uuid_length = 16;
                                }
                        }
                }

                return true;
        }

        return false;
}

/* The UDF Volume Recognition Sequence is made of 2048-byte ECMA-167
   structures beginning at sector 16.  Walk a bounded sixteen descriptors and
   require the complete BEA -> NSR -> TEA order, not an isolated NSR word. */
static fn storage_probe_udf(bipolar handle,
                            storage_identity address_to identity)
{
        p8 descriptor[2048];
        bool beginning = false;
        bool namespace_seen = false;

        for (positive sector = 16; sector < 32; sector++)
        {
                positive have = storage_read(handle, descriptor,
                                             sizeof(descriptor), sector * 2048);
                string_address name;

                if (have != sizeof(descriptor) || descriptor[6] != 1)
                        return;

                name = descriptor + 1;

                /*
                        A bridge disc -- what mkisofs -udf and every DVD
                        writes -- puts the ISO 9660 descriptors first, and
                        those carry their type in byte 0 where an ECMA-167
                        structure has a zero. They are stepped over, as
                        libblkid steps over them, and only the three UDF
                        words are held to the zero: refusing the sequence at
                        its first descriptor named every such disc iso9660.
                */
                if (memory_is_5(name, 'C', 'D', '0', '0', '1'))
                        continue;

                if (descriptor[0])
                        return;

                if (memory_is_5(name, 'B', 'E', 'A', '0', '1'))
                {
                        if (beginning || namespace_seen)
                                return;
                        beginning = true;
                }
                else if (memory_is_5(name, 'N', 'S', 'R', '0', '2') ||
                         memory_is_5(name, 'N', 'S', 'R', '0', '3'))
                {
                        if (!beginning || namespace_seen)
                                return;
                        namespace_seen = true;
                }
                else if (memory_is_5(name, 'T', 'E', 'A', '0', '1'))
                {
                        if (!beginning || !namespace_seen ||
                            !storage_udf_metadata(handle, identity))
                                return;

                        storage_set_type(identity, (string_address)"udf");
                        return;
                }
                else if (!memory_is_5(name, 'B', 'O', 'O', 'T', '2'))
                        return;
        }
}

static fn storage_probe_luks(storage_identity address_to identity,
                             p8 address_to bytes, positive have)
{
        static const p8 magic[6] = {'L', 'U', 'K', 'S', 0xba, 0xbe};
        positive version;

        if (have < 592 || memory_compare(bytes, magic, sizeof(magic)))
                return;

        version = storage_be16(bytes + 6);

        if (version == 1)
        {
                bool slots_valid = true;

                if (!storage_header_word(bytes + 8, 32) ||
                    !storage_header_word(bytes + 40, 32) ||
                    !storage_header_word(bytes + 72, 32) ||
                    !storage_be32(bytes + 104) ||
                    !storage_be32(bytes + 108) ||
                    storage_be32(bytes + 108) > 4096 ||
                    !storage_be32(bytes + 164) ||
                    !storage_uuid_header(bytes + 168, 40))
                        return;

                for (positive slot = 0; slot < 8; slot++)
                {
                        p8 address_to key = bytes + 208 + slot * 48;
                        p32 active = storage_be32(key);

                        if (active == 0x00ac71f3)
                        {
                                if (!storage_be32(key + 4) ||
                                    !storage_be32(key + 40) ||
                                    !storage_be32(key + 44))
                                        slots_valid = false;
                        }
                        else if (active != 0x0000dead)
                                slots_valid = false;
                }

                if (!slots_valid)
                        return;
        }
        else if (version == 2)
        {
                p64 header_size = storage_be64(bytes + 8);

                if (header_size < 16384 || header_size > (16 << 20) ||
                    (header_size & 4095) || !storage_be64(bytes + 16) ||
                    !storage_header_word(bytes + 72, 32) ||
                    memory_count(bytes + 104, 64, 0) == 64 ||
                    !storage_uuid_header(bytes + 168, 40) ||
                    storage_be64(bytes + 256) != 0)
                        return;
        }
        else
                return;

        storage_set_type(identity, (string_address)"crypto_LUKS");
        storage_trimmed(identity->uuid, sizeof(identity->uuid),
                        address_of identity->uuid_length,
                        bytes + 168, 40, false);

        if (version == 2)
                storage_trimmed(identity->label, sizeof(identity->label),
                                address_of identity->label_length,
                                bytes + 24, 48, false);
}

static bool storage_swap_magic(bipolar handle, p8 address_to bytes,
                               positive have, p64 address_to offset,
                               bool address_to modern)
{
        p8 tail[10];
        static const positive page_sizes[] = {
            2048, 4096, 8192, 16384, 32768, 65536, 131072,
        };

        for (positive at = 0;
             at < array_count(page_sizes); at++)
        {
                positive page = page_sizes[at];
                p8 address_to signature;

                if (bytes && have >= page)
                        signature = bytes + page - 10;
                else
                {
                        /* Modern swap carries version 1 in the fixed header.
                           Gate the extra page-size probes on it so every
                           ordinary unknown device does not pay four preads. */
                        if (have < 1068 || storage_le32(bytes + 1024) != 1)
                        {
                                p8 header[4];

                                if (storage_read(handle, header,
                                                 sizeof(header), 1024) !=
                                        sizeof(header) ||
                                    storage_le32(header) != 1)
                                        break;
                        }
                        if (storage_read(handle, tail, sizeof(tail), page - 10) !=
                            sizeof(tail))
                                continue;
                        signature = tail;
                }

                if (!memory_compare(signature, "SWAPSPACE2", 10))
                {
                        address_to modern = true;
                        address_to offset = page - 10;
                        return true;
                }
                if (!memory_compare(signature, "SWAP-SPACE", 10))
                {
                        address_to modern = false;
                        address_to offset = page - 10;
                        return true;
                }
        }

        return false;
}

static fn storage_probe_swap(bipolar handle,
                             storage_identity address_to identity,
                             p8 address_to bytes, positive have)
{
        p64 offset;
        bool modern;

        if (!storage_swap_magic(handle, bytes, have,
                                address_of offset, address_of modern))
                return;

        storage_set_type(identity, (string_address)"swap");

        if (modern && have >= 1068)
        {
                if (storage_uuid_present(bytes + 1036))
                        storage_set_uuid(identity, bytes + 1036);
                storage_trimmed(identity->label, sizeof(identity->label),
                                address_of identity->label_length,
                                bytes + 1052, 16, false);
        }
}

static fn storage_probe_btrfs(bipolar handle,
                              storage_identity address_to identity,
                              p8 address_to bytes)
{
        positive have = storage_read(handle, bytes, STORAGE_PROBE_ROOM, 0x10000);
        positive sector_size;
        positive node_size;

        if (have < 0x12b + 256 || !storage_bytes(bytes, have, 0x40,
                           (p8 address_to)"_BHRfS_M", 8) ||
            storage_le64(bytes + 0x30) != 0x10000 ||
            !storage_le64(bytes + 0x70))
                return;

        sector_size = storage_le32(bytes + 0x90);
        node_size = storage_le32(bytes + 0x94);
        if (!storage_power_of_two(sector_size) || sector_size < 4096 ||
            sector_size > 65536 || !storage_power_of_two(node_size) ||
            node_size < sector_size || node_size > 65536)
                return;

        storage_set_type(identity, (string_address)"btrfs");
        storage_set_uuid(identity, bytes + 0x20);
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + 0x12b, 256, false);
}

static bool storage_iso_descriptor(bipolar handle, p8 address_to bytes,
                                   storage_iso_volume address_to volume,
                                   positive address_to read_bytes)
{
        positive have = storage_read(handle, bytes, 2048, 0x8000);

        if (read_bytes)
                address_to read_bytes = have;

        if (have < 72 || bytes[0] != 1 || bytes[6] != 1 ||
            !storage_bytes(bytes, have, 1,
                           (p8 address_to)"CD001", 5))
                return false;

        if (volume)
        {
                volume->sectors = have >= 88 ? storage_le32(bytes + 80) : 0;
                volume->sector_size =
                    have >= 132 ? storage_le16(bytes + 128) : 0;
        }
        return true;
}

static fn storage_probe_iso9660(bipolar handle,
                                storage_identity address_to identity,
                                p8 address_to bytes)
{
        if (!storage_iso_descriptor(handle, bytes, null, null))
                return;

        storage_set_type(identity, (string_address)"iso9660");
        storage_trimmed(identity->label, sizeof(identity->label),
                        address_of identity->label_length,
                        bytes + 40, 32, true);

        /* libblkid derives the ISO identity from its volume creation time.
           storage_read zeroes unread tail bytes, so a short descriptor cannot
           accidentally satisfy the digit check. */
        {
                bool digits = true;

                for (positive at = 813; at < 829; at++)
                        if (bytes[at] < '0' || bytes[at] > '9')
                                digits = false;

                if (digits)
                {
                        positive source = 813;
                        positive out = 0;
                        p8 groups[7] = {4, 2, 2, 2, 2, 2, 2};

                        for (positive group = 0; group < 7; group++)
                        {
                                if (group)
                                        identity->uuid[out++] = '-';
                                memory_copy(identity->uuid + out,
                                            bytes + source, groups[group]);
                                out += groups[group];
                                source += groups[group];
                        }
                        identity->uuid[out] = end;
                        identity->uuid_length = out;
                }
        }
}

/*
        Partition identity is already parsed and validated by the kernel.

        /sys/dev/block is indexed by the device number, so this also works
        when the caller used /dev/disk/by-* or another alias whose basename
        says nothing about its parent disk.  The kernel's GPT parser checks
        the header and entry-array CRCs and its DOS parser checks table bounds
        before either value is published in uevent; consuming that result
        avoids a second, inevitably drifting partition-table implementation.
*/
static fn storage_partition_value(storage_identity address_to identity,
                                  string_address key,
                                  p8 address_to value, positive length)
{
        if (string_equals(key, "PARTUUID"))
        {
                positive copied = min(length, sizeof(identity->partuuid) - 1);

                memory_copy(identity->partuuid, value, copied);
                identity->partuuid[copied] = end;
                identity->partuuid_length = copied;
        }
        else if (string_equals(key, "PARTNAME"))
        {
                positive copied = min(length, sizeof(identity->partlabel) - 1);

                memory_copy(identity->partlabel, value, copied);
                identity->partlabel[copied] = end;
                identity->partlabel_length = copied;
        }
}

static fn storage_probe_partition(string_address path,
                                  storage_identity address_to identity)
{
        p8 facts[256];
        p8 sysfs[96];
        p8 text[4096];
        positive used;
        positive major;
        positive minor;
        bipolar got;

        memory_zero(facts, sizeof(facts));
        if (system_stat_at(AT_FDCWD, path, 0x800, 0x7ff, facts) < 0 ||
            (storage_le16(facts + 28) & 0170000) != 0060000)
                return;

        major = storage_le32(facts + 128);
        minor = storage_le32(facts + 132);
        used = sizeof("/sys/dev/block/") - 1;
        memory_copy(sysfs, "/sys/dev/block/", used);
        used += positive_into_string(sysfs + used, major);
        sysfs[used++] = ':';
        used += positive_into_string(sysfs + used, minor);
        memory_copy(sysfs + used, "/uevent", sizeof("/uevent"));

        got = file_slurp_once_at(AT_FDCWD, sysfs, text, sizeof(text));
        if (got <= 0)
                return;

        {
                p8 address_to line = text;
                p8 address_to limit = text + got;

                while (line < limit)
                {
                        p8 address_to newline = (p8 address_to)memory_first_of(
                            line, '\n', (positive)(limit - line));
                        p8 address_to line_end = newline ? newline : limit;
                        p8 address_to equal = (p8 address_to)memory_first_of(
                            line, '=', (positive)(line_end - line));

                        if (equal && equal > line)
                        {
                                p8 saved = *equal;

                                *equal = end;
                                storage_partition_value(
                                    identity, line, equal + 1,
                                    (positive)(line_end - equal - 1));
                                *equal = saved;
                        }
                        line = newline ? newline + 1 : limit;
                }
        }
}

static bool storage_signature_location(
    bipolar handle, storage_identity address_to identity,
    p64 address_to offset, p8 address_to length,
    string_address address_to usage)
{
        string_address type = identity->type;

        address_to usage = (string_address)"filesystem";
        if (string_equals(type, "crypto_LUKS"))
        {
                address_to offset = 0;
                address_to length = 6;
                address_to usage = (string_address)"crypto";
        }
        else if (string_equals(type, "xfs") ||
                 string_equals(type, "squashfs"))
        {
                address_to offset = 0;
                address_to length = 4;
        }
        else if (string_equals(type, "f2fs") ||
                 string_equals(type, "erofs"))
        {
                address_to offset = 0x400;
                address_to length = 4;
        }
        else if (string_equals(type, "exfat") ||
                 string_equals(type, "ntfs"))
        {
                address_to offset = 3;
                address_to length = 8;
        }
        else if (string_equals(type, "vfat"))
        {
                p8 boot[90];

                if (storage_read(handle, boot, sizeof(boot), 0) != sizeof(boot))
                        return false;
                address_to offset = storage_bytes(
                    boot, sizeof(boot), 82, (p8 address_to)"FAT32", 5)
                    ? 82 : 54;
                address_to length = 5;
        }
        else if (string_equals(type, "ext2") ||
                 string_equals(type, "ext3") ||
                 string_equals(type, "ext4"))
        {
                address_to offset = 0x438;
                address_to length = 2;
        }
        else if (string_equals(type, "swap"))
        {
                p8 bytes[STORAGE_PROBE_ROOM];
                bool modern;
                positive have = storage_read(handle, bytes,
                                             sizeof(bytes), 0);

                if (!storage_swap_magic(handle, bytes, have, offset,
                                        address_of modern))
                        return false;
                address_to length = 10;
                address_to usage = (string_address)"other";
                return true;
        }
        else if (string_equals(type, "btrfs"))
        {
                address_to offset = 0x10040;
                address_to length = 8;
        }
        else if (string_equals(type, "udf"))
        {
                p8 descriptor[2048];

                for (positive sector = 16; sector < 32; sector++)
                        if (storage_read(handle, descriptor,
                                         sizeof(descriptor), sector * 2048) ==
                                sizeof(descriptor) &&
                            memory_is_5(descriptor + 1,
                                        'B', 'E', 'A', '0', '1'))
                        {
                                address_to offset = sector * 2048 + 1;
                                address_to length = 5;
                                return true;
                        }
                return false;
        }
        else if (string_equals(type, "iso9660"))
        {
                address_to offset = 0x8001;
                address_to length = 5;
        }
        else
                return false;

        return true;
}

static bool storage_signature_emit(
    bipolar handle, storage_identity address_to identity,
    storage_signature_visitor visitor, address_any context)
{
        storage_signature signature;

        memory_zero(address_of signature, sizeof(signature));
        signature.identity = address_to identity;
        if (!storage_signature_location(
                handle, address_of signature.identity,
                address_of signature.offset, address_of signature.length,
                address_of signature.usage) ||
            signature.length > sizeof(signature.magic) ||
            storage_read(handle, signature.magic, signature.length,
                         signature.offset) != signature.length)
                return true;

        return visitor(address_of signature, context);
}

/* Enumerate every filesystem/container recogniser over one open descriptor.
   The recognisers remain the sole source of validation and metadata; wipefs
   merely receives their exact magic locations. */
static positive storage_each_signature_handle(
    bipolar handle, string_address path,
    storage_signature_visitor visitor, address_any context)
{
        p8 bytes[STORAGE_PROBE_ROOM];
        storage_identity identity;
        positive have;
        positive found = 0;

        have = storage_read(handle, bytes, sizeof(bytes), 0);

#define STORAGE_SIGNATURE_TRY(probe)                                      \
        do                                                                 \
        {                                                                  \
                memory_zero(address_of identity, sizeof(identity));        \
                identity.path = path;                                      \
                probe;                                                     \
                if (identity.type_length)                                  \
                {                                                          \
                        found++;                                           \
                        if (!storage_signature_emit(                       \
                                handle, address_of identity, visitor,       \
                                context))                                  \
                                goto done;                                 \
                }                                                          \
        } while (false)

        STORAGE_SIGNATURE_TRY(storage_probe_luks(address_of identity,
                                                  bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_xfs(address_of identity,
                                                 bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_squashfs(address_of identity,
                                                      bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_f2fs(address_of identity,
                                                  bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_exfat(handle, address_of identity,
                                                   bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_fat(address_of identity,
                                                 bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_swap(handle, address_of identity,
                                                  bytes, have));
        STORAGE_SIGNATURE_TRY(
            storage_probe_ntfs(address_of identity, bytes, have);
            if (identity.type_length)
                    storage_probe_ntfs_label(handle, address_of identity,
                                             bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_ext(address_of identity,
                                                 bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_erofs(address_of identity,
                                                   bytes, have));
        STORAGE_SIGNATURE_TRY(storage_probe_btrfs(handle, address_of identity,
                                                   bytes));
        STORAGE_SIGNATURE_TRY(storage_probe_udf(handle, address_of identity));
        STORAGE_SIGNATURE_TRY(storage_probe_iso9660(handle,
                                                     address_of identity,
                                                     bytes));

done:
#undef STORAGE_SIGNATURE_TRY
        return found;
}

static bipolar storage_each_signature(
    string_address path, storage_signature_visitor visitor,
    address_any context)
{
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return handle;
        positive found = storage_each_signature_handle(
            handle, path, visitor, context);
        system_close(handle);
        return (bipolar)found;
}

/*
        Probe one block device or image.  The path remains borrowed; all
        metadata is owned by `identity`.  false means unreadable or unknown.
*/
bool storage_probe_device(string_address path,
                          storage_identity address_to identity)
{
        p8 bytes[STORAGE_PROBE_ROOM];
        bipolar handle;
        positive have;

        memory_zero(identity, sizeof(*identity));
        identity->path = path;
        storage_probe_partition(path, identity);
        handle = system_open_at(AT_FDCWD, path,
                               FILE_READ | O_CLOEXEC);
        if (handle < 0)
                return identity->partuuid_length || identity->partlabel_length;

        have = storage_read(handle, bytes, sizeof(bytes), 0);

        /* Container and fixed-superblock formats precede the deliberately
           broad boot-sector families. */
        storage_probe_luks(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_xfs(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_squashfs(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_f2fs(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_exfat(handle, identity, bytes, have);
        if (!identity->type_length)
        {
                storage_probe_ntfs(identity, bytes, have);
                if (identity->type_length)
                        storage_probe_ntfs_label(handle, identity, bytes, have);
        }
        if (!identity->type_length)
                storage_probe_fat(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_ext(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_erofs(identity, bytes, have);
        if (!identity->type_length)
                storage_probe_swap(handle, identity, bytes, have);
        if (!identity->type_length)
                storage_probe_btrfs(handle, identity, bytes);
        if (!identity->type_length)
                storage_probe_udf(handle, identity);
        if (!identity->type_length)
                storage_probe_iso9660(handle, identity, bytes);

        system_close(handle);
        return identity->type_length || identity->partuuid_length ||
               identity->partlabel_length;
}

static const storage_tag_descriptor address_to storage_tag_find(
    string_address tag, positive length)
{
        for (positive at = 0; at < array_count(storage_tags);
             at++)
                if (file_same_word(tag, length, storage_tags[at].name))
                        return storage_tags + at;

        return null;
}

static string_address storage_tag_value(
    storage_identity address_to identity,
    const storage_tag_descriptor address_to descriptor)
{
        if (descriptor->show == STORAGE_SHOW_DEVNAME)
                return identity->path;

        return memory_load_unaligned(
                   positive,
                   (p8 address_to)identity + descriptor->length_offset)
                   ? (p8 address_to)identity + descriptor->value_offset
                   : null;
}

static bool storage_identity_matches(
    storage_identity address_to identity,
    storage_selector address_to selector)
{
        string_address value;

        if (!selector->tag_length)
                return true;

        const storage_tag_descriptor address_to descriptor = storage_tag_find(
            selector->tag, selector->tag_length);
        if (!descriptor)
                return false;
        value = storage_tag_value(identity, descriptor);
        return value && string_equals(value, selector->value);
}

static bool storage_selector_parse(string_address text,
                                   storage_selector address_to selector)
{
        string_address equal = string_first_of(text, '=');

        if (!equal || equal == text)
                return false;

        selector->tag = text;
        selector->tag_length = (positive)(equal - text);
        selector->value = equal + 1;

        return true;
}

typedef bool (*storage_visitor)(storage_identity address_to identity,
                                address_any context);

typedef bool (*storage_path_visitor)(string_address path,
                                     address_any context);

/* The class directory is the kernel's one complete block-device census.
   Keep its checked dirent walk shared by metadata consumers; callbacks must
   consume the stack-backed path before returning. */
static bool storage_each_block_path(storage_path_visitor visit,
                                    address_any context)
{
        bipolar directory;
        p8 block[STORAGE_DEVICE_BLOCK];
        bool stopped = false;

        directory = system_open_at(AT_FDCWD,
                                  "/sys/class/block",
                                  FILE_READ | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
                return false;

        while (!stopped)
        {
                bipolar got = system_read_directory(directory, block,
                                                    sizeof(block));
                positive at = 0;

                if (got <= 0)
                        break;

                while (at < (positive)got)
                {
                        struct linux_dirent64 address_to entry =
                            (struct linux_dirent64 address_to)(block + at);
                        positive remaining = (positive)got - at;
                        positive record;
                        positive name_room;
                        positive name_length;
                        p8 path[STORAGE_PATH_ROOM];

                        if (remaining < 19)
                        {
                                stopped = true;
                                break;
                        }

                        record = entry->d_reclen;
                        if (record < 20 || record > remaining)
                        {
                                stopped = true;
                                break;
                        }

                        at += record;
                        name_room = record - 19;
                        name_length = string_length_max(entry->d_name, name_room);
                        if (!name_length || name_length == name_room ||
                            (name_length == 1 && entry->d_name[0] == '.') ||
                            (name_length == 2 && entry->d_name[0] == '.' &&
                             entry->d_name[1] == '.'))
                                continue;

                        if (name_length > sizeof(path) - sizeof("/dev/"))
                                continue;

                        memory_copy(path, "/dev/", sizeof("/dev/") - 1);
                        memory_copy(path + sizeof("/dev/") - 1,
                                    entry->d_name, name_length);
                        path[sizeof("/dev/") - 1 + name_length] = end;

                        if (!visit(path, context))
                        {
                                stopped = true;
                                break;
                        }
                }
        }

        system_close(directory);
        return stopped;
}

typedef struct
{
        storage_visitor visit;
        address_any context;
} storage_device_visit;

static bool storage_device_probe_visit(string_address path,
                                       address_any opaque)
{
        storage_device_visit address_to request =
            (storage_device_visit address_to)opaque;
        storage_identity identity;

        return !storage_probe_device(path, address_of identity) ||
               request->visit(address_of identity, request->context);
}

static bool storage_each_device(storage_visitor visit, address_any context)
{
        storage_device_visit request = {visit, context};
        return storage_each_block_path(storage_device_probe_visit,
                                       address_of request);
}

typedef struct
{
        storage_selector selector;
        p8 address_to path;
        positive room;
        bool found;
} storage_resolve_context;

static bool storage_resolve_visit(
    storage_identity address_to identity, address_any opaque)
{
        storage_resolve_context address_to context =
            (storage_resolve_context address_to)opaque;

        if (!storage_identity_matches(identity, address_of context->selector))
                return true;

        if (!context->room ||
            string_length(identity->path) >= context->room)
                return true;

        string_copy(context->path, identity->path);
        context->found = true;
        return false;
}

/* Resolve filesystem and partition tags through the same engine as findfs. */
bool storage_resolve_tag(string_address expression, p8 address_to path,
                         positive room)
{
        storage_resolve_context context;

        memory_zero(address_of context, sizeof(context));
        if (!storage_selector_parse(expression, address_of context.selector))
                return false;

        context.path = path;
        context.room = room;
        storage_each_device(storage_resolve_visit, address_of context);
        return context.found;
}

typedef enum
{
        STORAGE_OUTPUT_FULL,
        STORAGE_OUTPUT_VALUE,
        STORAGE_OUTPUT_DEVICE,
        STORAGE_OUTPUT_EXPORT
} storage_output_mode;

typedef struct
{
        writer output;
        storage_output_mode mode;
        storage_selector selector;
        positive shown;
        positive found;
        bool select_seen;
        bool first_only;
} storage_blkid_context;

static positive storage_show_tag(string_address tag, positive length)
{
        const storage_tag_descriptor address_to descriptor =
            storage_tag_find(tag, length);

        return descriptor ? descriptor->show : 0;
}

/* One storage-family escaping engine.  blkid leaves spaces literal and quotes
   them; findmnt --raw must instead escape spaces, while quotes need no special
   treatment there. */
static fn storage_write_hex_escaped(writer output, string_address value,
                                    bool escape_space, bool escape_quote)
{
        string_address start = value;
        p8 first = escape_space ? 0x21 : 0x20;

        while (*value)
        {
                p8 byte = *value;

                if (byte >= first && byte < 0x7f && byte != '\\' &&
                    (!escape_quote || byte != '"'))
                {
                        value++;
                        continue;
                }

                if (value > start)
                        output(start, (positive)(value - start));

                {
                        p8 escaped[4] = {'\\', 'x',
                                         storage_hex_digit(byte >> 4, false),
                                         storage_hex_digit(byte & 15, false)};
                        output(escaped, sizeof(escaped));
                }

                value++;
                start = value;
        }

        if (value > start)
                output(start, (positive)(value - start));
}

static fn storage_write_encoded(writer output, string_address value)
{
        storage_write_hex_escaped(output, value, false, true);
}

static fn storage_output_field(writer output, string_address name,
                               string_address value, bool exported)
{
        string_format(output, exported ? "%s=" : " %s=\"", name);
        storage_write_encoded(output, value);
        output(exported ? "\n" : "\"", 1);
}

static bool storage_blkid_visit(
    storage_identity address_to identity, address_any opaque)
{
        storage_blkid_context address_to context =
            (storage_blkid_context address_to)opaque;

        if (!storage_identity_matches(identity, address_of context->selector))
                return true;

        context->found++;

        positive available = STORAGE_SHOW_DEVNAME;

        for (positive at = 0; at + 1 < array_count(storage_tags); at++)
                if (storage_tag_value(identity, storage_tags + at))
                        available |= storage_tags[at].show;

        /* A recognised device remains a successful blkid query even when a
           requested tag is absent.  Only `-o device` prints independently of
           the selected tag; the field-oriented formats emit no empty row. */
        if (context->select_seen && !(context->shown & available) &&
            context->mode != STORAGE_OUTPUT_DEVICE)
                return !context->first_only;

        if (context->mode == STORAGE_OUTPUT_DEVICE)
                string_format(context->output, "%s\n", identity->path);
        else
        {
                bool exported = context->mode == STORAGE_OUTPUT_EXPORT;

                if (exported && context->found > 1)
                        context->output("\n", 1);

                if (exported)
                        storage_output_field(context->output,
                            (string_address)"DEVNAME", identity->path, true);

                if (context->mode == STORAGE_OUTPUT_FULL)
                        string_format(context->output, "%s:", identity->path);

                for (positive at = 0; at < array_count(storage_tags); at++)
                {
                        const storage_tag_descriptor address_to tag =
                            storage_tags + at;
                        bool asked = (context->shown & tag->show) != 0;
                        string_address value = storage_tag_value(identity, tag);

                        if (!value ||
                            (exported && tag->show == STORAGE_SHOW_DEVNAME) ||
                            (context->mode == STORAGE_OUTPUT_VALUE && !asked) ||
                            (context->mode == STORAGE_OUTPUT_FULL &&
                             context->select_seen && !asked) ||
                            (context->mode == STORAGE_OUTPUT_FULL &&
                             !context->select_seen &&
                             tag->show == STORAGE_SHOW_DEVNAME) ||
                            (exported && tag->show != STORAGE_SHOW_DEVNAME &&
                             context->select_seen && !asked))
                                continue;

                        if (context->mode == STORAGE_OUTPUT_VALUE)
                                string_format(context->output, "%s\n", value);
                        else
                                storage_output_field(context->output, tag->name,
                                                     value, exported);
                }

                if (context->mode == STORAGE_OUTPUT_FULL)
                        context->output("\n", 1);
        }

        return !context->first_only;
}

/* Storage commands are callable both as shell builtins and through farm
   links, so they cannot use the process-global utility option reader.  One
   cursor owns the equivalent argv state machine for the whole storage family:
   short clusters, attached values, long values and the -- boundary. */
typedef struct
{
        positive argc;
        string_address address_to argv;
        positive at;
        string_address letters;
        string_address word;
        bool operands_only;
} storage_arguments;

/* Length occupies padding the generic named-byte row would leave unused, so
   table lookup never has to rescan constant option names. */
typedef struct
{
        string_address name;
        p8 value;
        p8 length;
} storage_argument_name;

#define STORAGE_ARGUMENT(name, value)                                      \
        {(string_address)(name), (value), sizeof(name) - 1}

enum
{
        STORAGE_ARGUMENT_END,
        STORAGE_ARGUMENT_OPERAND = 256,
        STORAGE_ARGUMENT_UNKNOWN = -1,
        STORAGE_ARGUMENT_MISSING = -2,
};

static PURE p8 storage_argument_long(string_address name, positive length,
                                     const storage_argument_name address_to options,
                                     positive count)
{
        for (positive at = 0; at < count; at++)
                if (options[at].length == length &&
                    !memory_compare(name, options[at].name, length))
                        return options[at].value;

        return 0;
}

static COLD b32 storage_argument_next(
    storage_arguments address_to taking, string_address allowed,
    string_address valued, const storage_argument_name address_to longs,
    positive long_count, string_address address_to value)
{
        while (true)
        {
                if (taking->letters && *taking->letters)
                {
                        p8 option = *taking->letters++;

                        if (!string_first_of(allowed, option))
                                return STORAGE_ARGUMENT_UNKNOWN;

                        if (string_first_of(valued, option))
                        {
                                if (*taking->letters)
                                {
                                        address_to value = taking->letters;
                                        taking->letters = null;
                                }
                                else if (taking->at < taking->argc)
                                        address_to value =
                                            taking->argv[taking->at++];
                                else
                                        return STORAGE_ARGUMENT_MISSING;
                        }
                        else
                                address_to value = null;

                        return option;
                }

                taking->letters = null;
                if (taking->at >= taking->argc)
                        return STORAGE_ARGUMENT_END;

                taking->word = taking->argv[taking->at++];
                if (taking->operands_only || taking->word[0] != '-' ||
                    !taking->word[1])
                {
                        address_to value = taking->word;
                        return STORAGE_ARGUMENT_OPERAND;
                }

                if (taking->word[1] == '-' && !taking->word[2])
                {
                        taking->operands_only = true;
                        continue;
                }

                if (taking->word[1] == '-')
                {
                        string_address name = taking->word + 2;
                        string_address mark = string_first_of(name, '=');
                        positive length = mark ? (positive)(mark - name)
                                               : string_length(name);
                        p8 option = storage_argument_long(name, length, longs,
                                                          long_count);

                        if (!option || (mark &&
                                        !string_first_of(valued, option)))
                                return STORAGE_ARGUMENT_UNKNOWN;

                        if (string_first_of(valued, option))
                        {
                                if (mark)
                                        address_to value = mark + 1;
                                else if (taking->at < taking->argc)
                                        address_to value =
                                            taking->argv[taking->at++];
                                else
                                        return STORAGE_ARGUMENT_MISSING;
                        }
                        else
                                address_to value = null;

                        return option;
                }

                taking->letters = taking->word + 1;
        }
}

/*
        util-linux compatible core.  Syntax errors are 4, no recognised
        device is 2, success is 0.  The writers make it equally usable from
        the in-process builtin and the multicall entry.
*/
b32 storage_blkid_run(positive argc, string_address address_to argv,
                      writer output, writer error)
{
        static const storage_argument_name options[] = {
            STORAGE_ARGUMENT("uuid", 'U'),
            STORAGE_ARGUMENT("label", 'L'),
            STORAGE_ARGUMENT("match-tag", 's'),
            STORAGE_ARGUMENT("match-token", 't'),
            STORAGE_ARGUMENT("output", 'o'),
        };
        storage_blkid_context context;
        string_address inline_devices[8];
        string_address address_to devices = inline_devices;
        positive device_room = array_count(inline_devices);
        positive device_count = 0;
        storage_arguments taking = {.argc = argc, .argv = argv, .at = 1};
        string_address value;
        b32 option;

        memory_zero(address_of context, sizeof(context));
        context.output = output;

        while ((option = storage_argument_next(
                    address_of taking, (string_address)"ULsto",
                    (string_address)"ULsto", options, array_count(options),
                    address_of value)) != STORAGE_ARGUMENT_END)
        {
                if (option == STORAGE_ARGUMENT_OPERAND)
                {
                        if (device_count == device_room)
                        {
                                if (devices == inline_devices)
                                {
                                        string_address address_to grown = null;
                                        positive grown_room = 0;

                                        if (!array_store_reserve(
                                                grown, grown_room, 0,
                                                device_count + 1, 8))
                                                goto usage;
                                        memory_copy_apart(grown, inline_devices,
                                                          device_count *
                                                              sizeof(grown[0]));
                                        devices = grown;
                                        device_room = grown_room;
                                }
                                else if (!array_store_reserve(
                                             devices, device_room,
                                             device_count, device_count + 1, 8))
                                        goto usage;
                        }
                        devices[device_count++] = value;
                        continue;
                }

                if (option == 'U')
                {
                        context.selector.tag = (string_address)"UUID";
                        context.selector.tag_length = 4;
                        context.selector.value = value;
                        context.mode = STORAGE_OUTPUT_DEVICE;
                        context.first_only = true;
                        continue;
                }

                if (option == 'L')
                {
                        context.selector.tag = (string_address)"LABEL";
                        context.selector.tag_length = 5;
                        context.selector.value = value;
                        context.mode = STORAGE_OUTPUT_DEVICE;
                        context.first_only = true;
                        continue;
                }

                if (option == 's')
                {
                        positive length = string_length(value);

                        context.select_seen = true;
                        context.shown |= storage_show_tag(value, length);
                        continue;
                }

                if (option == 't')
                {
                        if (!storage_selector_parse(value, address_of context.selector))
                                goto usage;
                        continue;
                }

                if (option == 'o')
                {
                        if (string_equals(value, "value"))
                                context.mode = STORAGE_OUTPUT_VALUE;
                        else if (string_equals(value, "device"))
                                context.mode = STORAGE_OUTPUT_DEVICE;
                        else if (string_equals(value, "export"))
                                context.mode = STORAGE_OUTPUT_EXPORT;
                        else if (string_equals(value, "full"))
                                context.mode = STORAGE_OUTPUT_FULL;
                        else
                                goto usage;
                        continue;
                }

                goto usage;
        }

        if (context.mode == STORAGE_OUTPUT_VALUE && !context.select_seen)
                goto usage;

        if (device_count)
        {
                for (positive at = 0; at < device_count; at++)
                {
                        storage_identity identity;

                        if (storage_probe_device(devices[at], address_of identity) &&
                            !storage_blkid_visit(address_of identity,
                                                 address_of context))
                                break;
                }
        }
        else
                storage_each_device(storage_blkid_visit, address_of context);

        {
                b32 answer = context.found ? 0 : 2;

                if (devices != inline_devices)
                        array_store_release(devices, device_room, device_count);
                return answer;
        }

usage:
        error("blkid: usage: blkid [-s TAG] [-o full|value|device|export] "
              "[-t TAG=VALUE] [-U UUID] [-L LABEL] [DEVICE ...]\n", 0);
        if (devices != inline_devices)
                array_store_release(devices, device_room, device_count);
        return 4;
}

b32 storage_findfs_run(positive argc, string_address address_to argv,
                       writer output, writer error)
{
        p8 path[STORAGE_PATH_ROOM];

        // Two for a usage error, as upstream answers: one is "not found".
        if (argc != 2)
        {
                error("findfs: usage: findfs UUID=value|LABEL=value|"
                      "PARTUUID=value|PARTLABEL=value\n", 0);
                return 2;
        }

        /* findfs doubles as the ordinary source-specifier normalizer used by
           mount callers: a path with no TAG= prefix passes through unchanged.
           util-linux preserves even an empty word here, including its line. */
        if (!*string_first_of_or_end(argv[1], '='))
        {
                string_format(output, "%s\n", argv[1]);
                return 0;
        }

        {
                storage_selector selector;

                if (!storage_selector_parse(argv[1], address_of selector))
                {
                        error("findfs: expected NAME=value\n", 0);
                        return 1;
                }
        }

        if (!storage_resolve_tag(argv[1], path, sizeof(path)))
                return 1;

        string_format(output, "%s\n", path);
        return 0;
}
