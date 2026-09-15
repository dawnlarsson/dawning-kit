/*
        gzip -- RFC 1952 members around RFC 1951 deflate.

        Decode uses primary Huffman tables and a shared assembly token loop,
        with a canonical decoder for long codes and refill tails. Encode
        deflates fixed 1 MiB blocks, each with the previous 32 KiB as
        history, through a 32 KiB hash chain with lazy matching and dynamic
        Huffman blocks that fall back to fixed or stored when the tree would
        not pay (see the encoder below). Match lengths are memory_common_prefix;
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
static byte_input gzip_input = {.buf = gzip_in_buf, .room = GZIP_IN};

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
static byte_store gzip_output;
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


static bool gzip_fail(string_address why)
{
        gzip_why = why;
        return false;
}

static bool gzip_in_need(void)
{
        bipolar got = byte_input_need(address_of gzip_input, 1);
        return got < 0 ? gzip_fail("gzip read failed") : got != 0;
}

static bipolar gzip_in_byte(void)
{
        if (gzip_input.at >= gzip_input.have && !gzip_in_need())
                return -1;
        return gzip_in_buf[gzip_input.at++];
}

/* A little-endian word, read a byte at a time across refills. */
static bool gzip_in_word(p32 address_to word)
{
        address_to word = 0;
        for (positive at = 0; at < 32; at += 8)
        {
                bipolar byte = gzip_in_byte();

                if (byte < 0)
                        return false;
                address_to word |= (p32)byte << at;
        }
        return true;
}

static bool gzip_align(void)
{
        positive rewind = gzip_bitn >> 3;

        if (rewind > gzip_input.at)
                return gzip_fail("gzip bit rewind");
        gzip_input.at -= rewind;
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
        if (gzip_output.bytes)
        {
                if (!byte_store_append_exact(address_of gzip_output,
                                              gzip_out_buf, gzip_out_fill))
                {
                        gzip_out_failed = true;
                        return gzip_fail("gzip output is too small");
                }
        }
        else if (gzip_out_fd >= 0 &&
                 system_write_all((positive)gzip_out_fd, gzip_out_buf,
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

static bool gzip_emit(p8 byte)
{
        gzip_window[gzip_wpos & GZIP_WMASK] = byte;
        gzip_wpos++;
        gzip_crc = hash_crc32(gzip_crc, address_of byte, 1);
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
                        positive k;
                        positive reversed = gzip_revbits((p16)code++, (p8)len);

                        for (k = reversed; k < ((positive)1 << bits);
                             k += (positive)1 << len)
                                table[k] = (p16)((len << 9) | symbol[index]);
                        index++;
                }
                code <<= 1;
        }
}

/* Count code lengths into count. The code space still unused, -1 for a
   length past limit, -2 for lengths that over-subscribe the space. */
static bipolar gzip_code_space(p8 address_to length, positive n, p8 limit,
                               p16 address_to count)
{
        bipolar left = 1;

        memory_fill(count, 0, (GZIP_MAXBITS + 1) * sizeof(p16));
        for (positive at = 0; at < n; at++)
                if (length[at] > limit)
                        return -1;
                else
                        count[length[at]]++;
        count[0] = 0;
        for (positive len = 1; len <= limit; len++)
        {
                left <<= 1;
                if (left < count[len])
                        return -2;
                left -= count[len];
        }
        return left;
}

static bool gzip_huffman(p8 address_to length, positive n,
                         p16 address_to count, p16 address_to symbol)
{
        positive len;
        positive at;
        p16 offs[GZIP_MAXBITS + 1];
        bipolar left = gzip_code_space(length, n, GZIP_MAXBITS, count);

        if (left < 0)
                return gzip_fail(left == -1 ? "gzip Huffman length"
                                            : "gzip Huffman over-subscribed");

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
                while (gzip_bitn < root && gzip_input.at < gzip_input.have)
                {
                        gzip_bits |= (p64)gzip_in_buf[gzip_input.at++] << gzip_bitn;
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

static p8 gzip_fixed_lit_len[GZIP_MAXLIT];
static p16 gzip_fixed_lit_code[GZIP_MAXLIT];
static p8 gzip_fixed_dist_len[GZIP_MAXDIST];
static p16 gzip_fixed_dist_code[GZIP_MAXDIST];
static bool gzip_fixed_codes;
static fn gzip_fixed_init(void);

static bool gzip_fixed(void)
{
        gzip_fixed_init();
        return gzip_huffman(gzip_fixed_lit_len, GZIP_MAXLIT, gzip_lit_count,
                            gzip_lit_symbol) &&
               gzip_huffman(gzip_fixed_dist_len, GZIP_MAXDIST, gzip_dist_count,
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
                gzip_in_buf + gzip_input.at, gzip_in_buf + gzip_input.have,
                gzip_out_buf + gzip_out_fill, gzip_out_buf + GZIP_OUT,
                gzip_wpos, gzip_lit_quick, gzip_dist_quick,
                gzip_length_info, gzip_distance_info};
        p8 address_to start = job.out;
        deflate_decode_span(address_of job);
        gzip_bits = job.bits;
        gzip_bitn = (p8)job.count;
        gzip_input.at = (positive)(job.next - gzip_in_buf);
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

                if (gzip_input.have - gzip_input.at >= 8 &&
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
                if (gzip_input.at == gzip_input.have && !gzip_in_need())
                        return gzip_fail("gzip truncated stored block");
                positive take = gzip_input.have - gzip_input.at;
                if (take > gzip_stored_left)
                        take = gzip_stored_left;
                if (take > GZIP_OUT - gzip_out_fill)
                        take = GZIP_OUT - gzip_out_fill;
                if (!take)
                        return gzip_fail("gzip truncated stored block");
                p8 address_to bytes = gzip_in_buf + gzip_input.at;
                memory_copy_apart(gzip_out_buf + gzip_out_fill, bytes, take);
                gzip_record(bytes, take);
                gzip_input.at += take;
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

        if (!gzip_in_word(address_of got_crc) || !gzip_in_word(address_of got_size))
                return gzip_fail("gzip truncated trailer");

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
        gzip_input.at = 0;
        gzip_input.have = 0;
        gzip_input.eof = false;
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
                if (!gzip_in_need())
                        break;
                if (gzip_input.at >= gzip_input.have)
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

        byte_input_open_memory(address_of gzip_input, src, src_len, gzip_in_buf,
                               GZIP_IN);
        gzip_out_fd = -1;
        gzip_output.bytes = dst;
        gzip_output.room = dst_cap;
        gzip_output.used = 0;
        ok = gzip_stream_decode();
        gzip_input.mem = null;
        gzip_output.bytes = null;
        return ok ? (bipolar)gzip_output.used : -1;
}

/*
        Encoder.

        Input is deflated in fixed blocks of GZIP_BLOCK bytes. A gzip_encoder
        sees the 32 KiB before its block as history, hashed but never
        emitted, so matches still reach back across the cut, and it ends the
        block on a byte boundary with an empty stored block. Blocks can so be
        deflated side by side and concatenated as they are; the stream closes
        with an empty final fixed block. Block size, history and every
        boundary come from the input alone, never from how many blocks are
        deflated at once or how the input arrived.
*/

#define GZIP_BLOCK (1u << 20)
#define GZIP_TOKEN_LIMIT 16384

typedef struct
{
        p8 level;
        /* history bytes, then the block: base[0, total) */
        p8 address_to base;
        positive total;
        p32 head[GZIP_HASH_SIZE];
        p32 prev[GZIP_WINDOW];
        p32 mpos[GZIP_TOKEN_LIMIT];
        p16 mlen[GZIP_TOKEN_LIMIT];
        p16 mdist[GZIP_TOKEN_LIMIT];
        p8 address_to out;
        positive out_room;
        positive out_n;
        p64 bits;
        p32 bitn;
} gzip_encoder;

static inline INLINE fn gzip_put_bits(gzip_encoder address_to e, p32 value, p32 n)
{
        e->bits |= (p64)value << e->bitn;
        e->bitn += n;
        if (e->bitn >= 32)
        {
                memory_store_unaligned(p32, e->out + e->out_n, (p32)e->bits);
                e->out_n += 4;
                e->bits >>= 32;
                e->bitn -= 32;
        }
}

static fn gzip_bits_align(gzip_encoder address_to e)
{
        while (e->bitn)
        {
                e->out[e->out_n++] = (p8)e->bits;
                e->bits >>= 8;
                e->bitn = e->bitn > 8 ? e->bitn - 8 : 0;
        }
        e->bits = 0;
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

        return gzip_code_space(length, n, limit, count) == 0;
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
        positive hb = 63 - bits_leading_zeros(v);
        return (hb << 1) + ((v >> (hb - 1)) & 1);
}

/* Stored blocks of at most 65535 bytes; length zero is the byte-aligning
   empty block that ends every deflated input block. */
static fn gzip_write_stored(gzip_encoder address_to e, p8 address_to src, positive length,
                            bool last)
{
        do
        {
                positive chunk = length > 65535 ? 65535 : length;

                gzip_put_bits(e, last && chunk == length, 1);
                gzip_put_bits(e, 0, 2);
                gzip_bits_align(e);
                e->out[e->out_n++] = (p8)chunk;
                e->out[e->out_n++] = (p8)(chunk >> 8);
                e->out[e->out_n++] = (p8)~chunk;
                e->out[e->out_n++] = (p8)(~chunk >> 8);
                if (chunk)
                        memory_copy_apart(e->out + e->out_n, src, chunk);
                e->out_n += chunk;
                src += chunk;
                length -= chunk;
        } while (length);
}

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

/* The tokens of one deflate block: literals from src, and pairs whose
   positions are offsets into src, in order. */
static fn gzip_write_tokens(gzip_encoder address_to e, p8 address_to src, positive length,
                            positive pairs, p16 address_to lit_code, p8 address_to lit_len,
                            p16 address_to dist_code, p8 address_to dist_len)
{
        positive at = 0;
        positive pair = 0;

        while (at < length)
        {
                if (pair < pairs && e->mpos[pair] == at)
                {
                        positive len = e->mlen[pair];
                        positive dist = e->mdist[pair];
                        positive lcode = gzip_length_code(len);
                        positive dcode = gzip_distance_code(dist);

                        gzip_put_bits(e, lit_code[257 + lcode], lit_len[257 + lcode]);
                        if (gzip_len_extra[lcode])
                                gzip_put_bits(e, (p32)(len - gzip_len_base[lcode]),
                                          gzip_len_extra[lcode]);
                        gzip_put_bits(e, dist_code[dcode], dist_len[dcode]);
                        if (gzip_dist_extra[dcode])
                                gzip_put_bits(e, (p32)(dist - gzip_dist_base[dcode]),
                                          gzip_dist_extra[dcode]);
                        at += len;
                        pair++;
                }
                else
                {
                        gzip_put_bits(e, lit_code[src[at]], lit_len[src[at]]);
                        at++;
                }
        }
        gzip_put_bits(e, lit_code[256], lit_len[256]);
}

static fn gzip_write_fixed(gzip_encoder address_to e, p8 address_to src, positive length,
                           positive pairs, bool last)
{
        gzip_put_bits(e, last ? 1 : 0, 1);
        gzip_put_bits(e, 1, 2);
        gzip_write_tokens(e, src, length, pairs, gzip_fixed_lit_code, gzip_fixed_lit_len,
                          gzip_fixed_dist_code, gzip_fixed_dist_len);
}

/* A dynamic block, or fixed or stored when the tree would not pay. */
static fn gzip_block_emit(gzip_encoder address_to e, p8 address_to src, positive length,
                          positive pairs, bool last)
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
                               ((8 - ((e->bitn + 3) & 7)) & 7) - 5;

        memory_fill(lit_freq, 0, sizeof(lit_freq));
        memory_fill(dist_freq, 0, sizeof(dist_freq));
        lit_freq[256] = 1;
        while (at < length)
        {
                if (pair < pairs && e->mpos[pair] == at)
                {
                        positive lcode = gzip_length_code(e->mlen[pair]);
                        positive dcode = gzip_distance_code(e->mdist[pair]);

                        lit_freq[257 + lcode]++;
                        dist_freq[dcode]++;
                        extra_bits += gzip_len_extra[lcode] + gzip_dist_extra[dcode];
                        at += e->mlen[pair];
                        pair++;
                }
                else
                {
                        lit_freq[src[at]]++;
                        at++;
                }
        }

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

                for (positive i = 0; i < GZIP_MAXDIST; i++)
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
                        gzip_write_stored(e, src, length, last);
                else
                        gzip_write_fixed(e, src, length, pairs, last);
                return;
        }
        gzip_lengths_to_codes(lit_len, GZIP_MAXLIT, lit_code);
        gzip_lengths_to_codes(dist_len, GZIP_MAXDIST, dist_code);

        bits = extra_bits;
        for (positive i = 0; i < GZIP_MAXLIT; i++)
                bits += lit_freq[i] * lit_len[i];
        for (positive i = 0; i < GZIP_MAXDIST; i++)
                bits += dist_freq[i] * dist_len[i];

        p8 clen[19];
        p16 ccode[19];
        p32 cfreq[19];
        p8 seq[288 + 32];
        positive nseq = 0;
        positive i;
        positive run;
        positive hlit = 286, hdist = 30, hclen = 19;
        p8 here;

        while (hlit > 257 && !lit_len[hlit - 1])
                hlit--;
        while (hdist > 1 && !dist_len[hdist - 1])
                hdist--;
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
                        while (run >= 3)
                        {
                                positive take = run > 6 ? 6 : run;

                                cfreq[16]++;
                                run -= take;
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
        if (!gzip_lengths_ok(clen, 19, 7) || !gzip_used_coded(cfreq, clen, 19))
        {
                if (stored_bits <= fixed_bits)
                        gzip_write_stored(e, src, length, last);
                else
                        gzip_write_fixed(e, src, length, pairs, last);
                return;
        }
        gzip_lengths_to_codes(clen, 19, ccode);

        while (hclen > 4 && !clen[gzip_clen_order[hclen - 1]])
                hclen--;
        bits += 17 + hclen * 3 + cfreq[16] * 2 + cfreq[17] * 3 + cfreq[18] * 7;
        for (positive c = 0; c < 19; c++)
                bits += cfreq[c] * clen[c];
        if (stored_bits <= bits && stored_bits <= fixed_bits)
        {
                gzip_write_stored(e, src, length, last);
                return;
        }
        if (fixed_bits <= bits)
        {
                gzip_write_fixed(e, src, length, pairs, last);
                return;
        }
        gzip_put_bits(e, last ? 1 : 0, 1);
        gzip_put_bits(e, 2, 2);
        gzip_put_bits(e, (p32)(hlit - 257), 5);
        gzip_put_bits(e, (p32)(hdist - 1), 5);
        gzip_put_bits(e, (p32)(hclen - 4), 4);
        for (i = 0; i < hclen; i++)
                gzip_put_bits(e, clen[gzip_clen_order[i]], 3);

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
                        gzip_put_bits(e, ccode[here], clen[here]);
                        run--;
                        while (run >= 3)
                        {
                                positive take = run > 6 ? 6 : run;

                                gzip_put_bits(e, ccode[16], clen[16]);
                                gzip_put_bits(e, (p32)(take - 3), 2);
                                run -= take;
                        }
                        while (run)
                        {
                                gzip_put_bits(e, ccode[here], clen[here]);
                                run--;
                        }
                }
                else if (run >= 11)
                {
                        gzip_put_bits(e, ccode[18], clen[18]);
                        gzip_put_bits(e, (p32)(run - 11), 7);
                }
                else if (run >= 3)
                {
                        gzip_put_bits(e, ccode[17], clen[17]);
                        gzip_put_bits(e, (p32)(run - 3), 3);
                }
                else
                        while (run)
                        {
                                gzip_put_bits(e, ccode[0], clen[0]);
                                run--;
                        }
        }
        gzip_write_tokens(e, src, length, pairs, lit_code, lit_len, dist_code, dist_len);
}

/* Positions are offsets into base, stored plus one so zero is empty. */
static positive gzip_match_at(gzip_encoder address_to e, positive pos, positive chain,
                              positive nice, positive best, positive address_to dist)
{
        positive left = e->total - pos;
        positive current = pos + 1;
        positive earliest = current > GZIP_WINDOW ? current - GZIP_WINDOW : 1;
        positive steps = e->level >= 8 ? 4096 : e->level >= 6 ? 128
                                               : e->level >= 4 ? 32 : 8;
        p8 address_to here = e->base + pos;

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
                positive next = e->prev[old & GZIP_WMASK];
                p8 address_to there = e->base + old - 1;
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

static inline INLINE fn gzip_insert(gzip_encoder address_to e, positive pos)
{
        if (pos + 2 >= e->total)
                return;
        p16 h = compression_hash3(e->base + pos);
        e->prev[(pos + 1) & GZIP_WMASK] = e->head[h];
        e->head[h] = (p32)(pos + 1);
}

/* Deflate data[0, n) after history bytes at data - history, ending on a
   byte boundary. The output span must hold n + n / 8 + 4096 bytes. */
static fn gzip_block_deflate(gzip_encoder address_to e, p8 address_to data,
                             positive history, positive n)
{
        positive nice = e->level >= 8 ? 258 : e->level >= 6 ? 128 : 16;
        bool lazy = e->level >= 4;
        positive cached_at = 0, cached_length = 0, cached_distance = 0;
        positive start = history, pairs = 0, tokens = 0, pos = history;

        e->base = data - history;
        e->total = history + n;
        e->out_n = 0;
        e->bits = 0;
        e->bitn = 0;
        memory_fill(e->head, 0, sizeof(e->head));
        memory_fill(e->prev, 0, sizeof(e->prev));
        for (positive at = 0; at < history; at++)
                gzip_insert(e, at);

        while (pos < e->total)
        {
                positive dist = 0;
                positive match = 0;

                gzip_insert(e, pos);
                if (cached_length && cached_at == pos)
                {
                        match = cached_length;
                        dist = cached_distance;
                }
                else if (pos + GZIP_MIN_MATCH <= e->total)
                        match = gzip_match_at(e, pos, e->prev[(pos + 1) & GZIP_WMASK], nice,
                                              0, address_of dist);
                cached_length = 0;

                if (lazy && match >= GZIP_MIN_MATCH && match < nice &&
                    pos + 1 + GZIP_MIN_MATCH <= e->total)
                {
                        /* Probe without inserting: the next iteration (or
                           the accepted match) owns that position. Inserting
                           twice makes its predecessor point to itself. The
                           block's end is fixed, so the probe stays valid. */
                        positive next_dist = 0;
                        positive next_match = gzip_match_at(
                            e, pos + 1, e->head[compression_hash3(e->base + pos + 1)], nice,
                            match, address_of next_dist);

                        if (next_match > match)
                        {
                                cached_at = pos + 1;
                                cached_length = next_match;
                                cached_distance = next_dist;
                                match = 0;
                        }
                }

                if (tokens >= GZIP_TOKEN_LIMIT)
                {
                        gzip_block_emit(e, e->base + start, pos - start, pairs, false);
                        start = pos;
                        pairs = 0;
                        tokens = 0;
                }
                tokens++;
                if (match >= GZIP_MIN_MATCH && dist && dist <= GZIP_WINDOW)
                {
                        positive step;

                        e->mpos[pairs] = (p32)(pos - start);
                        e->mlen[pairs] = (p16)match;
                        e->mdist[pairs] = (p16)dist;
                        pairs++;
                        /* A long short-period match repeats the same hash
                           keys. Keep the latest two periods instead of writing
                           the same buckets hundreds of times. */
                        step = match >= 128 && dist <= 16 ? match - 2 * dist : 1;
                        for (; step < match; step++)
                                gzip_insert(e, pos + step);
                        pos += match;
                }
                else
                        pos++;
        }
        if (pos > start)
                gzip_block_emit(e, e->base + start, pos - start, pairs, false);
        gzip_write_stored(e, e->base + pos, 0, false);
}

/*
        The member around the blocks: header, blocks, the empty final block,
        CRC-32 and size. Input waits in batches of whole blocks after a
        32 KiB history tail. Each block is one job deflating into its
        worker's gzip_encoder; the sink checksums the block's input and
        writes its bytes in block order on the calling thread. The batch
        only decides how much input waits in memory, never where a block
        starts or what history it sees.
*/
#define GZIP_BATCH_BLOCKS 64

typedef struct
{
        p8 level;
        /* [0, GZIP_WINDOW) history tail, then the batch */
        p8 address_to input;
        positive input_room;
        positive history;
        positive n;
        gzip_encoder address_to address_to slots;
        positive slot_count;
        p8 address_to done;
        p32 crc;
        p32 isize;
        byte_store address_to store;
        bipolar fd;
        bool failed;
} gzip_stream_writer;

static gzip_stream_writer gzip_writer;

static bool gzip_writer_emit(p8 address_to bytes, positive n)
{
        if (gzip_writer.failed)
                return false;
        if (gzip_writer.store)
        {
                if (!byte_store_append_exact(gzip_writer.store, bytes, n))
                {
                        gzip_writer.failed = true;
                        return gzip_fail("gzip output is too small");
                }
        }
        else if (gzip_writer.fd >= 0 &&
                 system_write_all((positive)gzip_writer.fd, bytes, n) != (bipolar)n)
        {
                gzip_writer.failed = true;
                return gzip_fail("gzip write failed");
        }
        return true;
}

static gzip_encoder address_to gzip_encoder_open(p8 level)
{
        gzip_encoder address_to e = (gzip_encoder address_to)memory(sizeof(gzip_encoder));

        if (!e || system_failed(e))
                return null;
        e->out_room = GZIP_BLOCK + GZIP_BLOCK / 8 + 4096;
        e->out = (p8 address_to)memory(e->out_room);
        if (!e->out || system_failed(e->out))
        {
                memory_free(e, sizeof(gzip_encoder));
                return null;
        }
        e->level = level;
        return e;
}

static fn gzip_writer_close(void)
{
        for (positive i = 0; i < gzip_writer.slot_count; i++)
        {
                gzip_encoder address_to e = gzip_writer.slots[i];

                if (e)
                {
                        memory_free(e->out, e->out_room);
                        memory_free(e, sizeof(gzip_encoder));
                }
        }
        memory_free(gzip_writer.slots, gzip_writer.slot_count * sizeof(gzip_encoder address_to));
        memory_free(gzip_writer.done, GZIP_BATCH_BLOCKS);
        memory_free(gzip_writer.input, gzip_writer.input_room);
        gzip_writer.slots = null;
        gzip_writer.slot_count = 0;
        gzip_writer.done = null;
        gzip_writer.input = null;
        gzip_writer.input_room = 0;
}

static positive gzip_batch_bytes(gzip_stream_writer address_to w, positive index)
{
        positive from = index * GZIP_BLOCK;

        return w->n - from < GZIP_BLOCK ? w->n - from : GZIP_BLOCK;
}

/* One block of the batch, on any thread: it touches only its worker's
   encoder, its own done byte and its own output. */
static fn gzip_batch_job(address_any context, positive index,
                         parallel_output address_to output)
{
        gzip_stream_writer address_to w = (gzip_stream_writer address_to)context;
        positive slot = parallel_slot();
        gzip_encoder address_to e = w->slots[slot];

        w->done[index] = false;
        if (!e)
        {
                e = gzip_encoder_open(w->level);
                if (!e)
                        return;
                w->slots[slot] = e;
        }
        gzip_block_deflate(e, w->input + GZIP_WINDOW + index * GZIP_BLOCK,
                           index ? GZIP_WINDOW : w->history, gzip_batch_bytes(w, index));
        w->done[index] = parallel_write(output, e->out, e->out_n);
}

/* A block's checksum and bytes, in block order, on the calling thread. */
static bool gzip_batch_sink(address_any context, positive index, address_any data,
                            positive length)
{
        gzip_stream_writer address_to w = (gzip_stream_writer address_to)context;
        positive n = gzip_batch_bytes(w, index);

        if (!w->done[index])
                return gzip_fail("gzip cannot map the encoder");
        w->crc = hash_crc32(w->crc, w->input + GZIP_WINDOW + index * GZIP_BLOCK, n);
        w->isize += (p32)n;
        return gzip_writer_emit((p8 address_to)data, length);
}

/* Every block of the batch, deflated side by side and written in block
   order, then the last 32 KiB kept as the next batch's history. */
static bool gzip_writer_batch(void)
{
        gzip_stream_writer address_to w = address_of gzip_writer;
        positive count = (w->n + GZIP_BLOCK - 1) / GZIP_BLOCK;

        if (!w->n)
                return !w->failed;
        if (!parallel_ordered(gzip_batch_job, gzip_batch_sink, w, count, w->n))
                return w->failed || gzip_why ? false
                                             : gzip_fail("gzip cannot map the encoder");

        positive keep = w->history + w->n;

        if (keep > GZIP_WINDOW)
                keep = GZIP_WINDOW;
        memory_copy(w->input + GZIP_WINDOW - keep, w->input + GZIP_WINDOW + w->n - keep, keep);
        w->history = keep;
        w->n = 0;
        return true;
}

static bool gzip_encode_setup(p8 level)
{
        p8 header[10] = {GZIP_MAGIC0, GZIP_MAGIC1, GZIP_METHOD, 0, 0, 0, 0, 0, 0, 3};

        gzip_writer_close();
        gzip_why = null;
        gzip_fixed_init();
        level = level ? level : 6;
        gzip_writer.level = level > 9 ? 9 : level;
        header[8] = gzip_writer.level == 1 ? 4 : gzip_writer.level == 9 ? 2 : 0;
        gzip_writer.history = 0;
        gzip_writer.n = 0;
        gzip_writer.crc = 0xffffffffu;
        gzip_writer.isize = 0;
        gzip_writer.failed = false;
        gzip_writer.store = gzip_output.bytes ? address_of gzip_output : null;
        gzip_writer.fd = gzip_out_fd;
        gzip_writer.slot_count = parallel_slots();
        gzip_writer.slots = (gzip_encoder address_to address_to)memory(
            gzip_writer.slot_count * sizeof(gzip_encoder address_to));
        gzip_writer.done = (p8 address_to)memory(GZIP_BATCH_BLOCKS);
        gzip_writer.input_room = GZIP_WINDOW + GZIP_BATCH_BLOCKS * GZIP_BLOCK;
        gzip_writer.input = (p8 address_to)memory(gzip_writer.input_room);
        if (!gzip_writer.slots || system_failed(gzip_writer.slots) || !gzip_writer.done ||
            system_failed(gzip_writer.done) || !gzip_writer.input ||
            system_failed(gzip_writer.input))
        {
                if (system_failed(gzip_writer.slots))
                        gzip_writer.slots = null;
                if (system_failed(gzip_writer.done))
                        gzip_writer.done = null;
                if (system_failed(gzip_writer.input))
                        gzip_writer.input = null;
                return gzip_fail("gzip cannot map the block input");
        }
        return gzip_writer_emit(header, sizeof(header));
}

static bool gzip_encode_trailer(void)
{
        p8 tail[10] = {0x03, 0x00};
        bool ok = gzip_writer_batch();

        if (ok)
        {
                memory_store_unaligned(p32, tail + 2, ~gzip_writer.crc);
                memory_store_unaligned(p32, tail + 6, gzip_writer.isize);
                ok = gzip_writer_emit(tail, sizeof(tail));
        }
        gzip_writer_close();
        return ok;
}

static bool gzip_stream_encode(void)
{
        positive capacity = GZIP_BATCH_BLOCKS * GZIP_BLOCK;

        for (;;)
        {
                if (gzip_writer.n == capacity && !gzip_writer_batch())
                        break;
                bipolar got = system_read_retry(
                    (positive)gzip_input.fd, gzip_writer.input + GZIP_WINDOW + gzip_writer.n,
                    capacity - gzip_writer.n);
                if (got < 0)
                {
                        gzip_fail("gzip read failed");
                        break;
                }
                if (!got)
                        return gzip_encode_trailer();
                gzip_writer.n += (positive)got;
        }
        gzip_writer_close();
        return false;
}

static bool gzip_encode_begin(bipolar out, p8 level)
{
        gzip_out_fd = out;
        gzip_output.bytes = null;
        gzip_input.fd = -1;
        return gzip_encode_setup(level);
}

static bool gzip_encode_write(p8 address_to src, positive n)
{
        positive capacity = GZIP_BATCH_BLOCKS * GZIP_BLOCK;

        while (n)
        {
                positive take = min(n, capacity - gzip_writer.n);

                memory_copy(gzip_writer.input + GZIP_WINDOW + gzip_writer.n, src, take);
                gzip_writer.n += take;
                src += take;
                n -= take;
                if (gzip_writer.n == capacity && !gzip_writer_batch())
                        return false;
        }
        return !gzip_writer.failed;
}

static bool gzip_encode_end(void)
{
        return gzip_encode_trailer();
}

static bipolar gzip_deflate_mem(p8 address_to src, positive src_len,
                                p8 address_to dst, positive dst_cap, p8 level)
{
        bool ok;

        gzip_input.fd = -1;
        gzip_out_fd = -1;
        gzip_output.bytes = dst;
        gzip_output.room = dst_cap;
        gzip_output.used = 0;
        ok = gzip_encode_setup(level) && gzip_encode_write(src, src_len) &&
             gzip_encode_trailer();
        if (!ok)
                gzip_writer_close();
        gzip_output.bytes = null;
        return ok ? (bipolar)gzip_output.used : -1;
}

static bool gzip_decode_begin(bipolar in)
{
        gzip_out_taken = 0;
        gzip_why = null;
        gzip_out_failed = false;
        byte_input_open_fd(address_of gzip_input, in, gzip_in_buf, GZIP_IN);
        gzip_out_fd = -1;
        gzip_output.bytes = null;
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
        gzip_input.have = n;
        gzip_input.at = 0;
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

                if (!gzip_in_need() && !gzip_head_done)
                {
                        gzip_finished = true;
                        break;
                }
                if (gzip_input.at >= gzip_input.have && gzip_input.eof && !gzip_head_done)
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

#ifndef GZIP_CORE_ONLY

static b32 gzip_stream(bipolar in, bipolar out, bool decode, p8 level)
{
        bool ok;

        byte_input_open_fd(address_of gzip_input, in, gzip_in_buf, GZIP_IN);
        gzip_out_fd = out;
        gzip_output.bytes = null;
        gzip_status = 0;
        if (decode)
                ok = gzip_stream_decode();
        else
                ok = gzip_encode_setup(level) && gzip_stream_encode();
        if (!ok)
        {
                if (gzip_why)
                        string_format(log_error, "gzip: %s\n", gzip_why);
                gzip_status = 1;
                return 1;
        }
        return 0;
}


static const file_codec_suffix gzip_suffixes[] = {
    {".gz", ""}, {".Z", ""}, {".tgz", ".tar"}};

static b32 file_gzip(void)
{
        file_codec_cli codec = {
            .name = "gzip", .decode_name = "gunzip", .cat_name = "zcat",
            .usage = "Usage: gzip [-cdfkqt123456789] [FILE...]",
            .version = "gzip from dawning-kit",
            .status = address_of gzip_status, .suffixes = gzip_suffixes,
            .suffix_count = array_count(gzip_suffixes),
            .decode_suffix_error = "unknown suffix; use -c",
            .encode_suffix_error = "cannot guess output name",
            .features = FILE_CODEC_LONG_QUIET | FILE_CODEC_LEVEL_WORDS |
                        FILE_CODEC_NO_NAME | FILE_CODEC_SHORT_VERSION,
            .remove_source = true, .level = 6,
            .run = gzip_stream};
        return file_codec_main(address_of codec);
}

#endif /* GZIP_CORE_ONLY */
