    //
    //       string_last_of_or_end, string_compare_max, string_length_max and
    //       memory_first_of. The x86_64 block above carries the reasoning for
    //       all four; what follows is only where arm64 does it differently.
    //
    //       These four used to be absent here, under a comment saying arm64
    //       ships its own in the kernel and freestanding code can fall back to
    //       the generic C. That was true and it left the library uneven: a
    //       program calling string_length_max linked on x86_64 and did not
    //       here. They are present on all three now.
    //
    ".text\n"
    //
    //       The last match, not the first. Within a word the highest set flag
    //       is wanted rather than the lowest, and arm64 has no bsr: clz counts
    //       from the other end, so the bit index is 63 minus it.
    //
    //       Hunting the terminator itself is the one case where the answer is
    //       the end of the string rather than a match inside it. It gets a byte
    //       walk, as it does on x86_64, being far too rare to shape the scan.
    //
    ASM_FUNC(string_last_of_or_end)
    "and w1, w1, #0xff\n   cbnz w1, 1f\n"
    "0:  ldrb w2, [x0]\n   cbz w2, 8f\n   add x0, x0, #1\n   b 0b\n"
    "8:  ret\n"
    "1:  mov x10, #0x0101010101010101\n   mul x3, x1, x10\n   mov x14, #0\n"
    "mov x15, #0x7f7f7f7f7f7f7f7f\n"
    "and x4, x0, #7\n   bic x5, x0, #7  // align down: same page, cannot fault\n"
    "ldr x6, [x5]\n   mov x7, #-1\n   lsl x4, x4, #3\n   lsl x7, x7, x4\n"
    "eor x8, x6, x3\n   orn x8, x8, x7  // the prefix matches nothing\n"
    "orn x6, x6, x7  // and is no terminator\n"
    "b 3f\n"
    "2:  eor x8, x6, x3\n"
    "3:  and x9, x8, x15\n   add x9, x9, x15\n   orr x9, x9, x8\n   orr x9, x9, x15\n   mvn x9, x9\n"
    "and x12, x6, x15\n   add x12, x12, x15\n   orr x12, x12, x6\n   orr x12, x12, x15\n   mvn x12, x12\n"
    "cbnz x12, 5f\n"
    "cbz x9, 4f  // no terminator here, so any match beats the one before\n"
    "clz x11, x9\n   mov x13, #63\n   sub x11, x13, x11\n   add x14, x5, x11, lsr #3\n"
    "4:  add x5, x5, #8\n   ldr x6, [x5]\n   b 2b\n"
    "5:  neg x13, x12\n   and x13, x13, x12  // the lowest terminator flag\n"
    "sub x13, x13, #1\n   and x9, x9, x13  // only matches under it count\n"
    "cbz x9, 9f\n"
    "clz x11, x9\n   mov x13, #63\n   sub x11, x13, x11\n   add x14, x5, x11, lsr #3\n"
    "9:  mov x0, x14\n"
    ASM_RET
    ASM_END(string_last_of_or_end)
    //
    //       Eight bytes from each while the count allows it, which needs no
    //       page argument: n bounds the read, so an unaligned load is legal.
    //       Anything but "equal and no terminator" drops to the byte loop,
    //       which already stops in the right place and gets the sign right.
    //
    ASM_FUNC(string_compare_max)
    "mov w9, #0\n   cbz x2, 4f\n   mov x10, #0x0101010101010101\n"
    "1:  cmp x2, #8\n"
    "b.lo 2f\n   ldr x6, [x0]\n   ldr x7, [x1]\n   cmp x6, x7\n"
    "b.ne 2f  // differ: let the byte step find where\n"
    "sub x9, x6, x10\n   bic x9, x9, x6\n   and x9, x9, #0x8080808080808080\n"
    "cbnz x9, 3f  // a terminator in them: equal, and the strings end\n"
    "add x0, x0, #8\n   add x1, x1, #8\n   sub x2, x2, #8\n   b 1b\n"
    "2:  cbz x2, 3f\n   ldrb w6, [x0]\n   ldrb w7, [x1]\n   subs w9, w6, w7\n"
    "b.ne 4f\n"
    "cbz w7, 3f\n   add x0, x0, #1\n   add x1, x1, #1\n   sub x2, x2, #1\n   b 2b\n"
    "3:  mov w9, #0\n"
    "4:  mov w0, w9\n"
    ASM_RET
    ASM_END(string_compare_max)
    //
    //       string_length with a fence: the same align-down scan, stopped when
    //       the walk reaches the bound, and a terminator found past it clamped
    //       back to n -- which is what the answer is when there is none inside.
    //
    ASM_FUNC(string_length_max)
    "cbz x1, 9f\n   add x13, x0, x1  // one past the last byte we may report\n"
    "and x4, x0, #7\n   bic x5, x0, #7\n   ldr x6, [x5]\n"
    "mov x10, #0x0101010101010101\n"
    "cbz x4, 1f\n   lsl x4, x4, #3\n   mov x9, #-1\n   lsl x9, x9, x4\n"
    "orn x6, x6, x9  // ones below the string, so it cannot end there\n"
    "1:  sub x9, x6, x10\n   bic x9, x9, x6\n   and x9, x9, #0x8080808080808080\n"
    "cbnz x9, 2f\n   add x5, x5, #8\n   cmp x5, x13\n"
    "b.hs 8f  // nothing within the bound\n"
    "ldr x6, [x5]\n   b 1b\n"
    "2:  rbit x9, x9\n   clz x9, x9\n   add x5, x5, x9, lsr #3\n"
    "sub x0, x5, x0  // how far in the terminator is\n"
    "cmp x0, x1\n"
    "csel x0, x1, x0, hi  // never more than n\n"
    "ret\n"
    "8:  mov x0, x1\n   ret\n"
    "9:  mov x0, #0\n"
    ASM_RET
    ASM_END(string_length_max)
    //
    //       The byte hunt with a fence instead of a terminator, so there is one
    //       hunt here and not two. A match in the word that reaches past the
    //       bound is thrown away by comparing its address, which costs less
    //       than stopping the scan short would.
    //
    ASM_FUNC(memory_first_of)
    "mov x9, #0\n   cbz x2, 9f\n   and w1, w1, #0xff\n"
    "mov x10, #0x0101010101010101\n   mul x3, x1, x10  // the byte, in all eight positions\n"
    "add x13, x0, x2  // one past the last byte we may report\n"
    "and x4, x0, #7\n   bic x5, x0, #7  // align down: same page, cannot fault\n"
    "ldr x6, [x5]\n   lsl x4, x4, #3\n   mov x7, #-1\n   lsl x7, x7, x4\n"
    "eor x8, x6, x3  // the byte that matched is now zero\n"
    "orn x8, x8, x7  // and the bytes before the string cannot be\n"
    "b 2f\n"
    "1:  eor x8, x6, x3\n"
    "2:  sub x9, x8, x10\n   bic x9, x9, x8\n   and x9, x9, #0x8080808080808080\n"
    "cbnz x9, 3f\n   add x5, x5, #8\n   cmp x5, x13\n"
    "b.hs 8f\n   ldr x6, [x5]\n   b 1b\n"
    "3:  rbit x9, x9\n   clz x9, x9\n   lsr x9, x9, #3\n   add x9, x5, x9\n"
    "cmp x9, x13\n"
    "b.lo 9f  // inside the bound: that is the answer\n"
    "8:  mov x9, #0\n"
    "9:  mov x0, x9\n"
    ASM_RET
    ASM_END(memory_first_of)
