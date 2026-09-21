/* Shared Huffman codebook construction: 1..288 symbols, limits 1..15,
   block-sized counts.
   Length limiting redistributes complete code space before assigning lengths.
   Per-symbol bit emission lives in the codec/assembly streaming kernels. */
#ifndef COMPRESSION_HUFFMAN_INCLUDED
#define COMPRESSION_HUFFMAN_INCLUDED

/* The match finders' hash of the three bytes at a position: 16 bits. */
static inline INLINE p16 compression_hash3(p8 address_to bytes)
{
        p32 h = ((p32)bytes[0] << 16) ^ ((p32)bytes[1] << 8) ^ bytes[2];

        h *= 0x1e35a7bdu;
        return (p16)(h >> 16);
}

/*
        Code lengths for the counts, limited to limit bits. The symbols in
        use go in order of count, equal counts in symbol order, by a byte
        of the count a pass; then the two least are joined repeatedly,
        taking from the leaves while theirs is no more than the next join's.
        That is a heap's order of (count, node) exactly -- a join is always
        numbered after every leaf -- so the tree is the heap's, in time
        linear past the sort.
*/
static bool compression_build_lengths(p32 address_to freq, positive n, p8 address_to length,
                                      p8 limit)
{
        p32 order[288];
        p32 spare[288];
        p32 joined[288];
        p32 up[576];
        p8 deep[288];
        p32 address_to from = order;
        p32 address_to into = spare;
        p32 top = 0;
        positive used = 0;
        positive counts[16] = {0};
        positive slots = 0;

        memory_fill(length, 0, n);
        for (positive at = 0; at < n; at++)
                if (freq[at])
                {
                        order[used++] = (p32)at;
                        top |= freq[at];
                }
        for (p8 shift = 0; shift < 32 && (top >> shift); shift += 8)
        {
                positive start[257] = {0};
                p32 address_to swap;

                for (positive i = 0; i < used; i++)
                        start[((freq[from[i]] >> shift) & 255) + 1]++;
                for (positive d = 1; d < 256; d++)
                        start[d] += start[d - 1];
                for (positive i = 0; i < used; i++)
                        into[start[(freq[from[i]] >> shift) & 255]++] = from[i];
                swap = from;
                from = into;
                into = swap;
        }

        if (!used)
        {
                length[0] = 1;
                if (n > 1)
                        length[1] = 1;
                return true;
        }
        if (used == 1)
        {
                length[from[0]] = 1;
                if (from[0] == 0 && n > 1)
                        length[1] = 1;
                else if (n)
                        length[0] = 1;
                return true;
        }

        //      Node i below used is the leaf from[i]; used + j is join j.
        {
                positive leaf = 0;
                positive head = 0;
                positive made = 0;

                while (used - leaf + made - head > 1)
                {
                        p32 pick[2];
                        p32 weight[2];

                        for (positive k = 0; k < 2; k++)
                                if (leaf < used && (head == made || freq[from[leaf]] <= joined[head]))
                                {
                                        weight[k] = freq[from[leaf]];
                                        pick[k] = (p32)leaf++;
                                }
                                else
                                {
                                        weight[k] = joined[head];
                                        pick[k] = (p32)(used + head++);
                                }
                        joined[made] = weight[0] + weight[1];
                        up[pick[0]] = (p32)(used + made);
                        up[pick[1]] = (p32)(used + made);
                        made++;
                }
                deep[made - 1] = 0;
                for (positive j = made - 1; j--;)
                        deep[j] = (p8)(deep[up[used + j] - used] + 1);
        }
        for (positive i = 0; i < used; i++)
        {
                positive depth = (positive)deep[up[i] - used] + 1;

                if (depth > limit)
                        depth = limit;
                length[from[i]] = (p8)depth;
                counts[depth]++;
                slots += (positive)1 << (limit - depth);
        }

        /* Merely truncating deep leaves oversubscribes the code space and
           forces entire blocks into fixed or stored output. Split a shorter
           code and remove one maximum-length code until the Kraft sum is
           exact, preserving the number of leaves at every step; then the
           longest codes go to the least counts. */
        positive capacity = (positive)1 << limit;
        if (slots > capacity)
        {
                while (slots > capacity)
                {
                        positive bits = limit - 1;
                        while (bits && !counts[bits]) bits--;
                        if (!bits || !counts[limit]) return false;
                        counts[bits]--;
                        counts[bits + 1] += 2;
                        counts[limit]--;
                        slots--;
                }
                positive item = 0;
                for (positive bits = limit; bits; bits--)
                        for (positive take = 0; take < counts[bits]; take++)
                                length[from[item++]] = (p8)bits;
        }

        return true;
}

#endif
