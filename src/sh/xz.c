/*
        xz -- LZMA2 inside the .xz stream (check none, CRC32 or CRC64).

        Decode is a range coder plus the twelve-state LZMA machine, then
        LZMA2 chunks and the stream wrapper. Encode walks the pending
        window into 64 KiB LZMA2 chunks using a stamped hash chain, repeat
        distances and memory_common_prefix. Probability state survives
        compressed chunks; a raw fallback resets it. Range trees use shared
        assembly kernels with a scalar refill tail. Checksums are hash_crc32/hash_crc64. Concatenated
        streams are accepted the way xz -d accepts them. There is no
        SHA-256 check and no BCJ.
*/

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
#define XZ_HASH_BITS 16
#define XZ_HASH_SIZE (1u << XZ_HASH_BITS)
#define XZ_ENC_WIN 32768

#define XZ_CHECK_NONE 0
#define XZ_CHECK_CRC32 1
#define XZ_CHECK_CRC64 4

static p8 xz_in_buf[XZ_IN];
static positive xz_in_at;
static positive xz_in_have;
static bool xz_in_eof;
static bipolar xz_in_fd;
static p8 address_to xz_in_mem;
static positive xz_in_mem_len;
static positive xz_in_mem_at;

static p8 xz_out_buf[XZ_OUT + XZ_MATCH_MAX];
static positive xz_out_fill;
static bipolar xz_out_fd;
static p8 address_to xz_out_mem;
static positive xz_out_cap;
static positive xz_out_used;
static string_address xz_why;
static b32 xz_status;

static bool xz_pull;
static bool xz_paused;
static bool xz_finished;
static bool xz_stream_open;
static bool xz_hdr_done;
static bool xz_block_live;
static p8 xz_lz2_kind;
static positive xz_lz2_raw_left;
static p64 xz_lz2_want;
static p64 xz_lz2_pack_from;
static p64 xz_lz2_pack_want;
static p64 xz_lz2_chunk_from;
static bool xz_lz2_have;
static positive xz_lz2_match_left;
static positive xz_lz2_match_dist;
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

static p16 xz_is_match[XZ_STATES][XZ_POS];
static p16 xz_is_rep[XZ_STATES];
static p16 xz_is_rep0[XZ_STATES];
static p16 xz_is_rep1[XZ_STATES];
static p16 xz_is_rep2[XZ_STATES];
static p16 xz_is_rep0_long[XZ_STATES][XZ_POS];
static p16 xz_dist_slot[4][XZ_DIST_SLOTS];
static p16 xz_dist_special[XZ_FULL_DIST - 14];
static p16 xz_dist_align[XZ_ALIGN];
static p16 xz_match_choice;
static p16 xz_match_choice2;
static p16 xz_match_low[XZ_POS][XZ_LEN_LOW];
static p16 xz_match_mid[XZ_POS][XZ_LEN_MID];
static p16 xz_match_high[XZ_LEN_HIGH];
static p16 xz_rep_choice;
static p16 xz_rep_choice2;
static p16 xz_rep_low[XZ_POS][XZ_LEN_LOW];
static p16 xz_rep_mid[XZ_POS][XZ_LEN_MID];
static p16 xz_rep_high[XZ_LEN_HIGH];
static p16 xz_lit[XZ_PROB_LIT];

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

static p32 xz_head[XZ_HASH_SIZE];
static p16 xz_prev[65536];
static p32 xz_inner_stamp;
static p8 xz_hold[XZ_ENC_WIN + XZ_MATCH_MAX];
static positive xz_hold_fill;
static positive xz_hold_at;
static positive xz_hold_abs;
static bool xz_hold_eof;
static p8 address_to xz_feed;
static positive xz_feed_len;
static positive xz_feed_at;
static p8 address_to xz_enc_mem;
static positive xz_enc_mem_len;
static positive xz_enc_mem_at;
static p8 xz_level;
static p8 xz_pending[65536];
static positive xz_pending_n;
static bool xz_need_reset;
static bool xz_enc_have_lzma;
static p8 xz_rc_buf[65536 + 32768];
static positive xz_rc_n;
static p64 xz_block_unpadded;
static p64 xz_block_unpacked;
static p64 xz_index_unpadded[32];
static p64 xz_index_unpacked[32];
static positive xz_index_n;
static p8 xz_index_buf[512];
static positive xz_index_fill;

static p32 xz_crc32_byte(p32 crc, p8 byte)
{
        return hash_crc32_tab[(crc ^ byte) & 255] ^ (crc >> 8);
}

static p32 xz_crc32_bytes(p32 crc, p8 address_to bytes, positive n)
{
        return hash_crc32(crc, bytes, n);
}

static p64 xz_crc64_byte(p64 crc, p8 byte)
{
        return hash_crc64_tab[(crc ^ byte) & 255] ^ (crc >> 8);
}

static bool xz_fail(string_address why)
{
        xz_why = why;
        return false;
}

static bool xz_in_need(positive want)
{
        bipolar got;

        if (xz_in_have - xz_in_at >= want)
                return true;
        if (xz_in_eof)
                return xz_in_have > xz_in_at;
        if (xz_in_at)
        {
                if (xz_in_at < xz_in_have)
                        memory_copy_apart(xz_in_buf, xz_in_buf + xz_in_at,
                                          xz_in_have - xz_in_at);
                xz_in_have -= xz_in_at;
                xz_in_at = 0;
        }
        if (xz_in_mem)
        {
                positive left = xz_in_mem_len - xz_in_mem_at;
                positive take = left > (XZ_IN - xz_in_have) ? (XZ_IN - xz_in_have)
                                                           : left;

                if (take)
                {
                        memory_copy(xz_in_buf + xz_in_have,
                                    xz_in_mem + xz_in_mem_at, take);
                        xz_in_have += take;
                        xz_in_mem_at += take;
                }
                if (xz_in_mem_at >= xz_in_mem_len)
                        xz_in_eof = true;
                return xz_in_have > xz_in_at;
        }
        got = system_read_retry((positive)xz_in_fd, xz_in_buf + xz_in_have,
                                XZ_IN - xz_in_have);
        if (got < 0)
                return xz_fail("xz read failed");
        if (!got)
                xz_in_eof = true;
        else
                xz_in_have += (positive)got;
        return xz_in_have > xz_in_at;
}

static bipolar xz_in_byte(void)
{
        if (xz_in_at >= xz_in_have && !xz_in_need(1))
                return -1;
        if (xz_in_at >= xz_in_have)
                return -1;
        xz_in_abs++;
        return xz_in_buf[xz_in_at++];
}

static bool xz_out_flush(void)
{
        if (!xz_out_fill)
                return true;
        if (xz_out_mem)
        {
                if (xz_out_used + xz_out_fill > xz_out_cap)
                        return xz_fail("xz output is too small");
                memory_copy(xz_out_mem + xz_out_used, xz_out_buf, xz_out_fill);
                xz_out_used += xz_out_fill;
                xz_out_fill = 0;
                return true;
        }
        if (xz_out_fd < 0)
        {
                xz_out_fill = 0;
                return true;
        }
        if (system_write_all((positive)xz_out_fd, xz_out_buf, xz_out_fill) !=
            xz_out_fill)
                return xz_fail("xz write failed");
        xz_out_fill = 0;
        return true;
}

static bool xz_emit(p8 byte)
{
        if (!xz_dict || xz_dict_size)
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
        }
        xz_unpacked++;
        if (xz_check == XZ_CHECK_CRC32)
                xz_crc32 = xz_crc32_byte(xz_crc32, byte);
        else if (xz_check == XZ_CHECK_CRC64)
                xz_crc64 = xz_crc64_byte(xz_crc64, byte);
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
        first = xz_dict_size - xz_dict_pos;
        if (first > length)
                first = length;
        memory_copy_apart(xz_dict + xz_dict_pos, into, first);
        if (length > first)
                memory_copy_apart(xz_dict, into + first, length - first);
        xz_dict_pos += length;
        if (xz_dict_pos >= xz_dict_size)
                xz_dict_pos -= xz_dict_size;
        xz_dict_full += length;
        if (xz_dict_full > xz_dict_size)
                xz_dict_full = xz_dict_size;
        xz_unpacked += length;
        if (xz_check == XZ_CHECK_CRC32)
                xz_crc32 = hash_crc32(xz_crc32, into, length);
        else if (xz_check == XZ_CHECK_CRC64)
                xz_crc64 = hash_crc64(xz_crc64, into, length);
        xz_out_fill += length;
        xz_lz2_match_left = 0;
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
        xz_range_input state = {xz_range, xz_code, xz_in_buf + xz_in_at,
                               xz_in_buf + xz_in_have};
        p8 address_to start = state.next;
        bipolar result = lzma_range_decode(address_of state, probs, mode);
        xz_range = state.range;
        xz_code = state.code;
        xz_in_at += (positive)(state.next - start);
        xz_in_abs += (positive)(state.next - start);
        return result;
}

static bipolar xz_rc_bittree(p16 address_to probs, p8 bits)
{
        if (!bits)
                return 0;
        if (xz_in_have - xz_in_at >= 24)
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
        if (xz_in_have - xz_in_at >= 24)
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
        if (xz_in_have - xz_in_at >= 24)
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

        if (xz_lz2_match_left)
                return xz_emit_match(0, 0);
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
                if (xz_in_at == xz_in_have && !xz_in_need(1))
                        return xz_fail("xz truncated uncompressed");
                positive take = xz_in_have - xz_in_at;
                if (take > xz_lz2_raw_left)
                        take = xz_lz2_raw_left;
                if (take > XZ_OUT - xz_out_fill)
                        take = XZ_OUT - xz_out_fill;
                if (take > xz_dict_size - xz_dict_pos)
                        take = xz_dict_size - xz_dict_pos;
                if (!take)
                        return xz_fail("xz truncated uncompressed");
                p8 address_to bytes = xz_in_buf + xz_in_at;
                memory_copy_apart(xz_out_buf + xz_out_fill, bytes, take);
                memory_copy_apart(xz_dict + xz_dict_pos, bytes, take);
                xz_dict_pos += take;
                if (xz_dict_pos == xz_dict_size)
                        xz_dict_pos = 0;
                xz_dict_full += take;
                if (xz_dict_full > xz_dict_size)
                        xz_dict_full = xz_dict_size;
                if (xz_check == XZ_CHECK_CRC32)
                        xz_crc32 = hash_crc32(xz_crc32, bytes, take);
                else if (xz_check == XZ_CHECK_CRC64)
                        xz_crc64 = hash_crc64(xz_crc64, bytes, take);
                xz_out_fill += take;
                xz_in_at += take;
                xz_in_abs += take;
                xz_unpacked += take;
                xz_lz2_raw_left -= take;
                if (!xz_pull && xz_out_fill >= XZ_OUT && !xz_out_flush())
                        return false;
        }

        xz_lz2_kind = 0;
        xz_lz2_have = false;
        return true;
}

static bool xz_lzma2_lzma(void)
{
        while (xz_unpacked - xz_lz2_chunk_from < xz_lz2_want)
        {
                if (xz_pull && xz_out_fill >= XZ_OUT && !xz_lz2_match_left)
                {
                        xz_paused = true;
                        return true;
                }
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
                        xz_lz2_have = true;
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

static bipolar xz_vli(void)
{
        p64 v = 0;
        p8 shift = 0;

        for (;;)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return -1;
                v |= (p64)((p8)byte & 0x7f) << shift;
                if (!((p8)byte & 0x80))
                        return (bipolar)v;
                shift += 7;
                if (shift >= 63)
                        return xz_fail("xz VLI"), -1;
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
        if (xz_check == XZ_CHECK_CRC32)
        {
                p32 got = 0;
                p8 at;

                for (at = 0; at < 4; at++)
                {
                        bipolar byte = xz_in_byte();

                        if (byte < 0)
                                return xz_fail("xz truncated check");
                        got |= (p32)(p8)byte << (8 * at);
                }
                (void)want64;
                if (got != (p32)want32)
                        return xz_fail("xz CRC32 mismatch");
                return true;
        }
        if (xz_check == XZ_CHECK_CRC64)
        {
                p64 got = 0;
                p8 at;

                for (at = 0; at < 8; at++)
                {
                        bipolar byte = xz_in_byte();

                        if (byte < 0)
                                return xz_fail("xz truncated check");
                        got |= (p64)(p8)byte << (8 * at);
                }
                if (got != want64)
                        return xz_fail("xz CRC64 mismatch");
                return true;
        }
        return xz_fail("xz check type");
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
                        xz_in_at--;
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
                expect = ~xz_crc32_bytes(0xffffffffu, header, header_size - 4);
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
                xz_lz2_have = false;
                xz_lz2_match_left = 0;
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
                address_to crc = xz_crc32_byte(address_to crc, (p8)byte);
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
        bipolar indicator;
        bipolar records;
        p32 crc;
        p32 got;
        p8 at;
        p8 flags[2];
        p32 back;
        positive hashed = 1;
        bipolar fb0;
        bipolar fb1;
        bipolar y;
        bipolar z;

        indicator = xz_in_byte();
        if (indicator != 0)
                return xz_fail("xz index indicator");
        crc = xz_crc32_byte(0xffffffffu, 0);
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
                crc = xz_crc32_byte(crc, 0);
                hashed++;
        }
        crc = ~crc;
        got = 0;
        for (at = 0; at < 4; at++)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated index CRC");
                got |= (p32)(p8)byte << (8 * at);
        }
        if (got != crc)
                return xz_fail("xz index CRC");

        got = 0;
        for (at = 0; at < 4; at++)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated footer");
                got |= (p32)(p8)byte << (8 * at);
        }
        back = 0;
        for (at = 0; at < 4; at++)
        {
                bipolar byte = xz_in_byte();

                if (byte < 0)
                        return xz_fail("xz truncated footer");
                back |= (p32)(p8)byte << (8 * at);
        }
        fb0 = xz_in_byte();
        fb1 = xz_in_byte();
        if (fb0 < 0 || fb1 < 0)
                return xz_fail("xz truncated footer");
        flags[0] = (p8)fb0;
        flags[1] = (p8)fb1;
        if (flags[0] || (flags[1] & 0xf0) || (flags[1] & 0xf) != xz_check)
                return xz_fail("xz footer flags");
        y = xz_in_byte();
        z = xz_in_byte();
        if (y != 'Y' || z != 'Z')
                return xz_fail("xz footer magic");
        {
                p8 body[6];

                body[0] = (p8)back;
                body[1] = (p8)(back >> 8);
                body[2] = (p8)(back >> 16);
                body[3] = (p8)(back >> 24);
                body[4] = flags[0];
                body[5] = flags[1];
                if (got != ~xz_crc32_bytes(0xffffffffu, body, 6))
                        return xz_fail("xz footer CRC");
        }
        return true;
}

static bool xz_stream(void)
{
        p8 magic[6];
        p8 flags[2];
        p32 crc;
        p32 got;
        p8 at;

        if (!xz_hdr_done)
        {
                for (at = 0; at < 6; at++)
                {
                        bipolar byte = xz_in_byte();

                        if (byte < 0)
                                return xz_fail("xz truncated header");
                        magic[at] = (p8)byte;
                }
                if (magic[0] != 0xfd || magic[1] != 0x37 || magic[2] != 0x7a ||
                    magic[3] != 0x58 || magic[4] != 0x5a || magic[5] != 0)
                        return xz_fail("xz bad magic");
                flags[0] = (p8)xz_in_byte();
                flags[1] = (p8)xz_in_byte();
                if (flags[0] || (flags[1] & 0xf0))
                        return xz_fail("xz reserved stream flags");
                xz_check = flags[1] & 0xf;
                if (xz_check != XZ_CHECK_NONE && xz_check != XZ_CHECK_CRC32 &&
                    xz_check != XZ_CHECK_CRC64)
                        return xz_fail("xz check type");
                crc = ~xz_crc32_bytes(0xffffffffu, flags, 2);
                got = 0;
                for (at = 0; at < 4; at++)
                {
                        bipolar byte = xz_in_byte();

                        if (byte < 0)
                                return xz_fail("xz truncated header CRC");
                        got |= (p32)(p8)byte << (8 * at);
                }
                if (got != crc)
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
                if (!xz_in_need(1))
                        return xz_fail("xz truncated stream");
                if (xz_in_at < xz_in_have && xz_in_buf[xz_in_at] == 0)
                        break;
                if (!xz_block())
                {
                        if (xz_in_at < xz_in_have && xz_in_buf[xz_in_at] == 0)
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
        bool any = false;

        xz_why = null;
        xz_in_at = 0;
        xz_in_have = 0;
        xz_in_eof = false;
        xz_out_fill = 0;
        xz_pull = false;
        xz_paused = false;
        xz_finished = false;
        xz_hdr_done = false;
        xz_block_live = false;
        xz_lz2_kind = 0;
        xz_lz2_match_left = 0;
        xz_in_abs = 0;
        xz_unpacked = 0;

        for (;;)
        {
                if (!xz_in_need(1))
                        break;
                if (xz_in_at >= xz_in_have)
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

        xz_in_fd = -1;
        xz_in_mem = src;
        xz_in_mem_len = src_len;
        xz_in_mem_at = 0;
        xz_out_fd = -1;
        xz_out_mem = dst;
        xz_out_cap = dst_cap;
        xz_out_used = 0;
        ok = xz_stream_decode();
        xz_in_mem = null;
        xz_out_mem = null;
        return ok ? (bipolar)xz_out_used : -1;
}

static fn xz_put(p8 byte)
{
        xz_out_buf[xz_out_fill++] = byte;
        if (xz_out_fill == XZ_OUT)
                xz_out_flush();
}

static fn xz_put32(p32 v)
{
        xz_put((p8)v);
        xz_put((p8)(v >> 8));
        xz_put((p8)(v >> 16));
        xz_put((p8)(v >> 24));
}

static fn xz_put64(p64 v)
{
        p8 at;

        for (at = 0; at < 8; at++)
                xz_put((p8)(v >> (8 * at)));
}

static fn xz_put_vli(p64 v)
{
        while (v >= 0x80)
        {
                xz_put((p8)(v | 0x80));
                v >>= 7;
        }
        xz_put((p8)v);
}

static p8 xz_prop_from_dict(positive dict)
{
        p8 prop;

        for (prop = 0; prop < 40; prop++)
                if (xz_dict_from_prop(prop) >= dict)
                        return prop;
        return 39;
}

static bool xz_write_header(p8 check)
{
        p8 flags[2];

        flags[0] = 0;
        flags[1] = check;
        xz_put(0xfd);
        xz_put(0x37);
        xz_put(0x7a);
        xz_put(0x58);
        xz_put(0x5a);
        xz_put(0);
        xz_put(flags[0]);
        xz_put(flags[1]);
        xz_put32(~xz_crc32_bytes(0xffffffffu, flags, 2));
        xz_check = check;
        xz_index_n = 0;
        xz_block_unpadded = 0;
        xz_block_unpacked = 0;
        return true;
}

static bool xz_write_uncompressed_chunk(p8 address_to src, positive n, bool reset)
{
        positive at;

        xz_put(reset ? 1 : 2);
        xz_put((p8)((n - 1) >> 8));
        xz_put((p8)(n - 1));
        for (at = 0; at < n; at++)
                xz_put(src[at]);
        xz_block_unpadded += 3 + n;
        xz_block_unpacked += n;
        return true;
}

static bool xz_write_block_header(p8 dict_prop)
{
        p8 header[12];
        p32 crc;

        header[0] = 2;
        header[1] = 0;
        header[2] = 0x21;
        header[3] = 1;
        header[4] = dict_prop;
        header[5] = 0;
        header[6] = 0;
        header[7] = 0;
        crc = ~xz_crc32_bytes(0xffffffffu, header, 8);
        header[8] = (p8)crc;
        header[9] = (p8)(crc >> 8);
        header[10] = (p8)(crc >> 16);
        header[11] = (p8)(crc >> 24);
        {
                positive at;

                for (at = 0; at < 12; at++)
                        xz_put(header[at]);
        }
        xz_block_unpadded = 12;
        return true;
}

static bool xz_write_index_footer(void)
{
        p8 index[512];
        positive n = 0;
        positive at;
        p32 crc;
        p32 back;
        p8 flags[2];
        p8 body[6];

        index[n++] = 0;
        {
                p64 rec = xz_index_n;
                p8 tmp[10];
                p8 tn = 0;

                do
                {
                        tmp[tn++] = (p8)(rec & 0x7f);
                        rec >>= 7;
                } while (rec);
                for (at = 0; at + 1 < tn; at++)
                        index[n++] = tmp[at] | 0x80;
                index[n++] = tmp[tn - 1];
        }
        for (at = 0; at < xz_index_n; at++)
        {
                p64 v = xz_index_unpadded[at];
                p8 tmp[10];
                p8 tn = 0;
                p8 k;

                do
                {
                        tmp[tn++] = (p8)(v & 0x7f);
                        v >>= 7;
                } while (v);
                for (k = 0; k + 1 < tn; k++)
                        index[n++] = tmp[k] | 0x80;
                index[n++] = tmp[tn - 1];
                v = xz_index_unpacked[at];
                tn = 0;
                do
                {
                        tmp[tn++] = (p8)(v & 0x7f);
                        v >>= 7;
                } while (v);
                for (k = 0; k + 1 < tn; k++)
                        index[n++] = tmp[k] | 0x80;
                index[n++] = tmp[tn - 1];
        }
        while (n & 3)
                index[n++] = 0;
        crc = ~xz_crc32_bytes(0xffffffffu, index, n);
        for (at = 0; at < n; at++)
                xz_put(index[at]);
        xz_put32(crc);
        back = n / 4;
        if (!back)
                back = 1;
        flags[0] = 0;
        flags[1] = xz_check;
        body[0] = (p8)back;
        body[1] = (p8)(back >> 8);
        body[2] = (p8)(back >> 16);
        body[3] = (p8)(back >> 24);
        body[4] = flags[0];
        body[5] = flags[1];
        xz_put32(~xz_crc32_bytes(0xffffffffu, body, 6));
        xz_put32(back);
        xz_put(flags[0]);
        xz_put(flags[1]);
        xz_put('Y');
        xz_put('Z');
        return xz_out_flush();
}

static fn xz_dict_push(p8 byte)
{
        if (!xz_dict || !xz_dict_size)
                return;
        xz_dict[xz_dict_pos] = byte;
        xz_dict_pos++;
        if (xz_dict_pos == xz_dict_size)
                xz_dict_pos = 0;
        if (xz_dict_full < xz_dict_size)
                xz_dict_full++;
}

static fn xz_enc_seen(p8 byte)
{
        xz_dict_push(byte);
        xz_unpacked++;
}

static fn xz_enc_seen_span(p8 address_to bytes, positive n)
{
        positive first = xz_dict_size - xz_dict_pos;
        if (first > n)
                first = n;
        memory_copy_apart(xz_dict + xz_dict_pos, bytes, first);
        if (n > first)
                memory_copy_apart(xz_dict, bytes + first, n - first);
        xz_dict_pos += n;
        if (xz_dict_pos >= xz_dict_size)
                xz_dict_pos -= xz_dict_size;
        xz_dict_full += n;
        if (xz_dict_full > xz_dict_size)
                xz_dict_full = xz_dict_size;
        xz_unpacked += n;
}

static fn xz_rc_shift(void)
{
        lzma_range_shift(address_of xz_rc);
}

static fn xz_rc_enc_init(void)
{
        xz_low = 0;
        xz_range = 0xffffffffu;
        xz_cache = 0;
        xz_cache_size = 1;
        xz_rc_n = 0;
        xz_rc.next = xz_rc_buf;
        xz_rc.limit = xz_rc_buf + sizeof(xz_rc_buf);
        xz_rc_full = false;
}

static fn xz_rc_enc_bit(p16 address_to prob, p8 bit)
{
        lzma_range_encode(address_of xz_rc, prob, bit, 0);
}

static fn xz_rc_enc_bittree(p16 address_to probs, p8 bits, positive v)
{
        lzma_range_encode(address_of xz_rc, probs, v, bits);
}

static fn xz_rc_enc_bittree_rev(p16 address_to probs, p8 bits, positive v)
{
        lzma_range_encode(address_of xz_rc, probs, v, 0x100 | bits);
}

static fn xz_rc_enc_direct(p8 bits, positive v)
{
        while (bits)
        {
                bits--;
                xz_range >>= 1;
                if ((v >> bits) & 1)
                        xz_low += xz_range;
                while (xz_range < 0x1000000u)
                {
                        xz_rc_shift();
                        xz_range <<= 8;
                }
        }
}

static fn xz_rc_enc_flush(void)
{
        p8 at;

        for (at = 0; at < 5; at++)
                xz_rc_shift();
}

static fn xz_enc_len(p16 address_to choice, p16 address_to choice2,
                     p16 address_to low, p16 address_to mid, p16 address_to high,
                     positive pos_state, positive len)
{
        positive v = len - 2;

        if (v < 8)
        {
                xz_rc_enc_bit(choice, 0);
                xz_rc_enc_bittree(low + pos_state * XZ_LEN_LOW, 3, v);
                return;
        }
        xz_rc_enc_bit(choice, 1);
        if (v < 16)
        {
                xz_rc_enc_bit(choice2, 0);
                xz_rc_enc_bittree(mid + pos_state * XZ_LEN_MID, 3, v - 8);
                return;
        }
        xz_rc_enc_bit(choice2, 1);
        xz_rc_enc_bittree(high, 8, v - 16);
}

static p8 xz_pos_slot(positive dist0)
{
        p8 hb;
        positive v;

        if (dist0 < 4)
                return (p8)dist0;
        v = dist0;
        hb = 0;
        while (v >= 2)
        {
                v >>= 1;
                hb++;
        }
        return (p8)((hb << 1) + ((dist0 >> (hb - 1)) & 1));
}

static fn xz_enc_dist(positive len_state, positive dist0)
{
        p8 slot = xz_pos_slot(dist0);
        p8 bits;
        positive base;
        positive extra;

        xz_rc_enc_bittree(xz_dist_slot[len_state], 6, slot);
        if (slot < 4)
                return;
        bits = (p8)((slot >> 1) - 1);
        base = (2 | (slot & 1)) << bits;
        extra = dist0 - base;
        if (slot < 14)
        {
                xz_rc_enc_bittree_rev(xz_dist_special + base - slot - 1, bits,
                                      extra);
                return;
        }
        xz_rc_enc_direct((p8)(bits - 4), extra >> 4);
        xz_rc_enc_bittree_rev(xz_dist_align, 4, extra & 15);
}

static fn xz_enc_literal(p8 byte)
{
        p8 prev = xz_dict_full ? xz_dict_get(1) : 0;
        positive lit_pos = xz_unpacked & (((positive)1 << xz_lp) - 1);
        p16 address_to probs = xz_lit +
                (((lit_pos << xz_lc) + (prev >> (8 - xz_lc))) * 0x300);
        positive ps = xz_unpacked & (((positive)1 << xz_pb) - 1);
        positive mode = 8;

        xz_rc_enc_bit(address_of xz_is_match[xz_state][ps], 0);
        if (xz_state >= 7)
                mode |= 0x200 | ((positive)xz_dict_get(xz_rep[0]) << 16);
        lzma_range_encode(address_of xz_rc, probs, byte, mode);
        xz_state = xz_state < 4 ? 0 : xz_state < 10 ? xz_state - 3 : xz_state - 6;
        xz_enc_seen(byte);
}

static fn xz_enc_match(positive dist, positive len)
{
        positive pos_state = (positive)xz_unpacked &
                             ((((positive)1 << xz_pb) - 1));

        xz_rc_enc_bit(address_of xz_is_match[xz_state][pos_state], 1);
        xz_rc_enc_bit(address_of xz_is_rep[xz_state], 0);
        xz_enc_len(address_of xz_match_choice, address_of xz_match_choice2,
                   (p16 address_to)xz_match_low, (p16 address_to)xz_match_mid,
                   xz_match_high, pos_state, len);
        xz_enc_dist(len < 6 ? len - 2 : 3, dist - 1);
        xz_rep[3] = xz_rep[2];
        xz_rep[2] = xz_rep[1];
        xz_rep[1] = xz_rep[0];
        xz_rep[0] = dist;
        xz_state = xz_state < 7 ? 7 : 10;
}

static fn xz_enc_repeat(positive which, positive len)
{
        positive ps = (positive)xz_unpacked & (((positive)1 << xz_pb) - 1);
        positive dist = xz_rep[which];

        xz_rc_enc_bit(address_of xz_is_match[xz_state][ps], 1);
        xz_rc_enc_bit(address_of xz_is_rep[xz_state], 1);
        xz_rc_enc_bit(address_of xz_is_rep0[xz_state], which != 0);
        if (!which)
                xz_rc_enc_bit(address_of xz_is_rep0_long[xz_state][ps], 1);
        else
        {
                xz_rc_enc_bit(address_of xz_is_rep1[xz_state], which != 1);
                if (which >= 2)
                        xz_rc_enc_bit(address_of xz_is_rep2[xz_state], which == 3);
                for (; which; which--)
                        xz_rep[which] = xz_rep[which - 1];
                xz_rep[0] = dist;
        }
        xz_enc_len(address_of xz_rep_choice, address_of xz_rep_choice2,
                   (p16 address_to)xz_rep_low, (p16 address_to)xz_rep_mid,
                   xz_rep_high, ps, len);
        xz_state = xz_state < 7 ? 8 : 11;
}

static p16 xz_chunk_hash(p8 address_to p)
{
        p32 h = ((p32)p[0] << 16) ^ ((p32)p[1] << 8) ^ p[2];

        h *= 0x1e35a7bdu;
        return (p16)(h >> 16);
}

static bool xz_lzma_chunk(p8 address_to src, positive n)
{
        positive pos;
        p8 reset;
        positive at;

        if (!n)
                return true;
        reset = xz_need_reset ? 3 : xz_enc_have_lzma ? 0 : 2;
        if (xz_need_reset)
        {
                xz_dict_pos = 0;
                xz_dict_full = 0;
                xz_need_reset = false;
        }
        if (reset >= 2)
        {
                if (!xz_props(0x5d))
                        return false;
                xz_probs_reset();
        }
        xz_rc_enc_init();
        xz_inner_stamp += 0x10000;
        if (!(xz_inner_stamp & 0xffff0000u))
        {
                memory_fill(xz_head, 0, sizeof(xz_head));
                xz_inner_stamp = 0x10000;
        }
        pos = 0;
        while (pos < n)
        {
                positive match = 0;
                positive dist = 0;
                positive rep_len = 0;
                positive rep_index = 0;
                positive r;
                positive limit = n - pos;
                if (limit > XZ_MATCH_MAX)
                        limit = XZ_MATCH_MAX;
                for (r = 0; r < 4; r++)
                {
                        positive d = xz_rep[r];
                        if (d <= pos && d <= xz_dict_full && limit >= 2 &&
                            src[pos] == src[pos - d] &&
                            src[pos + 1] == src[pos - d + 1])
                        {
                                positive k = memory_common_prefix(src + pos,
                                                        src + pos - d, limit);
                                if (k > rep_len)
                                        rep_len = k, rep_index = r;
                        }
                }

                if (pos + 3 <= n)
                {
                        p16 h = xz_chunk_hash(src + pos);
                        p32 old = xz_head[h];
                        positive chain = (old & 0xffff0000u) == xz_inner_stamp
                                            ? old & 0xffff : 0;
                        positive tries = xz_level <= 1 ? 4 : xz_level <= 3 ? 8 : 32;
                        positive nice = xz_level <= 1 ? 32 : xz_level <= 3 ? 64
                                                                                  : XZ_MATCH_MAX;
                        xz_prev[pos] = (p16)chain;
                        xz_head[h] = xz_inner_stamp | (p16)(pos + 1);
                        while (chain && tries--)
                        {
                                positive there = chain - 1;
                                if (there >= pos)
                                        break;
                                positive d = pos - there;
                                if (d <= xz_dict_full &&
                                    src[pos] == src[there] &&
                                    (!match || src[pos + match] == src[there + match]))
                                {
                                        positive k = memory_common_prefix(src + pos,
                                                                          src + there, limit);
                                        if (k >= (xz_level <= 1 && d >= 128 ? 4 : 3) &&
                                            k > match)
                                        {
                                                match = k;
                                                dist = d;
                                                if (k >= nice || k == limit)
                                                        break;
                                        }
                                }
                                positive next = xz_prev[there];
                                if (next >= chain)
                                        break;
                                chain = next;
                        }
                }
                /* A repeat avoids coding a new distance. Prefer it when
                   it saves that price at a cost of at most one byte. */
                bool repeat = rep_len >= 2 && rep_len + 1 >= match;
                if (repeat)
                        match = rep_len, dist = xz_rep[rep_index];
                if (match && dist)
                {
                        positive k;

                        if (repeat)
                                xz_enc_repeat(rep_index, match);
                        else
                                xz_enc_match(dist, match);
                        xz_enc_seen_span(src + pos, match);
                        for (k = 1; k < match; k++)
                        {
                                if (pos + k + 3 <= n)
                                {
                                        p16 hh = xz_chunk_hash(src + pos + k);

                                        p32 old = xz_head[hh];
                                        xz_prev[pos + k] =
                                            (old & 0xffff0000u) == xz_inner_stamp
                                                ? (p16)old : 0;
                                        xz_head[hh] = xz_inner_stamp |
                                                      (p16)(pos + k + 1);
                                }
                        }
                        pos += match;
                }
                else
                {
                        xz_enc_literal(src[pos]);
                        pos++;
                }
        }
        if (xz_check == XZ_CHECK_CRC32)
                xz_crc32 = hash_crc32(xz_crc32, src, n);
        else if (xz_check == XZ_CHECK_CRC64)
                xz_crc64 = hash_crc64(xz_crc64, src, n);
        xz_rc_enc_flush();
        xz_rc_n = (positive)(xz_rc.next - xz_rc_buf);
        if (xz_rc_full)
                return xz_fail("xz compressed chunk");
        if (!xz_rc_n || xz_rc_n > 65536 || xz_rc_n >= n)
        {
                /* Speculative probabilities/reps were not sent. The next
                   compressed chunk must start a fresh LZMA model. */
                xz_enc_have_lzma = false;
                return xz_write_uncompressed_chunk(src, n, reset >= 3);
        }
        xz_put((p8)(0x80 | (reset << 5) | ((n - 1) >> 16)));
        xz_put((p8)((n - 1) >> 8));
        xz_put((p8)(n - 1));
        xz_put((p8)((xz_rc_n - 1) >> 8));
        xz_put((p8)(xz_rc_n - 1));
        if (reset >= 2)
                xz_put(0x5d);
        for (at = 0; at < xz_rc_n; at++)
                xz_put(xz_rc_buf[at]);
        xz_block_unpadded += 5 + (reset >= 2 ? 1 : 0) + xz_rc_n;
        xz_block_unpacked += n;
        xz_enc_have_lzma = true;
        return true;
}

static bool xz_flush_pending(bool last)
{
        positive at = 0;

        if (!xz_pending_n && !last)
                return true;
        if (!xz_pending_n && last && !xz_block_unpadded)
                return true;
        if (!xz_block_unpadded)
                xz_write_block_header(xz_prop_from_dict(65536));
        while (at < xz_pending_n)
        {
                positive take = xz_pending_n - at;

                if (take > 65536)
                        take = 65536;
                if (!xz_lzma_chunk(xz_pending + at, take))
                        return false;
                at += take;
        }
        xz_pending_n = 0;
        if (last)
        {
                xz_put(0);
                xz_block_unpadded += 1;
                {
                        p64 padded = xz_block_unpadded;

                        while (padded & 3)
                        {
                                xz_put(0);
                                padded++;
                        }
                }
                if (xz_check == XZ_CHECK_CRC32)
                {
                        xz_put32(~xz_crc32);
                        xz_block_unpadded += 4;
                }
                else if (xz_check == XZ_CHECK_CRC64)
                {
                        xz_put64(~xz_crc64);
                        xz_block_unpadded += 8;
                }
                if (xz_index_n < 32)
                {
                        xz_index_unpadded[xz_index_n] = xz_block_unpadded;
                        xz_index_unpacked[xz_index_n] = xz_block_unpacked;
                        xz_index_n++;
                }
        }
        return true;
}

static bool xz_enc_pull(void)
{
        bipolar got;
        positive room;

        if (xz_hold_eof)
                return true;
        /* This is an input staging slab. The LZMA matcher owns its history
           in pending/dict; retaining bytes here only recopies dead input. */
        if (xz_hold_at)
        {
                positive drop = xz_hold_at;

                memory_copy_apart(xz_hold, xz_hold + drop, xz_hold_fill - drop);
                xz_hold_fill -= drop;
                xz_hold_at -= drop;
                xz_hold_abs += drop;
        }
        room = sizeof(xz_hold) - xz_hold_fill;
        if (!room)
                return true;
        if (xz_feed)
        {
                positive left = xz_feed_len - xz_feed_at;
                positive take = left > room ? room : left;

                if (take)
                {
                        memory_copy(xz_hold + xz_hold_fill, xz_feed + xz_feed_at,
                                    take);
                        xz_hold_fill += take;
                        xz_feed_at += take;
                }
                if (xz_feed_at >= xz_feed_len)
                        xz_feed = null;
                return true;
        }
        if (xz_enc_mem)
        {
                positive left = xz_enc_mem_len - xz_enc_mem_at;
                positive take = left > room ? room : left;

                if (take)
                {
                        memory_copy(xz_hold + xz_hold_fill,
                                    xz_enc_mem + xz_enc_mem_at, take);
                        xz_hold_fill += take;
                        xz_enc_mem_at += take;
                }
                if (xz_enc_mem_at >= xz_enc_mem_len)
                        xz_hold_eof = true;
                return true;
        }
        if (xz_in_fd < 0)
                return true;
        got = system_read_retry((positive)xz_in_fd, xz_hold + xz_hold_fill, room);
        if (got < 0)
                return xz_fail("xz read failed");
        if (!got)
                xz_hold_eof = true;
        else
                xz_hold_fill += (positive)got;
        return true;
}

static bool xz_encode_body(bool finish)
{
        for (;;)
        {
                positive have;
                positive room;
                positive take;

                if (xz_hold_fill - xz_hold_at < 1 && !xz_hold_eof)
                {
                        if (!xz_enc_pull())
                                return false;
                        if (xz_hold_at >= xz_hold_fill)
                        {
                                if (xz_hold_eof)
                                        break;
                                if (!finish)
                                        return true;
                                continue;
                        }
                }
                if (xz_hold_at >= xz_hold_fill)
                        break;
                if (xz_pending_n == sizeof(xz_pending) &&
                    !xz_flush_pending(false))
                        return false;
                have = xz_hold_fill - xz_hold_at;
                room = sizeof(xz_pending) - xz_pending_n;
                take = have < room ? have : room;
                memory_copy(xz_pending + xz_pending_n, xz_hold + xz_hold_at,
                            take);
                xz_pending_n += take;
                xz_hold_at += take;
                if (xz_pending_n == sizeof(xz_pending) &&
                    !xz_flush_pending(false))
                        return false;
                if (!finish && xz_hold_at >= xz_hold_fill && !xz_hold_eof)
                        return true;
        }
        if (finish)
                return xz_flush_pending(true);
        return true;
}

static bool xz_encode_setup(p8 level)
{
        xz_why = null;
        xz_level = level ? level : 6;
        xz_out_fill = 0;
        xz_hold_fill = 0;
        xz_hold_at = 0;
        xz_hold_abs = 0;
        xz_hold_eof = false;
        xz_feed = null;
        xz_pending_n = 0;
        xz_need_reset = true;
        xz_enc_have_lzma = false;
        xz_unpacked = 0;
        xz_crc32 = 0xffffffffu;
        xz_crc64 = 0xffffffffffffffffull;
        xz_index_n = 0;
        xz_inner_stamp = 0;
        memory_fill(xz_head, 0, sizeof(xz_head));
        if (!xz_dict_open(65536))
                return false;
        if (!xz_props(0x5d))
                return false;
        xz_probs_reset();
        return xz_write_header(XZ_CHECK_CRC32);
}

static bool xz_stream_encode(p8 level)
{
        if (!xz_encode_setup(level))
                return false;
        for (;;)
        {
                if (!xz_enc_pull())
                        return false;
                if (xz_hold_eof && xz_hold_at >= xz_hold_fill)
                        break;
                if (xz_hold_at >= xz_hold_fill)
                        continue;
                if (!xz_encode_body(false))
                        return false;
        }
        if (!xz_encode_body(true))
                return false;
        return xz_write_index_footer();
}

static bipolar xz_deflate_mem(p8 address_to src, positive src_len,
                              p8 address_to dst, positive dst_cap, p8 level)
{
        bool ok;

        xz_in_fd = -1;
        xz_enc_mem = src;
        xz_enc_mem_len = src_len;
        xz_enc_mem_at = 0;
        xz_out_fd = -1;
        xz_out_mem = dst;
        xz_out_cap = dst_cap;
        xz_out_used = 0;
        ok = xz_stream_encode(level);
        xz_enc_mem = null;
        xz_out_mem = null;
        return ok ? (bipolar)xz_out_used : -1;
}

static fn xz_in_from_fd(bipolar in)
{
        xz_in_fd = in;
        xz_in_mem = null;
        xz_in_mem_len = 0;
        xz_in_mem_at = 0;
        xz_in_at = 0;
        xz_in_have = 0;
        xz_in_eof = false;
}

static bool xz_decode_begin(bipolar in)
{
        xz_why = null;
        xz_in_from_fd(in);
        xz_out_fd = -1;
        xz_out_mem = null;
        xz_out_fill = 0;
        xz_pull = true;
        xz_paused = false;
        xz_finished = false;
        xz_stream_open = false;
        xz_hdr_done = false;
        xz_block_live = false;
        xz_lz2_kind = 0;
        xz_lz2_have = false;
        xz_lz2_match_left = 0;
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
        xz_in_have = n;
        xz_in_at = 0;
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
                        take = xz_out_fill > n - copied ? n - copied : xz_out_fill;
                        memory_copy(dst + copied, xz_out_buf, take);
                        if (take < xz_out_fill)
                                memory_copy_apart(xz_out_buf, xz_out_buf + take,
                                                  xz_out_fill - take);
                        xz_out_fill -= take;
                        copied += take;
                        continue;
                }
                if (xz_finished)
                        break;
                if (!xz_in_need(1) && !xz_hdr_done && !xz_block_live)
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
                    !xz_in_need(1))
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
        xz_out_mem = null;
        xz_in_fd = -1;
        xz_enc_mem = null;
        xz_feed = null;
        return xz_encode_setup(level);
}

static bool xz_encode_write(p8 address_to src, positive n)
{
        xz_feed = src;
        xz_feed_len = n;
        xz_feed_at = 0;
        xz_hold_eof = false;
        while (xz_feed)
        {
                if (!xz_enc_pull())
                        return false;
                if (!xz_encode_body(false))
                        return false;
                if (xz_feed && xz_feed_at >= xz_feed_len)
                        xz_feed = null;
        }
        return xz_out_flush();
}

static bool xz_encode_end(void)
{
        xz_feed = null;
        xz_hold_eof = true;
        if (!xz_encode_body(true))
                return false;
        return xz_write_index_footer();
}

#ifndef XZ_CORE_ONLY

static b32 xz_stream_cli(bipolar in, bipolar out, bool decode, p8 level)
{
        bool ok;

        xz_in_fd = in;
        xz_in_mem = null;
        xz_out_fd = out;
        xz_out_mem = null;
        xz_enc_mem = null;
        xz_feed = null;
        xz_status = 0;
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

static fn xz_refuse(string_address message)
{
        string_format(log_error, "xz: %s\n", message);
        xz_status = 1;
}

static string_address xz_called(void)
{
        string_address path = program_argument(0);
        string_address slash;

        if (!path)
                return "xz";
        slash = string_last_of(path, '/');
        return slash && slash[1] ? slash + 1 : path;
}

static bool xz_suffix_out(string_address in, p8 address_to into, positive room,
                          bool decode)
{
        positive n = string_length(in);

        if (decode)
        {
                if (n >= 3 && !memory_compare(in + n - 3, ".xz", 3))
                {
                        if (n - 2 >= room)
                                return false;
                        memory_copy(into, in, n - 3);
                        into[n - 3] = end;
                        return true;
                }
                if (n >= 4 && !memory_compare(in + n - 4, ".txz", 4))
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
        memory_copy(into + n, ".xz", 4);
        return true;
}

static b32 file_xz(void)
{
        string_address name = xz_called();
        positive count = (positive)program_argument_count();
        positive at;
        bool decompress = string_equals(name, "unxz") ||
                          string_equals(name, "xzcat");
        bool stdout_out = string_equals(name, "xzcat");
        bool force = false;
        bool test = false;
        bool remove_src = true;
        p8 level = 6;
        p8 out_name[4096];

        xz_status = 0;
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
                        if (string_equals(word, "--help"))
                        {
                                string_format(log,
                                              "Usage: xz [-cdfkqt123456789] [FILE...]\n");
                                log_flush();
                                return 0;
                        }
                        if (string_equals(word, "--version"))
                        {
                                string_format(log, "xz from dawning-kit\n");
                                log_flush();
                                return 0;
                        }
                        string_format(log_error, "xz: unrecognized option '%s'\n",
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
                                else if (*letters == 'q')
                                        ;
                                else if (*letters == 't')
                                {
                                        test = true;
                                        decompress = true;
                                }
                                else if (*letters == 'h')
                                {
                                        string_format(log,
                                                      "Usage: xz [-cdfkqt123456789] [FILE...]\n");
                                        log_flush();
                                        return 0;
                                }
                                else
                                {
                                        p8 shown[2];

                                        shown[0] = *letters;
                                        shown[1] = end;
                                        string_format(log_error,
                                                      "xz: invalid option -- '%s'\n",
                                                      shown);
                                        return 2;
                                }
                        }
                }
        }

        if (at >= count)
        {
                bipolar out = test ? -1 : 1;

                return xz_stream_cli(0, out, decompress, level);
        }

        for (; at < count && !xz_status; at++)
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
                                string_format(log_error, "xz: %s: %s\n", path,
                                              file_reason(in));
                                xz_status = 1;
                                break;
                        }
                        close_in = true;
                        if (test)
                                out = -1;
                        else if (stdout_out)
                                out = 1;
                        else if (!xz_suffix_out(path, out_name, sizeof(out_name),
                                                decompress))
                        {
                                xz_refuse(decompress ? "unknown suffix; use -c"
                                                     : "cannot guess output name");
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
                                        string_format(log_error, "xz: %s: %s\n",
                                                      out_name, file_reason(out));
                                        system_close(in);
                                        xz_status = 1;
                                        break;
                                }
                                close_out = true;
                        }
                }

                xz_stream_cli(in, out, decompress, level);
                if (close_in)
                        system_close(in);
                if (close_out)
                        system_close(out);
                if (!xz_status && remove_src && close_in && !stdout_out && !test)
                        system_remove_at(AT_FDCWD, path, 0);
        }

        log_flush();
        return xz_status;
}

#endif /* XZ_CORE_ONLY */
