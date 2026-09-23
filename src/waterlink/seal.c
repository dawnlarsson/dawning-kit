/*
        Sealing and opening a waterlink datagram, over lib.c's assembly.

        AES-128-GCM, shaped to the datagram rather than to a general record.
        Because the datagram is whole blocks and the cleartext header is
        exactly one of them (waterlink.c says why), the header, box and tag
        sit at fixed offsets and the whole operation is one assembly body
        around two calls into lib.c: aes128_ctr_blocks once, ghash_blocks
        once. A datagram of
        acknowledgements alone is the same shape cut shorter. The general path in
        net.c pays for an arbitrary record -- a separate call for the tag's
        block, padding for partial blocks, and a framing layer around both --
        and a datagram needs none of it.

        The one trick is the tag's mask. GCM encrypts counter one for the tag
        and counters two onward for the text, which is one counter-mode run
        starting at one if the block in front of the text is zero. The header
        is that block: it is set aside, the slot is zeroed, seventy four
        blocks go through in a single call, and the first block out is the
        mask. Then the header comes back and is hashed as what it always was,
        the associated data.

        This is included where lib.c and net.c's crypto are already in scope;
        the prepared key is net.c's crypto_aesgcm_key.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_SEAL_INCLUDED
#define WATERLINK_SEAL_INCLUDED

#include "waterlink.c"

#define WATERLINK_BLOCKS (WATERLINK_DATAGRAM / 16)
#define WATERLINK_BOXED (1 + WATERLINK_PAYLOAD / 16) // header and box

/*
        The two bodies, one per machine, each around one call to
        aes128_ctr_blocks and one to ghash_blocks.

        Sealing: the box is zeroed from used to its end, and the counter
        runs over header and box from J0 -- the nonce is the header's
        counter, little endian, behind four zero bytes (WireGuard's layout;
        each direction of a session has its own key, so two ends counting up
        from one never use a nonce twice under one key), then GCM's block
        counter at one. The first block's key stream is the tag's mask; the
        header goes back, GCM's lengths block sits where the tag goes (the
        header is 128 bits of associated data, the box is the text), and
        header, box and lengths
        are hashed in one run -- one reduction of the state instead of two,
        which is the part of hashing a datagram nothing runs beside. The tag
        is the hash xored with the mask.

        Opening: the tag that arrived is kept, the lengths block borrows its
        slot, the hash is taken over the ciphertext and the slot given back;
        then the counter run decrypts, and the hash with the mask must equal
        the tag in all sixteen bytes, compared whole so a wrong tag says
        nothing about where it went wrong. A box that does not open is
        wiped: the counter run has already turned it into plaintext-shaped
        bytes, and unauthenticated plaintext is not something to leave in a
        buffer the caller might read.

        They were C around memory_copy, memory_zero and crypto_forget, and
        what that cost was not the calls: the zero state, the counter block,
        the lengths and the header went to memory a byte or eight bytes at a
        time just before the two routines loaded them sixteen or sixty four
        at a time, and a load that no single store can answer waits for every
        store in front of it -- the whole counter run's -- to reach the
        cache. On the 9950X ghash_blocks spent more of a seal waiting on its
        state than hashing. Here each of those blocks is one store, made as
        early as it can be; the header is never zeroed, since the run turns
        it into the header xored with the mask and the mask is that xored
        with the header again; and the hash, the mask and the tag live in a
        few words of stack that are wiped before returning. The key is
        net.c's prepared key: the 176-byte schedule, then the GHASH table at
        192.
*/
_Static_assert(__builtin_offsetof(crypto_aesgcm_key, table) == 192,
               "the seal bodies find the GHASH table at 192");

fn waterlink_seal_box(crypto_aesgcm_key address_to key, p8 address_to datagram,
                      positive used, positive box);
bool waterlink_open_box(crypto_aesgcm_key address_to key,
                        p8 address_to datagram, positive box);

#if X64
__asm__(
    ASM_FUNC(waterlink_seal_box)
    "push %rbx\n   push %r12\n   push %r13\n   push %r14\n   push %r15\n"
    "sub $48, %rsp\n"
    "mov %rdi, %rbx\n   mov %rsi, %r12\n   mov %rcx, %r13\n"
    "cmp %rcx, %rdx\n   jae 1f\n"
    "lea 16(%rsi,%rdx), %rdi\n   mov %rcx, %rsi\n   sub %rdx, %rsi\n"
    "call memory_zero\n"
    //  Every block the two calls load is written whole, in one store, and
    //  long enough before: a block written in two halves cannot be forwarded
    //  to a load of all of it, and the load then waits for every store in
    //  front of it to reach the cache -- the counter run's included. The
    //  lengths go in the tag's slot now, the hash's zero state beside J0.
    "1:  lea 16(%r12,%r13), %r14\n"
    "movabs $0x8000000000000000, %rax\n   mov %r13, %rdx\n   shl $3, %rdx\n   bswap %rdx\n"
    "movq %rax, %xmm0\n   movq %rdx, %xmm1\n   punpcklqdq %xmm1, %xmm0\n   movdqu %xmm0, (%r14)\n"
    "pxor %xmm0, %xmm0\n   movdqu %xmm0, 32(%rsp)\n"
    //  J0 at 0(%rsp): four zero bytes, the counter, then 00 00 00 01.
    "mov 8(%r12), %r15\n   mov %r15, %rax\n   shl $32, %rax\n   mov %r15, %rdx\n   shr $32, %rdx\n"
    "mov $0x01000000, %ecx\n   shl $32, %rcx\n   or %rcx, %rdx\n"
    "movq %rax, %xmm0\n   movq %rdx, %xmm1\n   punpcklqdq %xmm1, %xmm0\n   movdqu %xmm0, (%rsp)\n"
    //  The run covers the header too, so the header's own block comes out
    //  as the header xored with the tag's mask; it is put back after.
    "movdqu (%r12), %xmm2\n   movdqu %xmm2, 16(%rsp)\n"
    "mov %rbx, %rdi\n   mov %rsp, %rsi\n   mov %r12, %rdx\n   mov %r12, %rcx\n"
    "mov %r13, %r8\n   shr $4, %r8\n   inc %r8\n"
    "call aes128_ctr_blocks\n"
    "movdqu 16(%rsp), %xmm2\n   movdqu (%r12), %xmm0\n   pxor %xmm2, %xmm0\n"
    "movdqu %xmm0, 16(%rsp)\n   movdqu %xmm2, (%r12)\n"
    "lea 32(%rsp), %rdi\n   lea 192(%rbx), %rsi\n   mov %r12, %rdx\n"
    "mov %r13, %rcx\n   shr $4, %rcx\n   add $2, %rcx\n"
    "call ghash_blocks\n"
    "movdqu 32(%rsp), %xmm0\n   movdqu 16(%rsp), %xmm1\n   pxor %xmm1, %xmm0\n   movdqu %xmm0, (%r14)\n"
    "pxor %xmm0, %xmm0\n   pxor %xmm1, %xmm1\n   pxor %xmm2, %xmm2\n"
    "movdqu %xmm0, (%rsp)\n   movdqu %xmm0, 16(%rsp)\n   movdqu %xmm0, 32(%rsp)\n"
    "add $48, %rsp\n"
    "pop %r15\n   pop %r14\n   pop %r13\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(waterlink_seal_box)
    ASM_FUNC(waterlink_open_box)
    "push %rbx\n   push %r12\n   push %r13\n   push %r14\n   push %r15\n"
    "sub $64, %rsp\n"
    "mov %rdi, %rbx\n   mov %rsi, %r12\n   mov %rdx, %r13\n"
    "lea 16(%rsi,%rdx), %r14\n"
    //  The tag that came to 48(%rsp), the lengths in its slot and the zero
    //  state at 32(%rsp), each in one store; J0 at 0(%rsp) while they land.
    "movdqu (%r14), %xmm0\n   movdqu %xmm0, 48(%rsp)\n"
    "movabs $0x8000000000000000, %rax\n   mov %r13, %rdx\n   shl $3, %rdx\n   bswap %rdx\n"
    "movq %rax, %xmm0\n   movq %rdx, %xmm1\n   punpcklqdq %xmm1, %xmm0\n   movdqu %xmm0, (%r14)\n"
    "pxor %xmm0, %xmm0\n   movdqu %xmm0, 32(%rsp)\n"
    "mov 8(%r12), %r15\n   mov %r15, %rax\n   shl $32, %rax\n   mov %r15, %rdx\n   shr $32, %rdx\n"
    "mov $0x01000000, %ecx\n   shl $32, %rcx\n   or %rcx, %rdx\n"
    "movq %rax, %xmm0\n   movq %rdx, %xmm1\n   punpcklqdq %xmm1, %xmm0\n   movdqu %xmm0, (%rsp)\n"
    "lea 32(%rsp), %rdi\n   lea 192(%rbx), %rsi\n   mov %r12, %rdx\n"
    "mov %r13, %rcx\n   shr $4, %rcx\n   add $2, %rcx\n"
    "call ghash_blocks\n"
    "movdqu 48(%rsp), %xmm0\n   movdqu %xmm0, (%r14)\n"
    //  The run over header and box: the header comes out xored with the
    //  mask, and goes back.
    "movdqu (%r12), %xmm2\n   movdqu %xmm2, 16(%rsp)\n"
    "mov %rbx, %rdi\n   mov %rsp, %rsi\n   mov %r12, %rdx\n   mov %r12, %rcx\n"
    "mov %r13, %r8\n   shr $4, %r8\n   inc %r8\n"
    "call aes128_ctr_blocks\n"
    //  Hash, mask and tag xored together are zero in every bit, or not.
    "movdqu 16(%rsp), %xmm2\n   movdqu (%r12), %xmm0\n   pxor %xmm2, %xmm0\n"
    "movdqu %xmm2, (%r12)\n"
    "movdqu 32(%rsp), %xmm1\n   pxor %xmm1, %xmm0\n   movdqu 48(%rsp), %xmm1\n   pxor %xmm1, %xmm0\n"
    "movq %xmm0, %rax\n   psrldq $8, %xmm0\n   movq %xmm0, %rdx\n   or %rdx, %rax\n"
    "mov %rax, %r15\n   test %rax, %rax\n   jz 1f\n"
    "lea 16(%r12), %rdi\n   mov %r13, %rsi\n   call memory_zero\n"
    "1:  pxor %xmm0, %xmm0\n   pxor %xmm1, %xmm1\n   pxor %xmm2, %xmm2\n"
    "movdqu %xmm0, (%rsp)\n   movdqu %xmm0, 16(%rsp)\n   movdqu %xmm0, 32(%rsp)\n   movdqu %xmm0, 48(%rsp)\n"
    "xor %eax, %eax\n   test %r15, %r15\n   sete %al\n"
    "add $64, %rsp\n"
    "pop %r15\n   pop %r14\n   pop %r13\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(waterlink_open_box)
);
#elif ARM64
__asm__(
    ASM_FUNC(waterlink_seal_box)
    "stp x29, x30, [sp, #-112]!\n   mov x29, sp\n"
    "stp x19, x20, [sp, #16]\n   stp x21, x22, [sp, #32]\n"
    "mov x19, x0\n   mov x20, x1\n   mov x21, x3\n"
    "cmp x2, x3\n   b.hs 1f\n"
    "add x0, x1, #16\n   add x0, x0, x2\n   sub x1, x3, x2\n   bl memory_zero\n"
    //  The lengths in the tag's slot and the zero state at sp+80 first, as
    //  whole blocks; the header to sp+64 and J0 to sp+48.
    "1:  add x22, x20, #16\n   add x22, x22, x21\n"
    "mov x9, #0x8000000000000000\n   lsl x10, x21, #3\n   rev x10, x10\n"
    "stp x9, x10, [x22]\n   stp xzr, xzr, [sp, #80]\n"
    "ldp x9, x10, [x20]\n   stp x9, x10, [sp, #64]\n"
    "lsl x11, x10, #32\n   lsr x12, x10, #32\n   mov x13, #0x0100000000000000\n"
    "orr x12, x12, x13\n   stp x11, x12, [sp, #48]\n"
    "mov x0, x19\n   add x1, sp, #48\n   mov x2, x20\n   mov x3, x20\n"
    "lsr x4, x21, #4\n   add x4, x4, #1\n   bl aes128_ctr_blocks\n"
    //  The header came out xored with the mask: the mask to sp+96, the
    //  header back.
    "ldp x9, x10, [sp, #64]\n   ldp x11, x12, [x20]\n"
    "eor x11, x11, x9\n   eor x12, x12, x10\n   stp x11, x12, [sp, #96]\n"
    "stp x9, x10, [x20]\n"
    "add x0, sp, #80\n   add x1, x19, #192\n   mov x2, x20\n"
    "lsr x3, x21, #4\n   add x3, x3, #2\n   bl ghash_blocks\n"
    "ldp x9, x10, [sp, #80]\n   ldp x11, x12, [sp, #96]\n"
    "eor x9, x9, x11\n   eor x10, x10, x12\n   stp x9, x10, [x22]\n"
    "stp xzr, xzr, [sp, #48]\n   stp xzr, xzr, [sp, #64]\n"
    "stp xzr, xzr, [sp, #80]\n   stp xzr, xzr, [sp, #96]\n"
    "ldp x21, x22, [sp, #32]\n   ldp x19, x20, [sp, #16]\n"
    "ldp x29, x30, [sp], #112\n"
    ASM_RET
    ASM_END(waterlink_seal_box)
    ASM_FUNC(waterlink_open_box)
    "stp x29, x30, [sp, #-112]!\n   mov x29, sp\n"
    "stp x19, x20, [sp, #16]\n   stp x21, x22, [sp, #32]\n"
    "mov x19, x0\n   mov x20, x1\n   mov x21, x2\n"
    "add x22, x1, #16\n   add x22, x22, x2\n"
    //  The tag that came to sp+96, the lengths in its slot, the zero state
    //  at sp+80, the header at sp+64 and J0 at sp+48.
    "ldp x9, x10, [x22]\n   stp x9, x10, [sp, #96]\n"
    "mov x9, #0x8000000000000000\n   lsl x10, x21, #3\n   rev x10, x10\n"
    "stp x9, x10, [x22]\n   stp xzr, xzr, [sp, #80]\n"
    "ldp x9, x10, [x20]\n   stp x9, x10, [sp, #64]\n"
    "lsl x11, x10, #32\n   lsr x12, x10, #32\n   mov x13, #0x0100000000000000\n"
    "orr x12, x12, x13\n   stp x11, x12, [sp, #48]\n"
    "add x0, sp, #80\n   add x1, x19, #192\n   mov x2, x20\n"
    "lsr x3, x21, #4\n   add x3, x3, #2\n   bl ghash_blocks\n"
    "ldp x9, x10, [sp, #96]\n   stp x9, x10, [x22]\n"
    "mov x0, x19\n   add x1, sp, #48\n   mov x2, x20\n   mov x3, x20\n"
    "lsr x4, x21, #4\n   add x4, x4, #1\n   bl aes128_ctr_blocks\n"
    //  The header back; header, what came out in its place, hash and tag
    //  xored together are zero in every bit, or not.
    "ldp x9, x10, [sp, #64]\n   ldp x11, x12, [x20]\n   stp x9, x10, [x20]\n"
    "eor x11, x11, x9\n   eor x12, x12, x10\n"
    "ldp x13, x14, [sp, #80]\n   eor x11, x11, x13\n   eor x12, x12, x14\n"
    "ldp x13, x14, [sp, #96]\n   eor x11, x11, x13\n   eor x12, x12, x14\n"
    "orr x22, x11, x12\n"
    "cbz x22, 1f\n"
    "add x0, x20, #16\n   mov x1, x21\n   bl memory_zero\n"
    "1:  stp xzr, xzr, [sp, #48]\n   stp xzr, xzr, [sp, #64]\n"
    "stp xzr, xzr, [sp, #80]\n   stp xzr, xzr, [sp, #96]\n"
    "cmp x22, #0\n   cset w0, eq\n"
    "ldp x21, x22, [sp, #32]\n   ldp x19, x20, [sp, #16]\n"
    "ldp x29, x30, [sp], #112\n"
    ASM_RET
    ASM_END(waterlink_open_box)
);
#elif RISCV64
__asm__(
    ASM_FUNC(waterlink_seal_box)
    "addi sp, sp, -128\n   sd ra, 120(sp)\n   sd s0, 112(sp)\n   sd s1, 104(sp)\n"
    "sd s2, 96(sp)\n   sd s3, 88(sp)\n"
    "mv s0, a0\n   mv s1, a1\n   mv s2, a3\n"
    "bgeu a2, a3, 1f\n"
    "addi a0, a1, 16\n   add a0, a0, a2\n   sub a1, a3, a2\n   call memory_zero\n"
    //  The lengths in the tag's slot -- (box * 8) big endian in its last
    //  three bytes, with no byte reverse to hand -- and the zero state at
    //  sp+32; the header to sp+16 and J0 to sp+0.
    "1:  add s3, s1, s2\n   addi s3, s3, 16\n"
    "li t0, 1\n   slli t0, t0, 63\n   sd t0, 0(s3)\n"
    "slli t0, s2, 3\n   srli t1, t0, 16\n   andi t1, t1, 255\n   slli t1, t1, 40\n"
    "srli t2, t0, 8\n   andi t2, t2, 255\n   slli t2, t2, 48\n   or t1, t1, t2\n"
    "andi t2, t0, 255\n   slli t2, t2, 56\n   or t1, t1, t2\n   sd t1, 8(s3)\n"
    "sd zero, 32(sp)\n   sd zero, 40(sp)\n"
    "ld t0, 0(s1)\n   ld t1, 8(s1)\n   sd t0, 16(sp)\n   sd t1, 24(sp)\n"
    "slli t0, t1, 32\n   srli t1, t1, 32\n   li t2, 1\n   slli t2, t2, 56\n"
    "or t1, t1, t2\n   sd t0, 0(sp)\n   sd t1, 8(sp)\n"
    "mv a0, s0\n   mv a1, sp\n   mv a2, s1\n   mv a3, s1\n"
    "srli a4, s2, 4\n   addi a4, a4, 1\n   call aes128_ctr_blocks\n"
    //  The header came out xored with the mask: the mask to sp+48, the
    //  header back.
    "ld t0, 16(sp)\n   ld t1, 24(sp)\n   ld t2, 0(s1)\n   ld t3, 8(s1)\n"
    "xor t2, t2, t0\n   xor t3, t3, t1\n   sd t2, 48(sp)\n   sd t3, 56(sp)\n"
    "sd t0, 0(s1)\n   sd t1, 8(s1)\n"
    "addi a0, sp, 32\n   addi a1, s0, 192\n   mv a2, s1\n"
    "srli a3, s2, 4\n   addi a3, a3, 2\n   call ghash_blocks\n"
    "ld t0, 32(sp)\n   ld t1, 48(sp)\n   xor t0, t0, t1\n   sd t0, 0(s3)\n"
    "ld t0, 40(sp)\n   ld t1, 56(sp)\n   xor t0, t0, t1\n   sd t0, 8(s3)\n"
    "sd zero, 0(sp)\n   sd zero, 8(sp)\n   sd zero, 16(sp)\n   sd zero, 24(sp)\n"
    "sd zero, 32(sp)\n   sd zero, 40(sp)\n   sd zero, 48(sp)\n   sd zero, 56(sp)\n"
    "ld ra, 120(sp)\n   ld s0, 112(sp)\n   ld s1, 104(sp)\n"
    "ld s2, 96(sp)\n   ld s3, 88(sp)\n   addi sp, sp, 128\n"
    ASM_RET
    ASM_END(waterlink_seal_box)
    ASM_FUNC(waterlink_open_box)
    "addi sp, sp, -128\n   sd ra, 120(sp)\n   sd s0, 112(sp)\n   sd s1, 104(sp)\n"
    "sd s2, 96(sp)\n   sd s3, 88(sp)\n"
    "mv s0, a0\n   mv s1, a1\n   mv s2, a2\n"
    "add s3, a1, a2\n   addi s3, s3, 16\n"
    //  The tag that came to sp+48, the lengths in its slot, the zero state
    //  at sp+32, the header at sp+16 and J0 at sp+0.
    "ld t0, 0(s3)\n   ld t1, 8(s3)\n   sd t0, 48(sp)\n   sd t1, 56(sp)\n"
    "li t0, 1\n   slli t0, t0, 63\n   sd t0, 0(s3)\n"
    "slli t0, s2, 3\n   srli t1, t0, 16\n   andi t1, t1, 255\n   slli t1, t1, 40\n"
    "srli t2, t0, 8\n   andi t2, t2, 255\n   slli t2, t2, 48\n   or t1, t1, t2\n"
    "andi t2, t0, 255\n   slli t2, t2, 56\n   or t1, t1, t2\n   sd t1, 8(s3)\n"
    "sd zero, 32(sp)\n   sd zero, 40(sp)\n"
    "ld t0, 0(s1)\n   ld t1, 8(s1)\n   sd t0, 16(sp)\n   sd t1, 24(sp)\n"
    "slli t0, t1, 32\n   srli t1, t1, 32\n   li t2, 1\n   slli t2, t2, 56\n"
    "or t1, t1, t2\n   sd t0, 0(sp)\n   sd t1, 8(sp)\n"
    "addi a0, sp, 32\n   addi a1, s0, 192\n   mv a2, s1\n"
    "srli a3, s2, 4\n   addi a3, a3, 2\n   call ghash_blocks\n"
    "ld t0, 48(sp)\n   ld t1, 56(sp)\n   sd t0, 0(s3)\n   sd t1, 8(s3)\n"
    "mv a0, s0\n   mv a1, sp\n   mv a2, s1\n   mv a3, s1\n"
    "srli a4, s2, 4\n   addi a4, a4, 1\n   call aes128_ctr_blocks\n"
    //  The header back; header, what came out in its place, hash and tag
    //  xored together are zero in every bit, or not.
    "ld t0, 16(sp)\n   ld t1, 24(sp)\n   ld t2, 0(s1)\n   ld t3, 8(s1)\n"
    "sd t0, 0(s1)\n   sd t1, 8(s1)\n   xor t2, t2, t0\n   xor t3, t3, t1\n"
    "ld t0, 32(sp)\n   ld t1, 40(sp)\n   xor t2, t2, t0\n   xor t3, t3, t1\n"
    "ld t0, 48(sp)\n   ld t1, 56(sp)\n   xor t2, t2, t0\n   xor t3, t3, t1\n"
    "or s3, t2, t3\n"
    "beqz s3, 1f\n"
    "addi a0, s1, 16\n   mv a1, s2\n   call memory_zero\n"
    "1:  sd zero, 0(sp)\n   sd zero, 8(sp)\n   sd zero, 16(sp)\n   sd zero, 24(sp)\n"
    "sd zero, 32(sp)\n   sd zero, 40(sp)\n   sd zero, 48(sp)\n   sd zero, 56(sp)\n"
    "seqz a0, s3\n"
    "ld ra, 120(sp)\n   ld s0, 112(sp)\n   ld s1, 104(sp)\n"
    "ld s2, 96(sp)\n   ld s3, 88(sp)\n   addi sp, sp, 128\n"
    ASM_RET
    ASM_END(waterlink_open_box)
);
#endif

/*
        Seal a datagram whose header is written and whose box holds used
        bytes of frames. The rest of the box is zeroed first -- padding is
        part of what is authenticated, and a box that ends in leftover bytes
        from the last datagram would be sealing whatever they were.
*/
fn waterlink_seal(crypto_aesgcm_key address_to key, p8 address_to datagram,
                  positive used)
{
        waterlink_seal_box(key, datagram, used, WATERLINK_PAYLOAD);
}

/*
        A datagram that carries only acknowledgements is cut to whole blocks
        instead of padded. It says nothing its timing does not already say --
        that the other way is carrying something -- and at full size it made
        the return path carry as many bytes as the forward one, so on any
        path slower one way than the other the acknowledgements became the
        bottleneck and their losses looked like lost data. Returns the
        datagram's length, a multiple of sixteen from 48 to the full size.
*/
positive waterlink_seal_short(crypto_aesgcm_key address_to key,
                              p8 address_to datagram, positive used)
{
        positive box = (used + 15) & ~(positive)15;

        if (!box)
                box = 16;
        if (box > WATERLINK_PAYLOAD)
                box = WATERLINK_PAYLOAD;

        waterlink_seal_box(key, datagram, used, box);
        return 16 + box + 16;
}

/*
        Open a datagram of length bytes in place. The tag is checked over the
        ciphertext before anything is trusted, compared whole so a wrong tag
        says nothing about where it went wrong, and on failure the box is
        wiped: the counter run has already turned it into plaintext-shaped
        bytes, and unauthenticated plaintext is not something to leave lying
        in a buffer the caller might read.

        Returns false for a tag that does not verify, or a length that is not
        a datagram's. The counter is only trustworthy after this returns true,
        which is why the replay window is asked afterwards and never before.
*/
bool waterlink_open_length(crypto_aesgcm_key address_to key,
                           p8 address_to datagram, positive length)
{
        if (length < 48 || length > WATERLINK_DATAGRAM || length % 16)
                return false;

        return waterlink_open_box(key, datagram, length - 32);
}

bool waterlink_open(crypto_aesgcm_key address_to key, p8 address_to datagram)
{
        return waterlink_open_length(key, datagram, WATERLINK_DATAGRAM);
}

#endif // WATERLINK_SEAL_INCLUDED
