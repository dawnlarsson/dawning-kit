#include "span.h"
/* LZMA packet kernel. The caller guarantees 48 readable input bytes from
   any next < in_stop, and at least 32 writable bytes past copy_end. Every
   packet starts at out < out_stop <= out_end. Buffer index (out - base)
   is congruent to the uncompressed position since the dictionary reset
   modulo 16, and out[-1] is the previous byte (zero after a reset).

   A source out - rep0 >= lo is valid without further checks. Below lo the
   exact rule applies: rep0 <= dmax, and a source below bottom is remapped
   by wrap when wrap is nonzero (the liblzma mirror) or is invalid. */
#define TOP (1u << 24)
#define INLINE static inline __attribute__((always_inline))
#define NORM() do { if (range < TOP) { range <<= 8; code = (code << 8) | *in++; } } while (0)
/* Decision bits branch. */
#define NORM_E() (range < TOP ? (range <<= 8, code = (code << 8) | *in++) : 0)
#define IS0(cell) (NORM_E(), bound = (range >> 11) * (cell), code < bound)
#define ZERO(cell) do { range = bound; (cell) += (2048 - (cell)) >> 5; } while (0)
#define ONE(cell) do { range -= bound; code -= bound; (cell) -= (cell) >> 5; } while (0)
/* Tree bits do not. With p in [31, 2017], p - ((p - 2017) >> 5) is the
   zero update and p - (p >> 5) the one update. */
#define TBIT(cell, bit) do { \
        u16 *c_ = (cell); u32 p_ = *c_; NORM(); \
        u32 b_ = (range >> 11) * p_; \
        u32 one_ = code >= b_; \
        u32 r_ = range - b_, k_ = code - b_; \
        range = one_ ? r_ : b_; code = one_ ? k_ : code; \
        *c_ = (u16)(p_ - ((int)(one_ ? p_ : p_ - 2017) >> 5)); \
        bit = one_; } while (0)
#define FWD(probs, sym) do { u32 b__; TBIT((probs) + sym, b__); sym = (sym << 1) + b__; } while (0)
#define REV(probs, v, i) do { u32 b__; TBIT((probs) + (1u << (i)) + v, b__); v += b__ << (i); } while (0)

void lzma_decode_span(job *j)
{
        u32 range = j->range, code = j->code, bound;
        const u8 *in = j->next, *in_stop = j->in_stop;
        models *m = j->model;
        u8 *base = j->base, *out = j->out, *out_stop = j->out_stop;
        u64 rep0 = j->rep[0], rep1 = j->rep[1], rep2 = j->rep[2], rep3 = j->rep[3];
        u32 state = j->state, lc = j->lc;
        u32 pbmask = (1u << j->pb) - 1;
        u32 litmask = (0x100u << j->lp) - (0x100u >> lc);
        u8 *src;
        u32 len;

        while (in < in_stop && out < out_stop)
        {
                u32 pos = (u32)(out - base);
                u32 ps = pos & pbmask;
                u16 *cell = &m->is_match[state][ps];
                if (IS0(*cell))
                {
                        ZERO(*cell);
                        u16 *lit = m->lit + 3 * ((((pos << 8) + out[-1]) & litmask) << lc);
                        u32 sym = 1;
                        if (state < 7)
                        {
                                state = state < 4 ? 0 : state - 3;
                                FWD(lit, sym); FWD(lit, sym); FWD(lit, sym); FWD(lit, sym);
                                FWD(lit, sym); FWD(lit, sym); FWD(lit, sym); FWD(lit, sym);
                        }
                        else
                        {
                                state = state < 10 ? state - 3 : state - 6;
                                src = out - rep0;
                                if ((long long)(src - j->lo) < 0)
                                {
                                        if (rep0 > j->dmax) { j->error = 1; break; }
                                        if (src < j->bottom) { if (!j->wrap) { j->error = 1; break; } src += j->wrap; }
                                }
                                u32 match = *src, offset = 0x100;
                                for (int i = 0; i < 8; i++)
                                {
                                        match <<= 1;
                                        u32 mb = match & offset, b;
                                        TBIT(lit + offset + mb + sym, b);
                                        sym = (sym << 1) + b;
                                        offset &= ~(mb ^ (0u - b));
                                }
                        }
                        *out++ = (u8)sym;
                        continue;
                }
                ONE(*cell);
                if (IS0(m->is_rep[state]))
                {
                        ZERO(m->is_rep[state]);
                        state = state < 7 ? 7 : 10;
                        rep3 = rep2; rep2 = rep1; rep1 = rep0;
                        u32 sym = 1;
                        if (IS0(m->match_choice))
                        {
                                ZERO(m->match_choice);
                                u16 *t = m->match_low[ps];
                                FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                len = sym - 8 + 2;
                        }
                        else
                        {
                                ONE(m->match_choice);
                                if (IS0(m->match_choice2))
                                {
                                        ZERO(m->match_choice2);
                                        u16 *t = m->match_mid[ps];
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        len = sym - 8 + 10;
                                }
                                else
                                {
                                        ONE(m->match_choice2);
                                        u16 *t = m->match_high;
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        len = sym - 256 + 18;
                                }
                        }
                        u16 *t = m->dist_slot[len < 6 ? len - 2 : 3];
                        sym = 1;
                        FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym);
                        u32 slot = sym - 64;
                        if (slot < 4)
                                rep0 = slot + 1;
                        else
                        {
                                u32 bits = (slot >> 1) - 1;
                                u32 dist = (2 | (slot & 1)) << bits;
                                u32 v = 0;
                                if (slot < 14)
                                {
                                        u16 *t2 = m->dist_special + dist - slot - 1;
                                        for (u32 i = 0; i < bits; i++) REV(t2, v, i);
                                }
                                else
                                {
                                        for (u32 i = 0; i < bits - 4; i++)
                                        {
                                                NORM();
                                                range >>= 1;
                                                u32 k = code - range, one = code >= range;
                                                code = one ? k : code;
                                                v = (v << 1) + one;
                                        }
                                        u16 *a = m->dist_align;
                                        u32 low = 0;
                                        REV(a, low, 0); REV(a, low, 1); REV(a, low, 2); REV(a, low, 3);
                                        v = (v << 4) + low;
                                }
                                rep0 = (u64)dist + v + 1;
                        }
                }
                else
                {
                        ONE(m->is_rep[state]);
                        if (IS0(m->is_rep0[state]))
                        {
                                ZERO(m->is_rep0[state]);
                                u16 *c = &m->is_rep0_long[state][ps];
                                if (IS0(*c))
                                {
                                        ZERO(*c);
                                        state = state < 7 ? 9 : 11;
                                        src = out - rep0;
                                        if ((long long)(src - j->lo) < 0)
                                        {
                                                if (rep0 > j->dmax) { j->error = 1; break; }
                                                if (src < j->bottom) { if (!j->wrap) { j->error = 1; break; } src += j->wrap; }
                                        }
                                        *out++ = *src;
                                        continue;
                                }
                                ONE(*c);
                        }
                        else
                        {
                                ONE(m->is_rep0[state]);
                                u64 dist;
                                if (IS0(m->is_rep1[state]))
                                {
                                        ZERO(m->is_rep1[state]);
                                        dist = rep1;
                                }
                                else
                                {
                                        ONE(m->is_rep1[state]);
                                        if (IS0(m->is_rep2[state]))
                                        {
                                                ZERO(m->is_rep2[state]);
                                                dist = rep2;
                                        }
                                        else
                                        {
                                                ONE(m->is_rep2[state]);
                                                dist = rep3;
                                                rep3 = rep2;
                                        }
                                        rep2 = rep1;
                                }
                                rep1 = rep0;
                                rep0 = dist;
                        }
                        state = state < 7 ? 8 : 11;
                        u32 sym = 1;
                        if (IS0(m->rep_choice))
                        {
                                ZERO(m->rep_choice);
                                u16 *t = m->rep_low[ps];
                                FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                len = sym - 8 + 2;
                        }
                        else
                        {
                                ONE(m->rep_choice);
                                if (IS0(m->rep_choice2))
                                {
                                        ZERO(m->rep_choice2);
                                        u16 *t = m->rep_mid[ps];
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        len = sym - 8 + 10;
                                }
                                else
                                {
                                        ONE(m->rep_choice2);
                                        u16 *t = m->rep_high;
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        FWD(t, sym); FWD(t, sym); FWD(t, sym); FWD(t, sym);
                                        len = sym - 256 + 18;
                                }
                        }
                }
                src = out - rep0;
                if ((long long)(src - j->lo) < 0)
                {
                        if (rep0 > j->dmax) { j->error = 1; break; }
                        if (src < j->bottom) { if (!j->wrap) { j->error = 1; break; } src += j->wrap; }
                }
                u8 *end = out + len;
                if (end > j->copy_end)
                {
                        if (end > j->out_end) { j->error = 1; break; }
                        do *out++ = *src++; while (out < end);
                        continue;
                }
#if defined(__riscv)
                /* No misaligned word loads here: bytes, which also carry
                   forward overlap. */
                do *out++ = *src++; while (out < end);
                continue;
#endif
                if (rep0 < 8)
                {
                        /* Seed one period-aligned eight-byte stride, then copy
                           words: the pattern repeats every rep0 bytes. */
                        for (int i = 0; i < 8; i++) out[i] = src[i];
                        src = out + 8 - ((8 + rep0 - 1) / rep0) * rep0;
                        out += 8;
                }
                do
                {
                        u64 w;
                        __builtin_memcpy(&w, src, 8); __builtin_memcpy(out, &w, 8);
                        __builtin_memcpy(&w, src + 8, 8); __builtin_memcpy(out + 8, &w, 8);
                        out += 16; src += 16;
                }
                while (out < end);
                out = end;
        }
        j->range = range; j->code = code; j->next = (u8 *)in;
        j->out = out; j->state = state;
        j->rep[0] = rep0; j->rep[1] = rep1; j->rep[2] = rep2; j->rep[3] = rep3;
}
