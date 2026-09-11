/*
        zstd -- RFC 8878 decoder, no encoder.

        Content checksum is hash_xxh64. Match copies are memory_copy_match.
        The window is an anonymous map, not BSS: Arch bootstrap is --long
        (128 MiB). Dictionaries are refused. Concatenated frames and
        skippable frames are accepted the way zstd -d accepts them.
*/

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_SKIP_MAGIC 0x184D2A50u
#define ZSTD_SKIP_MASK 0xFFFFFFF0u
#define ZSTD_WINDOW_MAX (1u << 27)
#define ZSTD_BLOCK_MAX (1u << 17)
#define ZSTD_IN 262144
#define ZSTD_FSE_MAX 512
#define ZSTD_HUF_MAX 2048
#define ZSTD_ERROR ((positive)-1)

#define ZSTD_P1 0x9E3779B185EBCA87ull
#define ZSTD_P2 0xC2B2AE3D27D4EB4Full
#define ZSTD_P3 0x165667B19E3779F9ull
#define ZSTD_P4 0x85EBCA77C2B2AE63ull
#define ZSTD_P5 0x27D4EB2F165667C5ull

static const p8 zstd_ll_bits[36] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16};
static const p32 zstd_ll_base[36] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048,
    4096, 8192, 16384, 32768, 65536};
static const p8 zstd_ml_bits[53] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16};
static const p32 zstd_ml_base[53] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027,
    2051, 4099, 8195, 16387, 32771, 65539};
static const bipolar zstd_ll_default[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1};
static const bipolar zstd_ml_default[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1};
static const bipolar zstd_of_default[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

typedef struct
{
        p16 next;
        p8 bits;
        p8 symbol;
} zstd_fse_cell;

typedef struct
{
        p8 log;
        p8 rle;
        bool valid;
        zstd_fse_cell cell[ZSTD_FSE_MAX];
} zstd_fse;

typedef struct
{
        p8 max_bits;
        bool valid;
        p8 symbol[ZSTD_HUF_MAX];
        p8 bits[ZSTD_HUF_MAX];
} zstd_huff;

typedef struct
{
        p64 bits;
        positive consumed;
        p8 address_to ptr;
        p8 address_to start;
        p8 address_to end;
} zstd_bits;

typedef struct
{
        p64 total;
        p64 acc[4];
        p64 seed;
        p8 hold[32];
        p8 held;
} zstd_xxh;

typedef struct
{
        bipolar fd;
        p8 address_to mem;
        positive mem_len;
        positive mem_at;
        p8 buf[ZSTD_IN];
        positive at;
        positive have;
        bool eof;
} zstd_in;

static string_address zstd_why;
static zstd_in zstd_src;
static p8 address_to zstd_window;
static positive zstd_window_cap;
static positive zstd_window_size;
static positive zstd_pos;
static positive zstd_keep;
static p8 address_to zstd_out_mem;
static positive zstd_out_cap;
static positive zstd_out_used;
static bipolar zstd_out_fd;
static bool zstd_hashing;
static zstd_xxh zstd_hash;
static p64 zstd_decoded;
static p32 zstd_rep[3];
static zstd_huff zstd_lit_huff;
static zstd_fse zstd_ll;
static zstd_fse zstd_of;
static zstd_fse zstd_ml;
static zstd_fse zstd_ll_prev;
static zstd_fse zstd_of_prev;
static zstd_fse zstd_ml_prev;
static p8 zstd_literals[ZSTD_BLOCK_MAX + 64];
static bool zstd_fse_ready;
static zstd_fse zstd_ll_def;
static zstd_fse zstd_of_def;
static zstd_fse zstd_ml_def;

static p16 zstd_get16(p8 address_to p)
{
        return (p16)p[0] | ((p16)p[1] << 8);
}

static p32 zstd_get24(p8 address_to p)
{
        return (p32)p[0] | ((p32)p[1] << 8) | ((p32)p[2] << 16);
}

static p32 zstd_get32(p8 address_to p)
{
        return (p32)p[0] | ((p32)p[1] << 8) | ((p32)p[2] << 16) |
               ((p32)p[3] << 24);
}

static p64 zstd_get64(p8 address_to p)
{
        return (p64)zstd_get32(p) | ((p64)zstd_get32(p + 4) << 32);
}

static p8 zstd_highbit32(p32 value)
{
        p8 n = 0;

        if (value >= 0x10000u)
        {
                n += 16;
                value >>= 16;
        }
        if (value >= 0x100u)
        {
                n += 8;
                value >>= 8;
        }
        if (value >= 0x10u)
        {
                n += 4;
                value >>= 4;
        }
        if (value >= 4u)
        {
                n += 2;
                value >>= 2;
        }
        if (value >= 2u)
                n += 1;

        return n;
}

static p64 zstd_rotl(p64 value, p8 rot)
{
        return (value << rot) | (value >> (64 - rot));
}

static fn zstd_xxh_start(zstd_xxh address_to h, p64 seed)
{
        h->seed = seed;
        h->total = 0;
        h->held = 0;
        h->acc[0] = seed + ZSTD_P1 + ZSTD_P2;
        h->acc[1] = seed + ZSTD_P2;
        h->acc[2] = seed;
        h->acc[3] = seed - ZSTD_P1;
}

static p64 zstd_xxh_round(p64 acc, p64 lane)
{
        return zstd_rotl(acc + lane * ZSTD_P2, 31) * ZSTD_P1;
}

static fn zstd_xxh_stripe(zstd_xxh address_to h, p8 address_to p)
{
        h->acc[0] = zstd_xxh_round(h->acc[0], zstd_get64(p));
        h->acc[1] = zstd_xxh_round(h->acc[1], zstd_get64(p + 8));
        h->acc[2] = zstd_xxh_round(h->acc[2], zstd_get64(p + 16));
        h->acc[3] = zstd_xxh_round(h->acc[3], zstd_get64(p + 24));
}

static fn zstd_xxh_add(zstd_xxh address_to h, p8 address_to p, positive n)
{
        h->total += n;

        if (h->held)
        {
                positive take = 32 - h->held;

                if (take > n)
                        take = n;
                memory_copy(h->hold + h->held, p, take);
                h->held = (p8)(h->held + take);
                p += take;
                n -= take;
                if (h->held == 32)
                {
                        zstd_xxh_stripe(h, h->hold);
                        h->held = 0;
                }
        }

        while (n >= 32)
        {
                zstd_xxh_stripe(h, p);
                p += 32;
                n -= 32;
        }

        if (n)
        {
                memory_copy(h->hold + h->held, p, n);
                h->held = (p8)(h->held + n);
        }
}

static p64 zstd_xxh_merge(p64 acc, p64 lane)
{
        acc ^= zstd_xxh_round(0, lane);
        return acc * ZSTD_P1 + ZSTD_P4;
}

static p64 zstd_xxh_end(zstd_xxh address_to h)
{
        p64 acc;
        p8 address_to p = h->hold;
        positive n = h->held;

        if (h->total >= 32)
        {
                acc = zstd_rotl(h->acc[0], 1) + zstd_rotl(h->acc[1], 7) +
                      zstd_rotl(h->acc[2], 12) + zstd_rotl(h->acc[3], 18);
                acc = zstd_xxh_merge(acc, h->acc[0]);
                acc = zstd_xxh_merge(acc, h->acc[1]);
                acc = zstd_xxh_merge(acc, h->acc[2]);
                acc = zstd_xxh_merge(acc, h->acc[3]);
        }
        else
                acc = h->seed + ZSTD_P5;

        acc += h->total;

        while (n >= 8)
        {
                acc ^= zstd_xxh_round(0, zstd_get64(p));
                acc = zstd_rotl(acc, 27) * ZSTD_P1 + ZSTD_P4;
                p += 8;
                n -= 8;
        }

        if (n >= 4)
        {
                acc ^= (p64)zstd_get32(p) * ZSTD_P1;
                acc = zstd_rotl(acc, 23) * ZSTD_P2 + ZSTD_P3;
                p += 4;
                n -= 4;
        }

        while (n)
        {
                acc ^= (p64)address_to p * ZSTD_P5;
                acc = zstd_rotl(acc, 11) * ZSTD_P1;
                p++;
                n--;
        }

        acc ^= acc >> 33;
        acc *= ZSTD_P2;
        acc ^= acc >> 29;
        acc *= ZSTD_P3;
        acc ^= acc >> 32;
        return acc;
}

static bool zstd_fail(string_address why)
{
        zstd_why = why;
        return false;
}

static bool zstd_in_need(positive n)
{
        if (zstd_src.have >= n)
                return true;

        if (n > ZSTD_IN)
                return zstd_fail("zstd block larger than the input window");

        if (zstd_src.at)
        {
                if (zstd_src.have)
                        memory_copy(zstd_src.buf, zstd_src.buf + zstd_src.at,
                                    zstd_src.have);
                zstd_src.at = 0;
        }

        while (zstd_src.have < n && !zstd_src.eof)
        {
                positive room = ZSTD_IN - zstd_src.have;
                bipolar got;

                if (zstd_src.fd < 0)
                {
                        positive left = zstd_src.mem_len - zstd_src.mem_at;

                        if (!left)
                        {
                                zstd_src.eof = true;
                                break;
                        }
                        if (left > room)
                                left = room;
                        memory_copy(zstd_src.buf + zstd_src.have,
                                    zstd_src.mem + zstd_src.mem_at, left);
                        zstd_src.mem_at += left;
                        zstd_src.have += left;
                        continue;
                }

                got = system_read_retry((positive)zstd_src.fd,
                                        zstd_src.buf + zstd_src.have, room);
                if (got < 0)
                        return zstd_fail("zstd: read failed");
                if (!got)
                {
                        zstd_src.eof = true;
                        break;
                }
                zstd_src.have += (positive)got;
        }

        return zstd_src.have >= n;
}

static p8 address_to zstd_in_at(void)
{
        return zstd_src.buf + zstd_src.at;
}

static fn zstd_in_skip(positive n)
{
        zstd_src.at += n;
        zstd_src.have -= n;
}

static bool zstd_in_take(p8 address_to into, positive n)
{
        if (!zstd_in_need(n))
                return false;
        memory_copy(into, zstd_in_at(), n);
        zstd_in_skip(n);
        return true;
}

static bool zstd_bits_open(zstd_bits address_to b, p8 address_to src,
                           positive size)
{
        p8 last;

        if (!size)
                return zstd_fail("zstd empty bitstream");

        last = src[size - 1];
        if (!last)
                return zstd_fail("zstd bitstream missing the end mark");

        b->start = src;
        b->end = src + size;

        if (size >= 8)
        {
                b->ptr = src + size - 8;
                b->bits = zstd_get64(b->ptr);
                b->consumed = 8 - zstd_highbit32(last);
        }
        else
        {
                positive i;

                b->ptr = src;
                b->bits = src[0];
                for (i = 1; i < size; i++)
                        b->bits |= (p64)src[i] << (8 * i);
                b->consumed = (8 - (p8)size) * 8 +
                              (8 - zstd_highbit32(last));
        }

        return true;
}

static p64 zstd_bits_look(zstd_bits address_to b, p8 n)
{
        if (!n)
                return 0;
        return (b->bits >> ((64 - b->consumed - n) & 63)) &
               (((p64)1 << n) - 1);
}

static fn zstd_bits_skip(zstd_bits address_to b, p8 n)
{
        b->consumed += n;
}

static p64 zstd_bits_get(zstd_bits address_to b, p8 n)
{
        p64 value = zstd_bits_look(b, n);

        zstd_bits_skip(b, n);
        return value;
}

static bool zstd_bits_reload(zstd_bits address_to b)
{
        positive bytes;

        if (b->consumed > 64)
                return zstd_fail("zstd bitstream over-read");

        if (b->ptr == b->start)
        {
                if (b->consumed == 64)
                        return true;
                return b->consumed <= 64;
        }

        bytes = b->consumed >> 3;
        if (b->ptr - bytes < b->start)
                bytes = (positive)(b->ptr - b->start);
        b->ptr -= bytes;
        b->consumed -= bytes * 8;
        if ((positive)(b->end - b->ptr) >= 8)
                b->bits = zstd_get64(b->ptr);
        else
        {
                positive have = (positive)(b->end - b->ptr);
                positive i;

                b->bits = 0;
                for (i = 0; i < have; i++)
                        b->bits |= (p64)b->ptr[i] << (8 * i);
        }

        return true;
}

static bool zstd_bits_done(zstd_bits address_to b)
{
        return b->ptr == b->start && b->consumed == 64;
}

static p8 zstd_fse_symbol(zstd_fse address_to table, p16 address_to state,
                          zstd_bits address_to bits)
{
        zstd_fse_cell cell;

        if (!table->log)
                return table->rle;

        cell = table->cell[address_to state];
        address_to state =
            (p16)(cell.next + (p16)zstd_bits_get(bits, cell.bits));
        return cell.symbol;
}

static p8 zstd_fse_peek(zstd_fse address_to table, p16 state)
{
        if (!table->log)
                return table->rle;
        return table->cell[state].symbol;
}

static fn zstd_fse_step(zstd_fse address_to table, p16 address_to state,
                        zstd_bits address_to bits)
{
        zstd_fse_cell cell;

        if (!table->log)
                return;

        cell = table->cell[address_to state];
        address_to state =
            (p16)(cell.next + (p16)zstd_bits_get(bits, cell.bits));
}

static bool zstd_fse_build(zstd_fse address_to table, const bipolar address_to norm,
                           positive max_sym, p8 log)
{
        positive size = (positive)1 << log;
        positive high = size - 1;
        positive step = (size >> 1) + (size >> 3) + 3;
        positive mask = size - 1;
        positive pos = 0;
        p16 next[256];
        p8 symbol[ZSTD_FSE_MAX];
        positive s;
        positive u;

        if (log > 9 || size > ZSTD_FSE_MAX)
                return zstd_fail("zstd FSE table too large");

        memory_fill(symbol, 0xff, size);
        memory_fill(next, 0, sizeof(next));

        for (s = 0; s <= max_sym; s++)
                if (norm[s] == -1)
                {
                        symbol[high] = (p8)s;
                        high--;
                        next[s] = 1;
                }
                else
                        next[s] = (p16)(norm[s] < 0 ? 1 : norm[s]);

        for (s = 0; s <= max_sym; s++)
        {
                bipolar count = norm[s];
                bipolar i;

                if (count <= 0)
                        continue;
                for (i = 0; i < count; i++)
                {
                        while (pos > high)
                                pos = (pos + step) & mask;
                        symbol[pos] = (p8)s;
                        pos = (pos + step) & mask;
                }
        }

        if (pos)
                return zstd_fail("zstd FSE table did not fill");

        for (u = 0; u < size; u++)
        {
                p8 sym = symbol[u];
                p16 n = next[sym]++;
                p8 bits = (p8)(log - zstd_highbit32(n));

                table->cell[u].symbol = sym;
                table->cell[u].bits = bits;
                table->cell[u].next = (p16)((n << bits) - size);
        }

        table->log = log;
        table->rle = 0;
        table->valid = true;
        return true;
}

static fn zstd_fse_rle(zstd_fse address_to table, p8 symbol)
{
        table->log = 0;
        table->rle = symbol;
        table->valid = true;
}

static bool zstd_fse_read(p8 address_to src, positive src_len,
                          positive address_to used, bipolar address_to norm,
                          positive max_sym, p8 address_to log, p8 max_log)
{
        p64 bits = 0;
        positive left = 0;
        p8 address_to p = src;
        p8 address_to end = src + src_len;
        p8 table_log;
        positive remaining;
        positive threshold;
        p8 nb_bits;
        positive charnum = 0;
        bool previous0 = false;

        memory_fill(norm, 0, (max_sym + 1) * sizeof(norm[0]));

        if (!src_len)
                return zstd_fail("zstd truncated FSE header");

#define ZSTD_FSE_LOAD()                                                        \
        do                                                                     \
        {                                                                      \
                while (left < 32 && p < end)                                   \
                {                                                              \
                        bits |= (p64)address_to p << left;                     \
                        p++;                                                   \
                        left += 8;                                             \
                }                                                              \
        } while (0)

        ZSTD_FSE_LOAD();
        table_log = (p8)((bits & 15) + 5);
        bits >>= 4;
        left -= 4;
        if (table_log > max_log)
                return zstd_fail("zstd FSE accuracy log too large");

        remaining = ((positive)1 << table_log) + 1;
        threshold = (positive)1 << table_log;
        nb_bits = table_log + 1;

        while (remaining > 1 && charnum <= max_sym)
        {
                if (previous0)
                {
                        positive repeats;

                        ZSTD_FSE_LOAD();
                        while ((bits & 0xffff) == 0xffff)
                        {
                                charnum += 24;
                                if (charnum > max_sym + 1)
                                        return zstd_fail("zstd FSE zero run");
                                bits >>= 16;
                                left -= 16;
                                ZSTD_FSE_LOAD();
                        }
                        while ((bits & 3) == 3)
                        {
                                charnum += 3;
                                if (charnum > max_sym + 1)
                                        return zstd_fail("zstd FSE zero run");
                                bits >>= 2;
                                left -= 2;
                                ZSTD_FSE_LOAD();
                        }
                        repeats = (positive)(bits & 3);
                        bits >>= 2;
                        left -= 2;
                        charnum += repeats;
                        if (charnum > max_sym + 1)
                                return zstd_fail("zstd FSE zero run");
                        previous0 = false;
                        continue;
                }

                {
                        positive max = (2 * threshold - 1) - remaining;
                        positive count;
                        p32 mask = ((p32)1 << nb_bits) - 1;
                        p32 low = ((p32)1 << (nb_bits - 1)) - 1;

                        ZSTD_FSE_LOAD();
                        if ((bits & low) < max)
                        {
                                count = (positive)(bits & low);
                                bits >>= (nb_bits - 1);
                                left -= (nb_bits - 1);
                        }
                        else
                        {
                                count = (positive)(bits & mask);
                                if (count >= threshold)
                                        count -= max;
                                bits >>= nb_bits;
                                left -= nb_bits;
                        }

                        count--;
                        remaining -= count >= ((positive)-1 >> 1)
                                         ? (positive)(-(bipolar)count)
                                         : count;
                        if (charnum > max_sym)
                                return zstd_fail("zstd FSE too many symbols");
                        norm[charnum++] = (bipolar)count;
                        previous0 = !count;
                        while (remaining < threshold)
                        {
                                nb_bits--;
                                threshold >>= 1;
                        }
                }
        }

        if (remaining != 1)
                return zstd_fail("zstd FSE counts do not sum");

        address_to log = table_log;
        address_to used = (positive)(p - src) - (left / 8);
        if (address_to used > src_len)
                address_to used = src_len;
        return true;
#undef ZSTD_FSE_LOAD
}

static fn zstd_fse_defaults(void)
{
        if (zstd_fse_ready)
                return;
        zstd_fse_build(address_of zstd_ll_def, zstd_ll_default, 35, 6);
        zstd_fse_build(address_of zstd_ml_def, zstd_ml_default, 52, 6);
        zstd_fse_build(address_of zstd_of_def, zstd_of_default, 28, 5);
        zstd_fse_ready = true;
}

static bool zstd_huff_from_weights(zstd_huff address_to huff, p8 address_to weight,
                                   positive last)
{
        p32 rank[13];
        positive sum = 0;
        positive s;
        p8 max_bits;
        p32 rest;
        p8 last_weight;
        positive start;
        p8 w;

        memory_fill(rank, 0, sizeof(rank));

        for (s = 0; s < last; s++)
        {
                if (weight[s] > 12)
                        return zstd_fail("zstd Huffman weight too large");
                if (weight[s])
                        sum += (positive)1 << (weight[s] - 1);
        }

        if (!sum)
                return zstd_fail("zstd Huffman weights are empty");

        max_bits = (p8)(zstd_highbit32(sum) + 1);
        rest = ((p32)1 << max_bits) - (p32)sum;
        if (!rest || (rest & (rest - 1)))
                return zstd_fail("zstd Huffman weights are not a power of two");
        last_weight = (p8)(zstd_highbit32(rest) + 1);
        if (last_weight > 12 || last >= 256)
                return zstd_fail("zstd Huffman last weight");
        weight[last] = last_weight;
        if (max_bits > 11)
                return zstd_fail("zstd Huffman deeper than 11");

        huff->max_bits = max_bits;
        start = 0;
        for (w = 1; w <= 12; w++)
        {
                p8 bits = (p8)(max_bits + 1 - w);
                positive span;

                if (bits > max_bits)
                        continue;
                span = (positive)1 << (max_bits - bits);
                for (s = 0; s <= last; s++)
                {
                        positive i;

                        if (weight[s] != w)
                                continue;
                        if (start + span > ((positive)1 << max_bits))
                                return zstd_fail("zstd Huffman table overflow");
                        for (i = 0; i < span; i++)
                        {
                                huff->symbol[start + i] = (p8)s;
                                huff->bits[start + i] = bits;
                        }
                        start += span;
                }
        }

        if (start != ((positive)1 << max_bits))
                return zstd_fail("zstd Huffman table did not fill");
        huff->valid = true;
        return true;
}

static bool zstd_huff_read(p8 address_to src, positive src_len,
                           positive address_to used, zstd_huff address_to huff)
{
        p8 header;
        p8 weight[256];
        positive last;

        if (!src_len)
                return zstd_fail("zstd truncated Huffman header");

        header = src[0];
        memory_fill(weight, 0, sizeof(weight));

        if (header >= 128)
        {
                positive symbols = header - 127;
                positive bytes = (symbols + 1) / 2;
                positive i;

                if (1 + bytes > src_len)
                        return zstd_fail("zstd truncated Huffman weights");
                for (i = 0; i < symbols; i++)
                        weight[i] = (i & 1) ? (src[1 + i / 2] & 15)
                                            : (src[1 + i / 2] >> 4);
                last = symbols;
                address_to used = 1 + bytes;
        }
        else
        {
                bipolar norm[256];
                zstd_fse table;
                zstd_bits bits;
                p16 state1;
                p16 state2;
                positive ncount;
                p8 log;
                positive n = 0;

                if (!header)
                        return zstd_fail("zstd empty Huffman FSE header");
                if (!zstd_fse_read(src + 1, src_len - 1, address_of ncount, norm,
                                   255, address_of log, 6))
                        return false;
                if (!zstd_fse_build(address_of table, norm, 255, log))
                        return false;
                if (1 + ncount + header > src_len)
                        return zstd_fail("zstd truncated Huffman FSE stream");
                if (!zstd_bits_open(address_of bits, src + 1 + ncount, header))
                        return false;
                state1 = (p16)zstd_bits_get(address_of bits, table.log);
                if (!zstd_bits_reload(address_of bits))
                        return false;
                state2 = (p16)zstd_bits_get(address_of bits, table.log);
                if (!zstd_bits_reload(address_of bits))
                        return false;

                for (;;)
                {
                        if (n >= 255)
                                return zstd_fail("zstd Huffman too many weights");
                        weight[n++] = zstd_fse_peek(address_of table, state1);
                        zstd_fse_step(address_of table, address_of state1,
                                      address_of bits);
                        if (bits.consumed >= 64 - 7)
                        {
                                if (!zstd_bits_reload(address_of bits))
                                        return false;
                                if (bits.ptr == bits.start &&
                                    bits.consumed + table.cell[state1].bits > 64)
                                        break;
                        }
                        if (n >= 255)
                                return zstd_fail("zstd Huffman too many weights");
                        weight[n++] = zstd_fse_peek(address_of table, state2);
                        zstd_fse_step(address_of table, address_of state2,
                                      address_of bits);
                        if (bits.consumed >= 64 - 7)
                        {
                                if (!zstd_bits_reload(address_of bits))
                                        return false;
                                if (bits.ptr == bits.start &&
                                    bits.consumed + table.cell[state2].bits > 64)
                                        break;
                        }
                }

                last = n;
                address_to used = 1 + ncount + header;
        }

        if (!last)
                return zstd_fail("zstd Huffman no weights");
        return zstd_huff_from_weights(huff, weight, last - 1);
}

static bool zstd_huff_stream(zstd_huff address_to huff, p8 address_to into,
                             positive need, p8 address_to src, positive size)
{
        zstd_bits bits;
        positive n = 0;

        if (!zstd_bits_open(address_of bits, src, size))
                return false;

        while (n < need)
        {
                p8 bits_n;
                p64 index;

                if (bits.consumed > 64 - huff->max_bits)
                {
                        if (!zstd_bits_reload(address_of bits))
                                return false;
                }
                index = zstd_bits_look(address_of bits, huff->max_bits);
                bits_n = huff->bits[index];
                if (!bits_n)
                        return zstd_fail("zstd Huffman invalid code");
                into[n++] = huff->symbol[index];
                zstd_bits_skip(address_of bits, bits_n);
        }

        if (bits.consumed > 64)
                return zstd_fail("zstd Huffman over-read");
        if (!zstd_bits_reload(address_of bits))
                return false;
        return true;
}

static bool zstd_huff_four(zstd_huff address_to huff, p8 address_to into,
                           positive need, p8 address_to src, positive size)
{
        p16 s1;
        p16 s2;
        p16 s3;
        p16 s4;
        positive n1;
        positive n2;
        positive n3;
        positive n4;
        p8 address_to p;

        if (size < 6)
                return zstd_fail("zstd Huffman jump table truncated");

        s1 = zstd_get16(src);
        s2 = zstd_get16(src + 2);
        s3 = zstd_get16(src + 4);
        if ((positive)s1 + s2 + s3 + 6 >= size)
                return zstd_fail("zstd Huffman stream sizes");
        s4 = (p16)(size - 6 - s1 - s2 - s3);
        n1 = (need + 3) / 4;
        n2 = n1;
        n3 = n1;
        n4 = need - 3 * n1;
        p = src + 6;
        if (!zstd_huff_stream(huff, into, n1, p, s1))
                return false;
        p += s1;
        if (!zstd_huff_stream(huff, into + n1, n2, p, s2))
                return false;
        p += s2;
        if (!zstd_huff_stream(huff, into + n1 + n2, n3, p, s3))
                return false;
        p += s3;
        return zstd_huff_stream(huff, into + n1 + n2 + n3, n4, p, s4);
}

static bool zstd_emit(p8 address_to p, positive n)
{
        if (!n)
                return true;

        zstd_decoded += n;
        if (zstd_hashing)
                zstd_xxh_add(address_of zstd_hash, p, n);

        if (zstd_out_mem)
        {
                if (zstd_out_used + n > zstd_out_cap)
                        return zstd_fail("zstd output larger than the destination");
                memory_copy(zstd_out_mem + zstd_out_used, p, n);
                zstd_out_used += n;
                return true;
        }

        if (zstd_out_fd >= 0)
        {
                if (system_write_all((positive)zstd_out_fd, p, n) != n)
                        return zstd_fail("zstd: write failed");
        }

        return true;
}

static bool zstd_window_room(positive need)
{
        if (zstd_pos + need <= zstd_window_cap)
                return true;

        if (zstd_pos > zstd_keep)
        {
                if (!zstd_emit(zstd_window, zstd_pos - zstd_keep))
                        return false;
                memory_copy(zstd_window, zstd_window + zstd_pos - zstd_keep,
                            zstd_keep);
                zstd_pos = zstd_keep;
        }

        return zstd_pos + need <= zstd_window_cap
                   ? true
                   : zstd_fail("zstd window overflow");
}

static bool zstd_put(p8 address_to p, positive n)
{
        if (!zstd_window_room(n))
                return false;
        memory_copy(zstd_window + zstd_pos, p, n);
        zstd_pos += n;
        return true;
}

static bool zstd_put_fill(p8 value, positive n)
{
        if (!zstd_window_room(n))
                return false;
        memory_fill(zstd_window + zstd_pos, value, n);
        zstd_pos += n;
        return true;
}

static bool zstd_put_match(positive offset, positive n)
{
        if (!offset || offset > zstd_pos + (zstd_decoded - zstd_pos))
                return zstd_fail("zstd match offset");
        if (offset > zstd_window_size && zstd_window_size)
                return zstd_fail("zstd match past the window");
        if (!zstd_window_room(n))
                return false;
        memory_copy_match(zstd_window + zstd_pos, offset, n);
        zstd_pos += n;
        return true;
}

static bool zstd_seq_table(zstd_fse address_to table, zstd_fse address_to prev,
                           p8 mode, p8 address_to src, positive src_len,
                           positive address_to used, const bipolar address_to def,
                           positive max_sym, p8 def_log, p8 max_log)
{
        address_to used = 0;

        if (mode == 0)
        {
                zstd_fse_defaults();
                memory_copy(table, def == zstd_ll_default ? address_of zstd_ll_def
                                  : def == zstd_ml_default ? address_of zstd_ml_def
                                                           : address_of zstd_of_def,
                            sizeof(zstd_fse));
                table->valid = true;
                return true;
        }
        if (mode == 1)
        {
                if (!src_len)
                        return zstd_fail("zstd truncated RLE table");
                zstd_fse_rle(table, src[0]);
                address_to used = 1;
                return true;
        }
        if (mode == 2)
        {
                bipolar norm[256];
                p8 log;
                positive ncount;

                if (!zstd_fse_read(src, src_len, address_of ncount, norm, max_sym,
                                   address_of log, max_log))
                        return false;
                if (!zstd_fse_build(table, norm, max_sym, log))
                        return false;
                address_to used = ncount;
                return true;
        }
        if (mode == 3)
        {
                if (!prev->valid)
                        return zstd_fail("zstd repeat FSE with no previous table");
                memory_copy(table, prev, sizeof(zstd_fse));
                return true;
        }

        return zstd_fail("zstd unknown FSE mode");
}

static bool zstd_literals(p8 address_to src, positive src_len,
                          positive address_to used, p8 address_to lit,
                          positive address_to lit_len)
{
        p8 type;
        p8 format;
        positive regen = 0;
        positive compressed = 0;
        positive header = 0;
        p8 address_to body;

        if (!src_len)
                return zstd_fail("zstd truncated literals");

        type = src[0] & 3;
        format = (src[0] >> 2) & 3;

        if (type <= 1)
        {
                if (format == 0 || format == 2)
                {
                        header = 1;
                        regen = src[0] >> 3;
                }
                else if (format == 1)
                {
                        if (src_len < 2)
                                return zstd_fail("zstd truncated literals size");
                        header = 2;
                        regen = zstd_get16(src) >> 4;
                }
                else
                {
                        if (src_len < 3)
                                return zstd_fail("zstd truncated literals size");
                        header = 3;
                        regen = zstd_get24(src) >> 4;
                }
                if (regen > ZSTD_BLOCK_MAX)
                        return zstd_fail("zstd literals larger than a block");
                body = src + header;
                if (type == 0)
                {
                        if (header + regen > src_len)
                                return zstd_fail("zstd truncated raw literals");
                        memory_copy(lit, body, regen);
                        address_to used = header + regen;
                }
                else
                {
                        if (header + 1 > src_len)
                                return zstd_fail("zstd truncated RLE literals");
                        memory_fill(lit, body[0], regen);
                        address_to used = header + 1;
                }
                address_to lit_len = regen;
                return true;
        }

        {
                p32 pack = src_len >= 4 ? zstd_get32(src) : zstd_get24(src);

                if (format <= 1)
                {
                        header = 3;
                        regen = (pack >> 4) & 0x3ff;
                        compressed = (pack >> 14) & 0x3ff;
                }
                else if (format == 2)
                {
                        header = 4;
                        if (src_len < 4)
                                return zstd_fail("zstd truncated literals header");
                        regen = (pack >> 4) & 0x3fff;
                        compressed = pack >> 18;
                }
                else
                {
                        header = 5;
                        if (src_len < 5)
                                return zstd_fail("zstd truncated literals header");
                        regen = (pack >> 4) & 0x3ffff;
                        compressed = (pack >> 22) + ((positive)src[4] << 10);
                }
        }

        if (regen > ZSTD_BLOCK_MAX || compressed + header > src_len)
                return zstd_fail("zstd compressed literals size");

        body = src + header;
        if (type == 2)
        {
                positive tree;

                if (!zstd_huff_read(body, compressed, address_of tree,
                                    address_of zstd_lit_huff))
                        return false;
                if (tree > compressed)
                        return zstd_fail("zstd Huffman tree larger than literals");
                body += tree;
                compressed -= tree;
        }
        else if (!zstd_lit_huff.valid)
                return zstd_fail("zstd treeless literals with no Huffman table");

        if (!format)
        {
                if (!zstd_huff_stream(address_of zstd_lit_huff, lit, regen, body,
                                      compressed))
                        return false;
        }
        else
        {
                if (regen < 6)
                        return zstd_fail("zstd 4-stream literals too small");
                if (!zstd_huff_four(address_of zstd_lit_huff, lit, regen, body,
                                    compressed))
                        return false;
        }

        address_to used = header + (type == 2
                                        ? (positive)(body - (src + header)) +
                                              compressed
                                        : compressed);
        /* header + original compressed payload */
        address_to used = header + (type == 2 ? 0 : 0);
        {
                p32 pack = src_len >= 4 ? zstd_get32(src) : 0;
                positive payload;

                if (format <= 1)
                        payload = (pack >> 14) & 0x3ff;
                else if (format == 2)
                        payload = pack >> 18;
                else
                        payload = (pack >> 22) + ((positive)src[4] << 10);
                address_to used = header + payload;
        }
        address_to lit_len = regen;
        return true;
}
