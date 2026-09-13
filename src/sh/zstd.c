/*
        zstd -- RFC 8878 decode and encode.

        Decode is the existing frame walker: content checksum is hash_xxh64,
        match copies are memory_copy_match, backward bitstreams are
        zstd_bits_open. Encode uses a two-candidate 16-bit hash, 128 KiB
        history, lazy matching, repeat offsets and predefined sequence FSE.
        Literals use four Huffman streams with direct or FSE-coded weights;
        raw literals/blocks win when entropy coding would grow. Match lengths
        are memory_common_prefix. Dictionaries are refused. Concatenated
        frames and skippable frames are accepted the way zstd -d accepts them.
*/

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_SKIP_MAGIC 0x184D2A50u
#define ZSTD_SKIP_MASK 0xFFFFFFF0u
#define ZSTD_WINDOW_MAX (1u << 27)
#define ZSTD_BLOCK_MAX (1u << 17)
#define ZSTD_IN 262144
#define ZSTD_OUT 131072
#define ZSTD_FSE_MAX 512
#define ZSTD_HUF_MAX 2048
#define ZSTD_SEQ_MAX (ZSTD_BLOCK_MAX / 4)

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

/*
        One FSE cell, eight bytes, the same layout zstd's sequence walker
        keeps: next state, extra bits, FSE nbits, baseline. Huffman weight
        tables leave extra 0 and the symbol in base, and are never fused.
*/
typedef struct
{
        p16 next;
        p8 extra;
        p8 bits;
        p32 base;
} zstd_fse_cell;

typedef struct
{
        p8 log;
        p8 rle;
        bool valid;
        p8 pad;
        p32 cells_at_8;
        zstd_fse_cell cell[ZSTD_FSE_MAX];
} zstd_fse;

typedef struct
{
        p8 address_to window;
        positive pos;
        positive window_size;
        p8 address_to lits;
        positive lit_len;
        p8 address_to seq;
        positive seq_len;
        zstd_fse address_to ll;
        zstd_fse address_to of;
        zstd_fse address_to ml;
        p32 address_to rep;
        positive nseq;
        p8 address_to output_end;
} zstd_seq_job;

typedef struct
{
        p8 max_bits;
        bool valid;
        p16 cell[ZSTD_HUF_MAX];
} zstd_huff;

/* 48 bytes. Layout is the library.c floor ABI: bits, consumed, ptr,
   start, limit, last. Huffman, sequences, and FSE unpack share it. */
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
static p8 zstd_out_buf[ZSTD_OUT];
static positive zstd_out_fill;
static positive zstd_out_taken;
static bool zstd_pull;
static bool zstd_paused;
static bool zstd_live;
static bool zstd_finished;
static p8 address_to zstd_rest;
static positive zstd_rest_n;
static bool zstd_frame_open;
static bool zstd_block_last;
static bool zstd_need_trailer;
static bool zstd_checksum_on;
static p64 zstd_frame_begin;
static bool zstd_hold_emit;
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

static p8 zstd_highbit32(p32 value)
{
        p32 bit = 0;

        if (!value)
                return 0;
#if X64
        __asm__("bsr %1, %0" : "=r"(bit) : "r"(value));
        return (p8)bit;
#elif ARM64
        __asm__("clz %w0, %w1" : "=r"(bit) : "r"(value));
        return (p8)(31 - bit);
#else
        if (value >= 0x10000u)
        {
                bit += 16;
                value >>= 16;
        }
        if (value >= 0x100u)
        {
                bit += 8;
                value >>= 8;
        }
        if (value >= 0x10u)
        {
                bit += 4;
                value >>= 4;
        }
        if (value >= 4u)
        {
                bit += 2;
                value >>= 2;
        }
        if (value >= 2u)
                bit += 1;

        return (p8)bit;
#endif
}

static bool zstd_fail(string_address why);

/* FSE-compressed Huffman weights stop on OVERFLOW, not on END_BUFFER.
   The library reload maps every non-overflow to 0, which is the same
   stop rule, but the C look/skip here is the one host_huff was proved
   against: a tail shorter than tableLog zero-pads, and consumed may
   run past 64 before the next reload. */
#define ZSTD_BITS_UNFINISHED 0
#define ZSTD_BITS_END_BUFFER 1
#define ZSTD_BITS_COMPLETED 2
#define ZSTD_BITS_OVERFLOW 3

static bool zstd_wt_open(zstd_bits address_to b, p8 address_to src,
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
                b->bits = memory_get64(b->ptr);
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

static p64 zstd_wt_look(zstd_bits address_to b, p8 n)
{
        if (!n || b->consumed >= 64)
                return 0;
        return (b->bits << b->consumed) >> (64 - n);
}

static fn zstd_wt_skip(zstd_bits address_to b, p8 n)
{
        b->consumed += n;
}

static p64 zstd_wt_get(zstd_bits address_to b, p8 n)
{
        p64 value = zstd_wt_look(b, n);

        zstd_wt_skip(b, n);
        return value;
}

static p8 zstd_wt_reload(zstd_bits address_to b)
{
        if (b->consumed > 64)
                return ZSTD_BITS_OVERFLOW;

        if (b->ptr >= b->limit)
        {
                b->ptr -= b->consumed >> 3;
                b->consumed &= 7;
                b->bits = memory_get64(b->ptr);
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
                        b->bits = memory_get64(b->ptr);
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

static fn zstd_xxh_start(zstd_xxh address_to h, p64 seed)
{
        hash_xxh64_begin(h, seed);
}

static fn zstd_xxh_add(zstd_xxh address_to h, p8 address_to p, positive n)
{
        hash_xxh64_add(h, p, n);
}

static p64 zstd_xxh_end(zstd_xxh address_to h)
{
        return hash_xxh64_finish(h);
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

static p8 zstd_fse_peek(zstd_fse address_to table, p16 state)
{
        if (!table->log)
                return table->rle;
        return (p8)table->cell[state].base;
}

static fn zstd_fse_step(zstd_fse address_to table, p16 address_to state,
                        zstd_bits address_to bits)
{
        zstd_fse_cell cell;

        if (!table->log)
                return;

        cell = table->cell[address_to state];
        address_to state =
            (p16)(cell.next + (p16)zstd_wt_get(bits, cell.bits));
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
                table->cell[u].extra = 0;
                table->cell[u].bits = bits;
                table->cell[u].next = (p16)((n << bits) - size);
                table->cell[u].base = (p32)sym;
        }

        table->log = log;
        table->rle = 0;
        table->valid = true;
        return true;
}

static const p8 zstd_ll_extra[36] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16};
static const p8 zstd_ml_extra[53] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11,
        12, 13, 14, 15, 16};
static const p32 zstd_ll_base[36] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096,
        8192, 16384, 32768, 65536};
static const p32 zstd_ml_base[53] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
        35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051,
        4099, 8195, 16387, 32771, 65539};

/*
        Fold the sequence baselines into the FSE cells. The walker then
        loads one word per stream instead of a symbol plus two RIP tables.
        A lone RLE symbol lives in cell 0 with no FSE step, so the same
        load serves both the compressed tables and a constant one.
*/
static bool zstd_seq_fuse(zstd_fse address_to table, p8 kind)
{
        const p8 address_to extra;
        const p32 address_to base;
        positive max;
        positive n;
        positive i;

        if (kind == 0)
        {
                extra = zstd_ll_extra;
                base = zstd_ll_base;
                max = 35;
        }
        else if (kind == 1)
        {
                extra = null;
                base = null;
                max = 31;
        }
        else
        {
                extra = zstd_ml_extra;
                base = zstd_ml_base;
                max = 52;
        }

        n = table->log ? (positive)1 << table->log : 1;
        for (i = 0; i < n; i++)
        {
                p8 sym = table->log ? (p8)table->cell[i].base : table->rle;

                if (sym > max)
                        return zstd_fail("zstd sequence symbol too large");
                if (kind == 1)
                {
                        table->cell[i].extra = sym;
                        table->cell[i].base =
                            (p32)(sym < 2 ? (positive)sym : ((positive)1 << sym) - 3);
                }
                else
                {
                        table->cell[i].extra = extra[sym];
                        table->cell[i].base = base[sym];
                }
                if (!table->log)
                {
                        table->cell[i].next = 0;
                        table->cell[i].bits = 0;
                }
        }

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
        zstd_seq_fuse(address_of zstd_ll_def, 0);
        zstd_seq_fuse(address_of zstd_ml_def, 2);
        zstd_seq_fuse(address_of zstd_of_def, 1);
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

        if (!zstd_wt_open(address_of bits, src, size))
                return false;
        state1 = (p16)zstd_wt_get(address_of bits, table->log);
        state2 = (p16)zstd_wt_get(address_of bits, table->log);
        if (zstd_wt_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                return zstd_fail("zstd Huffman FSE overflow");

        for (;;)
        {
                if (n + 2 > max_out)
                        return zstd_fail("zstd Huffman too many weights");
                into[n++] = zstd_fse_symbol(table, address_of state1,
                                            address_of bits);
                if (zstd_wt_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
                {
                        into[n++] = zstd_fse_symbol(table, address_of state2,
                                                    address_of bits);
                        break;
                }
                into[n++] = zstd_fse_symbol(table, address_of state2,
                                            address_of bits);
                if (zstd_wt_reload(address_of bits) == ZSTD_BITS_OVERFLOW)
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
                                /* nbits in the low byte, symbol in the high
                                   byte: Facebook's 4X1 walker shifts by the
                                   cell itself, and x86 can store %ah. */
                                huff->cell[start + i] =
                                    (p16)bits | ((p16)s << 8);
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
        if (!need)
                return true;
        if (zstd_huffman_stream(into, need, src, size, huff->cell, huff->max_bits))
                return zstd_fail("zstd Huffman over-read");
        return true;
}

static bool zstd_huff_four(zstd_huff address_to huff, p8 address_to into,
                           positive need, p8 address_to src, positive size)
{
        if (zstd_huffman_4x(into, need, src, size, huff->cell, huff->max_bits))
                return zstd_fail("zstd Huffman over-read");
        return true;
}

static bool zstd_flush(void)
{
        if (!zstd_out_fill || zstd_out_fd < 0)
                return true;
        if (system_write_all((positive)zstd_out_fd, zstd_out_buf,
                             zstd_out_fill) != (bipolar)zstd_out_fill)
                return zstd_fail("zstd: write failed");
        zstd_out_fill = zstd_out_taken = 0;
        return true;
}

static bool zstd_emit(p8 address_to p, positive n)
{
        if (!n)
                return true;

        if (zstd_out_mem)
        {
                if (zstd_out_used + n > zstd_out_cap)
                        return zstd_fail("zstd output larger than the destination");
                if (zstd_hashing)
                        zstd_xxh_add(address_of zstd_hash, p, n);
                zstd_decoded += n;
                memory_copy(zstd_out_mem + zstd_out_used, p, n);
                zstd_out_used += n;
                return true;
        }

        if (zstd_out_fd < 0 && !zstd_pull)
        {
                if (zstd_hashing)
                        zstd_xxh_add(address_of zstd_hash, p, n);
                zstd_decoded += n;
                return true;
        }

        while (n)
        {
                positive room = ZSTD_OUT - zstd_out_fill;
                positive chunk;

                if (zstd_pull && !room)
                {
                        zstd_rest = p;
                        zstd_rest_n = n;
                        zstd_paused = true;
                        return true;
                }

                chunk = n < room ? n : room;
                if (zstd_hashing)
                        zstd_xxh_add(address_of zstd_hash, p, chunk);
                zstd_decoded += chunk;
                memory_copy(zstd_out_buf + zstd_out_fill, p, chunk);
                zstd_out_fill += chunk;
                p += chunk;
                n -= chunk;
                if (!zstd_pull && zstd_out_fill == ZSTD_OUT && !zstd_flush())
                        return false;
        }

        zstd_rest = null;
        zstd_rest_n = 0;
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
        if (!zstd_hold_emit && !zstd_emit(zstd_window + zstd_pos, n))
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
        if (!zstd_hold_emit && !zstd_emit(zstd_window + zstd_pos, n))
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
        prefetch_read(zstd_window + zstd_pos - offset);
        memory_copy_match(zstd_window + zstd_pos, offset, n);
        if (!zstd_hold_emit && !zstd_emit(zstd_window + zstd_pos, n))
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
                return zstd_seq_fuse(table, kind);
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
                return zstd_seq_fuse(table, kind);
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
        zstd_seq_job job;

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
                if (lit_len > zstd_block_limit)
                        return zstd_fail("zstd block output is too large");
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

        job.window = zstd_window;
        job.pos = zstd_pos;
        job.window_size = zstd_window_size;
        job.lits = lit;
        job.lit_len = lit_len;
        job.seq = p;
        job.seq_len = (positive)(stop - p);
        job.ll = address_of zstd_ll;
        job.of = address_of zstd_of;
        job.ml = address_of zstd_ml;
        job.rep = zstd_rep;
        job.nseq = nseq;
        job.output_end = zstd_window + zstd_pos + zstd_block_limit;
        if (zstd_sequences_run(address_of job))
                return zstd_fail("zstd sequence input or output overrun");
        zstd_pos = job.pos;
        return true;
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

        cap = window * 2 + ZSTD_BLOCK_MAX + 64;
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

        if (zstd_need_trailer)
                goto zstd_frame_trailer;
        if (zstd_frame_open)
                goto zstd_frame_blocks;

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
                zstd_fcs = memory_get64(scratch);
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

        zstd_frame_open = true;
        zstd_checksum_on = checksum;
        zstd_frame_begin = frame_start;

zstd_frame_blocks:
        checksum = zstd_checksum_on;
        frame_start = zstd_frame_begin;

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
                        /* Consume a whole block before pausing. The window
                           owns the remainder until the pull reader drains it. */
                        if (!zstd_in_need(size))
                                return false;
                        if (!zstd_put(zstd_in_at(), size))
                                return false;
                        zstd_in_skip(size);
                        if (zstd_paused)
                        {
                                zstd_block_last = last;
                                zstd_need_trailer = last;
                                return true;
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
                        if (zstd_paused)
                        {
                                zstd_block_last = last;
                                zstd_need_trailer = last;
                                return true;
                        }
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
                        if (!zstd_window_room(zstd_block_limit))
                                return false;
                        {
                                positive at = zstd_pos;
                                bool ok;

                                zstd_hold_emit = true;
                                ok = zstd_sequences(zstd_comp + lit_used,
                                                    size - lit_used, zstd_lit_buf,
                                                    lit_len);
                                zstd_hold_emit = false;
                                if (!ok)
                                        return false;
                                if (!zstd_emit(zstd_window + at, zstd_pos - at))
                                        return false;
                                if (zstd_paused)
                                {
                                        zstd_block_last = last;
                                        zstd_need_trailer = last;
                                        return true;
                                }
                        }
                }

                if (last)
                        break;
        }

zstd_frame_trailer:
        checksum = zstd_checksum_on;
        frame_start = zstd_frame_begin;
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
        zstd_frame_open = false;
        zstd_need_trailer = false;
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
        bool any = zstd_live && zstd_decoded > 0;

        if (!zstd_live)
        {
                zstd_decoded = 0;
                zstd_out_used = 0;
                if (!zstd_pull)
                        zstd_out_fill = zstd_out_taken = 0;
                zstd_hold_emit = false;
                zstd_why = null;
                zstd_live = true;
        }

        for (;;)
        {
                p8 peek[4];
                p32 magic;

                if (zstd_paused)
                        return true;
                if (zstd_frame_open || zstd_need_trailer)
                {
                        if (!zstd_frame())
                                return false;
                        if (zstd_paused)
                                return true;
                        any = true;
                        continue;
                }

                if (!zstd_src.have)
                {
                        zstd_why = null;
                        if (!zstd_in_need(1))
                        {
                                if (zstd_src.eof && !zstd_src.have)
                                {
                                        zstd_why = null;
                                        break;
                                }
                                return false;
                        }
                }
                if (!zstd_in_need(4))
                {
                        if (zstd_src.eof && !zstd_src.have)
                        {
                                zstd_why = null;
                                break;
                        }
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
        zstd_pull = false;
        zstd_live = false;
        zstd_frame_open = false;
        zstd_paused = false;
        zstd_need_trailer = false;
        ok = zstd_stream();
        if (ok)
                ok = zstd_flush();
        zstd_window_close();
        zstd_out_mem = null;
        return ok ? (bipolar)zstd_out_used : -1;
}

static bool zstd_decode_begin(bipolar in)
{
        zstd_src_fd(in);
        zstd_out_mem = null;
        zstd_out_fd = -1;
        zstd_out_fill = zstd_out_taken = 0;
        zstd_pull = true;
        zstd_paused = false;
        zstd_live = false;
        zstd_finished = false;
        zstd_frame_open = false;
        zstd_need_trailer = false;
        zstd_rest = null;
        zstd_rest_n = 0;
        zstd_why = null;
        return true;
}

static bool zstd_decode_begin_prefix(bipolar in, p8 address_to prefix, positive n)
{
        zstd_decode_begin(in);
        if (n > ZSTD_IN)
                return zstd_fail("zstd prefix");
        memory_copy(zstd_src.buf, prefix, n);
        zstd_src.have = n;
        zstd_src.at = 0;
        return true;
}

static bipolar zstd_decode_read(p8 address_to dst, positive n)
{
        positive copied = 0;

        while (copied < n)
        {
                positive take;

                if (zstd_out_fill)
                {
                        positive left = zstd_out_fill - zstd_out_taken;
                        take = left > n - copied ? n - copied : left;
                        memory_copy_apart(dst + copied, zstd_out_buf + zstd_out_taken, take);
                        zstd_out_taken += take;
                        if (zstd_out_taken == zstd_out_fill)
                                zstd_out_fill = zstd_out_taken = 0;
                        copied += take;
                        continue;
                }
                if (zstd_rest_n)
                {
                        zstd_paused = false;
                        if (!zstd_emit(zstd_rest, zstd_rest_n))
                                return -1;
                        continue;
                }
                if (zstd_finished)
                        break;
                zstd_paused = false;
                if (!zstd_stream())
                        return -1;
                if (!zstd_out_fill && !zstd_paused && !zstd_rest_n &&
                    zstd_src.eof && !zstd_src.have)
                {
                        zstd_finished = true;
                        break;
                }
        }
        return (bipolar)copied;
}

static bool zstd_decode_end(void)
{
        zstd_pull = false;
        zstd_finished = true;
        zstd_window_close();
        return zstd_why == null;
}

#include "compression_huffman.c"

static zstd_xxh zstd_enc_hash;
#define ZSTD_ENC_WINDOW (2 * 1024 * 1024)
static p8 zstd_enc_storage[2 * ZSTD_ENC_WINDOW + ZSTD_BLOCK_MAX];
static positive zstd_enc_position;
#define zstd_enc_block (zstd_enc_storage + zstd_enc_position)
static positive zstd_enc_abs;
static p32 zstd_enc_rep[3];
static positive zstd_enc_fill;
static bool zstd_enc_open;
static p8 zstd_cli_level;

typedef struct
{
        p16 state[512];
        p32 delta_nb[53];
        bipolar delta_find[53];
        p8 log;
} zstd_ctable;

typedef struct
{
        p32 lit;
        p32 match;
        p32 off;
        p8 ll_code, ml_code, of_code;
} zstd_enc_seq;

typedef struct
{
        p64 acc;
        p8 bits;
        p8 address_to buf;
        positive cap;
        positive n;
        bool full;
} zstd_bout;

static zstd_ctable zstd_ct_ll;
static zstd_ctable zstd_ct_of;
static zstd_ctable zstd_ct_ml;
static bool zstd_ct_ready;
static zstd_enc_seq zstd_seqs[ZSTD_SEQ_MAX];
static p8 zstd_enc_lits[ZSTD_BLOCK_MAX];
static p8 zstd_enc_bits[ZSTD_BLOCK_MAX + 64];
/* Both candidates share one naturally aligned load and one cache line. */
static p64 zstd_enc_head[65536];

static __attribute__((always_inline)) inline fn zstd_bout_add(zstd_bout address_to b, p64 v, p8 nbits)
{
        if (!nbits)
                return;
        if (nbits >= 64 || b->n + 16 >= b->cap)
        {
                b->full = true;
                return;
        }
        b->acc |= (v & (((p64)1 << nbits) - 1)) << b->bits;
        b->bits += nbits;
        if (b->bits >= 32)
        {
                b->buf[b->n] = (p8)b->acc;
                b->buf[b->n + 1] = (p8)(b->acc >> 8);
                b->buf[b->n + 2] = (p8)(b->acc >> 16);
                b->buf[b->n + 3] = (p8)(b->acc >> 24);
                b->n += 4;
                b->acc >>= 32;
                b->bits -= 32;
        }
}

static bool zstd_bout_close(zstd_bout address_to b)
{
        zstd_bout_add(b, 1, 1);
        while (b->bits >= 8)
        {
                b->buf[b->n++] = (p8)b->acc;
                b->acc >>= 8;
                b->bits -= 8;
        }
        if (b->bits)
        {
                b->buf[b->n++] = (p8)b->acc;
                b->acc = 0;
                b->bits = 0;
        }
        return !b->full && b->n && b->buf[b->n - 1];
}

static bool zstd_ctable_build(zstd_ctable address_to ct,
                              const bipolar address_to norm, positive max_sym,
                              p8 log)
{
        positive size = (positive)1 << log;
        positive high = size - 1;
        positive step = (size >> 1) + (size >> 3) + 3;
        positive mask = size - 1;
        positive pos = 0;
        p16 cumul[64];
        p8 symbol[512];
        positive s;
        positive u;
        positive total;

        if (log > 9 || size > 512 || max_sym > 52)
                return false;
        memory_fill(symbol, 0, size);
        cumul[0] = 0;
        for (s = 0; s <= max_sym; s++)
        {
                if (norm[s] == -1)
                {
                        symbol[high] = (p8)s;
                        high--;
                        cumul[s + 1] = cumul[s] + 1;
                }
                else
                        cumul[s + 1] =
                            cumul[s] + (norm[s] < 0 ? 0 : (p16)norm[s]);
        }
        for (s = 0; s <= max_sym; s++)
        {
                bipolar n = norm[s];
                bipolar i;

                if (n <= 0)
                        continue;
                for (i = 0; i < n; i++)
                {
                        while (pos > high)
                                pos = (pos + step) & mask;
                        symbol[pos] = (p8)s;
                        pos = (pos + step) & mask;
                }
        }
        if (pos)
                return false;
        for (u = 0; u < size; u++)
        {
                p8 sym = symbol[u];

                ct->state[cumul[sym]++] = (p16)(size + u);
        }
        total = 0;
        memory_fill(ct->delta_nb, 0, sizeof(ct->delta_nb));
        memory_fill(ct->delta_find, 0, sizeof(ct->delta_find));
        for (s = 0; s <= max_sym; s++)
        {
                bipolar n = norm[s];

                if (!n)
                        continue;
                if (n == -1 || n == 1)
                {
                        ct->delta_nb[s] = ((p32)log << 16) - ((p32)1 << log);
                        ct->delta_find[s] = (bipolar)total - 1;
                        total++;
                }
                else
                {
                        p8 max_bits = (p8)(log - zstd_highbit32((p32)n - 1));
                        p32 min_state = (p32)n << max_bits;

                        ct->delta_nb[s] = ((p32)max_bits << 16) - min_state;
                        ct->delta_find[s] = (bipolar)total - (bipolar)n;
                        total += (positive)n;
                }
        }
        ct->log = log;
        return true;
}

static fn zstd_ct_init(void)
{
        if (zstd_ct_ready)
                return;
        if (zstd_ctable_build(address_of zstd_ct_ll, zstd_ll_default, 35, 6) &&
            zstd_ctable_build(address_of zstd_ct_of, zstd_of_default, 28, 5) &&
            zstd_ctable_build(address_of zstd_ct_ml, zstd_ml_default, 52, 6))
                zstd_ct_ready = true;
}

static const p8 zstd_ll_codes[64] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21,
        22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23,
        24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
};
static const p8 zstd_ml_codes[131] = {
        0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
        29, 30, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 36, 36, 37,
        37, 37, 37, 38, 38, 38, 38, 38, 38, 38, 38, 39, 39, 39, 39, 39,
        39, 39, 39, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40, 40, 40, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
        41, 41, 41, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
        42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
        42, 42, 42,
};

static p8 zstd_seq_code(const p32 address_to base, p8 max, p32 value)
{
        (void)max;
        if (base == zstd_ll_base)
                return value < 64 ? zstd_ll_codes[value]
                                  : 19 + zstd_highbit32(value);
        return value < 131 ? zstd_ml_codes[value]
                           : 36 + zstd_highbit32(value - 3);
}

static p8 zstd_off_code(positive offset)
{
        return zstd_highbit32((p32)offset);
}

typedef struct
{
        p32 value;
        zstd_ctable address_to ct;
} zstd_cstate;

static bool zstd_cstate_init2(zstd_cstate address_to st, zstd_ctable address_to ct,
                              p8 symbol)
{
        p32 nb;
        bipolar idx;

        st->ct = ct;
        nb = (ct->delta_nb[symbol] + (1u << 15)) >> 16;
        st->value = (nb << 16) - ct->delta_nb[symbol];
        idx = (bipolar)(st->value >> nb) + ct->delta_find[symbol];
        if (idx < 0 || idx >= (bipolar)((positive)1 << ct->log))
                return false;
        st->value = ct->state[idx];
        return true;
}

static __attribute__((always_inline)) inline fn zstd_cstate_encode(zstd_bout address_to b, zstd_cstate address_to st,
                             p8 symbol)
{
        p32 nb = (st->value + st->ct->delta_nb[symbol]) >> 16;
        bipolar idx = (bipolar)(st->value >> nb) + st->ct->delta_find[symbol];

        zstd_bout_add(b, st->value, (p8)nb);
        if (idx < 0 || idx >= (bipolar)((positive)1 << st->ct->log))
        {
                b->full = true;
                return;
        }
        st->value = st->ct->state[idx];
}

static fn zstd_cstate_flush(zstd_bout address_to b, zstd_cstate address_to st)
{
        zstd_bout_add(b, st->value, st->ct->log);
}

/* RFC 8878 literals: a complete codebook, FSE-compressed weights when
   the alphabet exceeds the direct nibble form, and four backward streams.
   Sequence and weight FSE share the bounded state-table builder. */
static p8 zstd_packed_lits[ZSTD_BLOCK_MAX * 2 + 512];

static fn zstd_bout_pad(zstd_bout address_to b)
{
        while (b->bits >= 8)
        {
                b->buf[b->n++] = (p8)b->acc;
                b->acc >>= 8;
                b->bits -= 8;
        }
        if (b->bits)
                b->buf[b->n++] = (p8)b->acc;
        b->bits = 0;
        b->acc = 0;
}

static positive zstd_write_norm(p8 address_to dst, const bipolar address_to norm,
                                 positive max_sym, p8 log)
{
        zstd_bout b = {0};
        positive remaining = ((positive)1 << log) + 1;
        positive threshold = (positive)1 << log;
        positive sym = 0;
        p8 bits = log + 1;
        bool zero = false;

        b.buf = dst;
        b.cap = 256;
        zstd_bout_add(address_of b, log - 5, 4);
        while (remaining > 1 && sym <= max_sym)
        {
                if (zero)
                {
                        positive run = 0;
                        while (sym <= max_sym && !norm[sym])
                                sym++, run++;
                        while (run >= 3)
                                zstd_bout_add(address_of b, 3, 2), run -= 3;
                        zstd_bout_add(address_of b, run, 2);
                }
                if (sym > max_sym)
                        return 0;
                positive maximum = 2 * threshold - 1 - remaining;
                positive count = norm[sym] + 1;
                positive value = count;
                if (count >= threshold)
                        value += maximum;
                zstd_bout_add(address_of b, value,
                              count < maximum ? bits - 1 : bits);
                remaining -= norm[sym];
                zero = !norm[sym++];
                while (remaining < threshold)
                        threshold >>= 1, bits--;
        }
        zstd_bout_pad(address_of b);
        return remaining == 1 && !b.full ? b.n : 0;
}

static zstd_ctable zstd_ct_dynamic[3];

/* Fractional log estimate is used only to choose between legal tables.
   Both candidates use the exact FSE coder after the choice. */
static positive zstd_log_cost(p32 n)
{
        positive log = zstd_highbit32(n);
        return log * 256 + (((positive)n - ((positive)1 << log)) << 8) /
                               ((positive)1 << log);
}

static zstd_ctable address_to zstd_sequence_model(p32 address_to freq,
        positive n, positive max, p8 log, const bipolar address_to defaults,
        zstd_ctable address_to base, zstd_ctable address_to dynamic,
        p8 address_to header, positive address_to header_n)
{
        bipolar norm[53] = {0};
        positive total = 0, largest = 0, last_symbol = 0;
        positive size = (positive)1 << log;
        if (n < 64) return base;
        for (positive i = 0; i <= max; i++)
                if (freq[i])
                {
                        norm[i] = (freq[i] * size) / n;
                        if (!norm[i]) norm[i] = 1;
                        total += norm[i];
                        if (freq[i] > freq[largest]) largest = i;
                        last_symbol = i;
                }
        while (total > size)
        {
                positive most = largest;
                for (positive i = 0; i <= last_symbol; i++)
                        if (norm[i] > norm[most]) most = i;
                if (norm[most] <= 1) return base;
                norm[most]--; total--;
        }
        norm[largest] += size - total;
        positive hn = zstd_write_norm(header, norm, last_symbol, log);
        if (!hn) return base;
        positive old_cost = 0, new_cost = hn * 8 * 256;
        for (positive i = 0; i <= max; i++)
                if (freq[i])
                {
                        positive old = defaults[i] < 0 ? 1 : defaults[i];
                        old_cost += freq[i] * (base->log * 256 - zstd_log_cost((p32)old));
                        new_cost += freq[i] * (log * 256 - zstd_log_cost((p32)norm[i]));
                }
        if (new_cost + 16 * 256 >= old_cost ||
            !zstd_ctable_build(dynamic, norm, last_symbol, log)) return base;
        address_to header_n += hn;
        return dynamic;
}

static positive zstd_pack_weights(p8 address_to dst, p8 address_to weight,
                                   positive n)
{
        p32 freq[12] = {0};
        bipolar norm[12] = {0};
        zstd_ctable ct;
        zstd_cstate state[2];
        zstd_bout b = {0};
        positive at;
        positive total = 0;
        positive biggest = 0;
        positive max_sym = 0;
        positive head;

        if (n < 2)
                return 0;
        for (at = 0; at < n; at++)
                freq[weight[at]]++;
        for (at = 0; at < 12; at++)
        {
                if (freq[at] > freq[biggest])
                        biggest = at;
                if (freq[at])
                {
                        norm[at] = (freq[at] * 64) / n;
                        if (!norm[at])
                                norm[at] = 1;
                        total += norm[at];
                        max_sym = at;
                }
        }
        /* A one-symbol, zero-bit FSE machine has no finite end marker. */
        if (freq[biggest] == n)
        {
                positive other = biggest ? 0 : 1;
                norm[other] = 1;
                total++;
                if (other > max_sym)
                        max_sym = other;
        }
        while (total > 64)
        {
                positive most = biggest;
                for (at = 0; at <= max_sym; at++)
                        if (norm[at] > norm[most])
                                most = at;
                if (norm[most] <= 1)
                        return 0;
                norm[most]--;
                total--;
        }
        norm[biggest] += 64 - total;
        head = zstd_write_norm(dst + 1, norm, max_sym, 6);
        if (!head || !zstd_ctable_build(address_of ct, norm, max_sym, 6))
                return 0;
        b.buf = dst + 1 + head;
        b.cap = 256 - head;
        if (!zstd_cstate_init2(address_of state[(n - 1) & 1], address_of ct,
                                weight[n - 1]) ||
            !zstd_cstate_init2(address_of state[(n - 2) & 1], address_of ct,
                                weight[n - 2]))
                return 0;
        at = n - 2;
        while (at)
        {
                at--;
                zstd_cstate_encode(address_of b, address_of state[at & 1],
                                    weight[at]);
        }
        zstd_cstate_flush(address_of b, address_of state[1]);
        zstd_cstate_flush(address_of b, address_of state[0]);
        if (!zstd_bout_close(address_of b) || head + b.n >= 128)
                return 0;
        dst[0] = (p8)(head + b.n);
        return 1 + head + b.n;
}

static positive zstd_pack_literals(p8 address_to src, positive n,
                                    p8 address_to header, positive address_to hn)
{
        p32 freq[256] = {0}, f1[256] = {0}, f2[256] = {0}, f3[256] = {0};
        p32 work[256];
        p8 length[256];
        p8 weight[256];
        p32 table[256];
        positive max_sym = 0;
        positive symbols = 0;
        positive max_bits = 0;
        positive bit_cost = 0;
        positive at;
        positive head;
        positive size;
        positive start = 0;
        positive segment = (n + 3) / 4;
        positive position = 0;
        p64 field;
        positive header_n;

        if (n < 64)
                return 0;
        for (at = 0; at + 4 <= n; at += 4)
        {
                freq[src[at]]++; f1[src[at + 1]]++;
                f2[src[at + 2]]++; f3[src[at + 3]]++;
        }
        for (; at < n; at++) freq[src[at]]++;
        for (at = 0; at < 256; at++) freq[at] += f1[at] + f2[at] + f3[at];
        for (at = 0; at < 256; at++)
                if (freq[at])
                        symbols++, max_sym = at;
        if (symbols < 2)
                return 0;
        memory_copy_apart(work, freq, sizeof(freq));
        for (;;)
        {
                positive kraft = 0;
                max_bits = 0;
                compression_build_lengths(work, 256, length, 11);
                for (at = 0; at < 256; at++)
                        if (length[at])
                        {
                                kraft += (positive)1 << (11 - length[at]);
                                if (length[at] > max_bits)
                                        max_bits = length[at];
                        }
                if (kraft == 2048)
                        break;
                /* Flatten only when an unconstrained tree exceeds 11 bits.
                   Every used symbol stays present, and the rebuilt tree is
                   complete; truncating depths alone oversubscribes it. */
                for (at = 0; at < 256; at++)
                        if (work[at])
                                work[at] = (work[at] + 1) >> 1;
        }
        for (at = 0; at <= max_sym; at++)
        {
                weight[at] = length[at] ? max_bits + 1 - length[at] : 0;
                bit_cost += freq[at] * length[at];
        }
        if ((bit_cost + 7) / 8 + 12 >= n)
                return 0;
        for (positive w = 1; w <= max_bits; w++)
                for (at = 0; at <= max_sym; at++)
                        if (weight[at] == w)
                        {
                                table[at] = (p32)((length[at] << 16) |
                                                (start >> (w - 1)));
                                start += (positive)1 << (w - 1);
                        }
        if (max_sym <= 128)
        {
                zstd_packed_lits[0] = (p8)(127 + max_sym);
                for (at = 0; at < max_sym; at += 2)
                        zstd_packed_lits[1 + at / 2] = (p8)(weight[at] << 4) |
                                (at + 1 < max_sym ? weight[at + 1] : 0);
                head = 1 + (max_sym + 1) / 2;
        }
        else
        {
                head = zstd_pack_weights(zstd_packed_lits, weight, max_sym);
                if (!head)
                        return 0;
        }
        size = head + 6;
        for (at = 0; at < 4; at++)
        {
                positive take = at == 3 ? n - position : segment;
                positive packed = huffman_encode_back(zstd_packed_lits + size,
                                                       src + position, take, table);
                if (at < 3)
                {
                        zstd_packed_lits[head + 2 * at] = (p8)packed;
                        zstd_packed_lits[head + 2 * at + 1] = (p8)(packed >> 8);
                }
                size += packed;
                position += take;
        }
        if (n < 1024 && size < 1024)
        {
                field = 6 | (n << 4) | (size << 14);
                header_n = 3;
        }
        else if (n < 16384 && size < 16384)
        {
                field = 10 | (n << 4) | ((p64)size << 18);
                header_n = 4;
        }
        else
        {
                field = 14 | (n << 4) | ((p64)size << 22);
                header_n = 5;
        }
        if (size + header_n >= n + (n < 32 ? 1 : n < 4096 ? 2 : 3))
                return 0;
        for (at = 0; at < header_n; at++)
                header[at] = (p8)(field >> (at * 8));
        address_to hn = header_n;
        return size;
}

static bool zstd_enc_out(p8 address_to p, positive n)
{
        if (!n)
                return true;
        if (zstd_out_mem)
        {
                if (zstd_out_used + n > zstd_out_cap)
                        return zstd_fail("zstd output is too small");
                memory_copy(zstd_out_mem + zstd_out_used, p, n);
                zstd_out_used += n;
                return true;
        }
        if (system_write_all((positive)zstd_out_fd, p, n) != n)
                return zstd_fail("zstd write failed");
        return true;
}

static bool zstd_emit_raw_block(p8 address_to src, positive n, bool last)
{
        p32 pack = (last ? 1u : 0) | (0 << 1) | (n << 3);
        p8 header[3];

        header[0] = (p8)pack;
        header[1] = (p8)(pack >> 8);
        header[2] = (p8)(pack >> 16);
        if (!zstd_enc_out(header, 3))
                return false;
        return zstd_enc_out(src, n);
}

static p16 zstd_enc_hash4(p8 address_to p)
{
        p32 h = (p32)p[0] | ((p32)p[1] << 8) | ((p32)p[2] << 16) |
                ((p32)p[3] << 24);

        h *= 0x1e35a7bdu;
        return (p16)(h >> 16);
}

static bool zstd_emit_comp_block(p8 address_to src, positive n, bool last)
{
        zstd_bout bits;
        zstd_cstate ll_st;
        zstd_cstate of_st;
        zstd_cstate ml_st;
        positive lit_n = 0;
        positive lit_at = 0;
        positive nseq = 0;
        positive pos = 0;
        positive used;
        p32 pack;
        p8 header[3];
        p8 lit_hdr[5];
        p8 address_to lit_data = zstd_enc_lits;
        positive lit_size;
        p8 seq_hdr[4];
        p8 model_header[768];
        positive model_header_n = 0;
        p32 ll_freq[36] = {0}, ml_freq[53] = {0}, of_freq[29] = {0};
        zstd_ctable address_to ll_table = address_of zstd_ct_ll;
        zstd_ctable address_to ml_table = address_of zstd_ct_ml;
        zstd_ctable address_to of_table = address_of zstd_ct_of;
        positive seq_hdr_n;
        positive at;
        p32 rep[3] = {zstd_enc_rep[0], zstd_enc_rep[1], zstd_enc_rep[2]};

        zstd_ct_init();
        if (!zstd_ct_ready)
                return zstd_emit_raw_block(src, n, last);
        memory_fill(address_of bits, 0, sizeof(bits));
        bits.buf = zstd_enc_bits;
        bits.cap = sizeof(zstd_enc_bits);
        while (pos + 4 <= n && nseq < ZSTD_SEQ_MAX)
        {
                p16 h = zstd_enc_hash4(src + pos);
                p64 candidates = zstd_enc_head[h];
                positive match = 0;
                positive dist = 0;

                zstd_enc_head[h] = (candidates << 32) | (p32)(zstd_enc_abs + pos + 1);
                /* Repeat offsets cost fewer bits than a new distance. Probe
                   the first legal repeat before the hash candidates; skip
                   this work after a long unsuccessful literal run. */
                positive repeated = rep[pos == lit_at];
                if (pos - lit_at < 64 && repeated &&
                    repeated <= zstd_enc_abs + pos && repeated <= ZSTD_ENC_WINDOW &&
                    zstd_get32(src + pos) == zstd_get32(src + pos - repeated))
                {
                        match = 4 + memory_common_prefix(src + pos + 4,
                                      src + pos + 4 - repeated, n - pos - 4);
                        dist = repeated;
                }
                if (!match)
#pragma GCC unroll 2
                for (positive c = 0; c < 2; c++)
                {
                        positive old = (p32)(candidates >> (c * 32));
                        if (!old)
                                continue;
                        positive d = zstd_enc_abs + pos + 1 - old;
                        if (!d || d > ZSTD_ENC_WINDOW || old > zstd_enc_abs + pos)
                                continue;
                        p8 address_to there = src + pos - d;
                        if (zstd_get32(src + pos) != zstd_get32(there) ||
                            (match && src[pos + match] != there[match]))
                                continue;
                        positive k = 4 + memory_common_prefix(src + pos + 4,
                                                               there + 4, n - pos - 4);
                        if (k > match)
                        {
                                match = k;
                                dist = d;
                                if (k == n - pos)
                                        break;
                        }
                }
                /* One-step lazy parsing catches a much longer match after
                   a literal without inserting the lookahead position twice. */
                if (match && match < 16 && pos + 5 <= n)
                {
                        positive old = (p32)zstd_enc_head[zstd_enc_hash4(src + pos + 1)];
                        positive d = zstd_enc_abs + pos + 2 - old;
                        if (old && d && d <= ZSTD_ENC_WINDOW &&
                            old <= zstd_enc_abs + pos + 1 &&
                            zstd_get32(src + pos + 1) == zstd_get32(src + pos + 1 - d))
                        {
                                positive k = 4 + memory_common_prefix(src + pos + 5,
                                                        src + pos + 5 - d, n - pos - 5);
                                if (k > match + 1)
                                        match = 0;
                        }
                }
                if (match)
                {
                        positive run = pos - lit_at;
                        positive k;

                        if (lit_n + run > n)
                                return zstd_emit_raw_block(src, n, last);
                        memory_copy(zstd_enc_lits + lit_n, src + lit_at, run);
                        lit_n += run;
                        zstd_seqs[nseq].lit = run;
                        zstd_seqs[nseq].match = match;
                        /* RFC repeat offsets depend on whether LL is zero.
                           Keep the speculative history local until this block
                           has won against raw output. */
                        positive value = dist + 3;
                        positive which = 3;
                        if (run && dist == rep[0])
                                value = 1, which = 0;
                        else if (dist == rep[1])
                                value = run ? 2 : 1, which = 1;
                        else if (dist == rep[2])
                                value = run ? 3 : 2, which = 2;
                        else if (!run && rep[0] > 1 && dist == rep[0] - 1)
                                value = 3;
                        if (which)
                        {
                                if (which != 1)
                                        rep[2] = rep[1];
                                rep[1] = rep[0];
                                rep[0] = (p32)dist;
                        }
                        zstd_seqs[nseq].off = value;
                        p8 llc = zstd_seq_code(zstd_ll_base, 35, run);
                        p8 mlc = zstd_seq_code(zstd_ml_base, 52, match);
                        p8 ofc = zstd_off_code(value);
                        zstd_seqs[nseq].ll_code = llc;
                        zstd_seqs[nseq].ml_code = mlc;
                        zstd_seqs[nseq].of_code = ofc;
                        ll_freq[llc]++; ml_freq[mlc]++; of_freq[ofc]++;
                        nseq++;
                        /* Keep two complete periods plus the three hash bytes
                           crossing the match boundary. Earlier equal hashes
                           would be replaced by these same final positions. */
                        k = dist <= 16 && match > 2 * dist + 3
                                ? match - 2 * dist - 3 : 1;
                        for (; k < match; k++)
                                if (pos + k + 4 <= n)
                                {
                                        p16 hh = zstd_enc_hash4(src + pos + k);

                                        zstd_enc_head[hh] = (zstd_enc_head[hh] << 32) |
                                            (p32)(zstd_enc_abs + pos + k + 1);
                                }
                        pos += match;
                        lit_at = pos;
                }
                else
                {
                        /* Long literal runs make dense hash probes expensive
                           without producing sequences. Resume dense probing
                           immediately after a match. */
                        positive step = 1 + ((pos - lit_at) >> 8);
                        if (step > 16) step = 16;
                        pos += step < n - pos ? step : n - pos;
                }
        }
        {
                positive run = n - lit_at;

                memory_copy(zstd_enc_lits + lit_n, src + lit_at, run);
                lit_n += run;
        }
        if (lit_n < 32)
        {
                lit_hdr[0] = (p8)(lit_n << 3);
                used = 1;
        }
        else if (lit_n < 4096)
        {
                p16 v = (p16)((lit_n << 4) | 4);

                lit_hdr[0] = (p8)v;
                lit_hdr[1] = (p8)(v >> 8);
                used = 2;
        }
        else
        {
                p32 v = (lit_n << 4) | 12;

                lit_hdr[0] = (p8)v;
                lit_hdr[1] = (p8)(v >> 8);
                lit_hdr[2] = (p8)(v >> 16);
                used = 3;
        }
        lit_size = zstd_pack_literals(zstd_enc_lits, lit_n, lit_hdr,
                                        address_of used);
        if (lit_size)
                lit_data = zstd_packed_lits;
        else
                lit_size = lit_n;
        if (nseq < 128)
        {
                seq_hdr[0] = (p8)nseq;
                seq_hdr_n = 1;
        }
        else if (nseq >= 0x7f00)
        {
                seq_hdr[0] = 255;
                seq_hdr[1] = (p8)(nseq - 0x7f00);
                seq_hdr[2] = (p8)((nseq - 0x7f00) >> 8);
                seq_hdr_n = 3;
        }
        else
        {
                seq_hdr[0] = (p8)(128 + (nseq >> 8));
                seq_hdr[1] = (p8)nseq;
                seq_hdr_n = 2;
        }
        if (nseq)
        {
                ll_table = zstd_sequence_model(ll_freq, nseq, 35, 9, zstd_ll_default,
                    ll_table, address_of zstd_ct_dynamic[0], model_header, address_of model_header_n);
                of_table = zstd_sequence_model(of_freq, nseq, 28, 8, zstd_of_default,
                    of_table, address_of zstd_ct_dynamic[1], model_header + model_header_n, address_of model_header_n);
                ml_table = zstd_sequence_model(ml_freq, nseq, 52, 9, zstd_ml_default,
                    ml_table, address_of zstd_ct_dynamic[2], model_header + model_header_n, address_of model_header_n);
                seq_hdr[seq_hdr_n++] =
                    (ll_table != address_of zstd_ct_ll ? 2u << 6 : 0) |
                    (of_table != address_of zstd_ct_of ? 2u << 4 : 0) |
                    (ml_table != address_of zstd_ct_ml ? 2u << 2 : 0);
        }
        if (nseq)
        {
                zstd_enc_seq last_seq = zstd_seqs[nseq - 1];
                p8 llc = last_seq.ll_code;
                p8 mlc = last_seq.ml_code;
                p8 ofc = last_seq.of_code;
                p32 ll_x = last_seq.lit - zstd_ll_base[llc];
                p32 ml_x = last_seq.match - zstd_ml_base[mlc];
                p32 of_x = last_seq.off - ((positive)1 << ofc);

                if (!zstd_cstate_init2(address_of ll_st, ll_table,
                                       llc) ||
                    !zstd_cstate_init2(address_of of_st, of_table,
                                       ofc) ||
                    !zstd_cstate_init2(address_of ml_st, ml_table,
                                       mlc))
                        return zstd_emit_raw_block(src, n, last);
                zstd_bout_add(address_of bits, ll_x, zstd_ll_extra[llc]);
                zstd_bout_add(address_of bits, ml_x, zstd_ml_extra[mlc]);
                zstd_bout_add(address_of bits, of_x, ofc);
                at = nseq - 1;
                while (at)
                {
                        zstd_enc_seq seq;

                        at--;
                        seq = zstd_seqs[at];
                        llc = seq.ll_code;
                        mlc = seq.ml_code;
                        ofc = seq.of_code;
                        ll_x = seq.lit - zstd_ll_base[llc];
                        ml_x = seq.match - zstd_ml_base[mlc];
                        of_x = seq.off - ((positive)1 << ofc);
                        zstd_cstate_encode(address_of bits, address_of of_st,
                                           ofc);
                        zstd_cstate_encode(address_of bits, address_of ml_st,
                                           mlc);
                        zstd_cstate_encode(address_of bits, address_of ll_st,
                                           llc);
                        zstd_bout_add(address_of bits, ll_x, zstd_ll_extra[llc]);
                        zstd_bout_add(address_of bits, ml_x, zstd_ml_extra[mlc]);
                        zstd_bout_add(address_of bits, of_x, ofc);
                }
        }
        if (nseq)
        {
                zstd_cstate_flush(address_of bits, address_of ml_st);
                zstd_cstate_flush(address_of bits, address_of of_st);
                zstd_cstate_flush(address_of bits, address_of ll_st);
                if (!zstd_bout_close(address_of bits))
                        return zstd_emit_raw_block(src, n, last);
        }
        {
                positive csize = used + lit_size + seq_hdr_n + model_header_n + bits.n;

                if (csize >= n)
                        return zstd_emit_raw_block(src, n, last);
                pack = (last ? 1u : 0) | (2u << 1) | ((p32)csize << 3);
                header[0] = (p8)pack;
                header[1] = (p8)(pack >> 8);
                header[2] = (p8)(pack >> 16);
                if (!zstd_enc_out(header, 3) || !zstd_enc_out(lit_hdr, used) ||
                    !zstd_enc_out(lit_data, lit_size) ||
                    !zstd_enc_out(seq_hdr, seq_hdr_n) ||
                    !zstd_enc_out(model_header, model_header_n) ||
                    !zstd_enc_out(bits.buf, bits.n))
                        return false;
        }
        memory_copy_apart(zstd_enc_rep, rep, sizeof(rep));
        return true;
}

static bool zstd_emit_block(p8 address_to src, positive n, bool last)
{
        bool ok;
        if (n && memory_span_byte(src, src[0], n) == n)
        {
                p32 pack = (last ? 1u : 0) | 2u | ((p32)n << 3);
                p8 header[4] = {(p8)pack, (p8)(pack >> 8), (p8)(pack >> 16), src[0]};
                ok = zstd_enc_out(header, sizeof(header));
                if (n >= 5)
                        zstd_enc_head[zstd_enc_hash4(src)] =
                            ((p64)(p32)(zstd_enc_abs + n - 4) << 32) |
                            (p32)(zstd_enc_abs + n - 3);
        }
        else
                ok = n >= 12 ? zstd_emit_comp_block(src, n, last)
                             : zstd_emit_raw_block(src, n, last);
        if (ok)
        {
                zstd_enc_abs += n;
                zstd_enc_position += n;
                if (zstd_enc_position + ZSTD_BLOCK_MAX > sizeof(zstd_enc_storage))
                {
                        memory_copy_apart(zstd_enc_storage,
                            zstd_enc_storage + zstd_enc_position - ZSTD_ENC_WINDOW,
                            ZSTD_ENC_WINDOW);
                        zstd_enc_position = ZSTD_ENC_WINDOW;
                }
                if (zstd_enc_abs >= 0x80000000u)
                {
                        positive shift = zstd_enc_abs - ZSTD_ENC_WINDOW;
                        for (positive i = 0; i < 65536; i++)
                        {
                                p32 first = (p32)zstd_enc_head[i], second = (p32)(zstd_enc_head[i] >> 32);
                                first = first > shift ? first - shift : 0;
                                second = second > shift ? second - shift : 0;
                                zstd_enc_head[i] = ((p64)second << 32) | first;
                        }
                        zstd_enc_abs -= shift;
                }
        }
        return ok;
}

static bool zstd_encode_begin(bipolar out, p8 level)
{
        p8 head[6];

        (void)level;
        zstd_out_fd = out;
        zstd_out_mem = null;
        zstd_enc_fill = 0;
        zstd_enc_abs = 0;
        zstd_enc_position = 0;
        zstd_enc_rep[0] = 1;
        zstd_enc_rep[1] = 4;
        zstd_enc_rep[2] = 8;
        memory_fill(zstd_enc_head, 0, sizeof(zstd_enc_head));
        zstd_enc_open = true;
        zstd_why = null;
        zstd_xxh_start(address_of zstd_enc_hash, 0);
        head[0] = 0x28;
        head[1] = 0xb5;
        head[2] = 0x2f;
        head[3] = 0xfd;
        head[4] = 0x04;
        head[5] = 0x58; /* 2 MiB window */
        if (out >= 0)
        {
                if (system_write_all((positive)out, head, 6) != 6)
                        return zstd_fail("zstd write failed");
        }
        return true;
}

static bool zstd_encode_write(p8 address_to src, positive n)
{
        zstd_xxh_add(address_of zstd_enc_hash, src, n);
        while (n)
        {
                positive room = ZSTD_BLOCK_MAX - zstd_enc_fill;
                positive take = n < room ? n : room;

                memory_copy(zstd_enc_block + zstd_enc_fill, src, take);
                zstd_enc_fill += take;
                src += take;
                n -= take;
                if (zstd_enc_fill == ZSTD_BLOCK_MAX)
                {
                        if (!zstd_emit_block(zstd_enc_block, zstd_enc_fill,
                                             false))
                                return false;
                        zstd_enc_fill = 0;
                }
        }
        return true;
}

static bool zstd_encode_end(void)
{
        p32 sum;
        p8 tail[4];

        if (!zstd_emit_block(zstd_enc_block, zstd_enc_fill, true))
                return false;
        zstd_enc_fill = 0;
        sum = (p32)zstd_xxh_end(address_of zstd_enc_hash);
        tail[0] = (p8)sum;
        tail[1] = (p8)(sum >> 8);
        tail[2] = (p8)(sum >> 16);
        tail[3] = (p8)(sum >> 24);
        if (zstd_out_mem)
        {
                if (zstd_out_used + 4 > zstd_out_cap)
                        return zstd_fail("zstd output is too small");
                memory_copy(zstd_out_mem + zstd_out_used, tail, 4);
                zstd_out_used += 4;
                return true;
        }
        if (zstd_out_fd >= 0 &&
            system_write_all((positive)zstd_out_fd, tail, 4) != 4)
                return zstd_fail("zstd write failed");
        zstd_enc_open = false;
        return true;
}

static bipolar zstd_deflate_mem(p8 address_to src, positive src_len,
                                p8 address_to dst, positive dst_cap, p8 level)
{
        (void)level;
        zstd_out_mem = dst;
        zstd_out_cap = dst_cap;
        zstd_out_used = 0;
        zstd_out_fd = -1;
        zstd_enc_fill = 0;
        zstd_enc_abs = 0;
        zstd_enc_position = 0;
        zstd_enc_rep[0] = 1;
        zstd_enc_rep[1] = 4;
        zstd_enc_rep[2] = 8;
        memory_fill(zstd_enc_head, 0, sizeof(zstd_enc_head));
        zstd_enc_open = true;
        zstd_why = null;
        zstd_xxh_start(address_of zstd_enc_hash, 0);
        if (6 > dst_cap)
                return -1;
        dst[0] = 0x28;
        dst[1] = 0xb5;
        dst[2] = 0x2f;
        dst[3] = 0xfd;
        dst[4] = 0x04;
        dst[5] = 0x58; /* 2 MiB window */
        zstd_out_used = 6;
        if (!zstd_encode_write(src, src_len))
                return -1;
        if (!zstd_encode_end())
                return -1;
        zstd_out_mem = null;
        return (bipolar)zstd_out_used;
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

static bool zstd_suffix_add(string_address in, p8 address_to into, positive room)
{
        positive n = string_length(in);

        if (n + 5 >= room)
                return false;
        memory_copy(into, in, n);
        memory_copy(into + n, ".zst", 5);
        return true;
}

static b32 zstd_encode_fd(bipolar in, bipolar out)
{
        p8 buf[ZSTD_OUT];
        bipolar got;

        if (!zstd_encode_begin(out, zstd_cli_level))
        {
                zstd_refuse(zstd_why ? zstd_why : (string_address) "encode failed");
                return 1;
        }
        for (;;)
        {
                got = system_read_retry((positive)in, buf, sizeof(buf));
                if (got < 0)
                {
                        zstd_refuse("read failed");
                        return 1;
                }
                if (!got)
                        break;
                if (!zstd_encode_write(buf, (positive)got))
                {
                        zstd_refuse(zstd_why ? zstd_why
                                             : (string_address) "encode failed");
                        return 1;
                }
        }
        if (!zstd_encode_end())
        {
                zstd_refuse(zstd_why ? zstd_why : (string_address) "encode failed");
                return 1;
        }
        return 0;
}

static b32 zstd_one(bipolar in, bipolar out)
{
        bool ok;

        zstd_src_fd(in);
        zstd_out_mem = null;
        zstd_out_fd = out;
        zstd_pull = false;
        zstd_live = false;
        zstd_frame_open = false;
        zstd_paused = false;
        zstd_need_trailer = false;
        ok = zstd_stream();
        if (ok)
                ok = zstd_flush();
        zstd_window_close();
        if (!ok)
        {
                zstd_refuse(zstd_why ? zstd_why
                                     : (string_address) "decode failed");
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
        p8 level = 3;
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
                        if (string_equals(word, "--compress"))
                        {
                                decompress = false;
                                continue;
                        }
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
                                              "Usage: zstd [-cdfkqz123456789] [-o FILE] [--rm] [FILE...]\n");
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
                                else if (*letters == 'z')
                                        decompress = false;
                                else if (*letters == 'c')
                                        stdout_out = true;
                                else if (*letters >= '1' && *letters <= '9')
                                        level = (p8)(*letters - '0');
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
                                                      "Usage: zstd [-cdfkqz123456789] [-o FILE] [--rm] [FILE...]\n");
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

        zstd_cli_level = level;
        if (at >= count)
        {
                bipolar out = test ? -1 : 1;

                if (decompress)
                        return zstd_one(0, out);
                return zstd_encode_fd(0, out);
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
                                out = system_open_output_at(AT_FDCWD, out_path,
                                                            force, 0666);
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
                        else if (decompress
                                         ? !zstd_suffix_out(path, out_name,
                                                            sizeof(out_name))
                                         : !zstd_suffix_add(path, out_name,
                                                            sizeof(out_name)))
                        {
                                zstd_refuse("cannot guess output name; use -c or -o");
                                system_close(in);
                                break;
                        }
                        else
                        {
                                out = system_open_output_at(AT_FDCWD, out_name,
                                                            force, 0666);
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

                if (decompress)
                        zstd_one(in, out);
                else
                        zstd_encode_fd(in, out);
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
