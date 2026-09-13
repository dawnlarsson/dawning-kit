typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct {
        u16 is_match[12][16];
        u16 is_rep[12];
        u16 is_rep0[12];
        u16 is_rep1[12];
        u16 is_rep2[12];
        u16 is_rep0_long[12][16];
        u16 dist_slot[4][64];
        u16 dist_special[128 - 14];
        u16 dist_align[16];
        u16 match_choice;
        u16 match_choice2;
        u16 match_low[16][8];
        u16 match_mid[16][8];
        u16 match_high[256];
        u16 rep_choice;
        u16 rep_choice2;
        u16 rep_low[16][8];
        u16 rep_mid[16][8];
        u16 rep_high[256];
        u16 lit[(768*16)];

} models;
typedef struct
{
        u32 range, code;
        u8 * next;
        u8 * limit;
        models * model;
        u8 * dict;
        u64 size, pos, full;
        u64 unpacked, stop;
        u64 room;
        u32 state, lc, lp, pb;
        u64 rep[4];
        u64 error;
} job;
