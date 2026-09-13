/*
        gzip -- RFC 1952 members around RFC 1951 deflate.

        Decode uses primary Huffman tables and a shared assembly token loop,
        with a canonical decoder for long codes and refill tails. Encode
        is a 32 KiB hash chain with lazy matching and a
        dynamic Huffman block, falling back to fixed or stored when the
        tree would not pay. Match lengths are memory_common_prefix;
        checksums are hash_crc32. Concatenated members are accepted the
        way gzip -d accepts them. There is no encryption, no LZW, and no
        zlib wrapper.
*/

#define GZIP_MAGIC0 0x1f
#define GZIP_MAGIC1 0x8b
#define GZIP_METHOD 8
#define GZIP_WINDOW 32768
#define GZIP_WMASK (GZIP_WINDOW - 1)
#define GZIP_IN 16384
#define GZIP_OUT 16384
#define GZIP_MAXBITS 15
#define GZIP_MAXLIT 288
#define GZIP_MAXDIST 32
#define GZIP_HASH_BITS 16
#define GZIP_HASH_SIZE (1u << GZIP_HASH_BITS)
#define GZIP_MIN_MATCH 3
#define GZIP_MAX_MATCH 258

#define GZIP_FTEXT 1
#define GZIP_FHCRC 2
#define GZIP_FEXTRA 4
#define GZIP_FNAME 8
#define GZIP_FCOMMENT 16

static const p8 gzip_len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const p16 gzip_len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const p8 gzip_dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const p16 gzip_dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
        12289, 16385, 24577};
static const p8 gzip_clen_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static p8 gzip_window[GZIP_WINDOW];
static positive gzip_wpos;
static p32 gzip_crc;
static p32 gzip_isize;

static p64 gzip_bits;
static p8 gzip_bitn;
static p8 gzip_in_buf[GZIP_IN];
static positive gzip_in_at;
static positive gzip_in_have;
static bool gzip_in_eof;
static bipolar gzip_in_fd;
static p8 address_to gzip_in_mem;
static positive gzip_in_mem_len;
static positive gzip_in_mem_at;

/* A linear history prefix lets the assembly decoder copy whole matches.
   Framing and pull reads slide this prefix only once per output slab. */
static p8 gzip_out_storage[GZIP_WINDOW + GZIP_OUT + GZIP_MAX_MATCH];
#define gzip_out_buf (gzip_out_storage + GZIP_WINDOW)
static bool gzip_decoding;
static positive gzip_out_fill;
static positive gzip_out_taken;
static bool gzip_pull;
static bool gzip_paused;
static bool gzip_have_block;
static bool gzip_block_last;
static p8 gzip_block_kind;
static bool gzip_stored_open;
static positive gzip_stored_left;
static bool gzip_finished;
static bipolar gzip_out_fd;
static p8 address_to gzip_out_mem;
static positive gzip_out_cap;
static positive gzip_out_used;
static bool gzip_out_failed;
static string_address gzip_why;
static b32 gzip_status;

static p16 gzip_lit_count[GZIP_MAXBITS + 1];
static p16 gzip_lit_symbol[GZIP_MAXLIT];
static p16 gzip_dist_count[GZIP_MAXBITS + 1];
static p16 gzip_dist_symbol[GZIP_MAXDIST];
static p16 gzip_code_count[GZIP_MAXBITS + 1];
static p16 gzip_code_symbol[19];
/* Low bits are the wire prefix, low nine cell bits the symbol, high
   bits the number consumed. Zero cells go through the canonical walker. */
static p16 gzip_lit_quick[2048];
static p16 gzip_dist_quick[256];

static p32 gzip_head[GZIP_HASH_SIZE];
static p32 gzip_prev[GZIP_WINDOW];
static p8 gzip_level;

static p32 gzip_crc_byte(p32 crc, p8 byte)
{
        return hash_crc32_tab[(crc ^ byte) & 255] ^ (crc >> 8);
}

static bool gzip_fail(string_address why)
{
        gzip_why = why;
        return false;
}

static bool gzip_in_need(positive want)
{
        bipolar got;

        if (gzip_in_have - gzip_in_at >= want)
                return true;
        if (gzip_in_eof)
                return gzip_in_have > gzip_in_at;

        if (gzip_in_at)
        {
                if (gzip_in_at < gzip_in_have)
                        memory_copy_apart(gzip_in_buf, gzip_in_buf + gzip_in_at,
                                          gzip_in_have - gzip_in_at);
                gzip_in_have -= gzip_in_at;
                gzip_in_at = 0;
        }

        if (gzip_in_mem)
        {
                positive left = gzip_in_mem_len - gzip_in_mem_at;
                positive take = left > (GZIP_IN - gzip_in_have)
                                    ? (GZIP_IN - gzip_in_have)
                                    : left;

                if (take)
                {
                        memory_copy(gzip_in_buf + gzip_in_have,
                                    gzip_in_mem + gzip_in_mem_at, take);
                        gzip_in_have += take;
                        gzip_in_mem_at += take;
                }
                if (gzip_in_mem_at >= gzip_in_mem_len)
                        gzip_in_eof = true;
                return gzip_in_have > gzip_in_at;
        }

        got = system_read_retry((positive)gzip_in_fd,
                                gzip_in_buf + gzip_in_have,
                                GZIP_IN - gzip_in_have);
        if (got < 0)
                return gzip_fail("gzip read failed");
        if (!got)
                gzip_in_eof = true;
        else
                gzip_in_have += (positive)got;
        return gzip_in_have > gzip_in_at;
}

static bipolar gzip_in_byte(void)
{
        if (gzip_in_at >= gzip_in_have && !gzip_in_need(1))
                return -1;
        if (gzip_in_at >= gzip_in_have)
                return -1;
        return gzip_in_buf[gzip_in_at++];
}

static bool gzip_align(void)
{
        positive rewind = gzip_bitn >> 3;

        if (rewind > gzip_in_at)
                return gzip_fail("gzip bit rewind");
        gzip_in_at -= rewind;
        gzip_bits = 0;
        gzip_bitn = 0;
        return true;
}

static bipolar gzip_get(p8 n)
{
        p32 mask;
        p32 value;

        while (gzip_bitn < n)
        {
                bipolar byte = gzip_in_byte();

                if (byte < 0)
                        return gzip_fail("gzip truncated bitstream"), -1;
                gzip_bits |= (p64)(p8)byte << gzip_bitn;
                gzip_bitn += 8;
        }

        mask = n == 32 ? 0xffffffffu : (1u << n) - 1;
        value = (p32)gzip_bits & mask;
        gzip_bits >>= n;
        gzip_bitn -= n;
        return (bipolar)value;
}

static bool gzip_out_flush(void)
{
        if (gzip_out_failed)
                return false;
        if (!gzip_out_fill)
                return true;
        if (gzip_out_mem)
        {
                if (gzip_out_used > gzip_out_cap ||
                    gzip_out_fill > gzip_out_cap - gzip_out_used)
                {
                        gzip_out_failed = true;
                        return gzip_fail("gzip output is too small");
                }
                memory_copy(gzip_out_mem + gzip_out_used, gzip_out_buf,
                            gzip_out_fill);
                gzip_out_used += gzip_out_fill;
                if (gzip_decoding)
                        memory_copy(gzip_out_storage,
                                    gzip_out_buf + gzip_out_fill - GZIP_WINDOW,
                                    GZIP_WINDOW);
                gzip_out_fill = 0;
                return true;
        }
        if (gzip_out_fd < 0)
        {
                if (gzip_decoding)
                        memory_copy(gzip_out_storage,
                                    gzip_out_buf + gzip_out_fill - GZIP_WINDOW,
                                    GZIP_WINDOW);
                gzip_out_fill = 0;
                return true;
        }
        if (system_write_all((positive)gzip_out_fd, gzip_out_buf,
                             gzip_out_fill) != gzip_out_fill)
        {
                gzip_out_failed = true;
                return gzip_fail("gzip write failed");
        }
        if (gzip_decoding)
                memory_copy(gzip_out_storage,
                            gzip_out_buf + gzip_out_fill - GZIP_WINDOW,
                            GZIP_WINDOW);
        gzip_out_fill = 0;
        return true;
}

static bool gzip_out_reserve(positive need)
{
        if (gzip_out_failed || need > GZIP_OUT)
                return false;
        if (gzip_out_fill > GZIP_OUT - need && !gzip_out_flush())
                return false;
        return true;
}

static bool gzip_emit(p8 byte)
{
        gzip_window[gzip_wpos & GZIP_WMASK] = byte;
        gzip_wpos++;
        gzip_crc = gzip_crc_byte(gzip_crc, byte);
        gzip_isize++;
        gzip_out_buf[gzip_out_fill++] = byte;
        if (!gzip_pull && gzip_out_fill >= GZIP_OUT)
                return gzip_out_flush();
        return true;
}

/* Record a complete span in the circular history. Match expansion uses
   the shared forward-copy primitive, including distances shorter than a
   machine word; copying an overlapping LZ match is not memmove. */
static fn gzip_record(p8 address_to bytes, positive length)
{
        positive at = gzip_wpos & GZIP_WMASK;
        positive first = GZIP_WINDOW - at;

        if (first > length)
                first = length;
        memory_copy_apart(gzip_window + at, bytes, first);
        if (length > first)
                memory_copy_apart(gzip_window, bytes + first, length - first);
        gzip_wpos += length;
        gzip_isize += length;
        gzip_crc = hash_crc32(gzip_crc, bytes, length);
}

static bool gzip_emit_match(positive dist, positive length)
{
        p8 address_to into = gzip_out_buf + gzip_out_fill;
        positive seed = length < dist ? length : dist;
        positive at = (gzip_wpos - dist) & GZIP_WMASK;
        positive first = GZIP_WINDOW - at;

        if (dist <= gzip_out_fill)
                memory_copy_match(into, dist, length);
        else
        {
                if (first > seed)
                        first = seed;
                memory_copy_apart(into, gzip_window + at, first);
                if (seed > first)
                        memory_copy_apart(into + first, gzip_window, seed - first);
                if (length > seed)
                        memory_copy_match(into + seed, dist, length - seed);
        }
        gzip_record(into, length);
        gzip_out_fill += length;
        if (!gzip_pull && gzip_out_fill >= GZIP_OUT)
                return gzip_out_flush();
        return true;
}

static fn gzip_quick_table(p16 address_to count, p16 address_to symbol,
                           p16 address_to table, positive bits)
{
        positive code = 0;
        positive index = 0;
        positive len;

        memory_fill(table, 0, ((positive)1 << bits) * sizeof(p16));
        for (len = 1; len <= bits; len++)
        {
                positive stop = index + count[len];

                while (index < stop)
                {
                        positive reversed = 0;
                        positive k;
                        positive v = code++;
                        for (k = 0; k < len; k++)
                                reversed = (reversed << 1) | (v & 1), v >>= 1;
                        for (k = reversed; k < ((positive)1 << bits);
                             k += (positive)1 << len)
                                table[k] = (p16)((len << 9) | symbol[index]);
                        index++;
                }
                code <<= 1;
        }
}

static bool gzip_huffman(p8 address_to length, positive n,
                         p16 address_to count, p16 address_to symbol)
{
        positive len;
        positive left;
        positive at;
        p16 offs[GZIP_MAXBITS + 1];

        memory_fill(count, 0, (GZIP_MAXBITS + 1) * sizeof(p16));
        for (at = 0; at < n; at++)
                if (length[at] > GZIP_MAXBITS)
                        return gzip_fail("gzip Huffman length");
                else
                        count[length[at]]++;

        count[0] = 0;
        left = 1;
        for (len = 1; len <= GZIP_MAXBITS; len++)
        {
                left <<= 1;
                if (left < count[len])
                        return gzip_fail("gzip Huffman over-subscribed");
                left -= count[len];
        }

        offs[1] = 0;
        for (len = 1; len < GZIP_MAXBITS; len++)
                offs[len + 1] = offs[len] + count[len];

        for (at = 0; at < n; at++)
                if (length[at])
                        symbol[offs[length[at]]++] = (p16)at;
        if (count == gzip_lit_count)
                gzip_quick_table(count, symbol, gzip_lit_quick, 11);
        else if (count == gzip_dist_count)
                gzip_quick_table(count, symbol, gzip_dist_quick, 8);
        return true;
}

static bipolar gzip_decode_slow(p16 address_to count, p16 address_to symbol)
{
        positive len;
        p32 code = 0;
        p32 first = 0;
        positive index = 0;

        for (len = 1; len <= GZIP_MAXBITS; len++)
        {
                bipolar bit = gzip_get(1);
                p32 have;

                if (bit < 0)
                        return -1;
                code |= (p32)bit;
                have = count[len];
                if (code - first < have)
                        return symbol[index + (code - first)];
                index += have;
                first += have;
                first <<= 1;
                code <<= 1;
        }

        return gzip_fail("gzip bad Huffman code"), -1;
}

static bipolar gzip_decode(p16 address_to count, p16 address_to symbol)
{
        positive root = count == gzip_lit_count ? 11 : 8;
        p16 address_to table = count == gzip_lit_count ? gzip_lit_quick
                                                      : gzip_dist_quick;
        if (count != gzip_code_count)
        {
                /* Stay inside this input slab. The canonical tail does the
                   refills, so alignment can always return lookahead bytes. */
                while (gzip_bitn < root && gzip_in_at < gzip_in_have)
                {
                        gzip_bits |= (p64)gzip_in_buf[gzip_in_at++] << gzip_bitn;
                        gzip_bitn += 8;
                }
                p16 cell = table[gzip_bits & (((positive)1 << root) - 1)];
                positive take = cell >> 9;
                if (take && take <= gzip_bitn)
                {
                        gzip_bits >>= take;
                        gzip_bitn -= take;
                        return cell & 511;
                }
        }
        return gzip_decode_slow(count, symbol);
}

static bool gzip_dynamic(void)
{
        bipolar hlit;
        bipolar hdist;
        bipolar hclen;
        p8 lengths[GZIP_MAXLIT + GZIP_MAXDIST];
        p8 clen[19];
        positive nlit;
        positive ndist;
        positive ncode;
        positive at;
        p16 last = 0;

        hlit = gzip_get(5);
        hdist = gzip_get(5);
        hclen = gzip_get(4);
        if (hlit < 0 || hdist < 0 || hclen < 0)
                return false;
        nlit = (positive)hlit + 257;
        ndist = (positive)hdist + 1;
        ncode = (positive)hclen + 4;
        if (nlit > GZIP_MAXLIT || ndist > GZIP_MAXDIST)
                return gzip_fail("gzip dynamic tree size");

        memory_fill(clen, 0, sizeof(clen));
        for (at = 0; at < ncode; at++)
        {
                bipolar len = gzip_get(3);

                if (len < 0)
                        return false;
                clen[gzip_clen_order[at]] = (p8)len;
        }

        if (!gzip_huffman(clen, 19, gzip_code_count, gzip_code_symbol))
                return false;

        at = 0;
        while (at < nlit + ndist)
        {
                bipolar sym = gzip_decode(gzip_code_count, gzip_code_symbol);
                p16 repeat;
                p8 fill;

                if (sym < 0)
                        return false;
                if (sym < 16)
                {
                        lengths[at++] = (p8)sym;
                        last = (p16)sym;
                        continue;
                }
                if (sym == 16)
                {
                        bipolar extra = gzip_get(2);

                        if (extra < 0)
                                return false;
                        if (!at)
                                return gzip_fail("gzip repeat with no length");
                        repeat = (p16)extra + 3;
                        fill = (p8)last;
                }
                else if (sym == 17)
                {
                        bipolar extra = gzip_get(3);

                        if (extra < 0)
                                return false;
                        repeat = (p16)extra + 3;
                        fill = 0;
                }
                else
                {
                        bipolar extra = gzip_get(7);

                        if (extra < 0)
                                return false;
                        repeat = (p16)extra + 11;
                        fill = 0;
                }
                if (at + repeat > nlit + ndist)
                        return gzip_fail("gzip length overflow");
                while (repeat)
                {
                        lengths[at++] = fill;
                        repeat--;
                }
                last = fill;
        }

        if (!lengths[256])
                return gzip_fail("gzip missing end-of-block");
        if (!gzip_huffman(lengths, nlit, gzip_lit_count, gzip_lit_symbol))
                return false;
        if (!gzip_huffman(lengths + nlit, ndist, gzip_dist_count,
                          gzip_dist_symbol))
                return false;
        return true;
}

static bool gzip_fixed(void)
{
        p8 lengths[GZIP_MAXLIT];
        p8 dist[GZIP_MAXDIST];
        positive at;

        for (at = 0; at <= 143; at++)
                lengths[at] = 8;
        for (; at <= 255; at++)
                lengths[at] = 9;
        for (; at <= 279; at++)
                lengths[at] = 7;
        for (; at <= 287; at++)
                lengths[at] = 8;
        for (at = 0; at < GZIP_MAXDIST; at++)
                dist[at] = 5;
        if (!gzip_huffman(lengths, GZIP_MAXLIT, gzip_lit_count, gzip_lit_symbol))
                return false;
        return gzip_huffman(dist, GZIP_MAXDIST, gzip_dist_count,
                            gzip_dist_symbol);
}

static const p32 gzip_length_info[29] = {
        0x00000003, 0x00000004, 0x00000005, 0x00000006, 0x00000007, 0x00000008,
        0x00000009, 0x0000000a, 0x0001000b, 0x0001000d, 0x0001000f, 0x00010011,
        0x00020013, 0x00020017, 0x0002001b, 0x0002001f, 0x00030023, 0x0003002b,
        0x00030033, 0x0003003b, 0x00040043, 0x00040053, 0x00040063, 0x00040073,
        0x00050083, 0x000500a3, 0x000500c3, 0x000500e3, 0x00000102,
};
static const p32 gzip_distance_info[30] = {
        0x00000001, 0x00000002, 0x00000003, 0x00000004, 0x00010005, 0x00010007,
        0x00020009, 0x0002000d, 0x00030011, 0x00030019, 0x00040021, 0x00040031,
        0x00050041, 0x00050061, 0x00060081, 0x000600c1, 0x00070101, 0x00070181,
        0x00080201, 0x00080301, 0x00090401, 0x00090601, 0x000a0801, 0x000a0c01,
        0x000b1001, 0x000b1801, 0x000c2001, 0x000c3001, 0x000d4001, 0x000d6001,
};
typedef struct
{
        p64 bits;
        positive count;
        p8 address_to next;
        p8 address_to limit;
        p8 address_to out;
        p8 address_to out_limit;
        positive history;
        p16 address_to lit;
        p16 address_to dist;
        const p32 address_to lengths;
        const p32 address_to distances;
} gzip_decode_job;

static fn gzip_fast_span(void)
{
        gzip_decode_job job = {gzip_bits, gzip_bitn,
                gzip_in_buf + gzip_in_at, gzip_in_buf + gzip_in_have,
                gzip_out_buf + gzip_out_fill, gzip_out_buf + GZIP_OUT,
                gzip_wpos, gzip_lit_quick, gzip_dist_quick,
                gzip_length_info, gzip_distance_info};
        p8 address_to start = job.out;
        deflate_decode_span(address_of job);
        gzip_bits = job.bits;
        gzip_bitn = (p8)job.count;
        gzip_in_at = (positive)(job.next - gzip_in_buf);
        positive n = (positive)(job.out - start);
        if (n)
                gzip_record(start, n), gzip_out_fill += n;
}

static bool gzip_codes(void)
{
        for (;;)
        {
                bipolar extra;
                bipolar dist_sym;
                positive length;
                positive dist;
                bipolar sym;

                if (gzip_pull && gzip_out_fill >= GZIP_OUT)
                {
                        gzip_paused = true;
                        return true;
                }

                if (gzip_in_have - gzip_in_at >= 8 &&
                    GZIP_OUT - gzip_out_fill >= GZIP_MAX_MATCH)
                        gzip_fast_span();

                sym = gzip_decode(gzip_lit_count, gzip_lit_symbol);

                if (sym < 0)
                        return false;
                if (sym < 256)
                {
                        if (!gzip_emit((p8)sym))
                                return false;
                        continue;
                }
                if (sym == 256)
                        return true;
                if (sym > 285)
                        return gzip_fail("gzip length symbol");
                extra = gzip_get(gzip_len_extra[sym - 257]);
                if (extra < 0)
                        return false;
                length = gzip_len_base[sym - 257] + (positive)extra;
                dist_sym = gzip_decode(gzip_dist_count, gzip_dist_symbol);
                if (dist_sym < 0)
                        return false;
                if (dist_sym >= 30)
                        return gzip_fail("gzip distance symbol");
                extra = gzip_get(gzip_dist_extra[dist_sym]);
                if (extra < 0)
                        return false;
                dist = gzip_dist_base[dist_sym] + (positive)extra;
                if (!dist || dist > gzip_wpos)
                        return gzip_fail("gzip distance");
                if (!gzip_emit_match(dist, length))
                        return false;
        }
}

static bool gzip_stored(void)
{
        if (!gzip_stored_open)
        {
                bipolar len;
                bipolar nlen;

                gzip_align();
                len = gzip_get(16);
                nlen = gzip_get(16);
                if (len < 0 || nlen < 0)
                        return false;
                if ((p16)len != (p16)(~(p16)nlen))
                        return gzip_fail("gzip stored length");
                gzip_stored_left = (positive)len;
                gzip_bits = 0;
                gzip_bitn = 0;
                gzip_stored_open = true;
        }

        while (gzip_stored_left)
        {
                if (gzip_pull && gzip_out_fill >= GZIP_OUT)
                {
                        gzip_paused = true;
                        return true;
                }
                if (gzip_in_at == gzip_in_have && !gzip_in_need(1))
                        return gzip_fail("gzip truncated stored block");
                positive take = gzip_in_have - gzip_in_at;
                if (take > gzip_stored_left)
                        take = gzip_stored_left;
                if (take > GZIP_OUT - gzip_out_fill)
                        take = GZIP_OUT - gzip_out_fill;
                if (!take)
                        return gzip_fail("gzip truncated stored block");
                p8 address_to bytes = gzip_in_buf + gzip_in_at;
                memory_copy_apart(gzip_out_buf + gzip_out_fill, bytes, take);
                gzip_record(bytes, take);
                gzip_in_at += take;
                gzip_out_fill += take;
                gzip_stored_left -= take;
                if (!gzip_pull && gzip_out_fill >= GZIP_OUT && !gzip_out_flush())
                        return false;
        }

        gzip_stored_open = false;
        return true;
}

static bool gzip_blocks(void)
{
        for (;;)
        {
                if (!gzip_have_block)
                {
                        bipolar last = gzip_get(1);
                        bipolar type;

                        if (last < 0)
                                return false;
                        type = gzip_get(2);
                        if (type < 0)
                                return false;
                        gzip_block_last = last != 0;
                        if (type == 0)
                        {
                                gzip_stored_open = false;
                                gzip_block_kind = 0;
                        }
                        else if (type == 1)
                        {
                                if (!gzip_fixed())
                                        return false;
                                gzip_block_kind = 1;
                        }
                        else if (type == 2)
                        {
                                if (!gzip_dynamic())
                                        return false;
                                gzip_block_kind = 1;
                        }
                        else
                                return gzip_fail("gzip reserved block type");
                        gzip_have_block = true;
                }

                if (gzip_block_kind == 0)
                {
                        if (!gzip_stored())
                                return false;
                }
                else if (!gzip_codes())
                        return false;

                if (gzip_paused)
                        return true;

                gzip_have_block = false;
                if (gzip_block_last)
                        return true;
        }
}

static bool gzip_skip_string(void)
{
        bipolar byte;

        do
        {
                byte = gzip_in_byte();
                if (byte < 0)
                        return gzip_fail("gzip truncated header string");
        } while (byte);

        return true;
}

static bool gzip_head_done;

static bool gzip_member(void)
{
        bipolar method;
        bipolar flags;
        bipolar extra;
        p32 expect;
        p32 got_crc;
        p32 got_size;
        positive at;

        if (!gzip_head_done)
        {
                if (gzip_in_byte() != GZIP_MAGIC0 ||
                    gzip_in_byte() != GZIP_MAGIC1)
                        return gzip_fail("gzip bad magic");
                method = gzip_in_byte();
                flags = gzip_in_byte();
                if (method != GZIP_METHOD)
                        return gzip_fail("gzip method is not deflate");
                if (flags < 0)
                        return gzip_fail("gzip truncated header");
                if ((p8)flags & 0xe0)
                        return gzip_fail("gzip reserved header flags");
                for (at = 0; at < 6; at++)
                        if (gzip_in_byte() < 0)
                                return gzip_fail("gzip truncated header");

                if ((p8)flags & GZIP_FEXTRA)
                {
                        bipolar xlen = gzip_in_byte();
                        bipolar xlen_hi = gzip_in_byte();

                        if (xlen < 0 || xlen_hi < 0)
                                return gzip_fail("gzip truncated extra");
                        extra = xlen + (xlen_hi << 8);
                        while (extra--)
                                if (gzip_in_byte() < 0)
                                        return gzip_fail("gzip truncated extra");
                }
                if (((p8)flags & GZIP_FNAME) && !gzip_skip_string())
                        return false;
                if (((p8)flags & GZIP_FCOMMENT) && !gzip_skip_string())
                        return false;
                if ((p8)flags & GZIP_FHCRC)
                        if (gzip_in_byte() < 0 || gzip_in_byte() < 0)
                                return gzip_fail("gzip truncated header crc");

                gzip_decoding = true;
                gzip_wpos = 0;
                gzip_crc = 0xffffffffu;
                gzip_isize = 0;
                gzip_bits = 0;
                gzip_bitn = 0;
                gzip_have_block = false;
                gzip_stored_open = false;
                gzip_head_done = true;
        }

        gzip_paused = false;
        if (!gzip_blocks())
                return false;
        if (gzip_paused)
                return true;
        gzip_align();

        got_crc = 0;
        got_size = 0;
        for (at = 0; at < 4; at++)
        {
                bipolar byte = gzip_in_byte();

                if (byte < 0)
                        return gzip_fail("gzip truncated trailer");
                got_crc |= (p32)(p8)byte << (8 * at);
        }
        for (at = 0; at < 4; at++)
        {
                bipolar byte = gzip_in_byte();

                if (byte < 0)
                        return gzip_fail("gzip truncated trailer");
                got_size |= (p32)(p8)byte << (8 * at);
        }

        expect = ~gzip_crc;
        if (got_crc != expect)
                return gzip_fail("gzip crc mismatch");
        if (got_size != gzip_isize)
                return gzip_fail("gzip length mismatch");
        gzip_head_done = false;
        return true;
}

static bool gzip_stream_decode(void)
{
        gzip_out_taken = 0;
        bool any = false;

        gzip_why = null;
        gzip_out_failed = false;
        gzip_in_at = 0;
        gzip_in_have = 0;
        gzip_in_eof = false;
        gzip_out_fill = 0;
        gzip_bits = 0;
        gzip_bitn = 0;
        gzip_pull = false;
        gzip_paused = false;
        gzip_have_block = false;
        gzip_stored_open = false;
        gzip_head_done = false;
        gzip_finished = false;

        for (;;)
        {
                if (!gzip_in_need(1))
                        break;
                if (gzip_in_at >= gzip_in_have)
                        break;
                if (!gzip_member())
                        return false;
                any = true;
        }

        if (!any)
                return gzip_fail("gzip empty input");
        return gzip_out_flush();
}

static bipolar gzip_inflate_mem(p8 address_to src, positive src_len,
                                p8 address_to dst, positive dst_cap)
{
        bool ok;

        gzip_in_fd = -1;
        gzip_in_mem = src;
        gzip_in_mem_len = src_len;
        gzip_in_mem_at = 0;
        gzip_out_fd = -1;
        gzip_out_mem = dst;
        gzip_out_cap = dst_cap;
        gzip_out_used = 0;
        ok = gzip_stream_decode();
        gzip_in_mem = null;
        gzip_out_mem = null;
        return ok ? (bipolar)gzip_out_used : -1;
}

static p16 gzip_revbits(p16 code, p8 len)
{
        p16 reversed = 0;

        while (len)
        {
                reversed = (p16)((reversed << 1) | (code & 1));
                code >>= 1;
                len--;
        }
        return reversed;
}

static __attribute__((always_inline)) inline bool gzip_put(p32 value, p8 n)
{
        if (gzip_out_failed)
                return false;
        gzip_bits |= (p64)value << gzip_bitn;
        gzip_bitn += n;
        if (gzip_bitn >= 32)
        {
                if (!gzip_out_reserve(4))
                        return false;
                gzip_out_buf[gzip_out_fill] = (p8)gzip_bits;
                gzip_out_buf[gzip_out_fill + 1] = (p8)(gzip_bits >> 8);
                gzip_out_buf[gzip_out_fill + 2] = (p8)(gzip_bits >> 16);
                gzip_out_buf[gzip_out_fill + 3] = (p8)(gzip_bits >> 24);
                gzip_out_fill += 4;
                gzip_bits >>= 32;
                gzip_bitn -= 32;
                if (gzip_out_fill == GZIP_OUT && !gzip_out_flush())
                        return false;
        }
        return true;
}

static bool gzip_put_code(p16 code, p8 len)
{
        return gzip_put(code, len);
}

static bool gzip_put_flush(void)
{
        while (gzip_bitn)
        {
                if (!gzip_out_reserve(1))
                        return false;
                gzip_out_buf[gzip_out_fill++] = (p8)gzip_bits;
                gzip_bits >>= 8;
                gzip_bitn = gzip_bitn > 8 ? gzip_bitn - 8 : 0;
                if (gzip_out_fill == GZIP_OUT && !gzip_out_flush())
                        return false;
        }
        return gzip_out_flush();
}

static fn gzip_lengths_to_codes(p8 address_to length, positive n,
                                p16 address_to code)
{
        p16 next[GZIP_MAXBITS + 1];
        p16 count[GZIP_MAXBITS + 1];
        positive len;
        positive at;
        p16 walk = 0;

        memory_fill(count, 0, sizeof(count));
        memory_fill(code, 0, n * sizeof(p16));
        for (at = 0; at < n; at++)
                count[length[at]]++;
        count[0] = 0;
        next[0] = 0;
        for (len = 1; len <= GZIP_MAXBITS; len++)
        {
                walk = (p16)((walk + count[len - 1]) << 1);
                next[len] = walk;
        }
        for (at = 0; at < n; at++)
                if (length[at])
                        code[at] = gzip_revbits(next[length[at]]++, length[at]);
}

#include "compression_huffman.c"

static bool gzip_lengths_ok(p8 address_to length, positive n, p8 limit)
{
        p16 count[GZIP_MAXBITS + 1];
        positive left;
        positive len;
        positive at;

        memory_fill(count, 0, sizeof(count));
        for (at = 0; at < n; at++)
        {
                if (length[at] > limit)
                        return false;
                count[length[at]]++;
        }
        count[0] = 0;
        left = 1;
        for (len = 1; len <= limit; len++)
        {
                left <<= 1;
                if (left < count[len])
                        return false;
                left -= count[len];
        }
        return left == 0;
}

static bool gzip_used_coded(p32 address_to freq, p8 address_to length, positive n)
{
        positive at;

        for (at = 0; at < n; at++)
                if (freq[at] && !length[at])
                        return false;
        return true;
}

static const p8 gzip_length_codes[256] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11,
        12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15,
        16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17,
        18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19,
        20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
        21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
        22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
        23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
        24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
        24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
        25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
        25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
        26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
        26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
        27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
        27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28,
};

static positive gzip_length_code(positive length)
{
        return gzip_length_codes[length - 3];
}

static positive gzip_distance_code(positive dist)
{
        if (dist <= 4)
                return dist - 1;
        positive v = dist - 1;
#if X64 || ARM64
        positive hb = 63 - __builtin_clzll(v);
#else
        positive t = v;
        positive hb = 0;
        if (t >= 256) hb += 8, t >>= 8;
        if (t >= 16) hb += 4, t >>= 4;
        if (t >= 4) hb += 2, t >>= 2;
        if (t >= 2) hb++;
#endif
        return (hb << 1) + ((v >> (hb - 1)) & 1);
}

static bool gzip_write_stored(p8 address_to src, positive length, bool last)
{
        do
        {
                positive chunk = length > 65535 ? 65535 : length;
                positive left = chunk;
                if (!gzip_put(last && chunk == length, 1) ||
                    !gzip_put(0, 2) || !gzip_put_flush() ||
                    !gzip_out_reserve(4))
                        return false;
                gzip_out_buf[gzip_out_fill++] = (p8)chunk;
                gzip_out_buf[gzip_out_fill++] = (p8)(chunk >> 8);
                gzip_out_buf[gzip_out_fill++] = (p8)~chunk;
                gzip_out_buf[gzip_out_fill++] = (p8)(~chunk >> 8);
                while (left)
                {
                        if (!gzip_out_reserve(1))
                                return false;
                        positive room = GZIP_OUT - gzip_out_fill;
                        positive take = left < room ? left : room;
                        memory_copy(gzip_out_buf + gzip_out_fill, src, take);
                        gzip_out_fill += take;
                        src += take;
                        left -= take;
                        if (gzip_out_fill == GZIP_OUT && !gzip_out_flush())
                                return false;
                }
                length -= chunk;
        } while (length);
        return true;
}

/*
        Tokens: literal bytes, or matches stored as 0x8000|length plus dist.
        A block is one array of those plus the source slice they describe.
*/

#define GZIP_TOK_LIT 0
#define GZIP_TOK_MATCH 1

/* Keep a full history and a full input slab. A 258-byte lookahead-only
   buffer recopied 32 KiB after nearly every match and truncated chains. */
static p8 gzip_src_hold[2 * GZIP_WINDOW + GZIP_MAX_MATCH];
static positive gzip_src_fill;
static positive gzip_src_at;
static positive gzip_src_abs;
static bool gzip_src_eof;
static p8 address_to gzip_enc_mem;
static positive gzip_enc_mem_len;
static positive gzip_enc_mem_at;
static p8 address_to gzip_feed;
static positive gzip_feed_len;
static positive gzip_feed_at;

static bool gzip_enc_pull(void)
{
        bipolar got;
        positive room;

        if (gzip_src_eof)
                return true;

        if (gzip_src_at >= 2 * GZIP_WINDOW)
        {
                positive drop = GZIP_WINDOW;

                memory_copy_apart(gzip_src_hold, gzip_src_hold + drop,
                                  gzip_src_fill - drop);
                gzip_src_fill -= drop;
                gzip_src_at -= drop;
                gzip_src_abs += drop;
                if (gzip_src_abs >= 0x80000000u)
                {
                        positive shift = gzip_src_abs - GZIP_WINDOW;
                        for (positive i = 0; i < GZIP_HASH_SIZE; i++)
                                gzip_head[i] = gzip_head[i] >= shift
                                               ? gzip_head[i] - shift : 0;
                        for (positive i = 0; i < GZIP_WINDOW; i++)
                                gzip_prev[i] = gzip_prev[i] >= shift
                                               ? gzip_prev[i] - shift : 0;
                        gzip_src_abs -= shift;
                }
        }

        room = sizeof(gzip_src_hold) - gzip_src_fill;
        if (!room)
                return true;
        if (gzip_feed)
        {
                positive left = gzip_feed_len - gzip_feed_at;
                positive take = left > room ? room : left;

                if (take)
                {
                        memory_copy(gzip_src_hold + gzip_src_fill,
                                    gzip_feed + gzip_feed_at, take);
                        gzip_src_fill += take;
                        gzip_feed_at += take;
                }
                if (gzip_feed_at >= gzip_feed_len)
                        gzip_feed = null;
                return true;
        }
        if (gzip_enc_mem)
        {
                positive left = gzip_enc_mem_len - gzip_enc_mem_at;
                positive take = left > room ? room : left;

                if (take)
                {
                        memory_copy(gzip_src_hold + gzip_src_fill,
                                    gzip_enc_mem + gzip_enc_mem_at, take);
                        gzip_src_fill += take;
                        gzip_enc_mem_at += take;
                }
                if (gzip_enc_mem_at >= gzip_enc_mem_len)
                        gzip_src_eof = true;
                return gzip_src_at < gzip_src_fill || gzip_src_eof;
        }
        if (gzip_in_fd < 0)
                return true;
        got = system_read_retry((positive)gzip_in_fd,
                                gzip_src_hold + gzip_src_fill, room);
        if (got < 0)
                return gzip_fail("gzip read failed");
        if (!got)
                gzip_src_eof = true;
        else
                gzip_src_fill += (positive)got;
        return true;
}

static p16 gzip_hash3(p8 address_to bytes)
{
        p32 h = ((p32)bytes[0] << 16) ^ ((p32)bytes[1] << 8) ^ bytes[2];

        h *= 0x1e35a7bdu;
        return (p16)(h >> (32 - GZIP_HASH_BITS));
}

static positive gzip_match_at(positive pos, positive chain, positive nice,
                              positive best, positive address_to dist)
{
        positive left = gzip_src_fill - pos;
        positive current = gzip_src_abs + pos;
        positive earliest = current > GZIP_WINDOW ? current - GZIP_WINDOW : 1;
        positive steps = gzip_level >= 8 ? 4096 : gzip_level >= 6 ? 128
                                               : gzip_level >= 4 ? 32 : 8;
        p8 address_to here = gzip_src_hold + pos;
        if (earliest < gzip_src_abs) earliest = gzip_src_abs;
        if (!earliest) earliest = 1;
        if (left > GZIP_MAX_MATCH) left = GZIP_MAX_MATCH;
        if (left < GZIP_MIN_MATCH) return 0;
        if (best >= left || chain >= current) return best;
        if (best >= 8) steps >>= 2;
        while (chain >= earliest && steps--)
        {
                positive old = chain;
                /* Issue the dependent chain load beside the candidate load.
                   Strictly decreasing links make the upper-bound check an
                   entry check; only the window's lower bound remains hot. */
                positive next = gzip_prev[old & GZIP_WMASK];
                p8 address_to there = gzip_src_hold + (old - gzip_src_abs);
                chain = next < old ? next : 0;
                /* best is zero or at least three, and strictly below left.
                   The two-byte end check rejects common suffix collisions
                   before the front and full prefix loads. */
                if ((best && memory_load_unaligned(p16, here + best - 1) !=
                             memory_load_unaligned(p16, there + best - 1)) ||
                    memory_load_unaligned(p16, here) != memory_load_unaligned(p16, there) ||
                    here[2] != there[2] ||
                    (best >= 8 && memory_load_unaligned(p64, here) !=
                                  memory_load_unaligned(p64, there)))
                        continue;
                positive n = 3 + memory_common_prefix(here + 3, there + 3, left - 3);
                if (n > best)
                {
                        best = n;
                        address_to dist = current - old;
                        if (best >= nice || best == left) break;
                }
        }
        return best;
}

static fn gzip_insert(positive pos)
{
        p16 h;
        p32 prev;

        if (pos + 2 >= gzip_src_fill)
                return;
        h = gzip_hash3(gzip_src_hold + pos);
        prev = gzip_head[h];
        gzip_prev[(gzip_src_abs + pos) & GZIP_WMASK] = prev;
        gzip_head[h] = gzip_src_abs + pos;
}

static p8 gzip_fixed_lit_len[GZIP_MAXLIT];
static p16 gzip_fixed_lit_code[GZIP_MAXLIT];
static p8 gzip_fixed_dist_len[GZIP_MAXDIST];
static p16 gzip_fixed_dist_code[GZIP_MAXDIST];
static bool gzip_fixed_codes;

static fn gzip_fixed_init(void)
{
        positive at;

        if (gzip_fixed_codes)
                return;
        for (at = 0; at <= 143; at++)
                gzip_fixed_lit_len[at] = 8;
        for (; at <= 255; at++)
                gzip_fixed_lit_len[at] = 9;
        for (; at <= 279; at++)
                gzip_fixed_lit_len[at] = 7;
        for (; at <= 287; at++)
                gzip_fixed_lit_len[at] = 8;
        for (at = 0; at < GZIP_MAXDIST; at++)
                gzip_fixed_dist_len[at] = 5;
        gzip_lengths_to_codes(gzip_fixed_lit_len, GZIP_MAXLIT,
                              gzip_fixed_lit_code);
        gzip_lengths_to_codes(gzip_fixed_dist_len, GZIP_MAXDIST,
                              gzip_fixed_dist_code);
        gzip_fixed_codes = true;
}

static bool gzip_write_fixed(p8 address_to src, positive length,
                             p32 address_to mpos, p16 address_to mlen,
                             p16 address_to mdist, positive pairs, bool last)
{
        positive at = 0;
        positive pair = 0;

        gzip_fixed_init();
        if (!gzip_put(last ? 1 : 0, 1) || !gzip_put(1, 2))
                return false;
        while (at < length)
        {
                if (pair < pairs && mpos[pair] == at)
                {
                        positive lcode = gzip_length_code(mlen[pair]);
                        positive dcode = gzip_distance_code(mdist[pair]);

                        if (!gzip_put_code(gzip_fixed_lit_code[257 + lcode],
                                           gzip_fixed_lit_len[257 + lcode]) ||
                            (gzip_len_extra[lcode] &&
                             !gzip_put(mlen[pair] - gzip_len_base[lcode],
                                       gzip_len_extra[lcode])) ||
                            !gzip_put_code(gzip_fixed_dist_code[dcode],
                                           gzip_fixed_dist_len[dcode]) ||
                            (gzip_dist_extra[dcode] &&
                             !gzip_put(mdist[pair] - gzip_dist_base[dcode],
                                       gzip_dist_extra[dcode])))
                                return false;
                        at += mlen[pair];
                        pair++;
                }
                else
                {
                        if (!gzip_put_code(gzip_fixed_lit_code[src[at]],
                                           gzip_fixed_lit_len[src[at]]))
                                return false;
                        at++;
                }
        }
        return gzip_put_code(gzip_fixed_lit_code[256],
                             gzip_fixed_lit_len[256]);
}

static bool gzip_block_emit_dynamic(p8 address_to src, positive length,
                            p32 address_to mpos, p16 address_to mlen,
                            p16 address_to mdist, positive pairs, bool last)
{
        p32 lit_freq[GZIP_MAXLIT];
        p32 dist_freq[GZIP_MAXDIST];
        p8 lit_len[GZIP_MAXLIT];
        p8 dist_len[GZIP_MAXDIST];
        p16 lit_code[GZIP_MAXLIT];
        p16 dist_code[GZIP_MAXDIST];
        positive at = 0;
        positive pair = 0;
        positive bits = 0, extra_bits = 0, fixed_bits = 3;
        positive chunks = length ? (length + 65534) / 65535 : 1;
        positive stored_bits = length * 8 + chunks * 40 +
                               ((8 - ((gzip_bitn + 3) & 7)) & 7) - 5;

        memory_fill(lit_freq, 0, sizeof(lit_freq));
        memory_fill(dist_freq, 0, sizeof(dist_freq));
        lit_freq[256] = 1;

        at = 0;
        pair = 0;
        while (at < length)
        {
                if (pair < pairs && mpos[pair] == at)
                {
                        positive lcode = gzip_length_code(mlen[pair]);
                        positive dcode = gzip_distance_code(mdist[pair]);

                        lit_freq[257 + lcode]++;
                        dist_freq[dcode]++;
                        extra_bits += gzip_len_extra[lcode] + gzip_dist_extra[dcode];
                        at += mlen[pair];
                        pair++;
                }
                else
                {
                        lit_freq[src[at]]++;
                        at++;
                }
        }

        gzip_fixed_init();
        fixed_bits += extra_bits;
        for (positive i = 0; i < GZIP_MAXLIT; i++)
                fixed_bits += lit_freq[i] * gzip_fixed_lit_len[i];
        for (positive i = 0; i < GZIP_MAXDIST; i++)
                fixed_bits += dist_freq[i] * 5;

        compression_build_lengths(lit_freq, GZIP_MAXLIT, lit_len, GZIP_MAXBITS);
        compression_build_lengths(dist_freq, GZIP_MAXDIST, dist_len, GZIP_MAXBITS);
        if (!lit_len[256])
                lit_len[256] = 1;
        {
                bool any_dist = false;
                positive i;

                for (i = 0; i < GZIP_MAXDIST; i++)
                        if (dist_len[i])
                                any_dist = true;
                if (!any_dist)
                        dist_len[0] = 1;
        }
        if (!gzip_lengths_ok(lit_len, GZIP_MAXLIT, GZIP_MAXBITS) ||
            !gzip_lengths_ok(dist_len, GZIP_MAXDIST, GZIP_MAXBITS) ||
            !gzip_used_coded(lit_freq, lit_len, GZIP_MAXLIT) ||
            !gzip_used_coded(dist_freq, dist_len, GZIP_MAXDIST))
        {
                if (stored_bits <= fixed_bits)
                        return gzip_write_stored(src, length, last);
                return gzip_write_fixed(src, length, mpos, mlen, mdist, pairs,
                                        last);
        }
        gzip_lengths_to_codes(lit_len, GZIP_MAXLIT, lit_code);
        gzip_lengths_to_codes(dist_len, GZIP_MAXDIST, dist_code);

        bits = extra_bits;
        for (positive i = 0; i < GZIP_MAXLIT; i++) bits += lit_freq[i] * lit_len[i];
        for (positive i = 0; i < GZIP_MAXDIST; i++) bits += dist_freq[i] * dist_len[i];

        {
                p8 clen[19];
                p16 ccode[19];
                p32 cfreq[19];
                p8 seq[288 + 32];
                positive nseq = 0;
                positive i;
                positive run;
                positive hlit = 286, hdist = 30, hclen = 19;
                p8 here;

                while (hlit > 257 && !lit_len[hlit - 1]) hlit--;
                while (hdist > 1 && !dist_len[hdist - 1]) hdist--;
                for (i = 0; i < hlit; i++)
                        seq[nseq++] = lit_len[i];
                for (i = 0; i < hdist; i++)
                        seq[nseq++] = dist_len[i];

                memory_fill(cfreq, 0, sizeof(cfreq));
                i = 0;
                while (i < nseq)
                {
                        here = seq[i];
                        run = 1;
                        i++;
                        while (i < nseq && seq[i] == here && run < 138)
                        {
                                run++;
                                i++;
                        }
                        if (here)
                        {
                                cfreq[here]++;
                                run--;
                                if (run >= 3)
                                {
                                        while (run >= 3)
                                        {
                                                positive take = run > 6 ? 6 : run;

                                                cfreq[16]++;
                                                run -= take;
                                        }
                                }
                                cfreq[here] += run;
                        }
                        else if (run >= 11)
                                cfreq[18]++;
                        else if (run >= 3)
                                cfreq[17]++;
                        else
                                cfreq[0] += run;
                }
                compression_build_lengths(cfreq, 19, clen, 7);
                if (!gzip_lengths_ok(clen, 19, 7) ||
                    !gzip_used_coded(cfreq, clen, 19))
                {
                        if (stored_bits <= fixed_bits)
                                return gzip_write_stored(src, length, last);
                        return gzip_write_fixed(src, length, mpos, mlen, mdist,
                                                pairs, last);
                }
                gzip_lengths_to_codes(clen, 19, ccode);

                while (hclen > 4 && !clen[gzip_clen_order[hclen - 1]]) hclen--;
                bits += 17 + hclen * 3 + cfreq[16] * 2 + cfreq[17] * 3 + cfreq[18] * 7;
                for (positive c = 0; c < 19; c++) bits += cfreq[c] * clen[c];
                if (stored_bits <= bits && stored_bits <= fixed_bits)
                        return gzip_write_stored(src, length, last);
                if (fixed_bits <= bits)
                        return gzip_write_fixed(src, length, mpos, mlen, mdist,
                                                pairs, last);
                if (!gzip_put(last ? 1 : 0, 1) || !gzip_put(2, 2) ||
                    !gzip_put(hlit - 257, 5) || !gzip_put(hdist - 1, 5) ||
                    !gzip_put(hclen - 4, 4))
                        return false;
                for (i = 0; i < hclen; i++)
                        if (!gzip_put(clen[gzip_clen_order[i]], 3))
                                return false;

                i = 0;
                while (i < nseq)
                {
                        here = seq[i];
                        run = 1;
                        i++;
                        while (i < nseq && seq[i] == here && run < 138)
                        {
                                run++;
                                i++;
                        }
                        if (here)
                        {
                                if (!gzip_put_code(ccode[here], clen[here]))
                                        return false;
                                run--;
                                while (run >= 3)
                                {
                                        positive take = run > 6 ? 6 : run;

                                        if (!gzip_put_code(ccode[16], clen[16]) ||
                                            !gzip_put(take - 3, 2))
                                                return false;
                                        run -= take;
                                }
                                while (run)
                                {
                                        if (!gzip_put_code(ccode[here], clen[here]))
                                                return false;
                                        run--;
                                }
                        }
                        else if (run >= 11)
                        {
                                if (!gzip_put_code(ccode[18], clen[18]) ||
                                    !gzip_put(run - 11, 7))
                                        return false;
                        }
                        else if (run >= 3)
                        {
                                if (!gzip_put_code(ccode[17], clen[17]) ||
                                    !gzip_put(run - 3, 3))
                                        return false;
                        }
                        else
                                while (run)
                                {
                                        if (!gzip_put_code(ccode[0], clen[0]))
                                                return false;
                                        run--;
                                }
                }
        }

        at = 0;
        pair = 0;
        while (at < length)
        {
                if (pair < pairs && mpos[pair] == at)
                {
                        positive lcode = gzip_length_code(mlen[pair]);
                        positive dcode = gzip_distance_code(mdist[pair]);

                        if (!gzip_put_code(lit_code[257 + lcode],
                                           lit_len[257 + lcode]) ||
                            (gzip_len_extra[lcode] &&
                             !gzip_put(mlen[pair] - gzip_len_base[lcode],
                                       gzip_len_extra[lcode])) ||
                            !gzip_put_code(dist_code[dcode], dist_len[dcode]) ||
                            (gzip_dist_extra[dcode] &&
                             !gzip_put(mdist[pair] - gzip_dist_base[dcode],
                                       gzip_dist_extra[dcode])))
                                return false;
                        at += mlen[pair];
                        pair++;
                }
                else
                {
                        if (!gzip_put_code(lit_code[src[at]], lit_len[src[at]]))
                                return false;
                        at++;
                }
        }
        return gzip_put_code(lit_code[256], lit_len[256]);
}

static bool gzip_block_emit(p8 address_to src, positive length,
                            p32 address_to mpos, p16 address_to mlen,
                            p16 address_to mdist, positive pairs, bool last)
{
        gzip_crc = hash_crc32(gzip_crc, src, length);
        gzip_isize += length;
        return gzip_block_emit_dynamic(src, length, mpos, mlen, mdist, pairs,
                                       last);
}

#define GZIP_TOKEN_LIMIT 16384
#define GZIP_BLOCK_LIMIT (4 * 1024 * 1024)
static p32 gzip_mpos[GZIP_TOKEN_LIMIT];
static p16 gzip_mlen[GZIP_TOKEN_LIMIT];
static p16 gzip_mdist[GZIP_TOKEN_LIMIT];
static p8 gzip_blk[GZIP_BLOCK_LIMIT];
static positive gzip_tokens;
static positive gzip_used;
static positive gzip_pairs;
static bool gzip_hash_ready;
static positive gzip_cached_at, gzip_cached_length, gzip_cached_distance;

static bool gzip_deflate_pump(bool finish)
{
        positive nice = gzip_level >= 8 ? 258 : gzip_level >= 6 ? 128
                                                                : 16;
        bool lazy = gzip_level >= 4;

        if (!gzip_hash_ready)
        {
                memory_fill(gzip_head, 0, sizeof(gzip_head));
                memory_fill(gzip_prev, 0, sizeof(gzip_prev));
                gzip_used = 0;
                gzip_pairs = 0;
                gzip_tokens = 0;
                gzip_cached_length = 0;
                gzip_hash_ready = true;
        }

        for (;;)
        {
                positive pos;
                positive dist = 0;
                positive match;
                positive next_dist = 0;
                positive next_match;

                if (gzip_src_fill - gzip_src_at < GZIP_MAX_MATCH &&
                    !gzip_src_eof)
                {
                        if (!gzip_enc_pull())
                                return false;
                        if (gzip_src_fill - gzip_src_at < GZIP_MIN_MATCH &&
                            !gzip_src_eof && !finish)
                                break;
                }
                if (gzip_src_at >= gzip_src_fill)
                {
                        if (!gzip_src_eof && !finish)
                                break;
                        break;
                }

                pos = gzip_src_at;
                gzip_insert(pos);
                match = 0;
                if (gzip_cached_length && gzip_cached_at == gzip_src_abs + pos)
                {
                        match = gzip_cached_length;
                        dist = gzip_cached_distance;
                }
                else if (pos + GZIP_MIN_MATCH <= gzip_src_fill)
                        match = gzip_match_at(pos, gzip_prev[(gzip_src_abs + pos) & GZIP_WMASK],
                                              nice, 0, address_of dist);
                gzip_cached_length = 0;

                if (lazy && match >= GZIP_MIN_MATCH && match < nice &&
                    pos + 1 + GZIP_MIN_MATCH <= gzip_src_fill)
                {
                        /* Probe without inserting: the next iteration (or
                           the accepted match) owns that position. Inserting
                           twice makes its predecessor point to itself. */
                        next_match = gzip_match_at(
                            pos + 1, gzip_head[gzip_hash3(gzip_src_hold + pos + 1)],
                            nice, match, address_of next_dist);
                        if (next_match > match)
                        {
                                /* No insertion occurs between this probe and
                                   the next token. Retain it when its complete
                                   258-byte lookahead survives an input refill. */
                                if (gzip_src_fill - pos - 1 >= GZIP_MAX_MATCH)
                                {
                                        gzip_cached_at = gzip_src_abs + pos + 1;
                                        gzip_cached_length = next_match;
                                        gzip_cached_distance = next_dist;
                                }
                                match = 0;
                        }
                }

                if (gzip_used >= sizeof(gzip_blk) - GZIP_MAX_MATCH ||
                    gzip_tokens >= GZIP_TOKEN_LIMIT)
                {
                        if (!gzip_block_emit(gzip_blk, gzip_used, gzip_mpos,
                                             gzip_mlen, gzip_mdist, gzip_pairs,
                                             false))
                                return false;
                        gzip_used = 0;
                        gzip_pairs = 0;
                        gzip_tokens = 0;
                }

                gzip_tokens++;
                if (match >= GZIP_MIN_MATCH && dist && dist <= GZIP_WINDOW)
                {
                        positive step;

                        gzip_mpos[gzip_pairs] = (p32)gzip_used;
                        gzip_mlen[gzip_pairs] = (p16)match;
                        gzip_mdist[gzip_pairs] = (p16)dist;
                        gzip_pairs++;
                        if (gzip_used + match <= sizeof(gzip_blk))
                                memory_copy(gzip_blk + gzip_used,
                                            gzip_src_hold + pos, match);
                        gzip_used += match;
                        /* A long short-period match repeats the same hash
                           keys. Keep the latest two periods instead of writing
                           the same buckets hundreds of times. */
                        step = match >= 128 && dist <= 16 ? match - 2 * dist : 1;
                        for (; step < match; step++)
                                gzip_insert(pos + step);
                        gzip_src_at += match;
                }
                else
                {
                        p8 byte = gzip_src_hold[pos];

                        gzip_blk[gzip_used++] = byte;
                        gzip_src_at++;
                }
        }

        if (finish)
        {
                if (!gzip_block_emit(gzip_blk, gzip_used, gzip_mpos, gzip_mlen,
                                     gzip_mdist, gzip_pairs, true))
                        return false;
                gzip_used = 0;
                gzip_pairs = 0;
                gzip_tokens = 0;
                if (!gzip_put_flush())
                        return false;
        }
        return !gzip_out_failed;
}

static bool gzip_deflate_body(bool last_stream)
{
        return gzip_deflate_pump(last_stream);
}

static bool gzip_put32(p32 value)
{
        p8 at;

        for (at = 0; at < 4; at++)
        {
                if (!gzip_put(value & 255, 8))
                        return false;
                value >>= 8;
        }
        return true;
}

static bool gzip_encode_setup(p8 level)
{
        gzip_decoding = false;
        p8 xfl = 0;

        gzip_why = null;
        gzip_out_failed = false;
        gzip_level = level ? level : 6;
        if (gzip_level < 1)
                gzip_level = 1;
        if (gzip_level > 9)
                gzip_level = 9;
        if (gzip_level == 1)
                xfl = 4;
        else if (gzip_level == 9)
                xfl = 2;

        gzip_out_fill = 0;
        gzip_bits = 0;
        gzip_bitn = 0;
        gzip_crc = 0xffffffffu;
        gzip_isize = 0;
        gzip_src_fill = 0;
        gzip_src_at = 0;
        gzip_src_abs = 0;
        gzip_src_eof = false;
        gzip_hash_ready = false;
        gzip_feed = null;

        if (!gzip_out_reserve(10))
                return false;
        gzip_out_buf[gzip_out_fill++] = GZIP_MAGIC0;
        gzip_out_buf[gzip_out_fill++] = GZIP_MAGIC1;
        gzip_out_buf[gzip_out_fill++] = GZIP_METHOD;
        gzip_out_buf[gzip_out_fill++] = 0;
        gzip_out_buf[gzip_out_fill++] = 0;
        gzip_out_buf[gzip_out_fill++] = 0;
        gzip_out_buf[gzip_out_fill++] = 0;
        gzip_out_buf[gzip_out_fill++] = 0;
        gzip_out_buf[gzip_out_fill++] = xfl;
        gzip_out_buf[gzip_out_fill++] = 3;
        return true;
}

static bool gzip_encode_trailer(void)
{
        gzip_src_eof = true;
        if (!gzip_deflate_pump(true))
                return false;
        if (!gzip_put32(~gzip_crc) || !gzip_put32(gzip_isize))
                return false;
        return gzip_put_flush();
}

static bool gzip_stream_encode(p8 level)
{
        if (!gzip_encode_setup(level))
                return false;
        for (;;)
        {
                if (!gzip_enc_pull())
                        return false;
                if (gzip_src_eof && gzip_src_at >= gzip_src_fill)
                        break;
                if (gzip_src_at >= gzip_src_fill)
                        continue;
                if (!gzip_deflate_pump(false))
                        return false;
        }
        return gzip_encode_trailer();
}

static bipolar gzip_deflate_mem(p8 address_to src, positive src_len,
                                p8 address_to dst, positive dst_cap, p8 level)
{
        bool ok;

        gzip_in_fd = -1;
        gzip_enc_mem = src;
        gzip_enc_mem_len = src_len;
        gzip_enc_mem_at = 0;
        gzip_out_fd = -1;
        gzip_out_mem = dst;
        gzip_out_cap = dst_cap;
        gzip_out_used = 0;
        ok = gzip_stream_encode(level);
        gzip_enc_mem = null;
        gzip_out_mem = null;
        return ok ? (bipolar)gzip_out_used : -1;
}

static fn gzip_in_from_fd(bipolar in)
{
        gzip_in_fd = in;
        gzip_in_mem = null;
        gzip_in_mem_len = 0;
        gzip_in_mem_at = 0;
        gzip_in_at = 0;
        gzip_in_have = 0;
        gzip_in_eof = false;
}

static bool gzip_decode_begin(bipolar in)
{
        gzip_out_taken = 0;
        gzip_why = null;
        gzip_out_failed = false;
        gzip_in_from_fd(in);
        gzip_out_fd = -1;
        gzip_out_mem = null;
        gzip_out_fill = 0;
        gzip_bits = 0;
        gzip_bitn = 0;
        gzip_pull = true;
        gzip_paused = false;
        gzip_have_block = false;
        gzip_stored_open = false;
        gzip_head_done = false;
        gzip_finished = false;
        return true;
}

static bool gzip_decode_begin_prefix(bipolar in, p8 address_to prefix,
                                     positive n)
{
        gzip_decode_begin(in);
        if (n > GZIP_IN)
                return gzip_fail("gzip prefix");
        memory_copy(gzip_in_buf, prefix, n);
        gzip_in_have = n;
        gzip_in_at = 0;
        return true;
}

static bipolar gzip_decode_read(p8 address_to dst, positive n)
{
        positive copied = 0;

        while (copied < n)
        {
                positive take;

                if (gzip_out_fill)
                {
                        positive available = gzip_out_fill - gzip_out_taken;
                        take = available > n - copied ? n - copied : available;
                        memory_copy(dst + copied, gzip_out_buf + gzip_out_taken, take);
                        gzip_out_taken += take;
                        if (gzip_out_taken == gzip_out_fill)
                        {
                                memory_copy(gzip_out_storage,
                                            gzip_out_storage + gzip_out_fill,
                                            GZIP_WINDOW);
                                gzip_out_fill = 0;
                                gzip_out_taken = 0;
                        }
                        copied += take;
                        continue;
                }

                if (gzip_finished)
                        break;

                if (!gzip_in_need(1) && !gzip_head_done)
                {
                        gzip_finished = true;
                        break;
                }
                if (gzip_in_at >= gzip_in_have && gzip_in_eof && !gzip_head_done)
                {
                        gzip_finished = true;
                        break;
                }

                gzip_paused = false;
                if (!gzip_member())
                        return -1;
                if (!gzip_out_fill && !gzip_paused && !gzip_head_done)
                        continue;
                if (!gzip_out_fill && gzip_paused)
                        return copied ? (bipolar)copied : -1;
        }

        return (bipolar)copied;
}

static bool gzip_decode_end(void)
{
        gzip_pull = false;
        gzip_finished = true;
        return gzip_why == null;
}

static bool gzip_encode_begin(bipolar out, p8 level)
{
        gzip_out_fd = out;
        gzip_out_mem = null;
        gzip_in_fd = -1;
        gzip_enc_mem = null;
        gzip_feed = null;
        return gzip_encode_setup(level);
}

static bool gzip_encode_write(p8 address_to src, positive n)
{
        gzip_feed = src;
        gzip_feed_len = n;
        gzip_feed_at = 0;
        gzip_src_eof = false;
        while (gzip_feed)
        {
                if (!gzip_enc_pull())
                        return false;
                if (!gzip_deflate_pump(false))
                        return false;
                if (gzip_feed && gzip_feed_at >= gzip_feed_len)
                        gzip_feed = null;
        }
        return gzip_out_flush();
}

static bool gzip_encode_end(void)
{
        gzip_feed = null;
        return gzip_encode_trailer();
}

#ifndef GZIP_CORE_ONLY

static b32 gzip_stream(bipolar in, bipolar out, bool decode, p8 level)
{
        bool ok;

        gzip_in_fd = in;
        gzip_in_mem = null;
        gzip_out_fd = out;
        gzip_out_mem = null;
        gzip_enc_mem = null;
        gzip_feed = null;
        gzip_status = 0;
        if (decode)
                ok = gzip_stream_decode();
        else
                ok = gzip_stream_encode(level);
        if (!ok)
        {
                if (gzip_why)
                        string_format(log_error, "gzip: %s\n", gzip_why);
                gzip_status = 1;
                return 1;
        }
        return 0;
}

static fn gzip_refuse(string_address message)
{
        string_format(log_error, "gzip: %s\n", message);
        gzip_status = 1;
}

static string_address gzip_called(void)
{
        string_address path = program_argument(0);
        string_address slash;

        if (!path)
                return "gzip";
        slash = string_last_of(path, '/');
        return slash && slash[1] ? slash + 1 : path;
}

static bool gzip_suffix_out(string_address in, p8 address_to into, positive room,
                            bool decode)
{
        positive n = string_length(in);

        if (decode)
        {
                if (n >= 3 && !memory_compare(in + n - 3, ".gz", 3))
                {
                        if (n - 2 >= room)
                                return false;
                        memory_copy(into, in, n - 3);
                        into[n - 3] = end;
                        return true;
                }
                if (n >= 2 && !memory_compare(in + n - 2, ".Z", 2))
                {
                        if (n - 1 >= room)
                                return false;
                        memory_copy(into, in, n - 2);
                        into[n - 2] = end;
                        return true;
                }
                if (n >= 4 && !memory_compare(in + n - 4, ".tgz", 4))
                {
                        if (n + 1 > room)
                                return false;
                        memory_copy(into, in, n - 4);
                        memory_copy(into + n - 4, ".tar", 5);
                        return true;
                }
                return false;
        }
        if (n + 4 >= room)
                return false;
        memory_copy(into, in, n);
        memory_copy(into + n, ".gz", 4);
        return true;
}

static b32 file_gzip(void)
{
        string_address name = gzip_called();
        positive count = (positive)program_argument_count();
        positive at;
        bool decompress = string_equals(name, "gunzip") ||
                          string_equals(name, "zcat");
        bool stdout_out = string_equals(name, "zcat");
        bool force = false;
        bool test = false;
        bool remove_src = true;
        bool quiet = false;
        p8 level = 6;
        string_address out_path = null;
        p8 out_name[4096];

        gzip_status = 0;
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
                                remove_src = false;
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
                        {
                                remove_src = false;
                                continue;
                        }
                        if (string_equals(word, "--quiet"))
                        {
                                quiet = true;
                                continue;
                        }
                        if (string_equals(word, "--fast"))
                        {
                                level = 1;
                                continue;
                        }
                        if (string_equals(word, "--best"))
                        {
                                level = 9;
                                continue;
                        }
                        if (string_equals(word, "--help"))
                        {
                                string_format(log,
                                              "Usage: gzip [-cdfkqt123456789] [FILE...]\n");
                                log_flush();
                                return 0;
                        }
                        if (string_equals(word, "--version"))
                        {
                                string_format(log, "gzip from dawning-kit\n");
                                log_flush();
                                return 0;
                        }
                        string_format(log_error, "gzip: unrecognized option '%s'\n",
                                      word);
                        return 2;
                }
                {
                        string_address letters = word + 1;

                        for (; *letters; letters++)
                        {
                                if (*letters >= '1' && *letters <= '9')
                                        level = (p8)(*letters - '0');
                                else if (*letters == 'd')
                                        decompress = true;
                                else if (*letters == 'c')
                                {
                                        stdout_out = true;
                                        remove_src = false;
                                }
                                else if (*letters == 'f')
                                        force = true;
                                else if (*letters == 'k')
                                        remove_src = false;
                                else if (*letters == 'n')
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
                                        string_format(log, "gzip from dawning-kit\n");
                                        log_flush();
                                        return 0;
                                }
                                else if (*letters == 'h')
                                {
                                        string_format(log,
                                                      "Usage: gzip [-cdfkqt123456789] [FILE...]\n");
                                        log_flush();
                                        return 0;
                                }
                                else
                                {
                                        p8 shown[2];

                                        shown[0] = *letters;
                                        shown[1] = end;
                                        string_format(log_error,
                                                      "gzip: invalid option -- '%s'\n",
                                                      shown);
                                        return 2;
                                }
                        }
                }
        }

        if (at >= count)
        {
                bipolar out = test ? -1 : 1;

                return gzip_stream(0, out, decompress, level);
        }

        for (; at < count && !gzip_status; at++)
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
                                string_format(log_error, "gzip: %s: %s\n", path,
                                              file_reason(in));
                                gzip_status = 1;
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
                                        string_format(log_error, "gzip: %s: %s\n",
                                                      out_path, file_reason(out));
                                        system_close(in);
                                        gzip_status = 1;
                                        break;
                                }
                                close_out = true;
                        }
                        else if (!gzip_suffix_out(path, out_name, sizeof(out_name),
                                                  decompress))
                        {
                                gzip_refuse(decompress
                                                ? "unknown suffix; use -c"
                                                : "cannot guess output name");
                                system_close(in);
                                break;
                        }
                        else
                        {
                                out = system_open_output_at(AT_FDCWD, out_name,
                                                            force, 0666);
                                if (out < 0)
                                {
                                        string_format(log_error, "gzip: %s: %s\n",
                                                      out_name, file_reason(out));
                                        system_close(in);
                                        gzip_status = 1;
                                        break;
                                }
                                close_out = true;
                        }
                }

                gzip_stream(in, out, decompress, level);
                if (close_in)
                        system_close(in);
                if (close_out)
                        system_close(out);
                if (!gzip_status && remove_src && close_in && !stdout_out &&
                    !test)
                        system_remove_at(AT_FDCWD, path, 0);
                if (out_path)
                        out_path = null;
        }

        log_flush();
        return gzip_status;
}

#endif /* GZIP_CORE_ONLY */
