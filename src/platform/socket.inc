/*
        Experimental C standard library

        Sockets, and the byte order the wire asks for

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_SOCKET
#define STANDARD_MODERN_C_PLATFORM_SOCKET

/*
        Byte order.

        A port number and an IPv4 address travel most significant byte first,
        which is the opposite of every machine this library targets, so the
        conversion sits on the path of every connection made and every packet
        addressed. Two of the three architectures spell it in one instruction:

                x86_64   bswap %eax          rol $8, %ax
                arm64    rev   w0, w0        rev16 w0, w0

        riscv64 is the interesting one. Reversing bytes there is rev8, which
        lives in the Zbb extension, and the baseline this project builds for --
        rv64imafd_zicsr_zicntr -- does not have it. Asked for the swap anyway,
        gcc does not expand it inline at all: it emits a call to __bswapsi2 in
        libgcc. Every binary here links -nostdlib, so that is not merely a slow
        byte swap, it is an undefined reference and the link fails.
        __builtin_bswap32 is unusable on this target rather than just
        unattractive, and the sequence below is the only way to have it.

        Written as shifts and masks gcc produces twelve instructions. This is
        ten. Rotating the halves first lines both byte pairs up against the
        same mask, so 0x00FF00FF is materialised once and used twice, where
        the four-piece form gcc picks
        has to build 0x0000FF00 and 0x00FF0000 separately:

                x = B0 B1 B2 B3   rotate 16 ->  B2 B3 B0 B1
                swapping the bytes inside each half of that is B3 B2 B1 B0

        The shifts are the w forms, so the upper half of the register is never
        consulted and never has to be cleared first.

        Only the declared width is part of the result. In particular, AAPCS64
        needs no uxth after rev16 and SysV already widens the p16 argument in
        edi. A 32-bit return is different on RV64: that ABI requires even an
        unsigned p32 to be sign-extended to the register width. The slliw arm
        already carries result bit 31 through every upper bit, while the other
        arm cannot set bit 31, so their final OR is canonical without a
        separate sext.w.

        Reversing is its own inverse, so one routine serves both directions and
        network_order names the intent without a second body. On a big endian
        host the conversion is nothing at all, and the reversal stays available
        under its own name for the callers that mean the bytes.
*/

p32 bytes_reverse_32(p32 value);
p16 bytes_reverse_16(p16 value);
bipolar string_to_host(string_address input);
positive host_into(p8 address_to into, p32 host);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define network_order_32(value) (value)
#define network_order_16(value) (value)
#else
#define network_order_32(value) bytes_reverse_32(value)
#define network_order_16(value) bytes_reverse_16(value)
#endif

#if X64
__asm__(
    ASM_SECTION

    ASM_FUNC(bytes_reverse_32)
    "mov %edi, %eax\n   bswap %eax\n"
    ASM_RET
    ASM_END(bytes_reverse_32)

    //
    //       SysV has already widened the p16 argument in edi. Copying the
    //       whole register is one byte shorter than spelling the same fact as
    //       movzwl, and only the low sixteen bits are the declared result.
    //
    ASM_FUNC(bytes_reverse_16)
    "mov %edi, %eax\n   rol $8, %ax\n"
    ASM_RET
    ASM_END(bytes_reverse_16)
);
#elif ARM64
__asm__(
    ASM_SECTION

    ASM_FUNC(bytes_reverse_32)
    "rev w0, w0\n"
    ASM_RET
    ASM_END(bytes_reverse_32)

    //
    //       rev16 puts the answer in the declared low half. AAPCS64, like C,
    //       does not make the unused upper half part of a p16 return.
    //
    ASM_FUNC(bytes_reverse_16)
    "rev16 w0, w0\n"
    ASM_RET
    ASM_END(bytes_reverse_16)
);
#elif RISCV64
__asm__(
    ASM_SECTION

    ASM_FUNC(bytes_reverse_32)
    "slliw t0, a0, 16\n   srliw t1, a0, 16\n   or t0, t0, t1  # B2 B3 B0 B1\n"
    "lui t2, 0xff0\n   addi t2, t2, 255  # 0x00FF00FF, built once and used twice\n"
    "and t1, t0, t2\n   slliw t1, t1, 8\n"
    "srliw t0, t0, 8\n   and t0, t0, t2\n"
    "or a0, t0, t1\n"
    ASM_RET
    ASM_END(bytes_reverse_32)

    //
    //       Shift pairs rather than a mask: each one carries a single byte up
    //       against the end of the register and back down to where it belongs,
    //       so nothing the caller left above bit fifteen survives and no
    //       constant has to be materialised at all.
    //
    ASM_FUNC(bytes_reverse_16)
    "slli t0, a0, 56\n   srli t0, t0, 48  # the low byte, now the high one\n"
    "slli t1, a0, 48\n   srli t1, t1, 56  # the high byte, now the low one\n"
    "or a0, t0, t1\n"
    ASM_RET
    ASM_END(bytes_reverse_16)
);
#endif

/*
        A dotted quad, both ways.

        string_to_host is the one that has to be careful, because it is fed
        whatever was typed. Four octets, one to three digits each, separated by
        single dots and nothing after the last one: a fourth digit, an empty
        octet, an octet above 255, a missing dot or a trailing character are all
        refused. It answers with the address in host order, or -1, which cannot
        be confused with an address because every real one is a positive 32 bit
        value widened into a 64 bit register.

        host_into writes the other direction and returns the length, which is
        the shape positive_into already has and the shape a caller assembling a
        line in a buffer wants. It does not call positive_into once an octet,
        though. Nothing here is wider than 255, so the two divisions each
        collapse to a multiply and a shift -- 41 over 4096 for the hundreds and
        205 over 2048 for the tens, both exact across the whole range -- and
        four calls turn into straight-line code that never divides.
*/
#if X64
__asm__(
    ASM_SECTION

    ASM_FUNC(string_to_host)
    "xor %eax, %eax  # the address so far\n"
    "xor %r8d, %r8d  # which octet\n"
    "1:  test %r8d, %r8d\n   jz 2f\n"
    "cmpb $46, (%rdi)\n   jne 9f\n   inc %rdi  # the separating dot\n"
    "2:  xor %ecx, %ecx  # this octet\n   xor %edx, %edx  # digits taken\n"
    "3:  movzbl (%rdi), %esi\n   sub $48, %esi\n   cmp $9, %esi\n   ja 4f\n"
    "cmp $3, %edx\n   je 9f  # a fourth digit is not an octet\n"
"test %edx, %edx\n   jz 5f\n   test %ecx, %ecx\n   jz 9f  # a leading zero is not decimal\n"
"5:  "
    "lea (%rcx,%rcx,4), %ecx\n   lea (%rsi,%rcx,2), %ecx  # ten times and add\n"
    "inc %edx\n   inc %rdi\n   jmp 3b\n"
    "4:  test %edx, %edx\n   jz 9f  # an empty octet\n"
    "cmp $255, %ecx\n   ja 9f\n"
    "shl $8, %eax\n   or %ecx, %eax\n"
    "inc %r8d\n   cmp $4, %r8d\n   jne 1b\n"
    "cmpb $0, (%rdi)\n   jne 9f  # anything after the last octet\n"
    ASM_RET
    "9:  mov $-1, %rax\n"
    ASM_RET
    ASM_END(string_to_host)

    ASM_FUNC(host_into)
    "mov %rdi, %r8  # where it started\n"
    "mov $24, %ecx  # shift down to this octet\n"
    "1:  mov %esi, %eax\n   shr %cl, %eax\n   movzbl %al, %eax\n"
    "cmp $100, %eax\n   jb 2f\n"
    "imul $41, %eax, %edx\n   shr $12, %edx  # hundreds\n"
    "lea 48(%rdx), %r9d\n   mov %r9b, (%rdi)\n   inc %rdi\n"
    "imul $100, %edx, %edx\n   sub %edx, %eax\n"
    "jmp 3f  # having written a hundreds digit, the tens is written too\n"
    "2:  cmp $10, %eax\n   jb 4f\n"
    "3:  imul $205, %eax, %edx\n   shr $11, %edx  # tens\n"
    "lea 48(%rdx), %r9d\n   mov %r9b, (%rdi)\n   inc %rdi\n"
    "imul $10, %edx, %edx\n   sub %edx, %eax\n"
    "4:  add $48, %eax\n   mov %al, (%rdi)\n   inc %rdi\n"
    "test %ecx, %ecx\n   jz 5f\n"
    "movb $46, (%rdi)\n   inc %rdi\n   sub $8, %ecx\n   jmp 1b\n"
    "5:  mov %rdi, %rax\n   sub %r8, %rax\n"
    ASM_RET
    ASM_END(host_into)
);
#elif ARM64
__asm__(
    ASM_SECTION

    ASM_FUNC(string_to_host)
    "mov w1, #0  // the address so far\n"
    "mov w2, #0  // which octet\n"
    "mov w6, #10\n"
    "1:  cbz w2, 2f\n"
    "ldrb w3, [x0]\n   cmp w3, #46\n   b.ne 9f\n   add x0, x0, #1  // the separating dot\n"
    "2:  mov w4, #0  // this octet\n   mov w5, #0  // digits taken\n"
    "3:  ldrb w3, [x0]\n   sub w3, w3, #48\n   cmp w3, #9\n   b.hi 4f\n"
    "cmp w5, #3\n   b.eq 9f  // a fourth digit is not an octet\n"
"cbz w5, 5f\n   cbz w4, 9f  // a leading zero is not decimal\n"
"5:  "
    "madd w4, w4, w6, w3  // ten times and add\n"
    "add w5, w5, #1\n   add x0, x0, #1\n   b 3b\n"
    "4:  cbz w5, 9f  // an empty octet\n"
    "cmp w4, #255\n   b.hi 9f\n"
    "lsl w1, w1, #8\n   orr w1, w1, w4\n"
    "add w2, w2, #1\n   cmp w2, #4\n   b.ne 1b\n"
    "ldrb w3, [x0]\n   cbnz w3, 9f  // anything after the last octet\n"
    "mov w0, w1\n"
    ASM_RET
    "9:  mov x0, #-1\n"
    ASM_RET
    ASM_END(string_to_host)

    ASM_FUNC(host_into)
    "mov x8, x0  // where it started\n"
    "mov w2, #24  // shift down to this octet\n"
    "mov w9, #41\n   mov w10, #205\n   mov w11, #100\n   mov w12, #10\n"
    "1:  lsr w3, w1, w2\n   and w3, w3, #0xff\n"
    "cmp w3, #100\n   b.lo 2f\n"
    "mul w4, w3, w9\n   lsr w4, w4, #12  // hundreds\n"
    "add w5, w4, #48\n   strb w5, [x0], #1\n"
    "msub w3, w4, w11, w3\n"
    "b 3f  // having written a hundreds digit, the tens is written too\n"
    "2:  cmp w3, #10\n   b.lo 4f\n"
    "3:  mul w4, w3, w10\n   lsr w4, w4, #11  // tens\n"
    "add w5, w4, #48\n   strb w5, [x0], #1\n"
    "msub w3, w4, w12, w3\n"
    "4:  add w3, w3, #48\n   strb w3, [x0], #1\n"
    "cbz w2, 5f\n"
    "mov w5, #46\n   strb w5, [x0], #1\n   sub w2, w2, #8\n   b 1b\n"
    "5:  sub x0, x0, x8\n"
    ASM_RET
    ASM_END(host_into)
);
#elif RISCV64
__asm__(
    ASM_SECTION

    //
    //       Ten times a running value is a pair of shifts and an add rather
    //       than a multiply, which keeps this body inside the base integer
    //       set even though the floor this project builds for has M.
    //
    ASM_FUNC(string_to_host)
    "li t0, 0  # the address so far\n"
    "li t1, 0  # which octet\n"
    "1:  beqz t1, 2f\n"
    "lbu t2, 0(a0)\n   li t3, 46\n   bne t2, t3, 9f\n   addi a0, a0, 1  # the separating dot\n"
    "2:  li t4, 0  # this octet\n   li t5, 0  # digits taken\n"
    "3:  lbu t2, 0(a0)\n   addi t2, t2, -48\n   li t3, 10\n   bgeu t2, t3, 4f\n"
    "li t3, 3\n   beq t5, t3, 9f  # a fourth digit is not an octet\n"
"beqz t5, 5f\n   beqz t4, 9f  # a leading zero is not decimal\n"
"5:  "
    "slli t3, t4, 3\n   slli t6, t4, 1\n   add t4, t3, t6\n   add t4, t4, t2  # ten times and add\n"
    "addi t5, t5, 1\n   addi a0, a0, 1\n   j 3b\n"
    "4:  beqz t5, 9f  # an empty octet\n"
    "li t3, 256\n   bgeu t4, t3, 9f\n"
    "slli t0, t0, 8\n   or t0, t0, t4\n"
    "addi t1, t1, 1\n   li t3, 4\n   bne t1, t3, 1b\n"
    "lbu t2, 0(a0)\n   bnez t2, 9f  # anything after the last octet\n"
    "mv a0, t0\n"
    ASM_RET
    "9:  li a0, -1\n"
    ASM_RET
    ASM_END(string_to_host)

    ASM_FUNC(host_into)
    "mv t6, a0  # where it started\n"
    "li t0, 24  # shift down to this octet\n"
    "1:  srl t1, a1, t0\n   andi t1, t1, 255\n"
    "li t2, 100\n   bltu t1, t2, 2f\n"
    "li t3, 41\n   mul t3, t1, t3\n   srli t3, t3, 12  # hundreds\n"
    "addi t4, t3, 48\n   sb t4, 0(a0)\n   addi a0, a0, 1\n"
    "li t4, 100\n   mul t3, t3, t4\n   sub t1, t1, t3\n"
    "j 3f  # having written a hundreds digit, the tens is written too\n"
    "2:  li t2, 10\n   bltu t1, t2, 4f\n"
    "3:  li t3, 205\n   mul t3, t1, t3\n   srli t3, t3, 11  # tens\n"
    "addi t4, t3, 48\n   sb t4, 0(a0)\n   addi a0, a0, 1\n"
    "li t4, 10\n   mul t3, t3, t4\n   sub t1, t1, t3\n"
    "4:  addi t1, t1, 48\n   sb t1, 0(a0)\n   addi a0, a0, 1\n"
    "beqz t0, 5f\n"
    "li t1, 46\n   sb t1, 0(a0)\n   addi a0, a0, 1\n   addi t0, t0, -8\n   j 1b\n"
    "5:  sub a0, a0, t6\n"
    ASM_RET
    ASM_END(host_into)
);
#endif

/*
        The sockets themselves are Linux only, deliberately.

        syscall.inc carries Darwin's socket numbers and they are correct, but a
        sockaddr there is not the one below: Darwin spends the first byte on a
        length and leaves the family in the second, where Linux gives the
        family the full leading halfword. SOL_SOCKET and the option names
        differ as well. None of that is hard, but writing it here without a
        Darwin machine to run it on would be shipping a structure that is
        quietly the wrong shape, so it waits for someone who can test it.

        The byte order above is arithmetic and builds everywhere.
*/
#if defined(LINUX) && !defined(KERNEL_MODE)

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SHUT_READ 0
#define SHUT_WRITE 1
#define SHUT_BOTH 2

#define NETLINK_ROUTE 0

#define HOST_ANY 0x00000000u
#define HOST_LOOPBACK 0x7f000001u
#define HOST_BROADCAST 0xffffffffu

/*
        The address structures, at the sizes the kernel checks.

        A socket call is handed the length as well as the pointer and the
        kernel compares it, so these are not merely descriptive: the internet
        address is padded to sixteen bytes because that is what the kernel
        expects to be told, not because anything reads the padding. The port
        and the host are held in wire order, which is why they are written
        through network_order rather than assigned.
*/
typedef struct
{
        p16 family;
        p8 data[14];
} socket_address;

typedef struct
{
        p16 family;
        p16 port;
        p32 host;
        p8 padding[8];
} socket_address_internet;

typedef struct
{
        p16 family;
        p16 port;
        p32 flow;
        p8 host[16];
        p32 scope;
} socket_address_internet6;

typedef struct
{
        p16 family;
        p16 padding;
        p32 port;
        p32 groups;
} socket_address_netlink;

bipolar socket_new(b32 family, b32 kind, b32 protocol);
bipolar socket_bind(b32 handle, address_any address, positive size);
bipolar socket_connect(b32 handle, address_any address, positive size);
bipolar socket_listen(b32 handle, b32 waiting);
bipolar socket_accept(b32 handle, address_any address, address_any size, b32 flags);
bipolar socket_send(b32 handle, address_any data, positive size, b32 flags,
                    address_any to, positive to_size);
bipolar socket_receive(b32 handle, address_any data, positive size, b32 flags,
                       address_any from, address_any from_size);
bipolar socket_option_set(b32 handle, b32 level, b32 name, address_any value,
                          positive size);
bipolar socket_option_get(b32 handle, b32 level, b32 name, address_any value,
                          address_any size);
bipolar socket_name(b32 handle, address_any address, address_any size);
bipolar socket_shutdown(b32 handle, b32 direction);
bipolar socket_close(b32 handle);

/*
        The calls are the trap and nothing else.

        Each one takes its arguments where the C ABI already put them and the
        kernel already wants them, so the whole body is the syscall number and
        the trap. x86_64 is the one exception: the C ABI's fourth argument is
        in rcx and the kernel's is in r10, and the trap overwrites rcx anyway,
        so the five calls that reach that far move it first. arm64 and riscv64
        share the asm-generic numbering, which is why those two blocks carry
        the same numbers where x86_64 has its own.

        The syscall table remains the only copy of every number. The two-stage
        spelling below expands syscall(name) before turning its answer into
        instruction text, exactly as linux.inc does for its direct traps.

        Nothing checks the result. Every one of these returns exactly what the
        kernel returned, negative errno and all, which is the convention the
        file routines already keep.
*/
#define SOCKET_RUNTIME_TEXT_INNER(value) #value
#define SOCKET_RUNTIME_TEXT(value) SOCKET_RUNTIME_TEXT_INNER(value)

#if X64
__asm__(
    ASM_SECTION

    ASM_FUNC(socket_new)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(socket)) ", %eax  # socket\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_new)

    ASM_FUNC(socket_bind)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(bind)) ", %eax  # bind\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_bind)

    ASM_FUNC(socket_connect)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(connect)) ", %eax  # connect\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_connect)

    ASM_FUNC(socket_listen)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(listen)) ", %eax  # listen\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_listen)

    ASM_FUNC(socket_accept)
    "mov %rcx, %r10  # the fourth argument, where the trap wants it\n"
    "mov $" SOCKET_RUNTIME_TEXT(syscall(accept4)) ", %eax  # accept4\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_accept)

    ASM_FUNC(socket_send)
    "mov %rcx, %r10  # the fourth argument, where the trap wants it\n"
    "mov $" SOCKET_RUNTIME_TEXT(syscall(sendto)) ", %eax  # sendto\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_send)

    ASM_FUNC(socket_receive)
    "mov %rcx, %r10  # the fourth argument, where the trap wants it\n"
    "mov $" SOCKET_RUNTIME_TEXT(syscall(recvfrom)) ", %eax  # recvfrom\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_receive)

    ASM_FUNC(socket_option_set)
    "mov %rcx, %r10  # the fourth argument, where the trap wants it\n"
    "mov $" SOCKET_RUNTIME_TEXT(syscall(setsockopt)) ", %eax  # setsockopt\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_option_set)

    ASM_FUNC(socket_option_get)
    "mov %rcx, %r10  # the fourth argument, where the trap wants it\n"
    "mov $" SOCKET_RUNTIME_TEXT(syscall(getsockopt)) ", %eax  # getsockopt\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_option_get)

    ASM_FUNC(socket_name)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(getsockname)) ", %eax  # getsockname\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_name)

    ASM_FUNC(socket_shutdown)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(shutdown)) ", %eax  # shutdown\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_shutdown)

    ASM_FUNC(socket_close)
    "mov $" SOCKET_RUNTIME_TEXT(syscall(close)) ", %eax  # close\n"
    "syscall\n"
    ASM_RET
    ASM_END(socket_close)

);
#elif ARM64
__asm__(
    ASM_SECTION

    ASM_FUNC(socket_new)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(socket)) "  // socket\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_new)

    ASM_FUNC(socket_bind)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(bind)) "  // bind\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_bind)

    ASM_FUNC(socket_connect)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(connect)) "  // connect\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_connect)

    ASM_FUNC(socket_listen)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(listen)) "  // listen\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_listen)

    ASM_FUNC(socket_accept)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(accept4)) "  // accept4\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_accept)

    ASM_FUNC(socket_send)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(sendto)) "  // sendto\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_send)

    ASM_FUNC(socket_receive)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(recvfrom)) "  // recvfrom\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_receive)

    ASM_FUNC(socket_option_set)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(setsockopt)) "  // setsockopt\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_option_set)

    ASM_FUNC(socket_option_get)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(getsockopt)) "  // getsockopt\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_option_get)

    ASM_FUNC(socket_name)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(getsockname)) "  // getsockname\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_name)

    ASM_FUNC(socket_shutdown)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(shutdown)) "  // shutdown\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_shutdown)

    ASM_FUNC(socket_close)
    "mov x8, #" SOCKET_RUNTIME_TEXT(syscall(close)) "  // close\n"
    "svc #0\n"
    ASM_RET
    ASM_END(socket_close)

);
#elif RISCV64
__asm__(
    ASM_SECTION

    ASM_FUNC(socket_new)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(socket)) "  # socket\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_new)

    ASM_FUNC(socket_bind)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(bind)) "  # bind\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_bind)

    ASM_FUNC(socket_connect)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(connect)) "  # connect\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_connect)

    ASM_FUNC(socket_listen)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(listen)) "  # listen\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_listen)

    ASM_FUNC(socket_accept)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(accept4)) "  # accept4\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_accept)

    ASM_FUNC(socket_send)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(sendto)) "  # sendto\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_send)

    ASM_FUNC(socket_receive)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(recvfrom)) "  # recvfrom\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_receive)

    ASM_FUNC(socket_option_set)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(setsockopt)) "  # setsockopt\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_option_set)

    ASM_FUNC(socket_option_get)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(getsockopt)) "  # getsockopt\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_option_get)

    ASM_FUNC(socket_name)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(getsockname)) "  # getsockname\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_name)

    ASM_FUNC(socket_shutdown)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(shutdown)) "  # shutdown\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_shutdown)

    ASM_FUNC(socket_close)
    "li a7, " SOCKET_RUNTIME_TEXT(syscall(close)) "  # close\n"
    "ecall\n"
    ASM_RET
    ASM_END(socket_close)

);
#endif

#undef SOCKET_RUNTIME_TEXT
#undef SOCKET_RUNTIME_TEXT_INNER

#endif // LINUX userspace

#endif // STANDARD_MODERN_C_PLATFORM_SOCKET
