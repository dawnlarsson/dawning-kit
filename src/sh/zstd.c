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

#define ZSTD_BITS_UNFINISHED 0
#define ZSTD_BITS_END_BUFFER 1
#define ZSTD_BITS_COMPLETED 2
#define ZSTD_BITS_OVERFLOW 3

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
        p8 address_to limit;
        p8 address_to last;
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
static p64 zstd_fcs;
static bool zstd_have_fcs;
static p32 zstd_rep[3];
static zstd_huff zstd_lit_huff;
static zstd_fse zstd_ll;
static zstd_fse zstd_of;
static zstd_fse zstd_ml;
static zstd_fse zstd_ll_prev;
static zstd_fse zstd_of_prev;
static zstd_fse zstd_ml_prev;
static p8 zstd_lit_buf[ZSTD_BLOCK_MAX + 64];
static p8 zstd_comp[ZSTD_BLOCK_MAX + 32];
static bool zstd_fse_ready;
static zstd_fse zstd_ll_def;
static zstd_fse zstd_of_def;
static zstd_fse zstd_ml_def;
static positive zstd_block_limit;

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

        return zstd_src.have >= n ? true : zstd_fail("zstd truncated input");
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
        if (!n)
                return true;
        if (!zstd_in_need(n))
                return false;
        memory_copy(into, zstd_in_at(), n);
        zstd_in_skip(n);
        return true;
}

static bool zstd_in_skip_bytes(positive n)
{
        while (n)
        {
                positive chunk;

                if (!zstd_src.have && !zstd_in_need(1) && n)
                        return false;
                chunk = zstd_src.have;
                if (chunk > n)
                        chunk = n;
                zstd_in_skip(chunk);
                n -= chunk;
        }

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
        b->last = src + size;
        b->limit = src + 8;

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
                b->consumed = (8 - size) * 8 + (8 - zstd_highbit32(last));
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

static p8 zstd_bits_reload(zstd_bits address_to b)
{
        if (b->consumed > 64)
                return ZSTD_BITS_OVERFLOW;

        if (b->ptr >= b->limit)
        {
                b->ptr -= b->consumed >> 3;
                b->consumed &= 7;
                b->bits = zstd_get64(b->ptr);
                return ZSTD_BITS_UNFINISHED;
        }

        if (b->ptr == b->start)
        {
                if (b->consumed < 64)
                        return ZSTD_BITS_END_BUFFER;
                return ZSTD_BITS_COMPLETED;
        }

        {
                positive bytes = b->consumed >> 3;
                p8 status = ZSTD_BITS_UNFINISHED;

                if (b->ptr - bytes < b->start)
                {
                        bytes = (positive)(b->ptr - b->start);
                        status = ZSTD_BITS_END_BUFFER;
                }
                b->ptr -= bytes;
                b->consumed -= bytes * 8;
                if ((positive)(b->last - b->ptr) >= 8)
                        b->bits = zstd_get64(b->ptr);
                else
                {
                        positive have = (positive)(b->last - b->ptr);
                        positive i;

                        b->bits = 0;
                        for (i = 0; i < have; i++)
                                b->bits |= (p64)b->ptr[i] << (8 * i);
                }
                return status;
        }
}

static bool zstd_bits_ok(zstd_bits address_to b)
{
        return zstd_bits_reload(b) != ZSTD_BITS_OVERFLOW;
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

static p8 zstd_fse_symbol(zstd_fse address_to table, p16 address_to state,
                          zstd_bits address_to bits)
{
        p8 symbol = zstd_fse_peek(table, address_to state);

        zstd_fse_step(table, state, bits);
        return symbol;
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

        if (!log)
                return zstd_fail("zstd FSE table log is zero");
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
                        next[s] = (p16)(norm[s] < 0 ? 0 : norm[s]);

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
                p8 bits;

                if (!n)
                        return zstd_fail("zstd FSE empty cell");
                bits = (p8)(log - zstd_highbit32(n));
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
        p8 pad[520];
        p8 address_to ip;
        p8 address_to iend;
        p32 bit_stream;
        positive bit_count;
        p8 table_log;
        positive remaining;
        positive threshold;
        p8 nb_bits;
        positive charnum = 0;
        bool previous0 = false;

        if (!src_len)
                return zstd_fail("zstd truncated FSE header");
        if (src_len > 512)
                src_len = 512;

        memory_fill(pad, 0, sizeof(pad));
        memory_copy(pad, src, src_len);
        memory_fill(norm, 0, (max_sym + 1) * sizeof(norm[0]));

        ip = pad;
        iend = pad + src_len + 8;
        bit_stream = zstd_get32(ip);
        bit_count = 0;

        table_log = (p8)((bit_stream & 15) + 5);
        if (table_log > max_log)
                return zstd_fail("zstd FSE accuracy log too large");
        bit_stream >>= 4;
        bit_count = 4;
        remaining = ((positive)1 << table_log) + 1;
        threshold = (positive)1 << table_log;
        nb_bits = table_log + 1;

        for (;;)
        {
                if (previous0)
                {
                        while ((bit_stream & 0xffffu) == 0xffffu)
                        {
                                charnum += 24;
                                if (charnum > max_sym + 1)
                                        return zstd_fail("zstd FSE zero run");
                                bit_stream >>= 16;
                                bit_count += 16;
                                if (ip + 4 <= iend)
                                {
                                        ip += bit_count >> 3;
                                        bit_count &= 7;
                                        bit_stream = zstd_get32(ip) >> bit_count;
                                }
                        }
                        while ((bit_stream & 3) == 3)
                        {
                                charnum += 3;
                                if (charnum > max_sym + 1)
                                        return zstd_fail("zstd FSE zero run");
                                bit_stream >>= 2;
                                bit_count += 2;
                        }
                        charnum += bit_stream & 3;
                        bit_count += 2;
                        bit_stream >>= 2;
                        if (charnum > max_sym + 1)
                                return zstd_fail("zstd FSE zero run");
                        previous0 = false;
                        if (ip + 4 <= iend)
                        {
                                ip += bit_count >> 3;
                                bit_count &= 7;
                                bit_stream = zstd_get32(ip) >> bit_count;
                        }
                        continue;
                }

                {
                        positive max = (2 * threshold - 1) - remaining;
                        positive count;
                        p32 low = threshold - 1;
                        p32 mask = (threshold << 1) - 1;

                        if ((bit_stream & low) < max)
                        {
                                count = bit_stream & low;
                                bit_count += nb_bits - 1;
                                bit_stream >>= (nb_bits - 1);
                        }
                        else
                        {
                                count = bit_stream & mask;
                                if (count >= threshold)
                                        count -= max;
                                bit_count += nb_bits;
                                bit_stream >>= nb_bits;
                        }

                        {
                                bipolar prob = (bipolar)count - 1;

                                remaining -= prob < 0 ? 1 : (positive)prob;
                                if (charnum > max_sym)
                                        return zstd_fail("zstd FSE too many symbols");
                                norm[charnum++] = prob;
                                previous0 = !prob;
                        }
                        if (remaining <= 1)
                                break;
                        while (remaining < threshold)
                        {
                                nb_bits--;
                                threshold >>= 1;
                        }
                        if (ip + 4 <= iend)
                        {
                                ip += bit_count >> 3;
                                bit_count &= 7;
                                bit_stream = zstd_get32(ip) >> bit_count;
                        }
                }
        }

        if (remaining != 1)
                return zstd_fail("zstd FSE counts do not sum");

        ip += (bit_count + 7) >> 3;
        address_to used = (positive)(ip - pad);
        if (address_to used > src_len)
                return zstd_fail("zstd FSE header over-read");
        address_to log = table_log;
        return true;
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

static bool zstd_fse_unpack(zstd_fse address_to table, p8 address_to into,
                            positive max_out, positive address_to produced,
                            p8 address_to src, positive size)
{
        zstd_bits bits;
        p16 state1;
        p16 state2;
        positive n = 0;

        if (!zstd_bits_open(address_of bits, src, size))
                return false;
        state1 = (p16)zstd_bits_get(address_of bits, table->log);
        state2 = (p16)zstd_bits_get(address_of bits, table->log);
        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                return zstd_fail("zstd Huffman FSE overflow");

        for (;;)
        {
                if (n + 2 > max_out)
                        return zstd_fail("zstd Huffman too many weights");
                into[n++] = zstd_fse_symbol(table, address_of state1,
                                            address_of bits);
                if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                {
                        into[n++] = zstd_fse_symbol(table, address_of state2,
                                                    address_of bits);
                        break;
                }
                into[n++] = zstd_fse_symbol(table, address_of state2,
                                            address_of bits);
                if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                {
                        into[n++] = zstd_fse_symbol(table, address_of state1,
                                                    address_of bits);
                        break;
                }
        }

        address_to produced = n;
        return true;
}

static bool zstd_huff_from_weights(zstd_huff address_to huff, p8 address_to weight,
                                   positive provided)
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

        for (s = 0; s < provided; s++)
        {
                if (weight[s] > 11)
                        return zstd_fail("zstd Huffman weight too large");
                if (weight[s])
                {
                        sum += (positive)1 << (weight[s] - 1);
                        rank[weight[s]]++;
                }
        }

        if (!sum)
                return zstd_fail("zstd Huffman weights are empty");

        max_bits = (p8)(zstd_highbit32(sum) + 1);
        rest = ((p32)1 << max_bits) - (p32)sum;
        if (!rest || (rest & (rest - 1)))
                return zstd_fail("zstd Huffman weights are not a power of two");
        last_weight = (p8)(zstd_highbit32(rest) + 1);
        if (last_weight > 11 || provided >= 256)
                return zstd_fail("zstd Huffman last weight");
        weight[provided] = last_weight;
        rank[last_weight]++;
        if (max_bits > 11)
                return zstd_fail("zstd Huffman deeper than 11");
        if (rank[1] < 2 || (rank[1] & 1))
                return zstd_fail("zstd Huffman rank-1 weights");

        huff->max_bits = max_bits;
        start = 0;
        for (w = 1; w <= 12; w++)
        {
                p8 bits = (p8)(max_bits + 1 - w);
                positive span = (positive)1 << (w - 1);

                if (w > max_bits + 1)
                        continue;
                for (s = 0; s <= provided; s++)
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
        positive provided;

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
                for (i = 0; i < symbols; i += 2)
                {
                        weight[i] = src[1 + i / 2] >> 4;
                        if (i + 1 < symbols)
                                weight[i + 1] = src[1 + i / 2] & 15;
                }
                provided = symbols;
                address_to used = 1 + bytes;
        }
        else
        {
                bipolar norm[256];
                zstd_fse table;
                positive ncount;
                p8 log;
                positive unpacked;

                if (!header)
                        return zstd_fail("zstd empty Huffman FSE header");
                if (1 + header > src_len)
                        return zstd_fail("zstd truncated Huffman FSE header");
                if (!zstd_fse_read(src + 1, header, address_of ncount, norm, 255,
                                   address_of log, 6))
                        return false;
                if (!zstd_fse_build(address_of table, norm, 255, log))
                        return false;
                if (ncount >= header)
                        return zstd_fail("zstd Huffman FSE stream empty");
                if (!zstd_fse_unpack(address_of table, weight, 255,
                                     address_of unpacked, src + 1 + ncount,
                                     header - ncount))
                        return false;
                provided = unpacked;
                address_to used = 1 + header;
        }

        if (!provided)
                return zstd_fail("zstd Huffman no weights");
        return zstd_huff_from_weights(huff, weight, provided);
}

static bool zstd_huff_stream(zstd_huff address_to huff, p8 address_to into,
                             positive need, p8 address_to src, positive size)
{
        zstd_bits bits;
        positive n = 0;

        if (!need)
                return true;
        if (!zstd_bits_open(address_of bits, src, size))
                return false;

        while (n < need)
        {
                p8 bits_n;
                p64 index;

                if (bits.consumed > 64 - huff->max_bits)
                {
                        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                                return zstd_fail("zstd Huffman over-read");
                }
                index = zstd_bits_look(address_of bits, huff->max_bits);
                bits_n = huff->bits[index];
                if (!bits_n)
                        return zstd_fail("zstd Huffman invalid code");
                into[n++] = huff->symbol[index];
                zstd_bits_skip(address_of bits, bits_n);
        }

        return zstd_bits_ok(address_of bits);
}

static bool zstd_huff_four(zstd_huff address_to huff, p8 address_to into,
                           positive need, p8 address_to src, positive size)
{
        positive s1;
        positive s2;
        positive s3;
        positive s4;
        positive n1;
        positive n4;
        p8 address_to p;

        if (size < 10)
                return zstd_fail("zstd Huffman jump table truncated");

        s1 = zstd_get16(src);
        s2 = zstd_get16(src + 2);
        s3 = zstd_get16(src + 4);
        if (6 + s1 + s2 + s3 > size)
                return zstd_fail("zstd Huffman stream sizes");
        s4 = size - 6 - s1 - s2 - s3;
        if (!s1 || !s2 || !s3 || !s4)
                return zstd_fail("zstd Huffman empty stream");
        n1 = (need + 3) / 4;
        n4 = need - 3 * n1;
        p = src + 6;
        if (!zstd_huff_stream(huff, into, n1, p, s1))
                return false;
        p += s1;
        if (!zstd_huff_stream(huff, into + n1, n1, p, s2))
                return false;
        p += s2;
        if (!zstd_huff_stream(huff, into + n1 + n1, n1, p, s3))
                return false;
        p += s3;
        return zstd_huff_stream(huff, into + 3 * n1, n4, p, s4);
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
                if (system_write_all((positive)zstd_out_fd, p, n) != (bipolar)n)
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
        if (!n)
                return true;
        if (!zstd_window_room(n))
                return false;
        memory_copy(zstd_window + zstd_pos, p, n);
        if (!zstd_emit(zstd_window + zstd_pos, n))
                return false;
        zstd_pos += n;
        return true;
}

static bool zstd_put_fill(p8 value, positive n)
{
        if (!n)
                return true;
        if (!zstd_window_room(n))
                return false;
        memory_fill(zstd_window + zstd_pos, value, n);
        if (!zstd_emit(zstd_window + zstd_pos, n))
                return false;
        zstd_pos += n;
        return true;
}

static bool zstd_put_match(positive offset, positive n)
{
        if (!offset || offset > zstd_pos)
                return zstd_fail("zstd match offset");
        if (zstd_window_size && offset > zstd_window_size)
                return zstd_fail("zstd match past the window");
        if (!zstd_window_room(n))
                return false;
        memory_copy_match(zstd_window + zstd_pos, offset, n);
        if (!zstd_emit(zstd_window + zstd_pos, n))
                return false;
        zstd_pos += n;
        return true;
}

static bool zstd_seq_table(zstd_fse address_to table, zstd_fse address_to prev,
                           p8 mode, p8 address_to src, positive src_len,
                           positive address_to used, p8 kind, positive max_sym,
                           p8 max_log)
{
        address_to used = 0;

        if (mode == 0)
        {
                zstd_fse_defaults();
                if (kind == 0)
                        memory_copy(table, address_of zstd_ll_def, sizeof(zstd_fse));
                else if (kind == 1)
                        memory_copy(table, address_of zstd_of_def, sizeof(zstd_fse));
                else
                        memory_copy(table, address_of zstd_ml_def, sizeof(zstd_fse));
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
                if (regen > zstd_block_limit)
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
                p32 pack;

                if (format <= 1)
                {
                        if (src_len < 3)
                                return zstd_fail("zstd truncated literals header");
                        pack = zstd_get24(src);
                        header = 3;
                        regen = (pack >> 4) & 0x3ff;
                        compressed = (pack >> 14) & 0x3ff;
                }
                else if (format == 2)
                {
                        if (src_len < 4)
                                return zstd_fail("zstd truncated literals header");
                        pack = zstd_get32(src);
                        header = 4;
                        regen = (pack >> 4) & 0x3fff;
                        compressed = pack >> 18;
                }
                else
                {
                        if (src_len < 5)
                                return zstd_fail("zstd truncated literals header");
                        pack = zstd_get32(src);
                        header = 5;
                        regen = (pack >> 4) & 0x3ffff;
                        compressed = (pack >> 22) + ((positive)src[4] << 10);
                }
        }

        if (regen > zstd_block_limit || header + compressed > src_len)
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

        address_to used = header + (positive)(body - (src + header)) + compressed;
        address_to lit_len = regen;
        return true;
}

static bool zstd_repeat_offset(positive of_code, p64 extra, positive lit_len,
                               positive address_to offset)
{
        positive value;
        positive temp;

        if (of_code >= 2)
        {
                value = (positive)(((positive)1 << of_code) + extra);
                address_to offset = value - 3;
                zstd_rep[2] = zstd_rep[1];
                zstd_rep[1] = zstd_rep[0];
                zstd_rep[0] = (p32)address_to offset;
                return address_to offset != 0;
        }

        if (!of_code)
        {
                address_to offset = zstd_rep[lit_len == 0];
                zstd_rep[1] = zstd_rep[lit_len != 0];
                zstd_rep[0] = (p32)address_to offset;
                return address_to offset != 0;
        }

        value = 1 + (lit_len == 0) + (positive)extra;
        temp = value == 3 ? zstd_rep[0] - 1 : zstd_rep[value];
        if (!temp)
                return zstd_fail("zstd repeat offset is zero");
        if (value != 1)
                zstd_rep[2] = zstd_rep[1];
        zstd_rep[1] = zstd_rep[0];
        zstd_rep[0] = (p32)temp;
        address_to offset = temp;
        return true;
}

static bool zstd_sequences(p8 address_to src, positive src_len, p8 address_to lit,
                           positive lit_len)
{
        p8 address_to p = src;
        p8 address_to stop = src + src_len;
        positive nseq;
        p8 modes;
        positive used;
        zstd_bits bits;
        p16 state_ll;
        p16 state_of;
        p16 state_ml;
        positive lit_at = 0;
        positive i;

        if (p >= stop)
                return zstd_fail("zstd truncated sequences");

        if (p[0] < 128)
        {
                nseq = p[0];
                p += 1;
        }
        else if (p[0] < 255)
        {
                if (p + 2 > stop)
                        return zstd_fail("zstd truncated sequence count");
                nseq = ((positive)(p[0] - 128) << 8) + p[1];
                p += 2;
        }
        else
        {
                if (p + 3 > stop)
                        return zstd_fail("zstd truncated sequence count");
                nseq = 0x7f00u + zstd_get16(p + 1);
                p += 3;
        }

        if (!nseq)
        {
                if (p != stop)
                        return zstd_fail("zstd extra bytes after no sequences");
                return zstd_put(lit, lit_len);
        }

        if (p >= stop)
                return zstd_fail("zstd truncated sequence tables");
        modes = p[0];
        p += 1;
        if (modes & 3)
                return zstd_fail("zstd reserved sequence bits");

        if (!zstd_seq_table(address_of zstd_ll, address_of zstd_ll_prev,
                            (p8)(modes >> 6), p, (positive)(stop - p),
                            address_of used, 0, 35, 9))
                return false;
        p += used;
        if (!zstd_seq_table(address_of zstd_of, address_of zstd_of_prev,
                            (p8)((modes >> 4) & 3), p, (positive)(stop - p),
                            address_of used, 1, 31, 8))
                return false;
        p += used;
        if (!zstd_seq_table(address_of zstd_ml, address_of zstd_ml_prev,
                            (p8)((modes >> 2) & 3), p, (positive)(stop - p),
                            address_of used, 2, 52, 9))
                return false;
        p += used;

        memory_copy(address_of zstd_ll_prev, address_of zstd_ll, sizeof(zstd_fse));
        memory_copy(address_of zstd_of_prev, address_of zstd_of, sizeof(zstd_fse));
        memory_copy(address_of zstd_ml_prev, address_of zstd_ml, sizeof(zstd_fse));

        if (p >= stop)
                return zstd_fail("zstd truncated sequence bitstream");
        if (!zstd_bits_open(address_of bits, p, (positive)(stop - p)))
                return false;

        state_ll = (p16)zstd_bits_get(address_of bits, zstd_ll.log);
        state_of = (p16)zstd_bits_get(address_of bits, zstd_of.log);
        state_ml = (p16)zstd_bits_get(address_of bits, zstd_ml.log);
        if (!zstd_bits_ok(address_of bits))
                return false;

        for (i = 0; i < nseq; i++)
        {
                p8 ll_code = zstd_fse_peek(address_of zstd_ll, state_ll);
                p8 ml_code = zstd_fse_peek(address_of zstd_ml, state_ml);
                p8 of_code = zstd_fse_peek(address_of zstd_of, state_of);
                p8 ll_bits;
                p8 ml_bits;
                p64 of_extra;
                p64 ml_extra;
                p64 ll_extra;
                positive litlen;
                positive match;
                positive offset;

                if (ll_code > 35 || ml_code > 52 || of_code > 31)
                        return zstd_fail("zstd sequence code");

                ll_bits = zstd_ll_bits[ll_code];
                ml_bits = zstd_ml_bits[ml_code];
                if (bits.consumed > 64 - 32)
                {
                        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                                return zstd_fail("zstd sequence over-read");
                }
                of_extra = of_code ? zstd_bits_get(address_of bits, of_code) : 0;
                if (bits.consumed > 57)
                {
                        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                                return zstd_fail("zstd sequence over-read");
                }
                ml_extra = ml_bits ? zstd_bits_get(address_of bits, ml_bits) : 0;
                ll_extra = ll_bits ? zstd_bits_get(address_of bits, ll_bits) : 0;
                litlen = zstd_ll_base[ll_code] + (positive)ll_extra;
                match = zstd_ml_base[ml_code] + (positive)ml_extra;
                if (!zstd_repeat_offset(of_code, of_extra, litlen, address_of offset))
                        return zstd_fail("zstd bad offset");

                if (lit_at + litlen > lit_len)
                        return zstd_fail("zstd literals exhausted");
                if (!zstd_put(lit + lit_at, litlen))
                        return false;
                lit_at += litlen;
                if (!zstd_put_match(offset, match))
                        return false;

                if (i + 1 < nseq)
                {
                        zstd_fse_step(address_of zstd_ll, address_of state_ll,
                                      address_of bits);
                        zstd_fse_step(address_of zstd_ml, address_of state_ml,
                                      address_of bits);
                        zstd_fse_step(address_of zstd_of, address_of state_of,
                                      address_of bits);
                        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                                return zstd_fail("zstd sequence over-read");
                }
        }

        if (zstd_bits_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                return zstd_fail("zstd sequence bitstream");
        return zstd_put(lit + lit_at, lit_len - lit_at);
}

static bool zstd_window_open(positive window)
{
        positive cap;

        if (zstd_window)
        {
                memory_free(zstd_window, zstd_window_cap);
                zstd_window = null;
                zstd_window_cap = 0;
        }

        zstd_window_size = window;
        zstd_keep = window;
        zstd_pos = 0;
        if (!window)
                return true;

        cap = window + ZSTD_BLOCK_MAX + 64;
        zstd_window = (p8 address_to)memory(cap);
        if (!zstd_window || system_failed(zstd_window))
        {
                zstd_window = null;
                return zstd_fail("zstd cannot map the window");
        }
        zstd_window_cap = cap;
        return true;
}

static fn zstd_window_close(void)
{
        if (zstd_window)
        {
                memory_free(zstd_window, zstd_window_cap);
                zstd_window = null;
                zstd_window_cap = 0;
        }
}

static bool zstd_frame(void)
{
        p8 desc[1];
        p8 scratch[8];
        p32 dict = 0;
        p8 fcs_flag;
        p8 dict_flag;
        bool single;
        bool checksum;
        positive window = 0;
        p64 frame_start;

        if (!zstd_in_take(scratch, 4))
                return false;
        if (zstd_get32(scratch) != ZSTD_MAGIC)
                return zstd_fail("zstd bad magic");
        if (!zstd_in_take(desc, 1))
                return false;

        fcs_flag = (p8)(desc[0] >> 6);
        single = (desc[0] & 0x20) != 0;
        checksum = (desc[0] & 0x04) != 0;
        dict_flag = desc[0] & 3;
        if (desc[0] & 0x08)
                return zstd_fail("zstd reserved frame bit");

        if (!single)
        {
                p8 win[1];
                p8 mantissa;
                p8 exponent;
                positive base;

                if (!zstd_in_take(win, 1))
                        return false;
                exponent = (p8)(win[0] >> 3);
                mantissa = win[0] & 7;
                if (exponent > 17)
                        return zstd_fail("zstd window larger than 128 MiB");
                base = (positive)1 << (10 + exponent);
                window = base + (base >> 3) * mantissa;
        }

        if (dict_flag == 1)
        {
                if (!zstd_in_take(scratch, 1))
                        return false;
                dict = scratch[0];
        }
        else if (dict_flag == 2)
        {
                if (!zstd_in_take(scratch, 2))
                        return false;
                dict = zstd_get16(scratch);
        }
        else if (dict_flag == 3)
        {
                if (!zstd_in_take(scratch, 4))
                        return false;
                dict = zstd_get32(scratch);
        }
        if (dict)
                return zstd_fail("zstd dictionaries are refused");

        zstd_have_fcs = false;
        zstd_fcs = 0;
        if (fcs_flag == 0)
        {
                if (single)
                {
                        if (!zstd_in_take(scratch, 1))
                                return false;
                        zstd_fcs = scratch[0];
                        zstd_have_fcs = true;
                }
        }
        else if (fcs_flag == 1)
        {
                if (!zstd_in_take(scratch, 2))
                        return false;
                zstd_fcs = (p64)zstd_get16(scratch) + 256;
                zstd_have_fcs = true;
        }
        else if (fcs_flag == 2)
        {
                if (!zstd_in_take(scratch, 4))
                        return false;
                zstd_fcs = zstd_get32(scratch);
                zstd_have_fcs = true;
        }
        else
        {
                if (!zstd_in_take(scratch, 8))
                        return false;
                zstd_fcs = zstd_get64(scratch);
                zstd_have_fcs = true;
        }

        if (single)
        {
                if (!zstd_have_fcs)
                        return zstd_fail("zstd single-segment frame has no size");
                if (zstd_fcs > ZSTD_WINDOW_MAX)
                        return zstd_fail("zstd frame larger than 128 MiB");
                window = (positive)zstd_fcs;
        }

        if (window > ZSTD_WINDOW_MAX)
                return zstd_fail("zstd window larger than 128 MiB");

        zstd_block_limit = window && window < ZSTD_BLOCK_MAX ? window
                                                             : ZSTD_BLOCK_MAX;
        if (!zstd_window_open(window))
                return false;

        zstd_rep[0] = 1;
        zstd_rep[1] = 4;
        zstd_rep[2] = 8;
        zstd_ll.valid = false;
        zstd_of.valid = false;
        zstd_ml.valid = false;
        zstd_ll_prev.valid = false;
        zstd_of_prev.valid = false;
        zstd_ml_prev.valid = false;
        zstd_lit_huff.valid = false;
        frame_start = zstd_decoded;
        if (checksum)
        {
                zstd_hashing = true;
                zstd_xxh_start(address_of zstd_hash, 0);
        }
        else
                zstd_hashing = false;

        for (;;)
        {
                p8 header[3];
                p32 pack;
                bool last;
                p8 type;
                positive size;

                if (!zstd_in_take(header, 3))
                        return false;
                pack = zstd_get24(header);
                last = (pack & 1) != 0;
                type = (p8)((pack >> 1) & 3);
                size = pack >> 3;
                if (type == 3)
                        return zstd_fail("zstd reserved block type");
                if (type == 0)
                {
                        if (size > zstd_block_limit)
                                return zstd_fail("zstd raw block too large");
                        while (size)
                        {
                                positive chunk = size;

                                if (!zstd_in_need(1))
                                        return false;
                                if (chunk > zstd_src.have)
                                        chunk = zstd_src.have;
                                if (!zstd_put(zstd_in_at(), chunk))
                                        return false;
                                zstd_in_skip(chunk);
                                size -= chunk;
                        }
                }
                else if (type == 1)
                {
                        p8 value[1];

                        if (size > zstd_block_limit)
                                return zstd_fail("zstd RLE block too large");
                        if (!zstd_in_take(value, 1))
                                return false;
                        if (!zstd_put_fill(value[0], size))
                                return false;
                }
                else
                {
                        positive lit_used;
                        positive lit_len;

                        if (size > ZSTD_BLOCK_MAX || !size)
                                return zstd_fail("zstd compressed block size");
                        if (!zstd_in_take(zstd_comp, size))
                                return false;
                        if (!zstd_literals(zstd_comp, size, address_of lit_used,
                                           zstd_lit_buf, address_of lit_len))
                                return false;
                        if (lit_used > size)
                                return zstd_fail("zstd literals overran the block");
                        if (!zstd_sequences(zstd_comp + lit_used, size - lit_used,
                                            zstd_lit_buf, lit_len))
                                return false;
                }

                if (last)
                        break;
        }

        if (zstd_have_fcs && zstd_decoded - frame_start != zstd_fcs)
                return zstd_fail("zstd frame content size mismatch");

        if (checksum)
        {
                p32 got;
                p32 want;

                if (!zstd_in_take(scratch, 4))
                        return false;
                want = zstd_get32(scratch);
                got = (p32)zstd_xxh_end(address_of zstd_hash);
                if (got != want)
                        return zstd_fail("zstd content checksum mismatch");
        }

        zstd_hashing = false;
        return true;
}

static bool zstd_skippable(p32 magic)
{
        p8 sizeb[4];
        p32 size;

        (void)magic;
        if (!zstd_in_take(sizeb, 4))
                return false;
        size = zstd_get32(sizeb);
        return zstd_in_skip_bytes(size);
}

static bool zstd_stream(void)
{
        bool any = false;

        zstd_decoded = 0;
        zstd_out_used = 0;
        zstd_why = null;

        for (;;)
        {
                p8 peek[4];
                p32 magic;

                if (!zstd_src.have)
                {
                        zstd_why = null;
                        if (!zstd_in_need(1))
                        {
                                if (zstd_src.eof && !zstd_src.have)
                                        break;
                                return false;
                        }
                }
                if (!zstd_in_need(4))
                {
                        if (zstd_src.eof && !zstd_src.have)
                                break;
                        return false;
                }
                memory_copy(peek, zstd_in_at(), 4);
                magic = zstd_get32(peek);
                if (magic == ZSTD_MAGIC)
                {
                        if (!zstd_frame())
                                return false;
                        any = true;
                        continue;
                }
                if ((magic & ZSTD_SKIP_MASK) == ZSTD_SKIP_MAGIC)
                {
                        zstd_in_skip(4);
                        if (!zstd_skippable(magic))
                                return false;
                        continue;
                }
                if (!any)
                        return zstd_fail("zstd bad magic");
                return zstd_fail("zstd trailing garbage");
        }

        return true;
}

static fn zstd_src_mem(p8 address_to src, positive len)
{
        memory_fill(address_of zstd_src, 0, sizeof(zstd_src));
        zstd_src.fd = -1;
        zstd_src.mem = src;
        zstd_src.mem_len = len;
}

static fn zstd_src_fd(bipolar fd)
{
        memory_fill(address_of zstd_src, 0, sizeof(zstd_src));
        zstd_src.fd = fd;
}

static bipolar zstd_inflate(p8 address_to src, positive src_len,
                            p8 address_to dst, positive dst_cap)
{
        bool ok;

        zstd_src_mem(src, src_len);
        zstd_out_mem = dst;
        zstd_out_cap = dst_cap;
        zstd_out_fd = -1;
        ok = zstd_stream();
        zstd_window_close();
        zstd_out_mem = null;
        return ok ? (bipolar)zstd_out_used : -1;
}

#ifndef ZSTD_CORE_ONLY

static b32 zstd_status;

static fn zstd_refuse(string_address message)
{
        string_format(log_error, "zstd: %s\n", message);
        zstd_status = 1;
}

static string_address zstd_called(void)
{
        string_address path = program_argument(0);
        string_address slash;

        if (!path)
                return "zstd";
        slash = string_last_of(path, '/');
        return slash && slash[1] ? slash + 1 : path;
}

static bool zstd_suffix_out(string_address in, p8 address_to into, positive room)
{
        positive n = string_length(in);

        if (n >= 5 && !memory_compare(in + n - 5, ".tzst", 5))
        {
                if (n - 3 >= room)
                        return false;
                memory_copy(into, in, n - 5);
                memory_copy(into + n - 5, ".tar", 4);
                into[n - 1] = end;
                return true;
        }
        if (n >= 4 && !memory_compare(in + n - 4, ".zst", 4))
        {
                if (n - 3 >= room)
                        return false;
                memory_copy(into, in, n - 4);
                into[n - 4] = end;
                return true;
        }
        return false;
}

static b32 zstd_one(bipolar in, bipolar out)
{
        bool ok;

        zstd_src_fd(in);
        zstd_out_mem = null;
        zstd_out_fd = out;
        ok = zstd_stream();
        zstd_window_close();
        if (!ok)
        {
                zstd_refuse(zstd_why ? zstd_why : "decode failed");
                return zstd_status ? zstd_status : 1;
        }
        return 0;
}

static b32 file_zstd(void)
{
        string_address name = zstd_called();
        positive count = (positive)program_argument_count();
        positive at;
        bool decompress = string_equals(name, "unzstd") ||
                          string_equals(name, "zstdcat");
        bool stdout_out = string_equals(name, "zstdcat");
        bool force = false;
        bool test = false;
        bool remove_src = false;
        bool quiet = false;
        string_address out_path = null;
        p8 out_name[4096];

        zstd_status = 0;
        (void)quiet;

        for (at = 1; at < count; at++)
        {
                string_address word = program_argument((b32)at);

                if (string_equals(word, "--"))
                {
                        at++;
                        break;
                }
                if (word[0] != '-' || !word[1])
                        break;
                if (word[1] == '-')
                {
                        if (string_equals(word, "--decompress") ||
                            string_equals(word, "--uncompress"))
                        {
                                decompress = true;
                                continue;
                        }
                        if (string_equals(word, "--stdout") ||
                            string_equals(word, "--to-stdout"))
                        {
                                stdout_out = true;
                                continue;
                        }
                        if (string_equals(word, "--force"))
                        {
                                force = true;
                                continue;
                        }
                        if (string_equals(word, "--test"))
                        {
                                test = true;
                                decompress = true;
                                continue;
                        }
                        if (string_equals(word, "--keep"))
                                continue;
                        if (string_equals(word, "--rm"))
                        {
                                remove_src = true;
                                continue;
                        }
                        if (string_equals(word, "--quiet"))
                        {
                                quiet = true;
                                continue;
                        }
                        if (string_equals(word, "--help"))
                        {
                                string_format(log,
                                              "Usage: zstd -d [-cfkqt] [-o FILE] [--rm] [FILE...]\n");
                                log_flush();
                                return 0;
                        }
                        if (string_equals(word, "--version"))
                        {
                                string_format(log, "zstd from dawning-kit\n");
                                log_flush();
                                return 0;
                        }
                        if (memory_compare(word, "--output=", 9) == 0)
                        {
                                out_path = word + 9;
                                continue;
                        }
                        string_format(log_error, "zstd: unrecognized option '%s'\n",
                                      word);
                        return 2;
                }
                {
                        string_address letters = word + 1;

                        for (; *letters; letters++)
                        {
                                if (*letters == 'd')
                                        decompress = true;
                                else if (*letters == 'c')
                                        stdout_out = true;
                                else if (*letters == 'f')
                                        force = true;
                                else if (*letters == 'k')
                                        ;
                                else if (*letters == 'q')
                                        quiet = true;
                                else if (*letters == 't')
                                {
                                        test = true;
                                        decompress = true;
                                }
                                else if (*letters == 'V')
                                {
                                        string_format(log, "zstd from dawning-kit\n");
                                        log_flush();
                                        return 0;
                                }
                                else if (*letters == 'h')
                                {
                                        string_format(log,
                                                      "Usage: zstd -d [-cfkqt] [-o FILE] [--rm] [FILE...]\n");
                                        log_flush();
                                        return 0;
                                }
                                else if (*letters == 'o')
                                {
                                        if (letters[1])
                                        {
                                                out_path = letters + 1;
                                                letters += string_length(letters) - 1;
                                        }
                                        else
                                        {
                                                if (at + 1 >= count)
                                                {
                                                        zstd_refuse("option requires an argument -- 'o'");
                                                        return 2;
                                                }
                                                out_path = program_argument((b32)++at);
                                        }
                                }
                                else
                                {
                                        p8 shown[2];

                                        shown[0] = *letters;
                                        shown[1] = end;
                                        string_format(log_error,
                                                      "zstd: invalid option -- '%s'\n", shown);
                                        return 2;
                                }
                        }
                }
        }

        if (!decompress)
        {
                zstd_refuse("this applet decompresses only");
                return 1;
        }

        if (at >= count)
        {
                bipolar out = test ? -1 : 1;

                return zstd_one(0, out);
        }

        for (; at < count && !zstd_status; at++)
        {
                string_address path = program_argument((b32)at);
                bipolar in;
                bipolar out;
                bool close_in = false;
                bool close_out = false;

                if (string_equals(path, "-"))
                {
                        in = 0;
                        out = test ? -1 : 1;
                }
                else
                {
                        in = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
                        if (in < 0)
                        {
                                string_format(log_error, "zstd: %s: %s\n", path,
                                              file_reason(in));
                                zstd_status = 1;
                                break;
                        }
                        close_in = true;
                        if (test)
                                out = -1;
                        else if (stdout_out)
                                out = 1;
                        else if (out_path)
                        {
                                positive flags = FILE_WRITE | O_CLOEXEC;

                                if (!force)
                                        flags = 01 | FILE_CREATE | FILE_EXCLUSIVE |
                                                O_CLOEXEC;
                                out = system_open_at_mode(AT_FDCWD, out_path, flags,
                                                          0666);
                                if (out < 0)
                                {
                                        string_format(log_error, "zstd: %s: %s\n",
                                                      out_path, file_reason(out));
                                        system_close(in);
                                        zstd_status = 1;
                                        break;
                                }
                                close_out = true;
                        }
                        else if (!zstd_suffix_out(path, out_name, sizeof(out_name)))
                        {
                                zstd_refuse("cannot guess output name; use -c or -o");
                                system_close(in);
                                break;
                        }
                        else
                        {
                                positive flags = FILE_WRITE | O_CLOEXEC;

                                if (!force)
                                        flags = 01 | FILE_CREATE | FILE_EXCLUSIVE |
                                                O_CLOEXEC;
                                out = system_open_at_mode(AT_FDCWD, out_name, flags,
                                                          0666);
                                if (out < 0)
                                {
                                        string_format(log_error, "zstd: %s: %s\n",
                                                      out_name, file_reason(out));
                                        system_close(in);
                                        zstd_status = 1;
                                        break;
                                }
                                close_out = true;
                        }
                }

                zstd_one(in, out);
                if (close_in)
                        system_close(in);
                if (close_out)
                        system_close(out);
                if (!zstd_status && remove_src && close_in)
                        system_remove_at(AT_FDCWD, path, 0);
                if (out_path)
                        out_path = null;
        }

        log_flush();
        return zstd_status;
}

#endif /* ZSTD_CORE_ONLY */
