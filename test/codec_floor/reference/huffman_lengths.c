/* Semantic reference for huffman_lengths: code lengths of at most limit
   bits (1..15) for n <= 288 counts. The symbols in use go in order of
   count, equal counts in symbol order, by a byte of the count a pass; the
   two least are joined repeatedly, a leaf taken while its count is no more
   than the next join's -- a heap's order of (count, node), as a join is
   numbered after every leaf. Depths past limit are cut to it and the code
   space made exact again by splitting a shorter code for each surplus,
   the longest codes going to the least counts. Returns 0 when that cannot
   be done; an empty or one-symbol alphabet gets two codes of one bit. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

int huffman_lengths(const u32 *freq, u64 n, u8 *length, u64 limit)
{
        u32 order[288], spare[288], joined[288], up[576];
        u8 deep[288];
        u32 *from = order, *into = spare, top = 0;
        u64 used = 0, counts[16] = {0}, slots = 0;

        for (u64 at = 0; at < n; at++)
                length[at] = 0;
        for (u64 at = 0; at < n; at++)
                if (freq[at])
                {
                        order[used++] = (u32)at;
                        top |= freq[at];
                }
        for (u32 shift = 0; shift < 32 && (top >> shift); shift += 8)
        {
                u32 start[257] = {0}, *swap;
                for (u64 i = 0; i < used; i++)
                        start[((freq[from[i]] >> shift) & 255) + 1]++;
                for (u64 d = 1; d < 256; d++)
                        start[d] += start[d - 1];
                for (u64 i = 0; i < used; i++)
                        into[start[(freq[from[i]] >> shift) & 255]++] = from[i];
                swap = from; from = into; into = swap;
        }
        if (!used)
        {
                length[0] = 1;
                if (n > 1) length[1] = 1;
                return 1;
        }
        if (used == 1)
        {
                length[from[0]] = 1;
                if (from[0] == 0 && n > 1) length[1] = 1;
                else if (n) length[0] = 1;
                return 1;
        }
        {
                u64 leaf = 0, head = 0, made = 0;
                while (used - leaf + made - head > 1)
                {
                        u32 pick[2], weight[2];
                        for (int k = 0; k < 2; k++)
                                if (leaf < used && (head == made || freq[from[leaf]] <= joined[head]))
                                {
                                        weight[k] = freq[from[leaf]];
                                        pick[k] = (u32)leaf++;
                                }
                                else
                                {
                                        weight[k] = joined[head];
                                        pick[k] = (u32)(used + head++);
                                }
                        joined[made] = weight[0] + weight[1];
                        up[pick[0]] = (u32)(used + made);
                        up[pick[1]] = (u32)(used + made);
                        made++;
                }
                deep[made - 1] = 0;
                for (u64 j = made - 1; j--;)
                        deep[j] = (u8)(deep[up[used + j] - used] + 1);
        }
        for (u64 i = 0; i < used; i++)
        {
                u64 depth = (u64)deep[up[i] - used] + 1;
                if (depth > limit) depth = limit;
                length[from[i]] = (u8)depth;
                counts[depth]++;
                slots += (u64)1 << (limit - depth);
        }
        u64 capacity = (u64)1 << limit;
        if (slots > capacity)
        {
                while (slots > capacity)
                {
                        u64 bits = limit - 1;
                        while (bits && !counts[bits]) bits--;
                        if (!bits || !counts[limit]) return 0;
                        counts[bits]--;
                        counts[bits + 1] += 2;
                        counts[limit]--;
                        slots--;
                }
                u64 item = 0;
                for (u64 bits = limit; bits; bits--)
                        for (u64 take = 0; take < counts[bits]; take++)
                                length[from[item++]] = (u8)bits;
        }
        return 1;
}
