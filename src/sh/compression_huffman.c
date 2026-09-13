/* Shared Huffman codebook construction: 1..288 symbols, block-sized counts.
   Clipped depths require the caller's Kraft check before emitting a tree.
   Per-symbol bit emission lives in the codec/assembly streaming kernels. */
#ifndef COMPRESSION_HUFFMAN_INCLUDED
#define COMPRESSION_HUFFMAN_INCLUDED

typedef struct
{
        p32 freq;
        b32 dad;
} compression_tree;

static fn compression_heap_up(compression_tree address_to node, p32 address_to heap,
                       positive at)
{
        p32 item = heap[at];
        p32 freq = node[item].freq;

        while (at > 1)
        {
                positive parent = at >> 1;

                if (node[heap[parent]].freq < freq ||
                    (node[heap[parent]].freq == freq &&
                     heap[parent] <= item))
                        break;
                heap[at] = heap[parent];
                at = parent;
        }
        heap[at] = item;
}

static fn compression_heap_down(compression_tree address_to node, p32 address_to heap,
                         positive used, positive at)
{
        p32 item = heap[at];
        p32 freq = node[item].freq;

        for (;;)
        {
                positive child = at << 1;

                if (child > used)
                        break;
                if (child < used &&
                    (node[heap[child + 1]].freq < node[heap[child]].freq ||
                     (node[heap[child + 1]].freq == node[heap[child]].freq &&
                      heap[child + 1] < heap[child])))
                        child++;
                if (freq < node[heap[child]].freq ||
                    (freq == node[heap[child]].freq && item <= heap[child]))
                        break;
                heap[at] = heap[child];
                at = child;
        }
        heap[at] = item;
}

static bool compression_build_lengths(p32 address_to freq, positive n, p8 address_to length,
                               p8 limit)
{
        compression_tree node[288 * 2];
        p32 heap[288 * 2];
        positive used = 0;
        positive at;
        positive next;

        memory_fill(length, 0, n);
        for (at = 0; at < n; at++)
        {
                node[at].freq = freq[at] ? freq[at] : 0;
                node[at].dad = -1;
        }

        for (at = 0; at < n; at++)
                if (node[at].freq)
                {
                        used++;
                        heap[used] = (p32)at;
                        compression_heap_up(node, heap, used);
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
                length[heap[1]] = 1;
                if (heap[1] == 0 && n > 1)
                        length[1] = 1;
                else if (n)
                        length[0] = 1;
                return true;
        }

        next = n;
        while (used > 1)
        {
                p32 first;
                p32 second;

                first = heap[1];
                heap[1] = heap[used--];
                compression_heap_down(node, heap, used, 1);
                second = heap[1];
                node[next].freq = node[first].freq + node[second].freq;
                node[first].dad = (b32)next;
                node[second].dad = (b32)next;
                node[next].dad = -1;
                heap[1] = (p32)next;
                compression_heap_down(node, heap, used, 1);
                next++;
        }
        for (at = 0; at < n; at++)
        {
                bipolar walk;
                p8 depth = 0;

                if (!freq[at])
                        continue;
                walk = node[at].dad;
                while (walk >= 0)
                {
                        depth++;
                        walk = node[walk].dad;
                        if (depth > 32)
                                break;
                }
                if (depth > limit)
                        depth = limit;
                if (!depth)
                        depth = 1;
                length[at] = depth;
        }

        return true;
}

#endif
