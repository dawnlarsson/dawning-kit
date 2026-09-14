/*
        xz -- LZMA2 inside the .xz stream (check none, CRC32 or CRC64).

        Decode is a range coder plus the twelve-state LZMA machine, then
        LZMA2 chunks and the stream wrapper. Encode follows xz's presets:
        hash-chain match finders and the fast parser at -0 to -3, a binary
        tree and the price-driven parser at -4 to -9, in blocks of three
        dictionaries that each start fresh (see the encoder below). Range
        trees use shared assembly kernels with a scalar refill tail.
        Checksums are hash_crc32/hash_crc64. Concatenated streams are
        accepted the way xz -d accepts them. There is no SHA-256 check and
        no BCJ.
*/

#include "compression_huffman.c"

#define XZ_MAGIC0 0xfd
#define XZ_IN 16384
#define XZ_OUT 16384
#define XZ_MATCH_MAX 273
#define XZ_MATCH_MIN 2
#define XZ_DICT_MAX (1u << 26)
#define XZ_PROB_LIT (0x300u * 16)
#define XZ_STATES 12
#define XZ_POS 16
#define XZ_LEN_LOW 8
#define XZ_LEN_MID 8
#define XZ_LEN_HIGH 256
#define XZ_DIST_SLOTS 64
#define XZ_ALIGN 16
#define XZ_FULL_DIST 128

#define XZ_CHECK_NONE 0
#define XZ_CHECK_CRC32 1
#define XZ_CHECK_CRC64 4

static p8 xz_in_buf[XZ_IN];
static byte_input xz_input = {.buf = xz_in_buf, .room = XZ_IN};

static p8 xz_out_buf[XZ_OUT + XZ_MATCH_MAX];
static positive xz_out_fill;
static positive xz_out_taken;
static positive xz_out_hashed;
static bipolar xz_out_fd;
static byte_store xz_output;
static bool xz_out_failed;
static string_address xz_why;
static b32 xz_status;

static bool xz_pull;
static bool xz_paused;
static bool xz_finished;
static bool xz_hdr_done;
static bool xz_block_live;
static p8 xz_lz2_kind;
static positive xz_lz2_raw_left;
static p64 xz_lz2_want;
static p64 xz_lz2_pack_from;
static p64 xz_lz2_pack_want;
static p64 xz_lz2_chunk_from;
static p64 xz_in_abs;
static p64 xz_block_body_abs;
static positive xz_block_hdr_size;
static p8 xz_check;

/* Shared range-encoder ABI; the decoder also uses range. */
typedef struct
{
        p32 range;
        p32 cache;
        p64 low;
        positive pending;
        p8 address_to next;
        p8 address_to limit;
        positive full;
} xz_range_state;
static xz_range_state xz_rc;
#define xz_range xz_rc.range
#define xz_cache xz_rc.cache
#define xz_low xz_rc.low
#define xz_cache_size xz_rc.pending
#define xz_rc_full xz_rc.full
static p32 xz_code;
static bool xz_encoding;

typedef struct
{
        p16 is_match[XZ_STATES][XZ_POS];
        p16 is_rep[XZ_STATES];
        p16 is_rep0[XZ_STATES];
        p16 is_rep1[XZ_STATES];
        p16 is_rep2[XZ_STATES];
        p16 is_rep0_long[XZ_STATES][XZ_POS];
        p16 dist_slot[4][XZ_DIST_SLOTS];
        p16 dist_special[XZ_FULL_DIST - 14];
        p16 dist_align[XZ_ALIGN];
        p16 match_choice;
        p16 match_choice2;
        p16 match_low[XZ_POS][XZ_LEN_LOW];
        p16 match_mid[XZ_POS][XZ_LEN_MID];
        p16 match_high[XZ_LEN_HIGH];
        p16 rep_choice;
        p16 rep_choice2;
        p16 rep_low[XZ_POS][XZ_LEN_LOW];
        p16 rep_mid[XZ_POS][XZ_LEN_MID];
        p16 rep_high[XZ_LEN_HIGH];
        p16 lit[XZ_PROB_LIT];

} xz_probability_state;
static xz_probability_state xz_models;
#define xz_is_match xz_models.is_match
#define xz_is_rep xz_models.is_rep
#define xz_is_rep0 xz_models.is_rep0
#define xz_is_rep1 xz_models.is_rep1
#define xz_is_rep2 xz_models.is_rep2
#define xz_is_rep0_long xz_models.is_rep0_long
#define xz_dist_slot xz_models.dist_slot
#define xz_dist_special xz_models.dist_special
#define xz_dist_align xz_models.dist_align
#define xz_match_choice xz_models.match_choice
#define xz_match_choice2 xz_models.match_choice2
#define xz_match_low xz_models.match_low
#define xz_match_mid xz_models.match_mid
#define xz_match_high xz_models.match_high
#define xz_rep_choice xz_models.rep_choice
#define xz_rep_choice2 xz_models.rep_choice2
#define xz_rep_low xz_models.rep_low
#define xz_rep_mid xz_models.rep_mid
#define xz_rep_high xz_models.rep_high
#define xz_lit xz_models.lit

static p8 address_to xz_dict;
static positive xz_dict_cap;
static positive xz_dict_size;
static positive xz_dict_pos;
static positive xz_dict_full;
static p8 xz_lc;
static p8 xz_lp;
static p8 xz_pb;
static p8 xz_state;
static positive xz_rep[4];
static p64 xz_unpacked;
static p32 xz_crc32;
static p64 xz_crc64;


static bool xz_fail(string_address why)
{
        xz_why = why;
        return false;
}

static bool xz_in_need(void)
{
        bipolar got = byte_input_need(address_of xz_input, 1);
        return got < 0 ? xz_fail("xz read failed") : got != 0;
}

static bipolar xz_in_byte(void)
{
        if (xz_input.at >= xz_input.have && !xz_in_need())
                return -1;
        xz_in_abs++;
        return xz_in_buf[xz_input.at++];
}

/* A little-endian field, read a byte at a time across refills. */
static bool xz_in_le(p64 address_to value, p8 bytes)
{
        address_to value = 0;
        for (p8 at = 0; at < bytes; at++)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return false;
                address_to value |= (p64)byte << (8 * at);
        }
        return true;
}

/* Hash completed output spans once, before a drain or block check. A
   watermark separates blocks even when both share the same output slab. */
static fn xz_output_check(void)
{
        if (xz_encoding || xz_out_hashed == xz_out_fill)
                return;
        positive n = xz_out_fill - xz_out_hashed;
        p8 address_to p = xz_out_buf + xz_out_hashed;
        if (xz_check == XZ_CHECK_CRC32)
                xz_crc32 = hash_crc32(xz_crc32, p, n);
        else if (xz_check == XZ_CHECK_CRC64)
                xz_crc64 = hash_crc64(xz_crc64, p, n);
        xz_out_hashed = xz_out_fill;
}

static bool xz_out_flush(void)
{
        if (xz_out_failed)
                return false;
        if (!xz_out_fill)
                return true;
        xz_output_check();
        if (xz_output.bytes)
        {
                if (!byte_store_append_exact(address_of xz_output,
                                              xz_out_buf, xz_out_fill))
                {
                        xz_out_failed = true;
                        return xz_fail("xz output is too small");
                }
        }
        else if (xz_out_fd >= 0 &&
                 system_write_all((positive)xz_out_fd, xz_out_buf,
                                  xz_out_fill) != xz_out_fill)
        {
                xz_out_failed = true;
                return xz_fail("xz write failed");
        }
        xz_out_fill = xz_out_taken = xz_out_hashed = 0;
        return true;
}

/* Append a span no longer than the dictionary to its ring. */
static fn xz_dict_append(p8 address_to bytes, positive n)
{
        positive first = min(n, xz_dict_size - xz_dict_pos);

        memory_copy_apart(xz_dict + xz_dict_pos, bytes, first);
        if (n > first)
                memory_copy_apart(xz_dict, bytes + first, n - first);
        xz_dict_pos += n;
        if (xz_dict_pos >= xz_dict_size)
                xz_dict_pos -= xz_dict_size;
        xz_dict_full = min(xz_dict_full + n, xz_dict_size);
}

static bool xz_emit(p8 byte)
{
        if (xz_dict && xz_dict_size)
        {
                xz_dict[xz_dict_pos] = byte;
                xz_dict_pos++;
                if (xz_dict_pos == xz_dict_size)
                        xz_dict_pos = 0;
                if (xz_dict_full < xz_dict_size)
                        xz_dict_full++;
        }
        xz_unpacked++;
        xz_out_buf[xz_out_fill++] = byte;
        if (!xz_pull && xz_out_fill >= XZ_OUT)
                return xz_out_flush();
        return true;
}

static p8 xz_dict_get(positive dist)
{
        positive at;

        if (!dist || dist > xz_dict_full)
                return 0;
        at = xz_dict_pos;
        if (at >= dist)
                at -= dist;
        else
                at += xz_dict_size - dist;
        return xz_dict[at];
}

static bool xz_emit_match(positive dist, positive length)
{
        p8 address_to into = xz_out_buf + xz_out_fill;
        positive seed = length < dist ? length : dist;
        positive at = xz_dict_pos >= dist ? xz_dict_pos - dist
                                          : xz_dict_pos + xz_dict_size - dist;
        positive first = xz_dict_size - at;

        if (first > seed)
                first = seed;
        memory_copy_apart(into, xz_dict + at, first);
        if (seed > first)
                memory_copy_apart(into + first, xz_dict, seed - first);
        if (length > seed)
                memory_copy_match(into + seed, dist, length - seed);
        xz_dict_append(into, length);
        xz_unpacked += length;
        xz_out_fill += length;
        if (!xz_pull && xz_out_fill >= XZ_OUT)
                return xz_out_flush();
        return true;
}

static fn xz_probs_reset(void)
{
        positive at;
        positive i;

        for (at = 0; at < XZ_STATES; at++)
        {
                xz_is_rep[at] = 1024;
                xz_is_rep0[at] = 1024;
                xz_is_rep1[at] = 1024;
                xz_is_rep2[at] = 1024;
                for (i = 0; i < XZ_POS; i++)
                {
                        xz_is_match[at][i] = 1024;
                        xz_is_rep0_long[at][i] = 1024;
                }
        }
        for (at = 0; at < 4; at++)
                for (i = 0; i < XZ_DIST_SLOTS; i++)
                        xz_dist_slot[at][i] = 1024;
        for (at = 0; at < XZ_FULL_DIST - 14; at++)
                xz_dist_special[at] = 1024;
        for (at = 0; at < XZ_ALIGN; at++)
                xz_dist_align[at] = 1024;
        xz_match_choice = 1024;
        xz_match_choice2 = 1024;
        xz_rep_choice = 1024;
        xz_rep_choice2 = 1024;
        for (at = 0; at < XZ_POS; at++)
        {
                for (i = 0; i < XZ_LEN_LOW; i++)
                {
                        xz_match_low[at][i] = 1024;
                        xz_rep_low[at][i] = 1024;
                }
                for (i = 0; i < XZ_LEN_MID; i++)
                {
                        xz_match_mid[at][i] = 1024;
                        xz_rep_mid[at][i] = 1024;
                }
        }
        for (at = 0; at < XZ_LEN_HIGH; at++)
        {
                xz_match_high[at] = 1024;
                xz_rep_high[at] = 1024;
        }
        for (at = 0; at < XZ_PROB_LIT; at++)
                xz_lit[at] = 1024;
        xz_state = 0;
        xz_rep[0] = xz_rep[1] = xz_rep[2] = xz_rep[3] = 1;
}

static bool xz_dict_open(positive size)
{
        if (size > XZ_DICT_MAX || !size)
                return xz_fail("xz dictionary");
        if (xz_dict && xz_dict_cap >= size)
        {
                xz_dict_size = size;
                xz_dict_pos = 0;
                xz_dict_full = 0;
                return true;
        }
        if (xz_dict)
        {
                memory_free(xz_dict, xz_dict_cap);
                xz_dict = null;
        }
        xz_dict = (p8 address_to)memory(size);
        if (!xz_dict || system_failed(xz_dict))
        {
                xz_dict = null;
                return xz_fail("xz cannot map the dictionary");
        }
        xz_dict_cap = size;
        xz_dict_size = size;
        xz_dict_pos = 0;
        xz_dict_full = 0;
        return true;
}

static fn xz_dict_close(void)
{
        if (xz_dict)
        {
                memory_free(xz_dict, xz_dict_cap);
                xz_dict = null;
                xz_dict_cap = 0;
        }
}

static bool xz_rc_norm(void)
{
        if (xz_range >= 0x1000000u)
                return true;
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated range");
                xz_range <<= 8;
                xz_code = (xz_code << 8) | (p8)byte;
        }
        return true;
}

static bipolar xz_rc_bit(p16 address_to prob)
{
        p32 bound;

        if (!xz_rc_norm())
                return -1;
        bound = (xz_range >> 11) * address_to prob;
        if (xz_code < bound)
        {
                xz_range = bound;
                address_to prob = (p16)(address_to prob + ((2048 - address_to prob) >> 5));
                return 0;
        }
        xz_range -= bound;
        xz_code -= bound;
        address_to prob = (p16)(address_to prob - (address_to prob >> 5));
        return 1;
}

typedef struct
{
        p32 range;
        p32 code;
        p8 address_to next;
        p8 address_to limit;
} xz_range_input;

static bipolar xz_rc_tree_fast(p16 address_to probs, positive mode)
{
        xz_range_input state = {xz_range, xz_code, xz_in_buf + xz_input.at,
                               xz_in_buf + xz_input.have};
        p8 address_to start = state.next;
        bipolar result = lzma_range_decode(address_of state, probs, mode);
        xz_range = state.range;
        xz_code = state.code;
        xz_input.at += (positive)(state.next - start);
        xz_in_abs += (positive)(state.next - start);
        return result;
}

static bipolar xz_rc_bittree(p16 address_to probs, p8 bits)
{
        if (!bits)
                return 0;
        if (xz_input.have - xz_input.at >= 24)
                return xz_rc_tree_fast(probs, bits);
        positive sym = 1;
        p8 n;

        for (n = 0; n < bits; n++)
        {
                bipolar bit = xz_rc_bit(probs + sym);

                if (bit < 0)
                        return -1;
                sym = (sym << 1) + (positive)bit;
        }
        return (bipolar)(sym - ((positive)1 << bits));
}

static bipolar xz_rc_bittree_rev(p16 address_to probs, p8 bits)
{
        if (!bits)
                return 0;
        if (xz_input.have - xz_input.at >= 24)
                return xz_rc_tree_fast(probs, 0x100 | bits);
        positive sym = 0;
        p8 n;

        for (n = 0; n < bits; n++)
        {
                bipolar bit = xz_rc_bit(probs + (sym | ((positive)1 << n)));

                if (bit < 0)
                        return -1;
                if (bit)
                        sym |= (positive)1 << n;
        }
        return (bipolar)sym;
}

static bipolar xz_rc_direct_ok(p8 bits)
{
        positive v = 0;

        while (bits--)
        {
                p32 t;

                if (!xz_rc_norm())
                        return -1;
                xz_range >>= 1;
                t = xz_code - xz_range;
                v <<= 1;
                if (t < xz_code)
                {
                        xz_code = t;
                        v |= 1;
                }
        }
        return (bipolar)v;
}

static bipolar xz_len(p16 address_to choice, p16 address_to choice2,
                      p16 address_to low, p16 address_to mid, p16 address_to high,
                      positive pos_state)
{
        bipolar bit;
        bipolar v;

        bit = xz_rc_bit(choice);
        if (bit < 0)
                return -1;
        if (!bit)
        {
                v = xz_rc_bittree(low + pos_state * XZ_LEN_LOW, 3);
                return v < 0 ? -1 : v + 2;
        }
        bit = xz_rc_bit(choice2);
        if (bit < 0)
                return -1;
        if (!bit)
        {
                v = xz_rc_bittree(mid + pos_state * XZ_LEN_MID, 3);
                return v < 0 ? -1 : v + 2 + 8;
        }
        v = xz_rc_bittree(high, 8);
        return v < 0 ? -1 : v + 2 + 8 + 8;
}

static bool xz_literal(positive pos_state)
{
        p16 address_to probs;
        positive symbol = 1;
        p8 prev;
        p8 match;
        p8 lit_pos;
        (void)pos_state;

        prev = xz_dict_full ? xz_dict_get(1) : 0;
        lit_pos = (p8)(xz_unpacked & ((((positive)1 << xz_lp) - 1)));
        probs = xz_lit +
                (((((positive)lit_pos << xz_lc) + (prev >> (8 - xz_lc))) * 0x300));
        if (xz_input.have - xz_input.at >= 24)
        {
                positive mode = 8;
                if (xz_state >= 7)
                        mode |= 0x200 | ((positive)xz_dict_get(xz_rep[0]) << 16);
                bipolar value = xz_rc_tree_fast(probs, mode);
                if (value < 0)
                        return xz_fail("xz truncated literal");
                symbol = (positive)value;
                goto emit;
        }
        if (xz_state >= 7)
        {
                match = xz_dict_get(xz_rep[0]);
                do
                {
                        positive match_bit = (match >> 7) & 1;
                        bipolar bit;

                        match <<= 1;
                        bit = xz_rc_bit(probs + ((1 + match_bit) << 8) + symbol);
                        if (bit < 0)
                                return false;
                        symbol = (symbol << 1) | (positive)bit;
                        if ((positive)bit != match_bit)
                                break;
                } while (symbol < 0x100);
        }
        while (symbol < 0x100)
        {
                bipolar bit = xz_rc_bit(probs + symbol);

                if (bit < 0)
                        return false;
                symbol = (symbol << 1) | (positive)bit;
        }
emit:
        if (!xz_emit((p8)symbol))
                return false;
        xz_state = xz_state < 4 ? 0 : xz_state < 10 ? xz_state - 3 : xz_state - 6;
        return true;
}

static bool xz_distance(positive len_state, positive address_to dist)
{
        bipolar slot;
        bipolar extra;
        p8 bits;

        slot = xz_rc_bittree(xz_dist_slot[len_state], 6);
        if (slot < 0)
                return false;
        if (slot < 4)
        {
                address_to dist = (positive)slot;
                return true;
        }
        bits = (p8)((slot >> 1) - 1);
        address_to dist = (2 | (slot & 1)) << bits;
        if (slot < 14)
        {
                extra = xz_rc_bittree_rev(xz_dist_special + address_to dist - slot - 1,
                                          bits);
                if (extra < 0)
                        return false;
                address_to dist += (positive)extra;
                return true;
        }
        extra = xz_rc_direct_ok((p8)(bits - 4));
        if (extra < 0)
                return false;
        address_to dist += (positive)extra << 4;
        extra = xz_rc_bittree_rev(xz_dist_align, 4);
        if (extra < 0)
                return false;
        address_to dist += (positive)extra;
        return true;
}

static bool xz_lzma_packet(void)
{
        positive pos_state = (positive)xz_unpacked & ((((positive)1 << xz_pb) - 1));
        bipolar bit;
        bipolar len;
        positive dist;

        if (xz_pull && xz_out_fill >= XZ_OUT)
        {
                xz_paused = true;
                return true;
        }

        bit = xz_rc_bit(address_of xz_is_match[xz_state][pos_state]);
        if (bit < 0)
                return false;
        if (!bit)
                return xz_literal(pos_state);

        bit = xz_rc_bit(address_of xz_is_rep[xz_state]);
        if (bit < 0)
                return false;
        if (!bit)
        {
                xz_rep[3] = xz_rep[2];
                xz_rep[2] = xz_rep[1];
                xz_rep[1] = xz_rep[0];
                len = xz_len(address_of xz_match_choice, address_of xz_match_choice2,
                             (p16 address_to)xz_match_low, (p16 address_to)xz_match_mid,
                             xz_match_high, pos_state);
                if (len < 0)
                        return false;
                if (!xz_distance(len < 6 ? (positive)len - 2 : 3, address_of dist))
                        return false;
                xz_rep[0] = dist + 1;
                xz_state = xz_state < 7 ? 7 : 10;
                if (!xz_rep[0] || xz_rep[0] > xz_dict_full)
                        return xz_fail("xz distance");
                return xz_emit_match(xz_rep[0], (positive)len);
        }

        bit = xz_rc_bit(address_of xz_is_rep0[xz_state]);
        if (bit < 0)
                return false;
        if (!bit)
        {
                bit = xz_rc_bit(address_of xz_is_rep0_long[xz_state][pos_state]);
                if (bit < 0)
                        return false;
                if (!bit)
                {
                        xz_state = xz_state < 7 ? 9 : 11;
                        if (!xz_rep[0] || xz_rep[0] > xz_dict_full)
                                return xz_fail("xz distance");
                        return xz_emit_match(xz_rep[0], 1);
                }
        }
        else
        {
                bit = xz_rc_bit(address_of xz_is_rep1[xz_state]);
                if (bit < 0)
                        return false;
                if (!bit)
                        dist = xz_rep[1];
                else
                {
                        bit = xz_rc_bit(address_of xz_is_rep2[xz_state]);
                        if (bit < 0)
                                return false;
                        if (!bit)
                                dist = xz_rep[2];
                        else
                                dist = xz_rep[3], xz_rep[3] = xz_rep[2];
                        xz_rep[2] = xz_rep[1];
                }
                xz_rep[1] = xz_rep[0];
                xz_rep[0] = dist;
        }
        len = xz_len(address_of xz_rep_choice, address_of xz_rep_choice2,
                     (p16 address_to)xz_rep_low, (p16 address_to)xz_rep_mid,
                     xz_rep_high, pos_state);
        if (len < 0)
                return false;
        xz_state = xz_state < 7 ? 8 : 11;
        if (!xz_rep[0] || xz_rep[0] > xz_dict_full)
                return xz_fail("xz distance");
        return xz_emit_match(xz_rep[0], (positive)len);
}

static bool xz_rc_init(void)
{
        bipolar first;
        positive at;

        xz_range = 0xffffffffu;
        xz_code = 0;
        first = xz_in_byte();
        if (first < 0)
                return xz_fail("xz truncated LZMA");
        if (first)
                return xz_fail("xz LZMA range init");
        for (at = 0; at < 4; at++)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated LZMA");
                xz_code = (xz_code << 8) | (p8)byte;
        }
        return true;
}

static positive xz_dict_from_prop(p8 prop)
{
        if (prop > 40)
                return 0;
        if (prop == 40)
                return XZ_DICT_MAX;
        return (2u | (prop & 1)) << (prop / 2 + 11);
}

static bool xz_props(p8 packed)
{
        p8 lc;
        p8 rest;

        if (packed > (4 * 5 + 4) * 9 + 8)
                return xz_fail("xz LZMA properties");
        lc = packed % 9;
        rest = packed / 9;
        xz_lp = rest % 5;
        xz_pb = rest / 5;
        xz_lc = lc;
        if ((positive)xz_lc + xz_lp > 4)
                return xz_fail("xz lc+lp");
        return true;
}

static bool xz_lzma2_raw(void)
{
        while (xz_lz2_raw_left)
        {
                if (xz_pull && xz_out_fill >= XZ_OUT)
                {
                        xz_paused = true;
                        return true;
                }
                if (xz_input.at == xz_input.have && !xz_in_need())
                        return xz_fail("xz truncated uncompressed");
                positive take = xz_input.have - xz_input.at;
                if (take > xz_lz2_raw_left)
                        take = xz_lz2_raw_left;
                if (take > XZ_OUT - xz_out_fill)
                        take = XZ_OUT - xz_out_fill;
                if (take > xz_dict_size - xz_dict_pos)
                        take = xz_dict_size - xz_dict_pos;
                if (!take)
                        return xz_fail("xz truncated uncompressed");
                p8 address_to bytes = xz_in_buf + xz_input.at;
                memory_copy_apart(xz_out_buf + xz_out_fill, bytes, take);
                xz_dict_append(bytes, take);
                xz_out_fill += take;
                xz_input.at += take;
                xz_in_abs += take;
                xz_unpacked += take;
                xz_lz2_raw_left -= take;
                if (!xz_pull && xz_out_fill >= XZ_OUT && !xz_out_flush())
                        return false;
        }

        xz_lz2_kind = 0;
        return true;
}

/* Span ABI: fixed-width fields followed by the probability and dictionary
   pointers. The kernel emits into a contiguous part of the dictionary; the
   framing layer transfers that span once and owns checksums and refills. */
typedef struct
{
        p32 range, code;
        p8 address_to next;
        p8 address_to limit;
        xz_probability_state address_to model;
        p8 address_to dict;
        positive size, pos, full;
        p64 unpacked, stop;
        positive room;
        p32 state, lc, lp, pb;
        positive rep[4];
        positive error;
} xz_decode_job;

static bool xz_decode_fast(void)
{
        positive have = xz_input.have - xz_input.at;
        p64 packed = xz_in_abs - xz_lz2_pack_from;
        if (packed >= xz_lz2_pack_want)
                return true;
        if (have > xz_lz2_pack_want - packed)
                have = (positive)(xz_lz2_pack_want - packed);
        positive room = XZ_OUT > xz_out_fill ? XZ_OUT - xz_out_fill : 0;
        if (room > xz_dict_size - xz_dict_pos)
                room = xz_dict_size - xz_dict_pos;
        if (have < 64 || room < XZ_MATCH_MAX)
                return true;
        xz_decode_job job = {xz_range, xz_code, xz_in_buf + xz_input.at,
                xz_in_buf + xz_input.at + have, address_of xz_models, xz_dict,
                xz_dict_size, xz_dict_pos, xz_dict_full, xz_unpacked,
                xz_lz2_chunk_from + xz_lz2_want, room,
                xz_state, xz_lc, xz_lp, xz_pb,
                {xz_rep[0], xz_rep[1], xz_rep[2], xz_rep[3]}, 0};
        lzma_decode_span(address_of job);
        positive produced = job.pos - xz_dict_pos;
        positive consumed = (positive)(job.next - (xz_in_buf + xz_input.at));
        xz_range = job.range;
        xz_code = job.code;
        xz_input.at += consumed;
        xz_in_abs += consumed;
        xz_state = (p8)job.state;
        for (positive i = 0; i < 4; i++) xz_rep[i] = job.rep[i];
        memory_copy_apart(xz_out_buf + xz_out_fill, xz_dict + xz_dict_pos, produced);
        xz_out_fill += produced;
        xz_dict_pos = job.pos;
        if (xz_dict_pos == xz_dict_size) xz_dict_pos = 0;
        xz_dict_full = job.full;
        xz_unpacked = job.unpacked;
        if (job.error) return xz_fail("xz distance or chunk length");
        if (!xz_pull && xz_out_fill >= XZ_OUT)
                return xz_out_flush();
        return true;
}

static bool xz_lzma2_lzma(void)
{
        while (xz_unpacked - xz_lz2_chunk_from < xz_lz2_want)
        {
                if (xz_pull && xz_out_fill >= XZ_OUT)
                {
                        xz_paused = true;
                        return true;
                }
                p64 before = xz_unpacked;
                if (!xz_decode_fast()) return false;
                if (xz_unpacked != before) continue;
                if (!xz_lzma_packet())
                        return false;
                if (xz_paused)
                        return true;
        }
        xz_lz2_kind = 0;
        return true;
}

static bool xz_lzma2_finish_packed(void)
{
        p64 used = xz_in_abs - xz_lz2_pack_from;

        if (used > xz_lz2_pack_want)
                return xz_fail("xz LZMA2 compressed size");
        while (used < xz_lz2_pack_want)
        {
                if (xz_in_byte() < 0)
                        return xz_fail("xz truncated LZMA2");
                used++;
        }
        return true;
}

static bool xz_lzma2(void)
{
        if (xz_lz2_kind == 1)
        {
                if (!xz_lzma2_raw())
                        return false;
                if (xz_paused)
                        return true;
        }
        else if (xz_lz2_kind == 2)
        {
                if (!xz_lzma2_lzma())
                        return false;
                if (xz_paused)
                        return true;
                if (!xz_lzma2_finish_packed())
                        return false;
        }

        for (;;)
        {
                bipolar control = xz_in_byte();
                bipolar hi;
                bipolar lo;
                p64 want_packed;

                if (control < 0)
                        return xz_fail("xz truncated LZMA2");
                if (!control)
                {
                        xz_lz2_kind = 0;
                        return true;
                }
                if (control == 1 || control == 2)
                {
                        hi = xz_in_byte();
                        lo = xz_in_byte();
                        if (hi < 0 || lo < 0)
                                return xz_fail("xz truncated uncompressed");
                        xz_lz2_raw_left = ((positive)(p8)hi << 8) + (p8)lo + 1;
                        if (control == 1)
                        {
                                xz_dict_pos = 0;
                                xz_dict_full = 0;
                        }
                        xz_lz2_kind = 1;
                        if (!xz_lzma2_raw())
                                return false;
                        if (xz_paused)
                                return true;
                        continue;
                }
                if (control < 0x80)
                        return xz_fail("xz LZMA2 control");

                xz_lz2_want = (control & 0x1f);
                hi = xz_in_byte();
                lo = xz_in_byte();
                if (hi < 0 || lo < 0)
                        return xz_fail("xz truncated LZMA2 sizes");
                xz_lz2_want = (xz_lz2_want << 16) +
                              ((positive)(p8)hi << 8) + (p8)lo + 1;
                hi = xz_in_byte();
                lo = xz_in_byte();
                if (hi < 0 || lo < 0)
                        return xz_fail("xz truncated LZMA2 sizes");
                want_packed = ((positive)(p8)hi << 8) + (p8)lo + 1;

                {
                        p8 reset = (p8)((control >> 5) & 3);

                        if (reset >= 2)
                        {
                                bipolar prop = xz_in_byte();

                                if (prop < 0)
                                        return xz_fail("xz truncated properties");
                                if (!xz_props((p8)prop))
                                        return false;
                        }
                        xz_lz2_pack_from = xz_in_abs;
                        xz_lz2_pack_want = want_packed;
                        if (reset >= 1)
                                xz_probs_reset();
                        if (reset == 3)
                        {
                                xz_dict_pos = 0;
                                xz_dict_full = 0;
                        }
                        if (!xz_rc_init())
                                return false;
                }

                xz_lz2_chunk_from = xz_unpacked;
                xz_lz2_kind = 2;
                if (!xz_lzma2_lzma())
                        return false;
                if (xz_paused)
                        return true;
                if (!xz_lzma2_finish_packed())
                        return false;
        }
}

static bool xz_pad4(positive n)
{
        while (n & 3)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated padding");
                if (byte)
                        return xz_fail("xz padding");
                n++;
        }
        return true;
}

static bool xz_check_read(p64 want32, p64 want64)
{
        if (xz_check == XZ_CHECK_NONE)
                return true;
        if (xz_check != XZ_CHECK_CRC32 && xz_check != XZ_CHECK_CRC64)
                return xz_fail("xz check type");

        bool wide = xz_check == XZ_CHECK_CRC64;
        p64 got;

        if (!xz_in_le(address_of got, wide ? 8 : 4))
                return xz_fail("xz truncated check");
        if (got != (wide ? want64 : (p32)want32))
                return xz_fail(wide ? "xz CRC64 mismatch" : "xz CRC32 mismatch");
        return true;
}

static bool xz_block(void)
{
        bipolar hdr0;
        positive header_size;
        p8 flags;
        p8 header[1024];
        positive at;
        p32 got;
        p32 expect;
        p8 filters;
        p8 prop;
        positive dict;

        if (!xz_block_live)
        {
                hdr0 = xz_in_byte();
                if (hdr0 < 0)
                        return xz_fail("xz truncated block");
                if (!hdr0)
                {
                        xz_input.at--;
                        xz_in_abs--;
                        return false;
                }
                header_size = ((positive)(p8)hdr0 + 1) * 4;
                if (header_size > sizeof(header))
                        return xz_fail("xz block header");
                header[0] = (p8)hdr0;
                for (at = 1; at < header_size; at++)
                {
                        bipolar byte = xz_in_byte();

                        if (byte < 0)
                                return xz_fail("xz truncated block header");
                        header[at] = (p8)byte;
                }
                flags = header[1];
                if (flags & 0x3c)
                        return xz_fail("xz reserved block flags");
                filters = (flags & 3) + 1;
                if (filters != 1)
                        return xz_fail("xz filter chain");
                at = 2;
                if (flags & 0x40)
                {
                        while (at < header_size - 4 && (header[at] & 0x80))
                                at++;
                        at++;
                }
                if (flags & 0x80)
                {
                        while (at < header_size - 4 && (header[at] & 0x80))
                                at++;
                        at++;
                }
                if (at + 2 >= header_size - 4)
                        return xz_fail("xz filter flags");
                if (header[at] != 0x21)
                        return xz_fail("xz filter is not LZMA2");
                if (header[at + 1] != 1)
                        return xz_fail("xz LZMA2 properties size");
                prop = header[at + 2];
                dict = xz_dict_from_prop(prop);
                if (!dict || !xz_dict_open(dict))
                        return false;
                expect = ~hash_crc32(0xffffffffu, header, header_size - 4);
                got = 0;
                for (at = 0; at < 4; at++)
                        got |= (p32)header[header_size - 4 + at] << (8 * at);
                if (got != expect)
                        return xz_fail("xz block header CRC");

                xz_block_hdr_size = header_size;
                xz_block_body_abs = xz_in_abs;
                xz_crc32 = 0xffffffffu;
                xz_crc64 = 0xffffffffffffffffull;
                xz_probs_reset();
                xz_lz2_kind = 0;
                xz_block_live = true;
        }
        if (!xz_lzma2())
                return false;
        if (xz_paused)
                return true;
        {
                positive packed = (positive)(xz_in_abs - xz_block_body_abs);

                if (!xz_pad4(xz_block_hdr_size + packed))
                        return false;
        }
        xz_output_check();
        if (!xz_check_read(~xz_crc32, ~xz_crc64))
                return false;
        xz_block_live = false;
        xz_lz2_kind = 0;
        return true;
}

static bipolar xz_vli_crc(p32 address_to crc, positive address_to hashed)
{
        p64 v = 0;
        p8 shift = 0;

        for (;;)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return -1;
                p8 read = (p8)byte;

                address_to crc = hash_crc32(address_to crc, address_of read, 1);
                address_to hashed += 1;
                v |= (p64)((p8)byte & 0x7f) << shift;
                if (!((p8)byte & 0x80))
                        return (bipolar)v;
                shift += 7;
                if (shift >= 63)
                        return xz_fail("xz VLI"), -1;
        }
}

static bool xz_index_and_footer(void)
{
        p8 zero = 0;
        bipolar indicator;
        bipolar records;
        p32 crc;
        p64 got;
        p64 back;
        p8 body[6];
        positive hashed = 1;
        bipolar fb0;
        bipolar fb1;
        bipolar y;
        bipolar z;

        indicator = xz_in_byte();
        if (indicator != 0)
                return xz_fail("xz index indicator");
        crc = hash_crc32(0xffffffffu, address_of zero, 1);
        records = xz_vli_crc(address_of crc, address_of hashed);
        if (records < 0)
                return false;
        while (records--)
        {
                if (xz_vli_crc(address_of crc, address_of hashed) < 0 ||
                    xz_vli_crc(address_of crc, address_of hashed) < 0)
                        return xz_fail("xz truncated index");
        }
        while (hashed & 3)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated index padding");
                if (byte)
                        return xz_fail("xz index padding");
                crc = hash_crc32(crc, address_of zero, 1);
                hashed++;
        }
        if (!xz_in_le(address_of got, 4))
                return xz_fail("xz truncated index CRC");
        if (got != (p32)~crc)
                return xz_fail("xz index CRC");

        if (!xz_in_le(address_of got, 4) || !xz_in_le(address_of back, 4))
                return xz_fail("xz truncated footer");
        fb0 = xz_in_byte();
        fb1 = xz_in_byte();
        if (fb0 < 0 || fb1 < 0)
                return xz_fail("xz truncated footer");
        if (fb0 || (fb1 & 0xf0) || (fb1 & 0xf) != xz_check)
                return xz_fail("xz footer flags");
        y = xz_in_byte();
        z = xz_in_byte();
        if (y != 'Y' || z != 'Z')
                return xz_fail("xz footer magic");
        memory_store_unaligned(p32, body, (p32)back);
        body[4] = (p8)fb0;
        body[5] = (p8)fb1;
        if (got != (p32)~hash_crc32(0xffffffffu, body, 6))
                return xz_fail("xz footer CRC");
        return true;
}

static bool xz_stream(void)
{
        p64 magic;
        p8 flags[2];
        p64 got;

        /* Stream Padding: after a stream, NUL bytes in fours may precede the
           next one or the end of the file. */
        if (!xz_hdr_done && xz_in_abs)
        {
                p64 from = xz_in_abs;

                while ((xz_input.at < xz_input.have || xz_in_need()) &&
                       !xz_in_buf[xz_input.at])
                {
                        xz_input.at++;
                        xz_in_abs++;
                }
                if (xz_why)
                        return false;
                if ((xz_in_abs - from) & 3)
                        return xz_fail("xz stream padding");
                if (xz_input.at >= xz_input.have)
                        return true;
        }

        if (!xz_hdr_done)
        {
                if (!xz_in_le(address_of magic, 6))
                        return xz_fail("xz truncated header");
                if (magic != 0x005a587a37fdull)
                        return xz_fail("xz bad magic");
                flags[0] = (p8)xz_in_byte();
                flags[1] = (p8)xz_in_byte();
                if (flags[0] || (flags[1] & 0xf0))
                        return xz_fail("xz reserved stream flags");
                xz_check = flags[1] & 0xf;
                if (xz_check != XZ_CHECK_NONE && xz_check != XZ_CHECK_CRC32 &&
                    xz_check != XZ_CHECK_CRC64)
                        return xz_fail("xz check type");
                if (!xz_in_le(address_of got, 4))
                        return xz_fail("xz truncated header CRC");
                if (got != (p32)~hash_crc32(0xffffffffu, flags, 2))
                        return xz_fail("xz header CRC");
                xz_hdr_done = true;
        }

        for (;;)
        {
                if (xz_block_live)
                {
                        if (!xz_block())
                                return false;
                        if (xz_paused)
                                return true;
                        continue;
                }
                if (!xz_in_need())
                        return xz_fail("xz truncated stream");
                if (xz_input.at < xz_input.have && xz_in_buf[xz_input.at] == 0)
                        break;
                if (!xz_block())
                {
                        if (xz_input.at < xz_input.have && xz_in_buf[xz_input.at] == 0)
                                break;
                        return false;
                }
                if (xz_paused)
                        return true;
        }
        if (!xz_index_and_footer())
                return false;
        xz_hdr_done = false;
        xz_block_live = false;
        return true;
}

static bool xz_stream_decode(void)
{
        xz_encoding = false;
        bool any = false;

        xz_why = null;
        xz_out_failed = false;
        xz_input.at = 0;
        xz_input.have = 0;
        xz_input.eof = false;
        xz_out_fill = xz_out_taken = xz_out_hashed = 0;
        xz_pull = false;
        xz_paused = false;
        xz_finished = false;
        xz_hdr_done = false;
        xz_block_live = false;
        xz_lz2_kind = 0;
        xz_in_abs = 0;
        xz_unpacked = 0;

        for (;;)
        {
                if (!xz_in_need())
                        break;
                if (xz_input.at >= xz_input.have)
                        break;
                if (!xz_stream())
                        return false;
                any = true;
        }
        xz_dict_close();
        if (!any)
                return xz_fail("xz empty input");
        return xz_out_flush();
}

static bipolar xz_inflate_mem(p8 address_to src, positive src_len,
                              p8 address_to dst, positive dst_cap)
{
        bool ok;

        byte_input_open_memory(address_of xz_input, src, src_len, xz_in_buf, XZ_IN);
        xz_out_fd = -1;
        xz_output.bytes = dst;
        xz_output.room = dst_cap;
        xz_output.used = 0;
        ok = xz_stream_decode();
        xz_input.mem = null;
        xz_output.bytes = null;
        return ok ? (bipolar)xz_output.used : -1;
}

static positive xz_vli_put(p8 address_to into, p64 value)
{
        positive n = 0;

        for (; value >= 0x80; value >>= 7)
                into[n++] = (p8)value | 0x80;
        into[n++] = (p8)value;
        return n;
}

static p8 xz_prop_from_dict(positive dict)
{
        p8 prop;

        for (prop = 0; prop < 40; prop++)
                if (xz_dict_from_prop(prop) >= dict)
                        return prop;
        return 39;
}

/*
        Encoder.

        One xz_encoder holds everything a block needs and nothing a second
        block shares: the preset, a pointer to the block's input (readable
        for XZ_SLACK bytes past its end), match-finder tables sized from the
        dictionary, LZMA models, price tables, the optimum array, and the
        block's finished bytes. A block is max(3 * dictionary, 1 MiB) of
        input, decided by the preset alone, and starts with a dictionary
        reset, so blocks can be encoded side by side and the bytes never
        depend on how many are. The preset table, the fast and the price
        driven normal parsers, the hc3/hc4/bt4 match finders and the LZMA2
        chunk rules follow liblzma 5.8. Positions and distances inside the
        encoder are zero-based, as in liblzma.
*/

#define XZ_OPTS 4096
#define XZ_LOOP_INPUT (XZ_OPTS + 1)
#define XZ_INFINITY_PRICE (1u << 30)
#define XZ_HASH2_SIZE (1u << 10)
#define XZ_HASH3_SIZE (1u << 16)
#define XZ_CHUNK_PACKED_MAX 65536u
#define XZ_CHUNK_PLAIN_MAX (1u << 21)
#define XZ_BLOCK_HEADER_MAX 32
#define XZ_SLACK 64
#define XZ_LITERAL 0xffffffffu
#define XZ_LEN_SYMBOLS (XZ_LEN_LOW + XZ_LEN_MID + XZ_LEN_HIGH)
#define XZ_CHANGE_PAIR(small_dist, big_dist) (((big_dist) >> 7) > (small_dist))

enum { XZ_FINDER_HC3, XZ_FINDER_HC4, XZ_FINDER_BT4 };

typedef struct
{
        p8 dict_log;
        bool normal;
        p8 finder;
        p16 nice;
        p16 depth;
} xz_preset;

/* xz 5.8's -0 .. -9, all lc=3 lp=0 pb=2. Depth 0 means 16 + nice/2 for a
   binary tree and 4 + nice/4 for a hash chain. */
static const xz_preset xz_presets[10] = {
        {18, false, XZ_FINDER_HC3, 128, 4},
        {20, false, XZ_FINDER_HC4, 128, 8},
        {21, false, XZ_FINDER_HC4, 273, 24},
        {22, false, XZ_FINDER_HC4, 273, 48},
        {22, true, XZ_FINDER_BT4, 16, 0},
        {23, true, XZ_FINDER_BT4, 32, 0},
        {23, true, XZ_FINDER_BT4, 64, 0},
        {24, true, XZ_FINDER_BT4, 64, 0},
        {25, true, XZ_FINDER_BT4, 64, 0},
        {26, true, XZ_FINDER_BT4, 64, 0}};

typedef struct
{
        p32 len;
        p32 dist;
} xz_found;

typedef struct
{
        p8 state;
        bool prev_1_is_literal;
        bool prev_2;
        p32 pos_prev_2;
        p32 back_prev_2;
        p32 price;
        p32 pos_prev;
        p32 back_prev;
        p32 backs[4];
} xz_optimal;

typedef struct
{
        p32 prices[XZ_POS][XZ_LEN_SYMBOLS];
        p32 counters[XZ_POS];
} xz_length_price;

typedef struct
{
        const xz_preset address_to preset;

        /* The block: input_n bytes at input, zero-based read position,
           how many of those the parser has looked at but not coded, and the
           match finder's position bias (positions start past the window, so
           zero is always too far to be a candidate). */
        p8 address_to input;
        p32 input_n;
        p32 read_pos;
        p32 read_ahead;
        p32 offset;

        p32 address_to hash;
        positive hash_room;
        p32 address_to son;
        positive son_room;
        p32 dict;
        p32 hash_mask;
        p32 cyclic_pos;
        p32 cyclic_size;
        p32 nice;
        p32 depth;

        xz_probability_state models;
        xz_range_state rc;
        p32 lc;
        p32 lp;
        p32 pb;
        p32 lp_mask;
        p32 pos_mask;
        p8 state;
        p32 reps[4];
        p64 position;
        p32 match_count;
        p32 longest;
        xz_found matches[XZ_MATCH_MAX + 1];

        p8 bit_price[128];
        xz_length_price match_prices;
        xz_length_price rep_prices;
        p32 len_table_size;
        p32 dist_slot_prices[4][XZ_DIST_SLOTS];
        p32 dist_prices[4][XZ_FULL_DIST];
        p32 dist_table_size;
        p32 match_price_count;
        p32 align_prices[XZ_ALIGN];
        p32 align_price_count;
        p32 opts_end;
        p32 opts_current;
        xz_optimal opts[XZ_OPTS];

        /* The finished block: out_n bytes at out + out_at. */
        p8 address_to out;
        positive out_room;
        positive out_at;
        positive out_n;
        p64 unpadded;
        p8 chunk[XZ_CHUNK_PACKED_MAX + 32768];
} xz_encoder;

static bool xz_area(p8 address_to address_to area, positive address_to room,
                    positive need, bool address_to fresh)
{
        address_to fresh = false;
        if (address_to room >= need)
                return true;
        if (address_to area)
                memory_free(address_to area, address_to room);
        address_to area = null;
        address_to room = 0;
        p8 address_to bytes = (p8 address_to)memory(need);
        if (!bytes || system_failed(bytes))
                return false;
        address_to area = bytes;
        address_to room = need;
        address_to fresh = true;
        return true;
}

static xz_encoder address_to xz_encoder_open(p8 level)
{
        xz_encoder address_to e = (xz_encoder address_to)memory(sizeof(xz_encoder));

        if (!e || system_failed(e))
                return null;
        e->preset = xz_presets + (level > 9 ? 9 : level);
        e->lc = 3;
        e->lp = 0;
        e->pb = 2;
        e->lp_mask = 0;
        e->pos_mask = 3;
        for (p32 i = 8; i < 2048; i += 16)
        {
                p32 w = i;
                p32 bits = 0;

                for (p32 j = 0; j < 4; j++)
                {
                        w *= w;
                        bits <<= 1;
                        while (w >= (1u << 16))
                        {
                                w >>= 1;
                                bits++;
                        }
                }
                e->bit_price[i >> 4] = (p8)((11u << 4) - 15 - bits);
        }
        return e;
}

static fn xz_encoder_close(xz_encoder address_to e)
{
        if (!e)
                return;
        memory_free(e->hash, e->hash_room);
        memory_free(e->son, e->son_room);
        memory_free(e->out, e->out_room);
        memory_free(e, sizeof(xz_encoder));
}

/* Prices, in 1/16 bits. */
static inline INLINE p32 xz_price(xz_encoder address_to e, p32 prob, p32 bit)
{
        return e->bit_price[(prob ^ ((0u - bit) & 2047)) >> 4];
}

static inline INLINE p32 xz_price0(xz_encoder address_to e, p32 prob)
{
        return e->bit_price[prob >> 4];
}

static inline INLINE p32 xz_price1(xz_encoder address_to e, p32 prob)
{
        return e->bit_price[(prob ^ 2047) >> 4];
}

static inline INLINE p32 xz_tree_price(xz_encoder address_to e, p16 address_to probs,
                                       p32 bits, p32 symbol)
{
        p32 price = 0;

        symbol += 1u << bits;
        do
        {
                p32 bit = symbol & 1;

                symbol >>= 1;
                price += xz_price(e, probs[symbol], bit);
        } while (symbol != 1);
        return price;
}

static inline INLINE p32 xz_reverse_price(xz_encoder address_to e, p16 address_to probs,
                                          p32 bits, p32 symbol)
{
        p32 price = 0;
        p32 index = 1;

        do
        {
                p32 bit = symbol & 1;

                symbol >>= 1;
                price += xz_price(e, probs[index], bit);
                index = (index << 1) + bit;
        } while (--bits);
        return price;
}

/* Bit counts are one instruction where the ISA has them; RV64 without
   Zbb takes the library routine rather than a libgcc call. */
static inline INLINE p32 xz_top_bit(p32 value)
{
#if X64 || ARM64
        return 31 - (p32)__builtin_clz(value);
#else
        return 63 - (p32)bits_leading_zeros(value);
#endif
}

static inline INLINE p32 xz_low_bit(p64 value)
{
#if X64 || ARM64
        return (p32)__builtin_ctzll(value);
#else
        return (p32)bits_trailing_zeros(value);
#endif
}

static inline INLINE p32 xz_slot(p32 dist)
{
        if (dist < 4)
                return dist;
        p32 top = xz_top_bit(dist);
        return (top << 1) + ((dist >> (top - 1)) & 1);
}

static fn xz_length_prices(xz_encoder address_to e, bool rep, p32 ps)
{
        xz_probability_state address_to m = address_of e->models;
        xz_length_price address_to t = rep ? address_of e->rep_prices : address_of e->match_prices;
        p32 choice = rep ? m->rep_choice : m->match_choice;
        p32 choice2 = rep ? m->rep_choice2 : m->match_choice2;
        p16 address_to low = rep ? m->rep_low[ps] : m->match_low[ps];
        p16 address_to mid = rep ? m->rep_mid[ps] : m->match_mid[ps];
        p16 address_to high = rep ? m->rep_high : m->match_high;
        p32 a0 = xz_price0(e, choice);
        p32 a1 = xz_price1(e, choice);
        p32 b0 = a1 + xz_price0(e, choice2);
        p32 b1 = a1 + xz_price1(e, choice2);
        p32 size = e->len_table_size;
        p32 i;

        t->counters[ps] = size;
        for (i = 0; i < size && i < XZ_LEN_LOW; i++)
                t->prices[ps][i] = a0 + xz_tree_price(e, low, 3, i);
        for (; i < size && i < XZ_LEN_LOW + XZ_LEN_MID; i++)
                t->prices[ps][i] = b0 + xz_tree_price(e, mid, 3, i - XZ_LEN_LOW);
        for (; i < size; i++)
                t->prices[ps][i] = b1 + xz_tree_price(e, high, 8,
                                                      i - XZ_LEN_LOW - XZ_LEN_MID);
}

static fn xz_fill_dist_prices(xz_encoder address_to e)
{
        for (p32 ds = 0; ds < 4; ds++)
        {
                p32 address_to slot_prices = e->dist_slot_prices[ds];

                for (p32 slot = 0; slot < e->dist_table_size; slot++)
                        slot_prices[slot] = xz_tree_price(e, e->models.dist_slot[ds], 6, slot);
                for (p32 slot = 14; slot < e->dist_table_size; slot++)
                        slot_prices[slot] += (((slot >> 1) - 1) - 4) << 4;
                for (p32 i = 0; i < 4; i++)
                        e->dist_prices[ds][i] = slot_prices[i];
        }
        for (p32 i = 4; i < XZ_FULL_DIST; i++)
        {
                p32 slot = xz_slot(i);
                p32 footer = (slot >> 1) - 1;
                p32 base = (2 | (slot & 1)) << footer;
                p32 price = xz_reverse_price(e, e->models.dist_special + base - slot - 1,
                                             footer, i - base);

                for (p32 ds = 0; ds < 4; ds++)
                        e->dist_prices[ds][i] = price + e->dist_slot_prices[ds][slot];
        }
        e->match_price_count = 0;
}

static fn xz_fill_align_prices(xz_encoder address_to e)
{
        for (p32 i = 0; i < XZ_ALIGN; i++)
                e->align_prices[i] = xz_reverse_price(e, e->models.dist_align, 4, i);
        e->align_price_count = 0;
}

static inline INLINE p16 address_to xz_literal_probs(xz_encoder address_to e, p64 position,
                                                    p8 prev)
{
        return e->models.lit +
               0x300 * ((((p32)position & e->lp_mask) << e->lc) + ((p32)prev >> (8 - e->lc)));
}

static p32 xz_literal_price(xz_encoder address_to e, p64 position, p8 prev, bool matched,
                            p32 match_byte, p32 symbol)
{
        p16 address_to probs = xz_literal_probs(e, position, prev);

        if (!matched)
                return xz_tree_price(e, probs, 8, symbol);

        p32 price = 0;
        p32 offset = 0x100;

        symbol += 0x100;
        do
        {
                match_byte <<= 1;
                p32 match_bit = match_byte & offset;
                p32 index = offset + match_bit + (symbol >> 8);
                p32 bit = (symbol >> 7) & 1;

                price += xz_price(e, probs[index], bit);
                symbol <<= 1;
                offset &= ~(match_byte ^ symbol);
        } while (symbol < 0x10000);
        return price;
}

static inline INLINE p32 xz_short_rep_price(xz_encoder address_to e, p32 state, p32 ps)
{
        return xz_price0(e, e->models.is_rep0[state]) +
               xz_price0(e, e->models.is_rep0_long[state][ps]);
}

static inline INLINE p32 xz_pure_rep_price(xz_encoder address_to e, p32 rep, p32 state,
                                           p32 ps)
{
        xz_probability_state address_to m = address_of e->models;

        if (!rep)
                return xz_price0(e, m->is_rep0[state]) +
                       xz_price1(e, m->is_rep0_long[state][ps]);
        p32 price = xz_price1(e, m->is_rep0[state]);
        if (rep == 1)
                return price + xz_price0(e, m->is_rep1[state]);
        return price + xz_price1(e, m->is_rep1[state]) +
               xz_price(e, m->is_rep2[state], rep - 2);
}

static inline INLINE p32 xz_rep_price(xz_encoder address_to e, p32 rep, p32 len,
                                      p32 state, p32 ps)
{
        return e->rep_prices.prices[ps][len - 2] + xz_pure_rep_price(e, rep, state, ps);
}

static inline INLINE p32 xz_dist_len_price(xz_encoder address_to e, p32 dist, p32 len,
                                           p32 ps)
{
        p32 ds = len < 6 ? len - 2 : 3;
        p32 price = dist < XZ_FULL_DIST
                ? e->dist_prices[ds][dist]
                : e->dist_slot_prices[ds][xz_slot(dist)] + e->align_prices[dist & 15];

        return price + e->match_prices.prices[ps][len - 2];
}

/* Range coding into the chunk buffer. */
static inline INLINE fn xz_bit(xz_encoder address_to e, p16 address_to prob, p32 bit)
{
        lzma_range_encode(address_of e->rc, prob, bit, 0);
}

static fn xz_direct(xz_encoder address_to e, p32 value, p32 bits)
{
        while (bits)
        {
                bits--;
                e->rc.range >>= 1;
                if ((value >> bits) & 1)
                        e->rc.low += e->rc.range;
                while (e->rc.range < 0x1000000u)
                {
                        lzma_range_shift(address_of e->rc);
                        e->rc.range <<= 8;
                }
        }
}

static fn xz_length(xz_encoder address_to e, bool rep, p32 ps, p32 len)
{
        xz_probability_state address_to m = address_of e->models;
        p16 address_to choice = rep ? address_of m->rep_choice : address_of m->match_choice;
        p16 address_to choice2 = rep ? address_of m->rep_choice2 : address_of m->match_choice2;

        len -= 2;
        if (len < XZ_LEN_LOW)
        {
                xz_bit(e, choice, 0);
                lzma_range_encode(address_of e->rc, rep ? m->rep_low[ps] : m->match_low[ps],
                                  len, 3);
        }
        else
        {
                xz_bit(e, choice, 1);
                len -= XZ_LEN_LOW;
                if (len < XZ_LEN_MID)
                {
                        xz_bit(e, choice2, 0);
                        lzma_range_encode(address_of e->rc,
                                          rep ? m->rep_mid[ps] : m->match_mid[ps], len, 3);
                }
                else
                {
                        xz_bit(e, choice2, 1);
                        lzma_range_encode(address_of e->rc, rep ? m->rep_high : m->match_high,
                                          len - XZ_LEN_MID, 8);
                }
        }
        if (e->preset->normal)
        {
                xz_length_price address_to t = rep ? address_of e->rep_prices
                                                   : address_of e->match_prices;
                if (--t->counters[ps] == 0)
                        xz_length_prices(e, rep, ps);
        }
}

static fn xz_code_match(xz_encoder address_to e, p32 ps, p32 dist, p32 len)
{
        p32 slot = xz_slot(dist);

        e->state = e->state < 7 ? 7 : 10;
        xz_length(e, false, ps, len);
        lzma_range_encode(address_of e->rc, e->models.dist_slot[len < 6 ? len - 2 : 3],
                          slot, 6);
        if (slot >= 4)
        {
                p32 footer = (slot >> 1) - 1;
                p32 base = (2 | (slot & 1)) << footer;
                p32 reduced = dist - base;

                if (slot < 14)
                        lzma_range_encode(address_of e->rc,
                                          e->models.dist_special + base - slot - 1,
                                          reduced, 0x100 | footer);
                else
                {
                        xz_direct(e, reduced >> 4, footer - 4);
                        lzma_range_encode(address_of e->rc, e->models.dist_align,
                                          reduced & 15, 0x100 | 4);
                        e->align_price_count++;
                }
        }
        e->reps[3] = e->reps[2];
        e->reps[2] = e->reps[1];
        e->reps[1] = e->reps[0];
        e->reps[0] = dist;
        e->match_price_count++;
}

static fn xz_code_rep(xz_encoder address_to e, p32 ps, p32 rep, p32 len)
{
        xz_probability_state address_to m = address_of e->models;
        p8 state = e->state;

        if (!rep)
        {
                xz_bit(e, address_of m->is_rep0[state], 0);
                xz_bit(e, address_of m->is_rep0_long[state][ps], len != 1);
        }
        else
        {
                p32 distance = e->reps[rep];

                xz_bit(e, address_of m->is_rep0[state], 1);
                if (rep == 1)
                        xz_bit(e, address_of m->is_rep1[state], 0);
                else
                {
                        xz_bit(e, address_of m->is_rep1[state], 1);
                        xz_bit(e, address_of m->is_rep2[state], rep - 2);
                        if (rep == 3)
                                e->reps[3] = e->reps[2];
                        e->reps[2] = e->reps[1];
                }
                e->reps[1] = e->reps[0];
                e->reps[0] = distance;
        }
        if (len == 1)
                e->state = state < 7 ? 9 : 11;
        else
        {
                xz_length(e, true, ps, len);
                e->state = state < 7 ? 8 : 11;
        }
}

static fn xz_code_symbol(xz_encoder address_to e, p32 back, p32 len)
{
        xz_probability_state address_to m = address_of e->models;
        p32 ps = (p32)e->position & e->pos_mask;
        p8 state = e->state;

        if (back == XZ_LITERAL)
        {
                p8 address_to at = e->input + e->read_pos - e->read_ahead;
                p16 address_to probs = xz_literal_probs(e, e->position, at[-1]);

                xz_bit(e, address_of m->is_match[state][ps], 0);
                if (state < 7)
                {
                        e->state = state < 4 ? 0 : state - 3;
                        lzma_range_encode(address_of e->rc, probs, at[0], 8);
                }
                else
                {
                        p8 match = at[-(bipolar)e->reps[0] - 1];

                        e->state = state < 10 ? state - 3 : state - 6;
                        lzma_range_encode(address_of e->rc, probs, at[0],
                                          0x200 | 8 | ((positive)match << 16));
                }
        }
        else
        {
                xz_bit(e, address_of m->is_match[state][ps], 1);
                if (back < 4)
                {
                        xz_bit(e, address_of m->is_rep[state], 1);
                        xz_code_rep(e, ps, back, len);
                }
                else
                {
                        xz_bit(e, address_of m->is_rep[state], 0);
                        xz_code_match(e, ps, back - 4, len);
                }
        }
        e->read_ahead -= len;
        e->position += len;
}

/* Match finders. A candidate at distance delta (1-based) is stored as
   dist = delta - 1. Word compares read up to seven bytes past limit; the
   block's slack keeps that readable and the result is clamped. */
static inline INLINE p32 xz_common(p8 address_to a, p8 address_to b, p32 len, p32 limit)
{
        while (len < limit)
        {
                p64 x = memory_load_unaligned(p64, a + len) ^
                        memory_load_unaligned(p64, b + len);

                if (x)
                {
                        len += xz_low_bit(x) >> 3;
                        return len < limit ? len : limit;
                }
                len += 8;
        }
        return limit;
}

static inline INLINE bool xz_differ16(p8 address_to a, p8 address_to b)
{
        return memory_load_unaligned(p16, a) != memory_load_unaligned(p16, b);
}

static inline INLINE fn xz_move(xz_encoder address_to e)
{
        if (++e->cyclic_pos == e->cyclic_size)
                e->cyclic_pos = 0;
        e->read_pos++;
}

/* The two- and three-byte hashes keep the byte-table form: once the first
   byte of a candidate is known equal, an equal hash proves the second (and
   third) equal too, so those candidates start their compare past them. */
static inline INLINE p32 xz_hash_head(p8 address_to cur)
{
        return hash_crc32_tab[cur[0]] ^ cur[1];
}

static xz_found address_to xz_chain(xz_encoder address_to e, p32 len_limit, p32 pos,
                                    p8 address_to cur, p32 cur_match,
                                    xz_found address_to matches, p32 len_best)
{
        p32 address_to son = e->son;
        p32 cyclic_pos = e->cyclic_pos;
        p32 cyclic_size = e->cyclic_size;
        p32 depth = e->depth;

        son[cyclic_pos] = cur_match;
        for (;;)
        {
                p32 delta = pos - cur_match;

                if (depth-- == 0 || delta >= cyclic_size)
                        return matches;
                p8 address_to pb = cur - delta;
                cur_match = son[cyclic_pos - delta + (delta > cyclic_pos ? cyclic_size : 0)];
                if (pb[len_best] == cur[len_best] && pb[0] == cur[0])
                {
                        p32 len = xz_common(pb, cur, 1, len_limit);

                        if (len_best < len)
                        {
                                len_best = len;
                                matches->len = len;
                                matches->dist = delta - 1;
                                matches++;
                                if (len == len_limit)
                                        return matches;
                        }
                }
        }
}

static xz_found address_to xz_tree(xz_encoder address_to e, p32 len_limit, p32 pos,
                                   p8 address_to cur, p32 cur_match,
                                   xz_found address_to matches, p32 len_best, bool find)
{
        p32 address_to son = e->son;
        p32 cyclic_pos = e->cyclic_pos;
        p32 cyclic_size = e->cyclic_size;
        p32 depth = e->depth;
        p32 address_to ptr0 = son + ((positive)cyclic_pos << 1) + 1;
        p32 address_to ptr1 = son + ((positive)cyclic_pos << 1);
        p32 len0 = 0;
        p32 len1 = 0;

        for (;;)
        {
                p32 delta = pos - cur_match;

                if (depth-- == 0 || delta >= cyclic_size)
                {
                        address_to ptr0 = 0;
                        address_to ptr1 = 0;
                        return matches;
                }
                p32 address_to pair = son + ((positive)(cyclic_pos - delta +
                                            (delta > cyclic_pos ? cyclic_size : 0)) << 1);
                p8 address_to pb = cur - delta;
                p32 len = len0 < len1 ? len0 : len1;

                if (pb[len] == cur[len])
                {
                        len = xz_common(pb, cur, len + 1, len_limit);
                        if (len_best < len)
                        {
                                len_best = len;
                                if (find)
                                {
                                        matches->len = len;
                                        matches->dist = delta - 1;
                                        matches++;
                                }
                        }
                        if (len == len_limit)
                        {
                                address_to ptr1 = pair[0];
                                address_to ptr0 = pair[1];
                                return matches;
                        }
                }
                if (pb[len] < cur[len])
                {
                        address_to ptr1 = cur_match;
                        ptr1 = pair + 1;
                        cur_match = address_to ptr1;
                        len1 = len;
                }
                else
                {
                        address_to ptr0 = cur_match;
                        ptr0 = pair;
                        cur_match = address_to ptr0;
                        len0 = len;
                }
        }
}

/* One find at read_pos: the matches in increasing length, their count. */
static p32 xz_finder_find(xz_encoder address_to e)
{
        const xz_preset address_to p = e->preset;
        p32 avail = e->input_n - e->read_pos;
        p32 len_min = p->finder == XZ_FINDER_HC3 ? 3 : 4;
        p32 len_limit = e->nice;

        if (avail < len_limit)
        {
                if (avail < len_min)
                {
                        e->read_pos++;
                        return 0;
                }
                len_limit = avail;
        }

        p8 address_to cur = e->input + e->read_pos;
        p32 pos = e->read_pos + e->offset;
        p32 address_to hash = e->hash;
        xz_found address_to matches = e->matches;
        p32 count = 0;
        p32 temp = xz_hash_head(cur);
        p32 h2 = temp & (XZ_HASH2_SIZE - 1);
        p32 delta2 = pos - hash[h2];

        hash[h2] = pos;
        temp ^= (p32)cur[2] << 8;
        if (p->finder == XZ_FINDER_HC3)
        {
                p32 hv = XZ_HASH2_SIZE + (temp & e->hash_mask);
                p32 cur_match = hash[hv];
                p32 len_best = 2;

                hash[hv] = pos;
                if (delta2 < e->cyclic_size && *(cur - delta2) == *cur)
                {
                        len_best = xz_common(cur - delta2, cur, 2, len_limit);
                        matches[0].len = len_best;
                        matches[0].dist = delta2 - 1;
                        count = 1;
                        if (len_best == len_limit)
                        {
                                e->son[e->cyclic_pos] = cur_match;
                                xz_move(e);
                                return 1;
                        }
                }
                count = (p32)(xz_chain(e, len_limit, pos, cur, cur_match, matches + count,
                                       len_best) - matches);
                xz_move(e);
                return count;
        }

        p32 h3 = XZ_HASH2_SIZE + (temp & (XZ_HASH3_SIZE - 1));
        p32 h4 = XZ_HASH2_SIZE + XZ_HASH3_SIZE +
                 ((temp ^ (hash_crc32_tab[cur[3]] << 5)) & e->hash_mask);
        p32 delta3 = pos - hash[h3];
        p32 cur_match = hash[h4];
        p32 len_best = 1;

        hash[h3] = pos;
        hash[h4] = pos;
        if (delta2 < e->cyclic_size && *(cur - delta2) == *cur)
        {
                len_best = 2;
                matches[0].len = 2;
                matches[0].dist = delta2 - 1;
                count = 1;
        }
        if (delta2 != delta3 && delta3 < e->cyclic_size && *(cur - delta3) == *cur)
        {
                len_best = 3;
                matches[count++].dist = delta3 - 1;
                delta2 = delta3;
        }
        bool tree = p->finder == XZ_FINDER_BT4;
        if (count)
        {
                len_best = xz_common(cur - delta2, cur, len_best, len_limit);
                matches[count - 1].len = len_best;
                if (len_best == len_limit)
                {
                        if (tree)
                                xz_tree(e, len_limit, pos, cur, cur_match, matches, len_best,
                                        false);
                        else
                                e->son[e->cyclic_pos] = cur_match;
                        xz_move(e);
                        return count;
                }
        }
        if (len_best < 3)
                len_best = 3;
        count = (p32)((tree ? xz_tree(e, len_limit, pos, cur, cur_match, matches + count,
                                      len_best, true)
                            : xz_chain(e, len_limit, pos, cur, cur_match, matches + count,
                                       len_best)) - matches);
        xz_move(e);
        return count;
}

static fn xz_finder_skip(xz_encoder address_to e, p32 amount)
{
        const xz_preset address_to p = e->preset;
        p32 len_min = p->finder == XZ_FINDER_HC3 ? 3 : 4;

        while (amount--)
        {
                p32 avail = e->input_n - e->read_pos;

                if (avail < len_min)
                {
                        e->read_pos++;
                        continue;
                }
                p8 address_to cur = e->input + e->read_pos;
                p32 pos = e->read_pos + e->offset;
                p32 address_to hash = e->hash;
                p32 temp = xz_hash_head(cur);
                p32 cur_match;

                hash[temp & (XZ_HASH2_SIZE - 1)] = pos;
                temp ^= (p32)cur[2] << 8;
                if (p->finder == XZ_FINDER_HC3)
                {
                        p32 hv = XZ_HASH2_SIZE + (temp & e->hash_mask);
                        cur_match = hash[hv];
                        hash[hv] = pos;
                }
                else
                {
                        p32 h4 = XZ_HASH2_SIZE + XZ_HASH3_SIZE +
                                 ((temp ^ (hash_crc32_tab[cur[3]] << 5)) & e->hash_mask);
                        hash[XZ_HASH2_SIZE + (temp & (XZ_HASH3_SIZE - 1))] = pos;
                        cur_match = hash[h4];
                        hash[h4] = pos;
                }
                if (p->finder == XZ_FINDER_BT4)
                        xz_tree(e, avail < e->nice ? avail : e->nice, pos, cur, cur_match,
                                e->matches, 0, false);
                else
                        e->son[e->cyclic_pos] = cur_match;
                xz_move(e);
        }
}

static p32 xz_find(xz_encoder address_to e, p32 address_to count_out)
{
        p32 count = xz_finder_find(e);
        p32 len_best = 0;

        if (count)
        {
                len_best = e->matches[count - 1].len;
                if (len_best == e->nice)
                {
                        p32 limit = e->input_n - e->read_pos + 1;
                        p8 address_to p1 = e->input + e->read_pos - 1;

                        if (limit > XZ_MATCH_MAX)
                                limit = XZ_MATCH_MAX;
                        len_best = xz_common(p1, p1 - e->matches[count - 1].dist - 1,
                                             len_best, limit);
                }
        }
        address_to count_out = count;
        e->read_ahead++;
        return len_best;
}

static fn xz_skip(xz_encoder address_to e, p32 amount)
{
        if (amount)
        {
                xz_finder_skip(e, amount);
                e->read_ahead += amount;
        }
}

static inline INLINE fn xz_make_literal(xz_optimal address_to o)
{
        o->back_prev = XZ_LITERAL;
        o->prev_1_is_literal = false;
}

static inline INLINE fn xz_make_short_rep(xz_optimal address_to o)
{
        o->back_prev = 0;
        o->prev_1_is_literal = false;
}

/* Levels 0-3: the longest match, repeats preferred by a distance rule and
   a one-byte lazy look. */
static fn xz_optimum_fast(xz_encoder address_to e, p32 address_to back_res,
                          p32 address_to len_res)
{
        p32 nice = e->nice;
        p32 len_main;
        p32 count;

        if (!e->read_ahead)
                len_main = xz_find(e, address_of count);
        else
        {
                len_main = e->longest;
                count = e->match_count;
        }

        p8 address_to buf = e->input + e->read_pos - 1;
        p32 buf_avail = e->input_n - e->read_pos + 1;

        if (buf_avail > XZ_MATCH_MAX)
                buf_avail = XZ_MATCH_MAX;
        address_to back_res = XZ_LITERAL;
        address_to len_res = 1;
        if (buf_avail < 2)
                return;

        p32 rep_len = 0;
        p32 rep_index = 0;

        for (p32 i = 0; i < 4; i++)
        {
                p8 address_to back = buf - e->reps[i] - 1;

                if (xz_differ16(buf, back))
                        continue;
                p32 len = xz_common(buf, back, 2, buf_avail);
                if (len >= nice)
                {
                        address_to back_res = i;
                        address_to len_res = len;
                        xz_skip(e, len - 1);
                        return;
                }
                if (len > rep_len)
                {
                        rep_index = i;
                        rep_len = len;
                }
        }
        if (len_main >= nice)
        {
                address_to back_res = e->matches[count - 1].dist + 4;
                address_to len_res = len_main;
                xz_skip(e, len_main - 1);
                return;
        }

        p32 back_main = 0;

        if (len_main >= 2)
        {
                back_main = e->matches[count - 1].dist;
                while (count > 1 && len_main == e->matches[count - 2].len + 1)
                {
                        if (!XZ_CHANGE_PAIR(e->matches[count - 2].dist, back_main))
                                break;
                        count--;
                        len_main = e->matches[count - 1].len;
                        back_main = e->matches[count - 1].dist;
                }
                if (len_main == 2 && back_main >= 0x80)
                        len_main = 1;
        }
        if (rep_len >= 2 &&
            (rep_len + 1 >= len_main ||
             (rep_len + 2 >= len_main && back_main > (1u << 9)) ||
             (rep_len + 3 >= len_main && back_main > (1u << 15))))
        {
                address_to back_res = rep_index;
                address_to len_res = rep_len;
                xz_skip(e, rep_len - 1);
                return;
        }
        if (len_main < 2 || buf_avail <= 2)
                return;

        e->longest = xz_find(e, address_of e->match_count);
        if (e->longest >= 2)
        {
                p32 new_dist = e->matches[e->match_count - 1].dist;

                if ((e->longest >= len_main && new_dist < back_main) ||
                    (e->longest == len_main + 1 && !XZ_CHANGE_PAIR(back_main, new_dist)) ||
                    e->longest > len_main + 1 ||
                    (e->longest + 1 >= len_main && len_main >= 3 &&
                     XZ_CHANGE_PAIR(new_dist, back_main)))
                        return;
        }
        buf++;
        p32 limit = len_main - 1 > 2 ? len_main - 1 : 2;
        for (p32 i = 0; i < 4; i++)
                if (xz_common(buf, buf - e->reps[i] - 1, 0, limit) == limit)
                        return;
        address_to back_res = back_main + 4;
        address_to len_res = len_main;
        xz_skip(e, len_main - 2);
}

static fn xz_backward(xz_encoder address_to e, p32 address_to len_res, p32 address_to back_res,
                      p32 cur)
{
        xz_optimal address_to opts = e->opts;
        p32 pos_mem = opts[cur].pos_prev;
        p32 back_mem = opts[cur].back_prev;

        e->opts_end = cur;
        do
        {
                if (opts[cur].prev_1_is_literal)
                {
                        xz_make_literal(opts + pos_mem);
                        opts[pos_mem].pos_prev = pos_mem - 1;
                        if (opts[cur].prev_2)
                        {
                                opts[pos_mem - 1].prev_1_is_literal = false;
                                opts[pos_mem - 1].pos_prev = opts[cur].pos_prev_2;
                                opts[pos_mem - 1].back_prev = opts[cur].back_prev_2;
                        }
                }
                p32 pos_prev = pos_mem;
                p32 back_cur = back_mem;

                back_mem = opts[pos_prev].back_prev;
                pos_mem = opts[pos_prev].pos_prev;
                opts[pos_prev].back_prev = back_cur;
                opts[pos_prev].pos_prev = cur;
                cur = pos_prev;
        } while (cur);
        e->opts_current = opts[0].pos_prev;
        address_to len_res = opts[0].pos_prev;
        address_to back_res = opts[0].back_prev;
}

static p32 xz_optimum_first(xz_encoder address_to e, p32 address_to back_res,
                            p32 address_to len_res, p32 position)
{
        xz_probability_state address_to m = address_of e->models;
        xz_optimal address_to opts = e->opts;
        p32 nice = e->nice;
        p32 len_main;
        p32 count;

        if (!e->read_ahead)
                len_main = xz_find(e, address_of count);
        else
        {
                len_main = e->longest;
                count = e->match_count;
        }

        p32 buf_avail = e->input_n - e->read_pos + 1;

        if (buf_avail > XZ_MATCH_MAX)
                buf_avail = XZ_MATCH_MAX;
        address_to back_res = XZ_LITERAL;
        address_to len_res = 1;
        if (buf_avail < 2)
                return XZ_LITERAL;

        p8 address_to buf = e->input + e->read_pos - 1;
        p32 rep_lens[4];
        p32 rep_max = 0;

        for (p32 i = 0; i < 4; i++)
        {
                p8 address_to back = buf - e->reps[i] - 1;

                if (xz_differ16(buf, back))
                {
                        rep_lens[i] = 0;
                        continue;
                }
                rep_lens[i] = xz_common(buf, back, 2, buf_avail);
                if (rep_lens[i] > rep_lens[rep_max])
                        rep_max = i;
        }
        if (rep_lens[rep_max] >= nice)
        {
                address_to back_res = rep_max;
                address_to len_res = rep_lens[rep_max];
                xz_skip(e, address_to len_res - 1);
                return XZ_LITERAL;
        }
        if (len_main >= nice)
        {
                address_to back_res = e->matches[count - 1].dist + 4;
                address_to len_res = len_main;
                xz_skip(e, len_main - 1);
                return XZ_LITERAL;
        }

        p8 current = buf[0];
        p8 match_byte = *(buf - e->reps[0] - 1);
        p32 state = e->state;

        if (len_main < 2 && current != match_byte && rep_lens[rep_max] < 2)
                return XZ_LITERAL;

        opts[0].state = e->state;
        p32 ps = position & e->pos_mask;

        opts[1].price = xz_price0(e, m->is_match[state][ps]) +
                        xz_literal_price(e, position, buf[-1], state >= 7, match_byte,
                                         current);
        xz_make_literal(opts + 1);

        p32 match_price = xz_price1(e, m->is_match[state][ps]);
        p32 rep_match_price = match_price + xz_price1(e, m->is_rep[state]);

        if (match_byte == current)
        {
                p32 short_rep_price = rep_match_price + xz_short_rep_price(e, state, ps);

                if (short_rep_price < opts[1].price)
                {
                        opts[1].price = short_rep_price;
                        xz_make_short_rep(opts + 1);
                }
        }

        p32 len_end = len_main > rep_lens[rep_max] ? len_main : rep_lens[rep_max];

        if (len_end < 2)
        {
                address_to back_res = opts[1].back_prev;
                address_to len_res = 1;
                return XZ_LITERAL;
        }
        opts[1].pos_prev = 0;
        for (p32 i = 0; i < 4; i++)
                opts[0].backs[i] = e->reps[i];

        p32 len = len_end;
        do
                opts[len].price = XZ_INFINITY_PRICE;
        while (--len >= 2);

        for (p32 i = 0; i < 4; i++)
        {
                p32 rep_len = rep_lens[i];

                if (rep_len < 2)
                        continue;
                p32 price = rep_match_price + xz_pure_rep_price(e, i, state, ps);
                do
                {
                        p32 cost = price + e->rep_prices.prices[ps][rep_len - 2];

                        if (cost < opts[rep_len].price)
                        {
                                opts[rep_len].price = cost;
                                opts[rep_len].pos_prev = 0;
                                opts[rep_len].back_prev = i;
                                opts[rep_len].prev_1_is_literal = false;
                        }
                } while (--rep_len >= 2);
        }

        p32 normal_match_price = match_price + xz_price0(e, m->is_rep[state]);

        len = rep_lens[0] >= 2 ? rep_lens[0] + 1 : 2;
        if (len <= len_main)
        {
                p32 i = 0;

                while (len > e->matches[i].len)
                        i++;
                for (;; len++)
                {
                        p32 dist = e->matches[i].dist;
                        p32 cost = normal_match_price + xz_dist_len_price(e, dist, len, ps);

                        if (cost < opts[len].price)
                        {
                                opts[len].price = cost;
                                opts[len].pos_prev = 0;
                                opts[len].back_prev = dist + 4;
                                opts[len].prev_1_is_literal = false;
                        }
                        if (len == e->matches[i].len && ++i == count)
                                break;
                }
        }
        return len_end;
}

static inline INLINE p8 xz_after_literal(p8 state)
{
        return state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
}

static inline INLINE p8 xz_after_match(p8 state)
{
        return state < 7 ? 7 : 10;
}

static inline INLINE p8 xz_after_long_rep(p8 state)
{
        return state < 7 ? 8 : 11;
}

static inline INLINE p8 xz_after_short_rep(p8 state)
{
        return state < 7 ? 9 : 11;
}

static p32 xz_optimum_next(xz_encoder address_to e, p32 address_to reps, p8 address_to buf,
                           p32 len_end, p32 position, p32 cur, p32 nice,
                           p32 buf_avail_full)
{
        xz_probability_state address_to m = address_of e->models;
        xz_optimal address_to opts = e->opts;
        p32 count = e->match_count;
        p32 new_len = e->longest;
        p32 pos_prev = opts[cur].pos_prev;
        p8 state;

        if (opts[cur].prev_1_is_literal)
        {
                pos_prev--;
                if (opts[cur].prev_2)
                {
                        state = opts[opts[cur].pos_prev_2].state;
                        state = opts[cur].back_prev_2 < 4 ? xz_after_long_rep(state)
                                                          : xz_after_match(state);
                }
                else
                        state = opts[pos_prev].state;
                state = xz_after_literal(state);
        }
        else
                state = opts[pos_prev].state;

        if (pos_prev == cur - 1)
        {
                state = opts[cur].back_prev == 0 ? xz_after_short_rep(state)
                                                 : xz_after_literal(state);
        }
        else
        {
                p32 pos;

                if (opts[cur].prev_1_is_literal && opts[cur].prev_2)
                {
                        pos_prev = opts[cur].pos_prev_2;
                        pos = opts[cur].back_prev_2;
                        state = xz_after_long_rep(state);
                }
                else
                {
                        pos = opts[cur].back_prev;
                        state = pos < 4 ? xz_after_long_rep(state) : xz_after_match(state);
                }
                if (pos < 4)
                {
                        p32 i;

                        reps[0] = opts[pos_prev].backs[pos];
                        for (i = 1; i <= pos; i++)
                                reps[i] = opts[pos_prev].backs[i - 1];
                        for (; i < 4; i++)
                                reps[i] = opts[pos_prev].backs[i];
                }
                else
                {
                        reps[0] = pos - 4;
                        for (p32 i = 1; i < 4; i++)
                                reps[i] = opts[pos_prev].backs[i - 1];
                }
        }
        opts[cur].state = state;
        for (p32 i = 0; i < 4; i++)
                opts[cur].backs[i] = reps[i];

        p32 cur_price = opts[cur].price;
        p8 current = buf[0];
        p8 match_byte = *(buf - reps[0] - 1);
        p32 ps = position & e->pos_mask;
        p32 cur_and_1_price = cur_price + xz_price0(e, m->is_match[state][ps]) +
                              xz_literal_price(e, position, buf[-1], state >= 7, match_byte,
                                               current);
        bool next_is_literal = false;

        if (cur_and_1_price < opts[cur + 1].price)
        {
                opts[cur + 1].price = cur_and_1_price;
                opts[cur + 1].pos_prev = cur;
                xz_make_literal(opts + cur + 1);
                next_is_literal = true;
        }

        p32 match_price = cur_price + xz_price1(e, m->is_match[state][ps]);
        p32 rep_match_price = match_price + xz_price1(e, m->is_rep[state]);

        if (match_byte == current &&
            !(opts[cur + 1].pos_prev < cur && opts[cur + 1].back_prev == 0))
        {
                p32 short_rep_price = rep_match_price + xz_short_rep_price(e, state, ps);

                if (short_rep_price <= opts[cur + 1].price)
                {
                        opts[cur + 1].price = short_rep_price;
                        opts[cur + 1].pos_prev = cur;
                        xz_make_short_rep(opts + cur + 1);
                        next_is_literal = true;
                }
        }
        if (buf_avail_full < 2)
                return len_end;

        p32 buf_avail = buf_avail_full < nice ? buf_avail_full : nice;

        if (!next_is_literal && match_byte != current)
        {
                /* Literal, then repeat 0. */
                p8 address_to back = buf - reps[0] - 1;
                p32 limit = buf_avail_full < nice + 1 ? buf_avail_full : nice + 1;
                p32 len_test = xz_common(buf, back, 1, limit) - 1;

                if (len_test >= 2)
                {
                        p8 state_2 = xz_after_literal(state);
                        p32 ps_next = (position + 1) & e->pos_mask;
                        p32 next_rep_match_price = cur_and_1_price +
                                xz_price1(e, m->is_match[state_2][ps_next]) +
                                xz_price1(e, m->is_rep[state_2]);
                        p32 offset = cur + 1 + len_test;

                        while (len_end < offset)
                                opts[++len_end].price = XZ_INFINITY_PRICE;
                        p32 cost = next_rep_match_price +
                                   xz_rep_price(e, 0, len_test, state_2, ps_next);
                        if (cost < opts[offset].price)
                        {
                                opts[offset].price = cost;
                                opts[offset].pos_prev = cur + 1;
                                opts[offset].back_prev = 0;
                                opts[offset].prev_1_is_literal = true;
                                opts[offset].prev_2 = false;
                        }
                }
        }

        p32 start_len = 2;

        for (p32 rep_index = 0; rep_index < 4; rep_index++)
        {
                p8 address_to back = buf - reps[rep_index] - 1;

                if (xz_differ16(buf, back))
                        continue;
                p32 len_test = xz_common(buf, back, 2, buf_avail);

                while (len_end < cur + len_test)
                        opts[++len_end].price = XZ_INFINITY_PRICE;

                p32 len_test_temp = len_test;
                p32 price = rep_match_price + xz_pure_rep_price(e, rep_index, state, ps);

                do
                {
                        p32 cost = price + e->rep_prices.prices[ps][len_test - 2];

                        if (cost < opts[cur + len_test].price)
                        {
                                opts[cur + len_test].price = cost;
                                opts[cur + len_test].pos_prev = cur;
                                opts[cur + len_test].back_prev = rep_index;
                                opts[cur + len_test].prev_1_is_literal = false;
                        }
                } while (--len_test >= 2);
                len_test = len_test_temp;
                if (!rep_index)
                        start_len = len_test + 1;

                /* Repeat, literal, repeat 0. */
                p32 len_test_2 = len_test + 1;
                p32 limit = buf_avail_full < len_test_2 + nice ? buf_avail_full
                                                               : len_test_2 + nice;
                if (len_test_2 < limit)
                        len_test_2 = xz_common(buf, back, len_test_2, limit);
                len_test_2 -= len_test + 1;
                if (len_test_2 >= 2)
                {
                        p8 state_2 = xz_after_long_rep(state);
                        p32 ps_next = (position + len_test) & e->pos_mask;
                        p32 cost_literal = price + e->rep_prices.prices[ps][len_test - 2] +
                                xz_price0(e, m->is_match[state_2][ps_next]) +
                                xz_literal_price(e, position + len_test, buf[len_test - 1],
                                                 true, back[len_test], buf[len_test]);

                        state_2 = xz_after_literal(state_2);
                        ps_next = (position + len_test + 1) & e->pos_mask;
                        p32 next_rep_match_price = cost_literal +
                                xz_price1(e, m->is_match[state_2][ps_next]) +
                                xz_price1(e, m->is_rep[state_2]);
                        p32 offset = cur + len_test + 1 + len_test_2;

                        while (len_end < offset)
                                opts[++len_end].price = XZ_INFINITY_PRICE;
                        p32 cost = next_rep_match_price +
                                   xz_rep_price(e, 0, len_test_2, state_2, ps_next);
                        if (cost < opts[offset].price)
                        {
                                opts[offset].price = cost;
                                opts[offset].pos_prev = cur + len_test + 1;
                                opts[offset].back_prev = 0;
                                opts[offset].prev_1_is_literal = true;
                                opts[offset].prev_2 = true;
                                opts[offset].pos_prev_2 = cur;
                                opts[offset].back_prev_2 = rep_index;
                        }
                }
        }

        if (new_len > buf_avail)
        {
                new_len = buf_avail;
                count = 0;
                while (new_len > e->matches[count].len)
                        count++;
                e->matches[count++].len = new_len;
        }
        if (new_len < start_len)
                return len_end;

        p32 normal_match_price = match_price + xz_price0(e, m->is_rep[state]);

        while (len_end < cur + new_len)
                opts[++len_end].price = XZ_INFINITY_PRICE;

        p32 i = 0;

        while (start_len > e->matches[i].len)
                i++;
        for (p32 len_test = start_len;; len_test++)
        {
                p32 cur_back = e->matches[i].dist;
                p32 cost = normal_match_price + xz_dist_len_price(e, cur_back, len_test, ps);

                if (cost < opts[cur + len_test].price)
                {
                        opts[cur + len_test].price = cost;
                        opts[cur + len_test].pos_prev = cur;
                        opts[cur + len_test].back_prev = cur_back + 4;
                        opts[cur + len_test].prev_1_is_literal = false;
                }
                if (len_test != e->matches[i].len)
                        continue;

                /* Match, literal, repeat 0. */
                p8 address_to back = buf - cur_back - 1;
                p32 len_test_2 = len_test + 1;
                p32 limit = buf_avail_full < len_test_2 + nice ? buf_avail_full
                                                               : len_test_2 + nice;

                if (len_test_2 < limit)
                        len_test_2 = xz_common(buf, back, len_test_2, limit);
                len_test_2 -= len_test + 1;
                if (len_test_2 >= 2)
                {
                        p8 state_2 = xz_after_match(state);
                        p32 ps_next = (position + len_test) & e->pos_mask;
                        p32 cost_literal = cost +
                                xz_price0(e, m->is_match[state_2][ps_next]) +
                                xz_literal_price(e, position + len_test, buf[len_test - 1],
                                                 true, back[len_test], buf[len_test]);

                        state_2 = xz_after_literal(state_2);
                        ps_next = (ps_next + 1) & e->pos_mask;
                        p32 next_rep_match_price = cost_literal +
                                xz_price1(e, m->is_match[state_2][ps_next]) +
                                xz_price1(e, m->is_rep[state_2]);
                        p32 offset = cur + len_test + 1 + len_test_2;

                        while (len_end < offset)
                                opts[++len_end].price = XZ_INFINITY_PRICE;
                        p32 cost_2 = next_rep_match_price +
                                     xz_rep_price(e, 0, len_test_2, state_2, ps_next);
                        if (cost_2 < opts[offset].price)
                        {
                                opts[offset].price = cost_2;
                                opts[offset].pos_prev = cur + len_test + 1;
                                opts[offset].back_prev = 0;
                                opts[offset].prev_1_is_literal = true;
                                opts[offset].prev_2 = true;
                                opts[offset].pos_prev_2 = cur;
                                opts[offset].back_prev_2 = cur_back + 4;
                        }
                }
                if (++i == count)
                        break;
        }
        return len_end;
}

/* Levels 4-9: price every way to cover the next positions, up to nice
   length or 4096 positions, and code the cheapest path. */
static fn xz_optimum_normal(xz_encoder address_to e, p32 address_to back_res,
                            p32 address_to len_res, p32 position)
{
        xz_optimal address_to opts = e->opts;

        if (e->opts_end != e->opts_current)
        {
                p32 at = e->opts_current;

                address_to len_res = opts[at].pos_prev - at;
                address_to back_res = opts[at].back_prev;
                e->opts_current = opts[at].pos_prev;
                return;
        }
        if (!e->read_ahead)
        {
                if (e->match_price_count >= (1u << 7))
                        xz_fill_dist_prices(e);
                if (e->align_price_count >= XZ_ALIGN)
                        xz_fill_align_prices(e);
        }

        p32 len_end = xz_optimum_first(e, back_res, len_res, position);

        if (len_end == XZ_LITERAL)
                return;

        p32 reps[4] = {e->reps[0], e->reps[1], e->reps[2], e->reps[3]};
        p32 cur;

        for (cur = 1; cur < len_end; cur++)
        {
                e->longest = xz_find(e, address_of e->match_count);
                if (e->longest >= e->nice)
                        break;
                p32 avail = e->input_n - e->read_pos + 1;
                if (avail > XZ_OPTS - 1 - cur)
                        avail = XZ_OPTS - 1 - cur;
                len_end = xz_optimum_next(e, reps, e->input + e->read_pos - 1, len_end,
                                          position + cur, cur, e->nice, avail);
        }
        xz_backward(e, len_res, back_res, cur);
}

static fn xz_lzma_reset(xz_encoder address_to e)
{
        p16 address_to cell = (p16 address_to)address_of e->models;

        for (positive i = 0; i < sizeof(e->models) / sizeof(p16); i++)
                cell[i] = 1024;
        e->state = 0;
        e->reps[0] = e->reps[1] = e->reps[2] = e->reps[3] = 0;
        e->match_price_count = 0x7fffffffu;
        e->align_price_count = 0x7fffffffu;
        e->opts_end = 0;
        e->opts_current = 0;
        if (e->preset->normal)
                for (p32 ps = 0; ps <= e->pos_mask; ps++)
                {
                        xz_length_prices(e, false, ps);
                        xz_length_prices(e, true, ps);
                }
}

/* Size the dictionary to the block, clear the match finder, and make sure
   the output span holds the block's worst case. */
static bool xz_block_prepare(xz_encoder address_to e, p32 n)
{
        const xz_preset address_to p = e->preset;
        p32 dict = (p32)1 << p->dict_log;
        bool fresh;

        while (dict > 4096 && dict / 2 >= n)
                dict >>= 1;
        e->dict = dict;
        e->cyclic_size = dict + 1;
        e->cyclic_pos = 0;
        e->read_pos = 0;
        e->read_ahead = 0;
        e->offset = e->cyclic_size;

        p32 hs = dict - 1;

        hs |= hs >> 1;
        hs |= hs >> 2;
        hs |= hs >> 4;
        hs |= hs >> 8;
        hs |= hs >> 16;
        hs >>= 1;
        hs |= 0xffff;
        if (hs > (1u << 24))
                hs = p->finder == XZ_FINDER_HC3 ? (1u << 24) - 1 : hs >> 1;
        e->hash_mask = hs;

        positive entries = (positive)hs + 1 + XZ_HASH2_SIZE +
                           (p->finder == XZ_FINDER_HC3 ? 0 : XZ_HASH3_SIZE);
        positive sons = (positive)e->cyclic_size * (p->finder == XZ_FINDER_BT4 ? 2 : 1);
        p8 address_to area = (p8 address_to)e->hash;

        if (!xz_area(address_of area, address_of e->hash_room, entries * sizeof(p32),
                     address_of fresh))
                return false;
        e->hash = (p32 address_to)area;
        if (!fresh)
                memory_fill(e->hash, 0, entries * sizeof(p32));
        area = (p8 address_to)e->son;
        if (!xz_area(address_of area, address_of e->son_room, sons * sizeof(p32),
                     address_of fresh))
                return false;
        e->son = (p32 address_to)area;
        if (!xz_area(address_of e->out, address_of e->out_room,
                     XZ_BLOCK_HEADER_MAX + (positive)n + (n >> 12) +
                             2 * XZ_CHUNK_PACKED_MAX + 64,
                     address_of fresh))
                return false;

        e->nice = p->nice;
        e->depth = p->depth ? p->depth
                 : p->finder == XZ_FINDER_BT4 ? 16 + e->nice / 2 : 4 + e->nice / 4;
        e->dist_table_size = 2 * xz_top_bit(dict);
        e->len_table_size = e->nice + 1 - 2;
        return true;
}

/* Encode input[0, n) as one complete block: header with both sizes, LZMA2
   chunks and end marker, padding, CRC32. The input must stay readable for
   XZ_SLACK bytes past n; what those bytes hold never changes the output. */
static bool xz_block_encode(xz_encoder address_to e, p8 address_to input, p32 n)
{
        xz_probability_state address_to m = address_of e->models;

        e->input = input;
        e->input_n = n;
        if (!n || !xz_block_prepare(e, n))
                return false;
        xz_lzma_reset(e);

        p8 address_to out = e->out + XZ_BLOCK_HEADER_MAX;
        positive at = 0;
        bool props = true;
        bool dict_reset = true;
        bool state_reset = false;
        bool started = false;

        e->position = 0;
        for (;;)
        {
                p32 start = e->read_pos - e->read_ahead;

                if (start >= n)
                        break;
                if (state_reset)
                        xz_lzma_reset(e);
                e->rc = (xz_range_state){0xffffffffu, 0, 0, 1, e->chunk,
                                         e->chunk + sizeof(e->chunk), 0};
                if (!started)
                {
                        xz_skip(e, 1);
                        e->read_ahead = 0;
                        xz_bit(e, address_of m->is_match[0][0], 0);
                        lzma_range_encode(address_of e->rc, m->lit, input[0], 8);
                        e->position = 1;
                        started = true;
                }

                p32 limit = start + XZ_CHUNK_PLAIN_MAX - XZ_MATCH_MAX;

                for (;;)
                {
                        if (e->read_pos - e->read_ahead >= limit ||
                            (positive)(e->rc.next - e->chunk) + e->rc.pending + 4 >=
                                    XZ_CHUNK_PACKED_MAX - XZ_LOOP_INPUT)
                                break;
                        if (e->read_pos >= n && !e->read_ahead)
                                break;

                        p32 back;
                        p32 len;

                        if (e->preset->normal)
                                xz_optimum_normal(e, address_of back, address_of len,
                                                  (p32)e->position);
                        else
                                xz_optimum_fast(e, address_of back, address_of len);
                        xz_code_symbol(e, back, len);
                }
                for (p32 i = 0; i < 5; i++)
                        lzma_range_shift(address_of e->rc);
                if (e->rc.full)
                        return false;

                positive packed = (positive)(e->rc.next - e->chunk);
                positive plain = e->read_pos - e->read_ahead - start;

                if (packed >= plain)
                {
                        /* Stored: the models coded speculatively are dropped,
                           so the next compressed chunk resets its state. */
                        plain += e->read_ahead;
                        e->read_ahead = 0;
                        for (positive from = 0; from < plain;)
                        {
                                positive take = plain - from;

                                if (take > XZ_CHUNK_PACKED_MAX)
                                        take = XZ_CHUNK_PACKED_MAX;
                                out[at++] = dict_reset ? 1 : 2;
                                out[at++] = (p8)((take - 1) >> 8);
                                out[at++] = (p8)(take - 1);
                                memory_copy_apart(out + at, input + start + from, take);
                                at += take;
                                from += take;
                                dict_reset = false;
                        }
                        state_reset = true;
                        continue;
                }
                out[at++] = (p8)((props ? (dict_reset ? 0xe0 : 0xc0)
                                        : state_reset ? 0xa0 : 0x80) |
                                 ((plain - 1) >> 16));
                out[at++] = (p8)((plain - 1) >> 8);
                out[at++] = (p8)(plain - 1);
                out[at++] = (p8)((packed - 1) >> 8);
                out[at++] = (p8)(packed - 1);
                if (props)
                        out[at++] = (p8)((e->pb * 5 + e->lp) * 9 + e->lc);
                memory_copy_apart(out + at, e->chunk, packed);
                at += packed;
                props = dict_reset = state_reset = false;
        }
        out[at++] = 0;

        p8 header[XZ_BLOCK_HEADER_MAX];
        positive h = 2;

        header[1] = 0x40 | 0x80;
        h += xz_vli_put(header + h, at);
        h += xz_vli_put(header + h, n);
        header[h++] = 0x21;
        header[h++] = 1;
        header[h++] = xz_prop_from_dict(e->dict);
        while ((h + 4) & 3)
                header[h++] = 0;
        header[0] = (p8)((h + 4) / 4 - 1);
        memory_store_unaligned(p32, header + h, ~hash_crc32(0xffffffffu, header, h));
        h += 4;
        memory_copy_apart(e->out + XZ_BLOCK_HEADER_MAX - h, header, h);

        e->unpadded = h + at + 4;
        while (at & 3)
                out[at++] = 0;
        memory_store_unaligned(p32, out + at, ~hash_crc32(0xffffffffu, input, n));
        e->out_at = XZ_BLOCK_HEADER_MAX - h;
        e->out_n = h + at + 4;
        return true;
}

/*
        The stream around the blocks: header, blocks, index and footer.
        Input waits in batches of whole blocks. Each block is one job that
        encodes into its worker's xz_encoder; the sink writes the jobs'
        bytes and index records in block order on the calling thread. A
        batch only decides how much input waits in memory, never where a
        block starts, so the bytes are the same for any batch or worker count.
*/
#define XZ_BATCH_BYTES ((positive)1 << 30)
#define XZ_BATCH_BLOCKS 64

typedef struct
{
        p8 level;
        positive block;
        positive batch_blocks;
        p8 address_to input;
        positive input_room;
        positive input_n;
        xz_encoder address_to address_to slots;
        positive slot_count;
        p64 address_to unpadded;
        p8 address_to index;
        positive index_room;
        positive index_n;
        p64 records;
        byte_store address_to store;
        bipolar fd;
        bool failed;
} xz_stream_writer;

static xz_stream_writer xz_writer;

/* 1 keeps encoding on the calling thread; set by the command line only. */
static bool xz_serial;

static bool xz_writer_emit(p8 address_to bytes, positive n)
{
        if (xz_writer.failed)
                return false;
        if (xz_writer.store)
        {
                if (!byte_store_append_exact(xz_writer.store, bytes, n))
                {
                        xz_writer.failed = true;
                        return xz_fail("xz output is too small");
                }
        }
        else if (xz_writer.fd >= 0 &&
                 system_write_all((positive)xz_writer.fd, bytes, n) != (bipolar)n)
        {
                xz_writer.failed = true;
                return xz_fail("xz write failed");
        }
        return true;
}

static fn xz_writer_close(void)
{
        for (positive i = 0; i < xz_writer.slot_count; i++)
                xz_encoder_close(xz_writer.slots[i]);
        memory_free(xz_writer.slots, xz_writer.slot_count * sizeof(xz_encoder address_to));
        memory_free(xz_writer.unpadded, xz_writer.batch_blocks * sizeof(p64));
        memory_free(xz_writer.input, xz_writer.input_room);
        memory_free(xz_writer.index, xz_writer.index_room);
        xz_writer.slots = null;
        xz_writer.slot_count = 0;
        xz_writer.unpadded = null;
        xz_writer.input = null;
        xz_writer.input_room = 0;
        xz_writer.index = null;
        xz_writer.index_room = 0;
}

static bool xz_writer_record(p64 unpadded, p64 uncompressed)
{
        if (xz_writer.index_n + 20 > xz_writer.index_room)
        {
                positive room = xz_writer.index_room ? 2 * xz_writer.index_room : 4096;
                p8 address_to grown = (p8 address_to)memory(room);

                if (!grown || system_failed(grown))
                        return xz_fail("xz cannot map the index");
                if (xz_writer.index)
                {
                        memory_copy_apart(grown, xz_writer.index, xz_writer.index_n);
                        memory_free(xz_writer.index, xz_writer.index_room);
                }
                xz_writer.index = grown;
                xz_writer.index_room = room;
        }
        xz_writer.index_n += xz_vli_put(xz_writer.index + xz_writer.index_n, unpadded);
        xz_writer.index_n += xz_vli_put(xz_writer.index + xz_writer.index_n, uncompressed);
        xz_writer.records++;
        return true;
}

static positive xz_batch_bytes(xz_stream_writer address_to w, positive index)
{
        positive from = index * w->block;

        return w->input_n - from < w->block ? w->input_n - from : w->block;
}

/* One block of the batch, on any thread: it touches only its worker's
   encoder, its own unpadded slot and its own output. A block that cannot be
   encoded leaves unpadded zero for the sink to report at its place. */
static fn xz_batch_job(address_any context, positive index,
                       parallel_output address_to output)
{
        xz_stream_writer address_to w = (xz_stream_writer address_to)context;
        positive slot = parallel_slot();
        xz_encoder address_to e = w->slots[slot];

        w->unpadded[index] = 0;
        if (!e)
        {
                e = xz_encoder_open(w->level);
                if (!e)
                        return;
                w->slots[slot] = e;
        }
        if (xz_block_encode(e, w->input + index * w->block, (p32)xz_batch_bytes(w, index)) &&
            parallel_write(output, e->out + e->out_at, e->out_n))
                w->unpadded[index] = e->unpadded;
}

/* A block's bytes and index record, in block order, on the calling thread. */
static bool xz_batch_sink(address_any context, positive index, address_any data,
                          positive length)
{
        xz_stream_writer address_to w = (xz_stream_writer address_to)context;

        if (!w->unpadded[index])
                return xz_fail("xz cannot encode a block");
        return xz_writer_emit((p8 address_to)data, length) &&
               xz_writer_record(w->unpadded[index], xz_batch_bytes(w, index));
}

/* Every block of the batch: encoded side by side, written in block order.
   Encoding is heavy per byte, so the pool spreads even a small batch. */
static bool xz_writer_batch(void)
{
        xz_stream_writer address_to w = address_of xz_writer;
        positive count = (w->input_n + w->block - 1) / w->block;

        if (!count)
                return !w->failed;
        if (!parallel_ordered(xz_batch_job, xz_batch_sink, w, count,
                              xz_serial ? 0 : PARALLEL_SPREAD))
                return w->failed || xz_why ? false : xz_fail("xz cannot encode a block");
        w->input_n = 0;
        return !w->failed;
}

static bool xz_encode_setup(p8 level)
{
        const xz_preset address_to p = xz_presets + (level > 9 ? 9 : level);
        positive dict = (positive)1 << p->dict_log;
        p8 header[12] = {0xfd, 0x37, 0x7a, 0x58, 0x5a, 0, 0, XZ_CHECK_CRC32};

        xz_writer_close();
        xz_why = null;
        xz_writer.level = level > 9 ? 9 : level;
        xz_writer.block = 3 * dict > ((positive)1 << 20) ? 3 * dict : (positive)1 << 20;
        xz_writer.batch_blocks = XZ_BATCH_BYTES / xz_writer.block;
        if (xz_writer.batch_blocks > XZ_BATCH_BLOCKS)
                xz_writer.batch_blocks = XZ_BATCH_BLOCKS;
        if (!xz_writer.batch_blocks)
                xz_writer.batch_blocks = 1;
        xz_writer.input_n = 0;
        xz_writer.index_n = 0;
        xz_writer.records = 0;
        xz_writer.failed = false;
        xz_writer.store = xz_output.bytes ? address_of xz_output : null;
        xz_writer.fd = xz_out_fd;
        xz_writer.slot_count = parallel_width();
        xz_writer.slots = (xz_encoder address_to address_to)memory(
            xz_writer.slot_count * sizeof(xz_encoder address_to));
        xz_writer.unpadded = (p64 address_to)memory(xz_writer.batch_blocks * sizeof(p64));
        xz_writer.input_room = xz_writer.batch_blocks * xz_writer.block + XZ_SLACK;
        xz_writer.input = (p8 address_to)memory(xz_writer.input_room);
        if (!xz_writer.slots || system_failed(xz_writer.slots) || !xz_writer.unpadded ||
            system_failed(xz_writer.unpadded) || !xz_writer.input ||
            system_failed(xz_writer.input))
        {
                if (system_failed(xz_writer.slots))
                        xz_writer.slots = null;
                if (system_failed(xz_writer.unpadded))
                        xz_writer.unpadded = null;
                if (system_failed(xz_writer.input))
                        xz_writer.input = null;
                return xz_fail("xz cannot map the block input");
        }
        memory_store_unaligned(p32, header + 8, ~hash_crc32(0xffffffffu, header + 6, 2));
        return xz_writer_emit(header, sizeof(header));
}

static bool xz_encode_write(p8 address_to src, positive n)
{
        positive capacity = xz_writer.batch_blocks * xz_writer.block;

        while (n)
        {
                positive take = min(n, capacity - xz_writer.input_n);

                memory_copy(xz_writer.input + xz_writer.input_n, src, take);
                xz_writer.input_n += take;
                src += take;
                n -= take;
                if (xz_writer.input_n == capacity && !xz_writer_batch())
                        return false;
        }
        return !xz_writer.failed;
}

static bool xz_encode_end(void)
{
        bool ok = xz_writer_batch();

        if (ok)
        {
                p8 head[16];
                p8 tail[8];
                p8 footer[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, XZ_CHECK_CRC32, 'Y', 'Z'};
                positive h = 1;
                positive size;
                p32 crc;

                head[0] = 0;
                h += xz_vli_put(head + h, xz_writer.records);
                size = h + xz_writer.index_n;
                crc = hash_crc32(0xffffffffu, head, h);
                crc = hash_crc32(crc, xz_writer.index, xz_writer.index_n);
                positive pad = (4 - (size & 3)) & 3;
                memory_fill(tail, 0, sizeof(tail));
                crc = hash_crc32(crc, tail, pad);
                memory_store_unaligned(p32, tail + pad, ~crc);
                size += pad + 4;
                memory_store_unaligned(p32, footer + 4, size / 4 - 1);
                memory_store_unaligned(p32, footer, ~hash_crc32(0xffffffffu, footer + 4, 6));
                ok = xz_writer_emit(head, h) &&
                     xz_writer_emit(xz_writer.index, xz_writer.index_n) &&
                     xz_writer_emit(tail, pad + 4) &&
                     xz_writer_emit(footer, sizeof(footer));
        }
        xz_writer_close();
        return ok;
}

static bool xz_stream_encode(p8 level)
{
        if (!xz_encode_setup(level))
        {
                xz_writer_close();
                return false;
        }

        positive capacity = xz_writer.batch_blocks * xz_writer.block;

        for (;;)
        {
                if (xz_writer.input_n == capacity && !xz_writer_batch())
                        break;
                bipolar got = system_read_retry((positive)xz_input.fd,
                                                xz_writer.input + xz_writer.input_n,
                                                capacity - xz_writer.input_n);
                if (got < 0)
                {
                        xz_fail("xz read failed");
                        break;
                }
                if (!got)
                        return xz_encode_end();
                xz_writer.input_n += (positive)got;
        }
        xz_writer_close();
        return false;
}

static bipolar xz_deflate_mem(p8 address_to src, positive src_len,
                              p8 address_to dst, positive dst_cap, p8 level)
{
        bool ok;

        xz_input.fd = -1;
        xz_out_fd = -1;
        xz_output.bytes = dst;
        xz_output.room = dst_cap;
        xz_output.used = 0;
        ok = xz_encode_setup(level) && xz_encode_write(src, src_len) && xz_encode_end();
        if (!ok)
                xz_writer_close();
        xz_output.bytes = null;
        return ok ? (bipolar)xz_output.used : -1;
}

static bool xz_decode_begin(bipolar in)
{
        xz_encoding = false;
        xz_why = null;
        xz_out_failed = false;
        byte_input_open_fd(address_of xz_input, in, xz_in_buf, XZ_IN);
        xz_out_fd = -1;
        xz_output.bytes = null;
        xz_out_fill = xz_out_taken = xz_out_hashed = 0;
        xz_pull = true;
        xz_paused = false;
        xz_finished = false;
        xz_hdr_done = false;
        xz_block_live = false;
        xz_lz2_kind = 0;
        xz_in_abs = 0;
        xz_unpacked = 0;
        return true;
}

static bool xz_decode_begin_prefix(bipolar in, p8 address_to prefix, positive n)
{
        xz_decode_begin(in);
        if (n > XZ_IN)
                return xz_fail("xz prefix");
        memory_copy(xz_in_buf, prefix, n);
        xz_input.have = n;
        xz_input.at = 0;
        return true;
}

static bipolar xz_decode_read(p8 address_to dst, positive n)
{
        positive copied = 0;

        while (copied < n)
        {
                positive take;

                if (xz_out_fill)
                {
                        xz_output_check();
                        positive left = xz_out_fill - xz_out_taken;
                        take = left > n - copied ? n - copied : left;
                        memory_copy_apart(dst + copied, xz_out_buf + xz_out_taken, take);
                        xz_out_taken += take;
                        if (xz_out_taken == xz_out_fill)
                                xz_out_fill = xz_out_taken = xz_out_hashed = 0;
                        copied += take;
                        continue;
                }
                if (xz_finished)
                        break;
                if (!xz_in_need() && !xz_hdr_done && !xz_block_live)
                {
                        xz_finished = true;
                        break;
                }
                xz_paused = false;
                if (!xz_stream())
                        return -1;
                if (xz_paused)
                        continue;
                if (!xz_out_fill && !xz_hdr_done && !xz_block_live &&
                    !xz_in_need())
                        xz_finished = true;
        }
        return (bipolar)copied;
}

static bool xz_decode_end(void)
{
        xz_pull = false;
        xz_finished = true;
        xz_dict_close();
        return xz_why == null;
}

static bool xz_encode_begin(bipolar out, p8 level)
{
        xz_out_fd = out;
        xz_output.bytes = null;
        xz_input.fd = -1;
        return xz_encode_setup(level);
}

#ifndef XZ_CORE_ONLY

static b32 xz_stream_cli(bipolar in, bipolar out, bool decode, p8 level)
{
        bool ok;

        byte_input_open_fd(address_of xz_input, in, xz_in_buf, XZ_IN);
        xz_out_fd = out;
        xz_output.bytes = null;
        xz_status = 0;
        xz_serial = file_codec_threads == 1;
        if (decode)
                ok = xz_stream_decode();
        else
                ok = xz_stream_encode(level);
        xz_dict_close();
        if (!ok)
        {
                if (xz_why)
                        string_format(log_error, "xz: %s\n", xz_why);
                xz_status = 1;
                return 1;
        }
        return 0;
}

static const file_codec_suffix xz_suffixes[] = {
    {".xz", ""}, {".txz", ".tar"}};

static b32 file_xz(void)
{
        file_codec_cli codec = {
            .name = "xz", .decode_name = "unxz", .cat_name = "xzcat",
            .usage = "Usage: xz [-cdfkqt0123456789] [-T N] [FILE...]",
            .version = "xz from dawning-kit",
            .status = address_of xz_status,
            .suffixes = xz_suffixes, .suffix_count = array_count(xz_suffixes),
            .decode_suffix_error = "unknown suffix; use -c",
            .encode_suffix_error = "cannot guess output name",
            .features = FILE_CODEC_LEVEL_ZERO | FILE_CODEC_THREADS,
            .remove_source = true, .level = 6,
            .run = xz_stream_cli};
        return file_codec_main(address_of codec);
}

#endif /* XZ_CORE_ONLY */
