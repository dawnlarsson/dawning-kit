/*
        Experimental C standard library

        The compatibility families, in the order the umbrella included them

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        WHAT THIS IS, AND WHY IT IS ONE FILE

        This was src/standard/, thirteen files included one after another by
        src/compiler_memory.c. It is the same thirteen, concatenated in that
        exact order, because the order is load bearing and always was: errno
        and allocation precede their consumers, and stream.c's last act is to
        undefine stdin, stdout and stderr and redefine them from descriptor
        numbers into the pointers a C program means -- so everything above
        that line gets the numbers and everything below gets the streams.
        Splitting them across files never expressed that; the include list
        did, and now the file order does.

        None of it is in a kernel build. Every family below carries the same
        guard it carried as its own file, and a kernel reaches none of them:
        what the kernel uses is already assembly in library.c.

        It is included where src/standard/ was included -- after the
        constant-size specializers in compiler_memory.c, not beside
        library.common.c at the top of it. That position is why a
        literal-size copy in here still folds to straight line stores instead
        of becoming a call into the general routine.

        A configuration that wants one family and not the rest defines
        STANDARD_SKIP_<FAMILY> for the ones it does not want; the standalone
        printf check is the one that does. That is deliberately not the
        family's own include guard: several families carry a minimal fallback
        for a neighbour that is absent -- format.c has its own FILE and errno
        for exactly that check -- guarded on the neighbour's guard, so
        claiming it would take the fallback away with the family. Being
        skipped and having been included are different questions.

        Two of the original fifteen are not here. declare.c held prototypes
        and nothing else and they are in library.c beside the assembly they
        name. text.c held eight bodies and they are on the floor, on all
        three machines.
*/

#ifndef STANDARD_MODERN_C_STANDARD
#define STANDARD_MODERN_C_STANDARD

#ifndef STANDARD_SKIP_ERROR
/* ---- error.c ---- */
/*
        Experimental C standard library

        errno, the message table, and the POSIX names that set them

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_ERROR
#define STANDARD_MODERN_C_STANDARD_ERROR

/*
        Guarded out of the kernel build and out of a no-platform build. core.c
        includes this umbrella and library.c sets KERNEL_MODE from __MODULE__,
        so without this the module would pull in a second struct stat, a second
        open and a second errno beside the ones <linux/...> already declares.
        The three families that shipped without this guard were each correct in
        isolation and wrong together; the ones that had it were right.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        Two error contracts, and why this file holds the seam between them.

        Every routine in library.c that traps into the kernel returns what the
        kernel returned. A missing file is -2, an interrupted read is -4, and
        the caller sees the number the kernel produced at the instruction that
        produced it. That is the better contract and it is not close: there is
        no global to be clobbered between the failure and the test, no
        thread-local to be established before the first failure can be
        recorded, and no second call needed to find out what went wrong. Two
        library routines can fail in sequence and both answers survive.

        C's contract is the opposite one. A standard routine returns -1 and
        leaves the reason in errno, and a program written against C -- which
        is every program anybody would want to run here -- reads errno. The
        two cannot be reconciled by choosing, because the library's callers
        want one and a ported program wants the other.

        So this file is a seam and not a conversion. The library keeps its
        negative-errno routines exactly as they are and nothing here changes
        one of them. The standard names are new, thin, and one-directional:
        each calls the raw trap, tests the result against the kernel's error
        window, and on a failure writes the negated value into errno and
        returns -1. Nothing in the library ever reads errno, so a program can
        mix the two styles in one function and neither interferes with the
        other -- file_read still returns -4 while read() returns -1 with
        EINTR in errno, from the same underlying trap.

        The wrappers are written against the *at forms of every call, not the
        legacy ones. open, stat, unlink, mkdir, rename, link, symlink,
        readlink, access, chmod, chown, dup2, pipe and poll do not exist as
        syscalls on arm64 or riscv64 at all -- asm-generic dropped them and
        kept only openat, newfstatat, unlinkat and the rest. Writing the
        legacy name in terms of the *at call is therefore not a portability
        nicety, it is the only way the name exists on two of the three
        machines this must run on. rename is renameat2 rather than renameat
        for the reason syscall.inc already records: riscv64 never had
        renameat.
*/

/*
        The numbers, which are the same on all three machines.

        Checked rather than assumed, because this tree has a scar from that
        exact assumption -- syscall.inc once carried a riscv64 table with four
        wrong entries in it. The check was

            echo '#include <asm/errno.h>' | $CC -E -dM -x c - | grep '^#define E'

        through gcc, aarch64-linux-gnu-gcc and riscv64-linux-gnu-gcc, sorted
        and diffed. x86_64 and arm64 are byte-identical. riscv64 differs by
        exactly two lines, EFSBADCRC and EFSCORRUPTED, which are kernel-side
        aliases for EBADMSG and EUCLEAN that its older headers do not carry
        and that no userspace program names. Every number below is the same on
        all three.

        Guarded one at a time because the tree already spells some of these
        elsewhere: src/sh/term.c defines EINTR as 4 and src/core.c uses bare
        -EINTR and -ENOENT, and the shell is one binary. A plain #define here
        would be a redefinition the moment those two land in a translation
        unit with this one.
*/
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EINTR
#define EINTR 4
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef ENOEXEC
#define ENOEXEC 8
#endif
#ifndef EBADF
#define EBADF 9
#endif
#ifndef ECHILD
#define ECHILD 10
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOTBLK
#define ENOTBLK 15
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif
#ifndef EISDIR
#define EISDIR 21
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENFILE
#define ENFILE 23
#endif
#ifndef EMFILE
#define EMFILE 24
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef ETXTBSY
#define ETXTBSY 26
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif
#ifndef ESPIPE
#define ESPIPE 29
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef EMLINK
#define EMLINK 31
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef EDOM
#define EDOM 33
#endif
#ifndef ERANGE
#define ERANGE 34
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef ENOLCK
#define ENOLCK 37
#endif
#ifndef ENOSYS
#define ENOSYS 38
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 39
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef ENOMSG
#define ENOMSG 42
#endif
#ifndef EIDRM
#define EIDRM 43
#endif
#ifndef ECHRNG
#define ECHRNG 44
#endif
#ifndef EL2NSYNC
#define EL2NSYNC 45
#endif
#ifndef EL3HLT
#define EL3HLT 46
#endif
#ifndef EL3RST
#define EL3RST 47
#endif
#ifndef ELNRNG
#define ELNRNG 48
#endif
#ifndef EUNATCH
#define EUNATCH 49
#endif
#ifndef ENOCSI
#define ENOCSI 50
#endif
#ifndef EL2HLT
#define EL2HLT 51
#endif
#ifndef EBADE
#define EBADE 52
#endif
#ifndef EBADR
#define EBADR 53
#endif
#ifndef EXFULL
#define EXFULL 54
#endif
#ifndef ENOANO
#define ENOANO 55
#endif
#ifndef EBADRQC
#define EBADRQC 56
#endif
#ifndef EBADSLT
#define EBADSLT 57
#endif
#ifndef EBFONT
#define EBFONT 59
#endif
#ifndef ENOSTR
#define ENOSTR 60
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ETIME
#define ETIME 62
#endif
#ifndef ENOSR
#define ENOSR 63
#endif
#ifndef ENONET
#define ENONET 64
#endif
#ifndef ENOPKG
#define ENOPKG 65
#endif
#ifndef EREMOTE
#define EREMOTE 66
#endif
#ifndef ENOLINK
#define ENOLINK 67
#endif
#ifndef EADV
#define EADV 68
#endif
#ifndef ESRMNT
#define ESRMNT 69
#endif
#ifndef ECOMM
#define ECOMM 70
#endif
#ifndef EPROTO
#define EPROTO 71
#endif
#ifndef EMULTIHOP
#define EMULTIHOP 72
#endif
#ifndef EDOTDOT
#define EDOTDOT 73
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif
#ifndef ENOTUNIQ
#define ENOTUNIQ 76
#endif
#ifndef EBADFD
#define EBADFD 77
#endif
#ifndef EREMCHG
#define EREMCHG 78
#endif
#ifndef ELIBACC
#define ELIBACC 79
#endif
#ifndef ELIBBAD
#define ELIBBAD 80
#endif
#ifndef ELIBSCN
#define ELIBSCN 81
#endif
#ifndef ELIBMAX
#define ELIBMAX 82
#endif
#ifndef ELIBEXEC
#define ELIBEXEC 83
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif
#ifndef ERESTART
#define ERESTART 85
#endif
#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif
#ifndef EUSERS
#define EUSERS 87
#endif
#ifndef ENOTSOCK
#define ENOTSOCK 88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE 91
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT 92
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT 96
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef EADDRINUSE
#define EADDRINUSE 98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL 99
#endif
#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH 101
#endif
#ifndef ENETRESET
#define ENETRESET 102
#endif
#ifndef ECONNABORTED
#define ECONNABORTED 103
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef ENOBUFS
#define ENOBUFS 105
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS 109
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN 112
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
#ifndef ESTALE
#define ESTALE 116
#endif
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#ifndef ENOTNAM
#define ENOTNAM 118
#endif
#ifndef ENAVAIL
#define ENAVAIL 119
#endif
#ifndef EISNAM
#define EISNAM 120
#endif
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef EDQUOT
#define EDQUOT 122
#endif
#ifndef ENOMEDIUM
#define ENOMEDIUM 123
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE 124
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif
#ifndef ENOKEY
#define ENOKEY 126
#endif
#ifndef EKEYEXPIRED
#define EKEYEXPIRED 127
#endif
#ifndef EKEYREVOKED
#define EKEYREVOKED 128
#endif
#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif
#ifndef EOWNERDEAD
#define EOWNERDEAD 130
#endif
#ifndef ENOTRECOVERABLE
#define ENOTRECOVERABLE 131
#endif
#ifndef ERFKILL
#define ERFKILL 132
#endif
#ifndef EHWPOISON
#define EHWPOISON 133
#endif

//      The three pairs that are one number under two names. A program may
//      compare against either and the kernel only ever produces the one.
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EDEADLOCK
#define EDEADLOCK EDEADLK
#endif

/*
        Where errno lives, which is the one decision in this file that was
        made against a measurement rather than from the standard.

        The standard says errno is a macro expanding to a modifiable lvalue of
        type int, and every Linux C library implements it as
        (*__errno_location()) with __thread storage behind the call. Keeping
        __errno_location as the sole accessor is not negotiable and is not
        what follows: it is the indirection that lets the storage decision
        below be a one-line change later instead of an edit to every one of
        the sixty call sites in this file.

        The storage itself is a plain object here, not a __thread object, and
        the reason is that __thread does not work in a spark binary. It does
        not work slowly or subtly; it segfaults, on all three machines, at the
        first access. The probe was a spark program with

            local b32 error_number_storage = 0;
            b32 address_to error_location(void) { return address_of error_number_storage; }

        and a main that wrote 42 through it, built with the same freestanding
        line test/run uses. x86_64 native, qemu-aarch64 and qemu-riscv64
        all took SIGSEGV and exited 139.

        The cause is that nothing establishes a thread pointer. A local-exec
        __thread access is not a call, it is a load through the architecture's
        thread register at a link-time offset -- fs on x86_64, tpidr_el0 on
        arm64, tp on riscv64. Linux does not set any of the three for a static
        binary; glibc's static startup mmaps a TLS block and installs it in
        __libc_setup_tls before main. This library's _start does no such thing
        and cannot be asked to, since it is assembly in library.c that every
        program shares.

        So the default is a plain object in .bss, which for a single-threaded
        freestanding binary is exactly the same thing a working __thread would
        be -- one process, one thread, one cell -- and which does not fault.
        The __thread spelling is written out below and reached by defining
        STANDARD_ERROR_THREAD_LOCAL, and it is not hypothetical: a second
        probe that set the thread register by hand before touching the
        variable ran correctly on all three, and the linker does emit .tbss
        and a PT_TLS program header without any help from spark.ld. What is
        missing is only the startup that installs the block.

        Two things the future implementer needs and this comment is the only
        place they are written down.

        First, initialized thread-locals. Everything here is zero-initialized,
        so it lands in .tbss, which is NOBITS, and a zeroed block is a correct
        initial image for it. A __thread object with a non-zero initializer
        lands in .tdata instead, and spark.ld has no output section rule for
        .tdata at all -- it would be placed wherever the linker felt like,
        outside every region the loader maps, and read as zero with no
        diagnostic. spark.ld needs .tdata and .tbss rules before any
        thread-local carries an initializer.

        Second, the block size. On x86_64 the thread register points at the
        *end* of the TLS block and variables sit at negative offsets from it;
        on arm64 it points at the start with sixteen bytes of thread control
        block ahead of the first variable; on riscv64 it points at the start.
        So the block handed over must be at least the whole program's PT_TLS
        memsz, plus sixteen on arm64, and every family that adds a __thread
        object grows that number. Overflowing it corrupts whatever sits before
        the block rather than faulting.
*/

#ifdef STANDARD_ERROR_THREAD_LOCAL
static local b32 error_number_storage;
#else
static b32 error_number_storage;
#endif

/*
        The accessor, spelled the way the platform ABI spells it.

        __errno_location is the name glibc exports and the name a compiler
        emits for errno when it inlines nothing, so keeping it makes an object
        built against real headers link against this without a shim. It is the
        one external symbol in this file; everything else is static, so a
        program pays only for the wrappers it actually calls.
*/
pub CONST RETURNS_NONNULL b32 address_to __errno_location(void);

CONST RETURNS_NONNULL b32 address_to __errno_location(void)
{
        return address_of error_number_storage;
}

#undef errno
#define errno (address_to __errno_location())

#ifdef STANDARD_ERROR_THREAD_LOCAL
/*
        Installing a thread pointer, which a program does once and on purpose.

        This is explicit rather than lazy, and the difference matters. A lazy
        version -- __errno_location checking a flag and installing the block
        on first use -- has a bug in exactly the future it would exist for: a
        program whose main thread never touches errno spawns a child with
        CLONE_SETTLS, the kernel gives that child its own thread block, the
        shared flag is still false, and the child's first errno access
        overwrites the kernel's block with this one. Both threads then share a
        single errno cell. Detecting that means reading the thread register
        back, which on x86_64 at baseline is an arch_prctl trap on every errno
        access. Being told once, by a program that knows it has one thread, is
        cheaper and cannot be wrong.

        The block must be zeroed and must be at least the program's PT_TLS
        memsz plus sixteen bytes; a 4096 byte static array is the easy answer
        and is what the probe used.

        The three instructions are the only assembly in this family and they
        are here rather than in a .inc under src/platform because in the
        shipped configuration they do not exist -- this file is pure C unless
        somebody asks for the other storage. When threads land they belong in
        src/platform/thread.inc beside whatever creates them, because setting
        the thread register is that layer's job and not this one's. x86_64 is
        not even assembly: the thread base is set by a syscall there, so the
        library's own system_call_2 does it.
*/
static bool error_thread_storage_begin(address_any block, positive size)
{
        if (is_null(block) || size < 64)
                return false;

#if X64
        //      ARCH_SET_FS is 0x1002, and the base is the end of the block
        //      because x86_64 local-exec offsets are negative from it.
        return system_call_2(syscall(arch_prctl), 0x1002,
                             (positive)((p8 address_to)block + size)) == 0;
#elif ARM64
        //      TPIDR_EL0 is writable at EL0, so no trap is involved.
        __asm__ volatile("msr tpidr_el0, %0" : : "r"(block) : "memory");
        return true;
#else
        //      tp is x4, reserved by the ABI for exactly this.
        __asm__ volatile("mv tp, %0" : : "r"(block) : "memory");
        return true;
#endif
}
#endif // STANDARD_ERROR_THREAD_LOCAL

/* Translate the kernel error window before each public return type narrows
   the result. Register-width offsets and mapped addresses stay intact. */
static bipolar error_result(bipolar result)
{
        if (system_failed(result))
        {
                errno = (b32) - result;
                return -1;
        }

        return result;
}

/*
        The longest shared errno message is "Invalid or incomplete multibyte or wide
        character" at forty nine bytes, and the longest line the unknown path
        can build is "Unknown error -2147483648" at twenty five, since the
        number is an int and cannot be wider. Sixty four holds either with its
        terminator, and the test walks the whole table rather than trusting
        this paragraph: a message added later that does not fit would
        otherwise be truncated by perror with nothing said.
*/
#define ERROR_MESSAGE_MAX 64

//      The prefix perror is given plus ": " plus a message plus a newline. A
//      prefix longer than what is left is cut rather than growing the frame,
//      which is the one place this deviates from glibc and is written down in
//      perror's own comment.
#define ERROR_LINE_MAX 256

/*
        Spelled as the literal rather than as a pointer to it, so that its
        length is a constant where it is used.

        `static const string_address` is a pointer variable, and
        string_length of a pointer is a call no amount of optimisation folds
        away -- the compiler has to assume the bytes it points at are only
        known at run time. sizeof of the literal, less its terminator, is
        fourteen at compile time, and that is what lets the copy below reach
        the known-size specializer in compiler_memory.c and become two stores
        instead of a call into the vectorised routine.
*/
#define ERROR_UNKNOWN_PREFIX "Unknown error "
#define ERROR_UNKNOWN_PREFIX_LENGTH (sizeof ERROR_UNKNOWN_PREFIX - 1)

/*
        POSIX strerror_r, shared by strerror and perror.

        Returns what POSIX strerror_r returns, and matched against glibc 2.44
        rather than against the wording of the standard, because the standard
        leaves two things open that a program can see.

        On an unknown number glibc writes "Unknown error N" and answers
        EINVAL, and it answers EINVAL even when the buffer was too small to
        hold that -- EINVAL wins over ERANGE, it is not the case that the
        truncation is reported first. Measured: strerror_r(41, buffer, 10)
        returns 22 and leaves "Unknown e" behind.

        On a known number that does not fit, the truncated prefix is written
        with its terminator and ERANGE comes back, so a caller that ignores
        the return still has something printable. A size of zero writes
        nothing at all and still answers ERANGE.

        Negative numbers are formatted with their sign, which is why this
        takes a b32 and not a p32: strerror_r(-7, ...) says "Unknown error -7"
        and a program that passes a raw kernel return by mistake sees the
        mistake rather than a plausible message.
*/
static b32 strerror_r(b32 number, string_address into, positive size)
{
        p8 built[ERROR_MESSAGE_MAX];
        string_address source = system_error_message(number);
        positive length;
        positive room;

        bool known = source != null;
        if (known)
                length = string_length(source);
        else
        {
                memory_copy_apart(built, (string_address)ERROR_UNKNOWN_PREFIX,
                                  ERROR_UNKNOWN_PREFIX_LENGTH);
                length = ERROR_UNKNOWN_PREFIX_LENGTH +
                         bipolar_into_string(built + ERROR_UNKNOWN_PREFIX_LENGTH,
                                             (bipolar)number);
                source = built;
        }

        if (is_null(into) || size == 0)
                return known ? ERANGE : EINVAL;

        room = length < size ? length : size - 1;
        memory_copy_apart(into, source, room);
        into[room] = end;
        return known ? (length < size ? 0 : ERANGE) : EINVAL;
}

/*
        strerror, which returns a pointer and therefore needs somewhere to
        keep an answer that is not in the table.

        Known numbers return the table entry itself, so the common case
        copies nothing and the returned pointer stays valid forever. Unknown
        ones are formatted into one static buffer, which makes two calls in
        one expression -- printf("%s %s", strerror(a), strerror(b)) -- share
        it and print the second message twice. That is exactly glibc's own
        behaviour and exactly why strerror_r exists; it is not a defect being
        introduced here, it is the defect the standard has.

        The returned type is string_address, which is p8 address_to and not
        char address_to. Every string in this tree is unsigned, and a program
        assigning the result to a char * is the same conversion it already
        makes for string_find and string_first_of.
*/
static p8 error_unknown_text[ERROR_MESSAGE_MAX];

static string_address strerror(b32 number)
{
        string_address known = system_error_message(number);
        if (known)
                return known;

        strerror_r(number, error_unknown_text, sizeof error_unknown_text);

        return error_unknown_text;
}

/*
        perror, written to the same place and in the same shape as glibc.

        "prefix: message\n" when the prefix is a non-empty string, and
        "message\n" when it is null or empty -- both measured, not assumed,
        against glibc 2.44, which prints no bare ": " for an empty prefix.

        It goes out through log_error, which is already assembly on all three
        machines and does the two things this needs: it flushes whatever is
        pending in the buffered log first, so the diagnostic does not appear
        before output that was produced earlier, and it writes to descriptor
        two directly rather than through the buffer. That also keeps this
        family independent of the stream family -- perror does not need stderr
        to be a FILE, and does not have to wait for fopen to exist.

        The line is built in one buffer and written once, because log_error is
        one write syscall per call and a perror that produced three of them
        can interleave with another process writing the same terminal. The
        cost is a bound: a prefix that does not fit is cut. glibc does not cut
        it, but glibc can also grow a buffer, and a family that cannot must
        choose between a fixed cut and an unbounded stack frame in the routine
        a program calls when it is already in trouble.
*/
static fn perror(string_address prefix)
{
        p8 line[ERROR_LINE_MAX];
        positive at = 0;
        positive length;

        if (!is_null(prefix) && string_get(prefix) != end)
        {
                length = string_length(prefix);

                //      Two for ": ", one for the newline, and the message and
                //      its terminator, all of which have to still fit.
                if (length > ERROR_LINE_MAX - ERROR_MESSAGE_MAX - 4)
                        length = ERROR_LINE_MAX - ERROR_MESSAGE_MAX - 4;

                //      `line` is this function's own frame, so the prefix
                //      the caller handed in cannot overlap it.
                memory_copy_apart(line, prefix, length);
                at = length;
                line[at++] = ':';
                line[at++] = ' ';
        }

        strerror_r(errno, line + at, ERROR_MESSAGE_MAX);
        at += string_length(line + at);
        line[at++] = '\n';

        log_error(line, at);
}

/*
        What newfstatat writes into, which is not one layout.

        x86_64 kept its own struct stat and asm-generic wrote a different one,
        so the fields are at different offsets on x86_64 than on arm64 and
        riscv64 and there is no arrangement of names that is right on both.
        The offsets below came off the build machine, from
        offsetof(struct stat, ...) against each cross toolchain's own
        <asm/stat.h>: mode at 24 and links at 16 on x86_64, mode at 16 and
        links at 20 on the other two, and the whole structure 144 bytes there
        against 128 here.

        This is deliberately not library.c's file_status. That structure is
        the one file_get_status fills and its comment says plainly that only
        size and blocks are at the kernel's offsets -- it has hard_links at 24
        and the special device at 40, which is neither machine's layout. It is
        correct for what reads it, which is the size field, and reusing it for
        a stat() a program will read st_mode out of would hand back whatever
        happened to be at that offset.

        The names are the POSIX ones rather than prose ones. A program calling
        stat writes buffer.st_mode, and a structure whose fields are spelled
        differently is a structure that program cannot use.
*/
typedef struct stat
{
        p64 st_dev;
        p64 st_ino;
#if X64
        p64 st_nlink;
        p32 st_mode;
        p32 st_uid;
        p32 st_gid;
        p32 error_stat_padding;
        p64 st_rdev;
#else
        p32 st_mode;
        p32 st_nlink;
        p32 st_uid;
        p32 st_gid;
        p64 st_rdev;
        p64 error_stat_reserved;
#endif
        b64 st_size;
#if X64
        b64 st_blksize;
#else
        b32 st_blksize;
        b32 error_stat_padding_2;
#endif
        b64 st_blocks;
        b64 st_atime;
        b64 st_atime_nsec;
        b64 st_mtime;
        b64 st_mtime_nsec;
        b64 st_ctime;
        b64 st_ctime_nsec;
        //      x86_64 reserves three words here and asm-generic two halves,
        //      which is the last sixteen bytes of the difference between 144
        //      and 128. The test pins both sizes because getting this wrong
        //      is invisible until a caller puts a stat on the stack next to
        //      something the kernel then writes over.
#if X64
        b64 error_stat_tail[3];
#else
        b32 error_stat_tail[2];
#endif
} error_stat;

//      The tag and the typedef are the same type, so a program may write
//      `struct stat one;` or `error_stat one;` and hand either to stat().
#define stat_address error_stat address_to

//      The file type bits, which every machine here agrees on and which a
//      program uses through the macros rather than by hand.
#ifndef S_IFMT
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#endif

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)
#endif

//      The *at flags. AT_FDCWD is already in library.c and is not repeated.
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW 0x400
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

#ifndef MAP_FAILED
#define MAP_FAILED address_bad
#endif

#ifndef SEEK_SET
#define SEEK_SET FILE_SEEK_SET
#define SEEK_CUR FILE_SEEK_CUR
#define SEEK_END FILE_SEEK_END
#endif

//      The bit that makes O_TMPFILE create something, which is the second
//      flag after O_CREAT that makes open read a mode argument. O_TMPFILE
//      itself is this or-ed with O_DIRECTORY, whose value differs between
//      arm64 and the other two, so the create bit is what is tested.
#ifndef O_TMPFILE_CREATE
#define O_TMPFILE_CREATE 020000000
#endif

#ifndef O_RDONLY
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define O_CREAT 0100
#define O_EXCL 0200
#define O_APPEND 02000
#endif

#ifndef WNOHANG
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#endif

//      AT_FDCWD is -100 and every *at call wants it as an argument-sized
//      value, so the sign extension happens once here instead of at forty
//      call sites.
#define ERROR_AT_HERE ((positive)(bipolar)AT_FDCWD)

//      The kernel writes a signed 32 bit status into an int, and every caller
//      of wait4 wants a pointer to one.
#define ERROR_STATUS_ADDRESS(pointer) ((positive)(pointer))

/*
        The wrappers themselves.

        Every one is the same three lines -- trap, test, translate -- and
        there is nothing else in any of them, which is the point: policy lives
        in error_result, so the only thing a reader has to check
        per routine is that the arguments went to the kernel in the right
        order.

        They are static. A spark program is one translation unit, so a static
        wrapper that nothing calls is deleted by the compiler and a program
        that opens no files does not carry open. The one external symbol is
        __errno_location, which has to be external to be the ABI name.

        Where the C signature is variadic it is variadic here too, because
        open(path, O_RDONLY) with two arguments is what real code writes and a
        three-parameter open would refuse to compile it.
*/

//      -- descriptors ------------------------------------------------------

#define ERROR_ENTRY(name, returned, parameters, call)                        \
        static returned name parameters { return (returned)error_result(call); }

#define ERROR_OPEN(name, parameters, directory)                              \
        static b32 name parameters                                           \
        {                                                                    \
                p32 mode = 0;                                                \
                if (flags & (O_CREAT | O_TMPFILE_CREATE))                    \
                {                                                            \
                        var_args list;                                        \
                        var_list(list, flags);                                \
                        mode = var_list_get(list, p32);                       \
                        var_list_end(list);                                   \
                }                                                            \
                return error_result(system_open_at_mode(                     \
                    directory, path, flags, mode));                          \
        }

ERROR_OPEN(openat,
           (b32 directory, string_address path, b32 flags, ...), directory)
ERROR_OPEN(open, (string_address path, b32 flags, ...), ERROR_AT_HERE)
#undef ERROR_OPEN

//      creat is open with the three flags spelled out, and is here because
//      shell scripts and old C both still use it.
static b32 creat(string_address path, p32 mode)
{
        return error_result(system_open_at_mode(
            ERROR_AT_HERE, path, O_WRONLY | O_CREAT | O_TRUNC, mode));
}

ERROR_ENTRY(close, b32, (b32 handle),
            system_close(handle))

/*
        read and write return ssize_t and not int, and the difference is
        reachable: a single read of more than two gigabytes is refused by
        Linux, but a caller that truncates the count to int has already lost
        the answer for anything above that, and pread on a large file has an
        offset that certainly does not fit.
*/
ERROR_ENTRY(read, bipolar, (b32 handle, address_any buffer, positive count),
            system_read_once(handle, buffer, count))
ERROR_ENTRY(write, bipolar,
            (b32 handle, const address_any buffer, positive count),
            system_write_once(handle, buffer, count))
ERROR_ENTRY(pread, bipolar,
            (b32 handle, address_any buffer, positive count, bipolar offset),
            system_call_4(syscall(pread64), (positive)handle,
                                      (positive)buffer, count, (positive)offset))
ERROR_ENTRY(pwrite, bipolar,
            (b32 handle, const address_any buffer, positive count,
             bipolar offset),
            system_call_4(syscall(pwrite64), (positive)handle,
                                      (positive)buffer, count, (positive)offset))
ERROR_ENTRY(lseek, bipolar, (b32 handle, bipolar offset, b32 whence),
            system_seek(handle, offset, whence))
ERROR_ENTRY(dup, b32, (b32 handle),
            system_call_1(syscall(dup), (positive)handle))

/*
        dup2 is dup3 with no flags, except in the one case where they are not
        the same call at all.

        dup2(fd, fd) succeeds and returns fd; dup3(fd, fd, 0) is defined to
        answer EINVAL. So the equal case is handled before the trap, and
        handled by asking whether the descriptor is open at all, because
        dup2 on a closed descriptor must still fail with EBADF rather than
        silently succeeding.
*/
ERROR_ENTRY(dup3, b32, (b32 from, b32 to, b32 flags),
            system_duplicate(from, to, flags))

static b32 dup2(b32 from, b32 to)
{
        if (from == to)
        {
                //      F_GETFD is 1 everywhere here, and asking for it is the
                //      cheapest way to find out whether the descriptor exists.
                if (system_failed(system_call_3(syscall(fcntl),
                                               (positive)from, 1, 0)))
                {
                        errno = EBADF;
                        return -1;
                }

                return to;
        }

        return dup3(from, to, 0);
}

ERROR_ENTRY(pipe2, b32, (b32 address_to pair, b32 flags),
            system_pipe(pair, flags))
ERROR_ENTRY(pipe, b32, (b32 address_to pair),
            system_pipe(pair, 0))

var_list_entry(fcntl, b32, (b32 handle, b32 command, ...), command,
               error_result(system_call_3(
                   syscall(fcntl), (positive)handle, (positive)command,
                   var_list_get(_variadic_list, positive))))
var_list_entry(ioctl, b32, (b32 handle, positive request, ...), request,
               error_result(system_call_3(
                   syscall(ioctl), (positive)handle, request,
                   var_list_get(_variadic_list, positive))))

/*
        isatty, which is a question with no syscall of its own.

        Every implementation asks for the terminal attributes and reports
        whether the ask succeeded. TCGETS is 0x5401 on all three machines --
        x86_64 takes asm-generic's ioctls.h unchanged and so do arm64 and
        riscv64 -- and the struct termios it fills is 60 bytes at most, so 64
        bytes of stack is enough to receive it.

        The errno it leaves behind on a false answer is the kernel's own:
        ENOTTY for a pipe or a regular file, EBADF for a descriptor that is
        not open. That distinction is the whole reason a caller would look.
*/
b32 isatty(b32 handle)
{
        p8 attributes[64];
        bipolar answer = system_control(handle, 0x5401, attributes);

        if (system_failed(answer))
        {
                errno = (b32) - answer;
                return 0;
        }

        return 1;
}

//      -- names in the file system ----------------------------------------

ERROR_ENTRY(fstatat, b32,
            (b32 directory, string_address path, stat_address into, b32 flags),
            system_status_at(directory, path, into, flags))
ERROR_ENTRY(stat, b32, (string_address path, stat_address into),
            system_status_at(ERROR_AT_HERE, path, into, 0))
ERROR_ENTRY(lstat, b32, (string_address path, stat_address into),
            system_status_at(ERROR_AT_HERE, path, into, AT_SYMLINK_NOFOLLOW))

/*
        fstat has a syscall of its own on all three, and is not newfstatat
        with AT_EMPTY_PATH: the empty-path form needs a pointer to an empty
        string and the plain form does not, and the plain form is one number
        on every machine here.
*/
ERROR_ENTRY(fstat, b32, (b32 handle, stat_address into),
            system_file_status(handle, into))
ERROR_ENTRY(unlinkat, b32,
            (b32 directory, string_address path, b32 flags),
            system_remove_at(directory, path, flags))
ERROR_ENTRY(unlink, b32, (string_address path),
            system_remove_at(ERROR_AT_HERE, path, 0))
ERROR_ENTRY(rmdir, b32, (string_address path),
            system_remove_at(ERROR_AT_HERE, path, AT_REMOVEDIR))
ERROR_ENTRY(mkdirat, b32,
            (b32 directory, string_address path, p32 mode),
            system_make_directory_at(directory, path, mode))
ERROR_ENTRY(mkdir, b32, (string_address path, p32 mode),
            system_make_directory_at(ERROR_AT_HERE, path, mode))

/*
        rename is renameat2 with no flags, not renameat.

        syscall.inc records the reason and it is the one genuine divergence
        between arm64 and riscv64 in the whole asm-generic table: riscv never
        had renameat, so the number 38 that arm64 uses for it is something
        else or nothing there. renameat2 is 276 on both and does everything
        renameat does when its flags are zero.
*/
ERROR_ENTRY(renameat2, b32,
            (b32 from_directory, string_address from, b32 to_directory,
             string_address to, p32 flags),
            system_rename_at(from_directory, from, to_directory, to, flags))
ERROR_ENTRY(rename, b32, (string_address from, string_address to),
            system_rename_at(ERROR_AT_HERE, from, ERROR_AT_HERE, to, 0))
ERROR_ENTRY(link, b32, (string_address from, string_address to),
            system_link_at(ERROR_AT_HERE, from, ERROR_AT_HERE, to, 0))
ERROR_ENTRY(symlink, b32, (string_address target, string_address path),
            system_symbolic_link_at(target, ERROR_AT_HERE, path))

/*
        readlink returns the number of bytes it placed and does not terminate
        them, which is the trap in this call and the reason it is spelled out
        here: a caller that hands it a buffer of exactly the link's length
        gets that length back and a string with no end on it.
*/
ERROR_ENTRY(readlink, bipolar,
            (string_address path, string_address into, positive size),
            system_read_link_at(ERROR_AT_HERE, path, into, size))
ERROR_ENTRY(readlinkat, bipolar,
            (b32 directory, string_address path, string_address into,
             positive size),
            system_read_link_at(directory, path, into, size))

/*
        access asks the kernel with the real user and group rather than the
        effective ones, which is what the name has always meant and what makes
        it the wrong call for a security decision. faccessat on asm-generic
        takes three arguments and no flags; the four argument form with
        AT_EACCESS is faccessat2, which is a much newer number and is not
        used here.
*/
ERROR_ENTRY(access, b32, (string_address path, b32 mode),
            system_access_at(ERROR_AT_HERE, path, mode))

static b32 faccessat(b32 directory, string_address path, b32 mode, b32 flags)
{
        (void)flags;
        return error_result(system_access_at(directory, path, mode));
}

//      fchmodat on asm-generic is three arguments; the flags-taking form is
//      fchmodat2 and is newer than the floor this targets.
ERROR_ENTRY(chmod, b32, (string_address path, p32 mode),
            system_change_mode_at(ERROR_AT_HERE, path, mode))
ERROR_ENTRY(fchmod, b32, (b32 handle, p32 mode),
            system_call_2(syscall(fchmod), (positive)handle, (positive)mode))
ERROR_ENTRY(chown, b32, (string_address path, p32 owner, p32 group),
            system_change_owner_at(ERROR_AT_HERE, path, owner, group, 0))
ERROR_ENTRY(lchown, b32, (string_address path, p32 owner, p32 group),
            system_change_owner_at(
                ERROR_AT_HERE, path, owner, group, AT_SYMLINK_NOFOLLOW))
ERROR_ENTRY(fchown, b32, (b32 handle, p32 owner, p32 group),
            system_call_3(syscall(fchown), (positive)handle, (positive)owner,
                          (positive)group))
ERROR_ENTRY(truncate, b32, (string_address path, bipolar length),
            system_call_2(syscall(truncate), (positive)path, (positive)length))
ERROR_ENTRY(ftruncate, b32, (b32 handle, bipolar length),
            system_truncate_handle(handle, length))
ERROR_ENTRY(fsync, b32, (b32 handle),
            system_call_1(syscall(fsync), (positive)handle))
ERROR_ENTRY(fdatasync, b32, (b32 handle),
            system_call_1(syscall(fdatasync), (positive)handle))
ERROR_ENTRY(chdir, b32, (string_address path),
            system_change_directory(path))
ERROR_ENTRY(fchdir, b32, (b32 handle),
            system_call_1(syscall(fchdir), (positive)handle))

/*
        getcwd is the one call here whose C shape and syscall shape disagree
        about what success looks like.

        The kernel returns the number of bytes it wrote, including the
        terminator. C returns the buffer, or a null pointer with errno set --
        and ERANGE specifically when the buffer was too small, which the
        kernel already reports as -ERANGE. So the translation is not
        error_result's: the count is discarded and the buffer comes back.

        Passing a null buffer for the library to allocate one is a GNU
        extension and is refused here with EINVAL rather than half-supported.
*/
static string_address getcwd(string_address into, positive size)
{
        bipolar wrote;

        if (is_null(into))
        {
                errno = EINVAL;
                return null;
        }

        wrote = system_call_2(syscall(getcwd), (positive)into, size);

        if (system_failed(wrote))
        {
                errno = (b32) - wrote;
                return null;
        }

        return into;
}

ERROR_ENTRY(getdents64, b32, (b32 handle, address_any into, positive size),
            system_read_directory(handle, into, size))

//      umask cannot fail: it returns the previous mask and there is no error
//      the kernel can report, so no translation happens and errno is not
//      touched. Wrapped anyway so a program does not have to know that.
static p32 umask(p32 mask)
{
        return (p32)system_call_1(syscall(umask), (positive)mask);
}

//      -- memory ----------------------------------------------------------

/*
        mmap, munmap and mprotect, which are the POSIX names for what
        library.c calls memory and memory_free.

        They are here and not with the allocator because the thing that makes
        them C rather than library routines is exactly what this file is for:
        mmap reports failure as MAP_FAILED with errno set, and memory reports
        it as the kernel's negative number. An allocator built on memory does
        not want either. A program porting code that calls mmap directly wants
        precisely these.

        The offset is in bytes on all three. mmap2, whose offset is in pages,
        is a 32 bit call and does not exist on any machine here.
*/
static address_any mmap(address_any hint, positive length, b32 protection,
                        b32 flags, b32 handle, bipolar offset)
{
        return (address_any)error_result(
            system_call_6(syscall(mmap), (positive)hint, length,
                          (positive)protection, (positive)flags,
                          (positive)handle, (positive)offset));
}

ERROR_ENTRY(munmap, b32, (address_any address, positive length),
            system_call_2(syscall(munmap), (positive)address, length))
ERROR_ENTRY(mprotect, b32,
            (address_any address, positive length, b32 protection),
            system_call_3(syscall(mprotect), (positive)address, length,
                          (positive)protection))

//      -- processes -------------------------------------------------------

//      Process identities cannot fail, so like umask they are one generated
//      unwrapped cast apiece and leave errno alone.
#define ERROR_ID(name, type)                                                 \
        static type name(void)                                               \
        { return (type)system_call(syscall(name)); }

ERROR_ID(getpid, b32)
ERROR_ID(getppid, b32)
ERROR_ID(getuid, p32)
ERROR_ID(geteuid, p32)
ERROR_ID(getgid, p32)
ERROR_ID(getegid, p32)
#undef ERROR_ID

ERROR_ENTRY(kill, b32, (b32 process, b32 signal),
            system_call_2(syscall(kill), (positive)process, (positive)signal))
ERROR_ENTRY(execve, b32,
            (string_address path, string_address address_to arguments,
             string_address address_to environment),
            system_execute(path, arguments, environment))

/*
        fork, which is clone with one flag and nothing else.

        There is no fork syscall on arm64 or riscv64, and the clone that
        stands in for it takes its arguments in a different order on x86_64
        than on the other two -- the tls and child-tid pointers are swapped.
        Every one of those arguments is zero here, so the order cannot matter,
        and that is the only reason this is one line instead of three. A
        clone doing anything else must be written per architecture.

        SIGCHLD in the low byte of the flags is what makes the parent get a
        SIGCHLD and makes wait work, and is the whole of what distinguishes
        this from a thread.
*/
ERROR_ENTRY(fork, b32, (void),
            system_fork())

/*
        wait4 without the EINTR retry that library.c's system_wait4_retry
        does.

        That retry is right for the library's own callers and wrong here:
        POSIX says waitpid returns -1 with EINTR when a signal arrives, and a
        program that installed a handler in order to be interrupted out of a
        wait is entitled to be. The retrying version is still there under its
        own name for anything that wants it.
*/
ERROR_ENTRY(wait4, b32,
            (b32 process, b32 address_to status, b32 options,
             address_any usage),
            system_call_4(syscall(wait4), (positive)process,
                                       ERROR_STATUS_ADDRESS(status),
                                       (positive)options, (positive)usage))
ERROR_ENTRY(waitpid, b32,
            (b32 process, b32 address_to status, b32 options),
            system_call_4(syscall(wait4), (positive)process,
                          ERROR_STATUS_ADDRESS(status), (positive)options, 0))
ERROR_ENTRY(wait, b32, (b32 address_to status),
            system_call_4(syscall(wait4), (positive)(bipolar)-1,
                          ERROR_STATUS_ADDRESS(status), 0, 0))

/*
        _exit and _Exit, which are exit_group and not exit.

        library.c already has exit and it already calls exit_group, so these
        two are aliases onto it rather than a second trap. The distinction
        that matters is the one against the C exit that the stdlib family will
        add: that one runs atexit handlers and flushes streams first, and
        these two must not. Naming them here keeps them independent of
        whichever family ends up owning atexit.
*/
DEAD_END fn _exit(b32 status)
{
        exit(status);
        __builtin_unreachable();
}

DEAD_END fn _Exit(b32 status) __attribute__((alias("_exit")));

ERROR_ENTRY(setsid, b32, (void),
            system_call(syscall(setsid)))
ERROR_ENTRY(sync, b32, (void),
            system_call(syscall(sync)))

#undef ERROR_ENTRY

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_ERROR
#endif // STANDARD_SKIP_ERROR

#ifndef STANDARD_SKIP_LOCK
/* ---- lock.c ---- */
/*
        Experimental C standard library

        lock: one word, one compare-and-swap, and a futex only when somebody
        is actually waiting

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_LOCK
#define STANDARD_MODERN_C_STANDARD_LOCK

/*
        Why this file exists at all, in a library that says everywhere else
        that it is single threaded.

        Three families in src/standard are wrong the moment a second thread
        exists, and each of them says so in its own words. The allocator says
        it plainly under a heading called THREADS: two threads in malloc at
        the same time corrupt the free lists, and the whole of the mutable
        state is six file-scope objects touched inside allocator_take and
        memory_give. error.c keeps errno in a plain .bss object because
        __thread faults in a spark binary with nothing to set the thread
        register. stream.c has no per-stream lock, so two threads writing to
        one FILE interleave into its buffer.

        None of those is fixable by argument. Each one turns on a number: what
        does the lock cost when nobody is contending for it. That number is
        the whole reason this file is here, and it is a small enough file that
        it can be deleted again if the answer is no.

        WHAT THIS IS NOT

        It is not a threading library. Nothing here creates a thread, joins
        one, or has an opinion about how one should be made. Creating a thread
        means a clone with CLONE_VM and a stack the child lands on, and the
        child cannot return into C from the syscall that made it -- its stack
        pointer has moved and the frame it was building is behind it -- so a
        thread entry needs a per-architecture assembly trampoline, which
        belongs in src/platform beside the other three-way code and not in a
        file of ordinary C. CHECK_lock in test/checks.c has one, because a test may reach
        for assembly where a shipped family may not, and that trampoline is
        what proves the code below excludes.

        WHAT IT IS

        Drepper's three-state futex lock, which is the smallest correct mutex
        anybody has found and is what glibc's own low-level lock still is
        underneath. The word is 0 free, 1 held with nobody waiting, 2 held
        with somebody possibly waiting. The uncontended path is exactly one
        compare-and-swap to take and one exchange to release, with no trap in
        either, which is the shape that makes the measurement meaningful: what
        a lock costs a single-threaded program is those two instructions and
        nothing else.

        The third state is what buys that. A two-state lock has to ask the
        kernel to wake somebody on every release, because a release cannot
        tell whether anyone is asleep. The 2 records that a waiter was seen,
        so a release that finds a 1 knows there is nobody to wake and returns
        without a trap. A release that finds a 2 when the last waiter has
        already given up wakes nobody and has wasted one syscall, which is the
        only way this errs and it errs in the harmless direction.

        THE ATOMICS ARE ALREADY HERE

        library.c line 446 onward defines atomic_add, atomic_exchange and
        atomic_compare_exchange over the __sync builtins, and the inventory's
        floor paragraph names the A extension as part of what riscv64 is built
        against. Verified rather than assumed: a program built with the shipped
        three lines -- plain gcc, aarch64-linux-gnu-gcc -mno-outline-atomics,
        riscv64-linux-gnu-gcc -march=rv64imafd_zicsr_zicntr -- links with no
        libatomic and runs the compare-exchange, the exchange and the add
        correctly on all three. So the honest answer to "does this library
        have atomics" is yes, inline, at the documented floor, and this file
        needed to invent nothing.

        WHAT IT COSTS, WHICH IS THE POINT

        Measured on a Ryzen 9 9950X, native, five rounds of five million and
        the best of them, with the shipped -O2 spark link. Picoseconds per
        operation:

              empty loop                        194
              one compare-and-swap             3609
              lock take + release              7231
              malloc(64) + free                3374
              lock + malloc + free + unlock    8897
              fputc to a buffered stream       3073
              lock + fputc + unlock            9224

        Read the third line against the second: a take and a release are two
        locked read-modify-writes and cost exactly twice one, with nothing
        else in them. Read it against the fourth and the sixth and the answer
        to the question this file exists for falls out. An uncontended lock
        costs 7.2 nanoseconds. malloc and free together cost 3.4. fputc costs
        3.1. So locking the allocator makes malloc and free 2.6 times slower
        and locking a stream makes fputc 3.0 times slower, and neither of
        those is a rounding error.

        Only the native figures mean anything. The same program under
        qemu-aarch64 and qemu-riscv64 reports a lock that looks four to five
        times cheaper relative to malloc, because qemu-user translates an
        atomic into ordinary host instructions and models no bus lock and no
        cache line at all. Those runs prove the code is correct on those
        machines. They do not measure it.

        AND WHAT THE ANSWER PROBABLY IS

        Not this lock on every call. The number that changes the decision is
        one more that was measured: a load of a "how many threads are live"
        counter and a predictable branch, with the atomics taken only when the
        answer is more than one, costs 245 picoseconds rather than 7231. That
        turns +164 percent on malloc into +6.6 percent, and +200 percent on
        fputc into +12 percent. It is what glibc did for years under the name
        SINGLE_THREAD_P, and it is nearly free because the branch is perfectly
        predicted in a program that never spawns anything.

        It is deliberately not built into lock_take here, because it is only
        correct if something owns thread creation and increments that counter
        before the second thread can run. Nothing in this tree does yet. When
        something does, the elision belongs in this file and the branch
        belongs in these two functions -- and the measurement above is what
        says it is worth the care it needs.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        The three states, named rather than spelled as numbers at eight sites.
*/
#define LOCK_FREE 0
#define LOCK_HELD 1
#define LOCK_WAITED 2

/*
        futex's two operations and the flag that says the word is private to
        one address space.

        FUTEX_WAIT is 0 and FUTEX_WAKE is 1 in the old multiplexed futex
        syscall, which is number 202 on x86_64 and 98 on the asm-generic
        table that arm64 and riscv64 share. src/platform/syscall.inc also
        carries futex_wait and futex_wake as syscalls 455 and 454 -- those are
        the new unmultiplexed entries added in 6.7, and a kernel older than
        that answers them ENOSYS. The old one has been there since 2.6 and is
        not going anywhere, so it is what this uses.

        PRIVATE is the difference between a lock two threads share and a lock
        two processes share, and it is not cosmetic. A private futex is keyed
        by the address space and the address; a shared one is keyed by the
        page's inode and offset. Two threads use the private key and save the
        kernel a page lookup on every wait. Two processes over a MAP_SHARED
        page have different address spaces, so a private key would put them in
        different wait queues and neither would ever wake the other -- a lock
        that appears to work until it deadlocks. That is why there are two
        public spellings below and not one with a comment.
*/
#define LOCK_FUTEX_WAIT 0
#define LOCK_FUTEX_WAKE 1
#define LOCK_FUTEX_PRIVATE 128

/*
        A lock is one 32 bit word, and it is a struct rather than a bare b32
        so that a caller cannot pass an unrelated integer by accident and so
        that the field can grow -- an owner id for recursion, a waiter count
        for fairness -- without touching a call site.

        futex requires the word be 32 bits and naturally aligned. A struct
        with one b32 in it is both, on all three.

        Zero is unlocked, so a lock in .bss is ready without an initialiser
        and lock_start is only needed where a local one is being written down.
*/
typedef struct
{
        b32 word;
} lock;

#define lock_start \
        {          \
                LOCK_FREE  \
        }

/*
        The two traps, which are the only place this file touches the kernel.

        The wait is allowed to return for reasons that are not a release: a
        signal, or the word having changed before the kernel got to look at
        it, which is what the val argument is for and is why the answer is
        discarded. Every caller re-checks the word in a loop, so a spurious
        return costs one more turn and cannot be wrong.
*/
#define lock_futex_wait(word, was, private)                                 \
        ((fn)system_call_6(syscall(futex), (positive)(word),                \
                           (positive)(LOCK_FUTEX_WAIT | (private)),          \
                           (positive)(was), 0, 0, 0))
#define lock_futex_wake(word, private)                                      \
        ((fn)system_call_6(syscall(futex), (positive)(word),                \
                           (positive)(LOCK_FUTEX_WAKE | (private)),          \
                           1, 0, 0, 0))

/*
        Take, which is one compare-and-swap when nobody else holds it.

        The fast path is the whole point and is written first so it reads that
        way: if the word is free, claim it and return, no trap, no loop, no
        second memory reference. Everything below that line runs only when the
        first swap lost.

        The slow path exchanges 2 in rather than testing first. That is
        deliberate and is the subtle half of Drepper's design. Testing for a 1
        and only then upgrading to a 2 has a window: the holder can release
        between the test and the upgrade, and then the sleeper waits on a word
        nobody will wake. An unconditional exchange to 2 cannot have that
        window, because the value it gets back is the truth at the instant it
        wrote: a 0 means the lock became free and is now held by us, and
        anything else means it is still held and is now marked as contended,
        which is exactly the state the release path tests for.

        The word is left at 2 when a waiter acquires it, so that thread's
        release does one futex_wake nobody needed. Correct, and cheaper than
        the bookkeeping that would avoid it.

        private is a constant at every call site below, so the or in the two
        traps folds and there is no branch on it.
*/
static fn lock_take_private(lock address_to it, b32 private)
{
        b32 was;

        if_common(atomic_compare_exchange(address_of it->word, LOCK_FREE,
                                          LOCK_HELD))
                return;

        was = atomic_exchange(address_of it->word, LOCK_WAITED);

        while (was != LOCK_FREE)
        {
                lock_futex_wait(address_of it->word, LOCK_WAITED, private);
                was = atomic_exchange(address_of it->word, LOCK_WAITED);
        }
}

/*
        Release, which is one exchange when nobody is waiting.

        An exchange rather than a compare-and-swap because there is nothing to
        compare against: the holder is releasing, so whatever the word says it
        is going to become 0, and the only question is what it said on the way
        out. A 1 means no waiter was ever recorded and there is nobody to
        wake. Anything else means at least one thread wrote a 2, and one of
        them gets woken.

        Waking exactly one and not all of them is the whole difference between
        a lock and a thundering herd: every woken thread but one would find
        the word held again and go straight back to sleep, having paid for a
        context switch to learn nothing.
*/
static fn lock_release_private(lock address_to it, b32 private)
{
        if_common(atomic_exchange(address_of it->word, LOCK_FREE) == LOCK_HELD)
                return;

        lock_futex_wake(address_of it->word, private);
}

/*
        Try, which never traps and never blocks.

        One compare-and-swap and the answer. This is the primitive
        ftrylockfile would be built on, and it is also what a caller that
        cannot afford to block -- a signal handler, a diagnostic path -- has
        to use instead of take.
*/
static bool lock_try(lock address_to it)
{
        return atomic_compare_exchange(address_of it->word, LOCK_FREE,
                                       LOCK_HELD) != 0;
}

//      Whether anybody holds it. A read of a word that another thread may be
//      writing, so it is a fact about the past and useful only for assertions
//      and for a test that wants to see the state machine move.
static b32 lock_state(lock address_to it)
{
        return __atomic_load_n(address_of it->word, __ATOMIC_RELAXED);
}

//      The two threads-in-one-process spellings, which is the case this
//      library will meet first.
#define lock_take(it) lock_take_private((it), LOCK_FUTEX_PRIVATE)
#define lock_release(it) lock_release_private((it), LOCK_FUTEX_PRIVATE)

//      The two across-processes spellings, for a lock that lives in a
//      MAP_SHARED page. Same algorithm, different futex key; see the note
//      above LOCK_FUTEX_PRIVATE for why mixing them silently deadlocks.
#define lock_take_shared(it) lock_take_private((it), 0)
#define lock_release_shared(it) lock_release_private((it), 0)

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_LOCK
#endif // STANDARD_SKIP_LOCK

//      _exit stays out of the allocator: the umbrella owns the shim.
#define STANDARD_NO_UNDERSCORE_EXIT

#ifndef STANDARD_SKIP_ALLOCATOR
/* ---- allocator.c ---- */
/*
        Experimental C standard library

        The allocator: malloc, free, calloc, realloc, and the aligned pair

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_ALLOCATOR
#define STANDARD_MODERN_C_STANDARD_ALLOCATOR

/*
        Nothing here in a kernel build, and nothing here on Windows.

        src/core.c defines STANDARD_MODERN_C_KERNEL and then includes
        compiler_memory.c, so everything in this file would otherwise be
        compiled into the module, where mmap is not a thing that can be
        called and where a symbol named free would be a very bad idea. A
        module allocates with kmalloc and vmalloc and always has.

        Windows is out for the plainer reason that memory() and memory_free()
        -- the mmap pair this stands on -- are themselves inside library.c's
        "not Windows" guard, so there is nothing underneath to build on.
*/
#if !defined(KERNEL_MODE) && !defined(WINDOWS)

/*
        This is ordinary C on purpose, for the same reason netlink.c is.

        library.c and everything it includes holds declarations and assembly
        and nothing else, and that is checked. An allocator is not a floor. It
        is policy: which shelf a size goes on, when a chunk is asked of the
        kernel, whether a block that shrank is worth moving. None of that is a
        thing one machine does differently from another, so it is written once
        here rather than three times there, and the same bytes are what run on
        x86_64, arm64 and riscv64.

        What it stands on is the assembly. memory() is mmap and memory_free()
        is munmap, both already written for all three; memory_copy_apart is
        memcpy and memory_zero is the store loop; bits_leading_zeros is one
        instruction on two of the three targets and a six step fold on the
        third. Nothing below reimplements any of them.

        THE CONTRACT PROBLEM, WHICH IS THE WHOLE DESIGN

        memory_free(address, size) is munmap and munmap wants the length. The
        caller of free(p) has one pointer and no length, and by the time it
        calls it has usually forgotten there ever was one. So the length has
        to be written down somewhere the pointer can reach, which means a
        header, which means the returned pointer is not the start of what was
        allocated.

        That is where alignment bites. malloc must return something suitable
        for any type the program might put there, which on all three of these
        targets is sixteen bytes -- long double on x86_64, and the pair
        instructions on arm64 and riscv64 want it even where the ABI would
        settle for eight. The obvious reading is that the header must
        therefore be sixteen bytes too, and that reading is wrong, and paying
        it costs half of every small allocation.

        What has to be a multiple of sixteen is the STRIDE, not the header. If
        blocks start at addresses that are eight modulo sixteen and every
        block size is a multiple of sixteen, then base plus eight is sixteen
        aligned for every block in the chunk, forever, and the header is one
        word. So that is the layout:

              block base   ->  [ tag ][ payload ..................... ]
                                 8 b    size - 8 bytes, 16 byte aligned

        The tag sits at payload minus eight and says what the block is. Two
        kinds of block need a second word -- a mapping has to remember how
        many bytes to hand back to munmap, and an over-aligned block has to
        remember where the real allocation started -- and those two put it at
        payload minus sixteen, in front of the tag:

              [ extra ][ tag ][ payload ......... ]     mapped, or shifted

        Reading is still one load at payload minus eight in every case, and
        the second word is only reached after the tag has already said it is
        there. glibc arrives at the same place from the other direction: its
        chunk header is two words, but one of them is the previous chunk's
        payload, so a live small chunk also costs eight.

        WHERE THIS CAME FROM

        src/sh/awk.c has had a working size class allocator in it for a while:
        awk_take and awk_give, twenty two power of two classes, the class in
        the eight bytes before the block, a bump pointer for fresh chunks and
        one free list per class. This is that allocator promoted, and it is
        worth being exact about what changed, because the changes are the
        difference between something awk can live with and something every
        program in the tree has to live with.

          - awk returns block + 8 from a block whose base is a power of two
            multiple, so its payloads are eight byte aligned and not sixteen.
            awk only ever puts characters and its own structures there and
            never noticed. A malloc cannot ship that, hence the stride above.

          - awk's classes are powers of two, so a 40 byte structure occupies
            64 bytes and a 1025 byte buffer occupies 2048. Here each power of
            two is cut into quarters above 64 -- 64 80 96 112 128 160 192 224
            and so on -- which caps the rounding waste at 25% instead of just
            under 100%, for one extra shift in the lookup and 52 free list
            heads instead of 22.

          - awk reports allocation failure and exits. A library returns null.

          - awk abandons whatever is left of a chunk when the next request
            does not fit. Here the remainder is cut into the largest classes
            that fit and pushed onto their free lists, so a chunk boundary
            costs nothing at all.

          - awk's chunk is four megabytes, always, so a program that allocates
            one string pays four megabytes of address space for it. Here the
            first chunk is 64 KiB and each next one doubles to a ceiling of
            four megabytes, which is the usual shape and costs one variable.

          - The free list link overwrites awk's class word, so a block on the
            list has forgotten what it is until it is popped again. Here the
            link lives in the payload -- every class is at least sixteen bytes
            so there are always at least eight to put it in -- and the tag is
            true at every moment of a block's life. That is what makes it safe
            for free() to look at the tag and refuse a block whose tag is not
            one it wrote.

        THREADS

        There are none. This allocator is single threaded and there is no lock
        anywhere in it: two threads calling malloc at the same time will
        corrupt the free lists, and two threads calling free on blocks of the
        same class will lose one of them. That is a deliberate choice for now
        and not an oversight -- nothing in the tree runs a second thread
        through it, and an atomic on every allocation is a real cost to pay
        for a case that does not exist yet.

        When it does exist, the whole of the mutable state is the six file
        scope objects declared below: the free list array, the bump pointer,
        what is left beside it, and the next chunk size. Every one of them is
        touched only inside allocator_take and memory_give. A single lock
        taken at the top of those two, or a per class list made atomic with a
        tagged head, is the whole of the work, and the layout above does not
        change either way.

        WHAT IS NEVER GIVEN BACK

        Chunk memory is never unmapped. A block freed goes onto its class's
        free list and waits there for a request of that class; it is not
        coalesced with its neighbours, because there are no neighbour links to
        coalesce with and adding them would put a second word back into every
        small block. So the high water mark of each class is held for the life
        of the process. Blocks too big for any class are their own mapping and
        those are handed straight back to the kernel on free, which is where
        the memory a long running program actually notices lives.
*/

//      The smallest alignment malloc may return. Sixteen on all three of
//      these targets and not likely to grow; if it ever does, every class
//      size below has to become a multiple of the new number.
#define ALLOCATOR_ALIGNMENT 16

//      One word in front of the payload for a class block, two for the two
//      kinds that carry a second word. The wide one is what a mapping's base
//      is offset by, so it also keeps the payload sixteen aligned.
#define ALLOCATOR_HEADER 8
#define ALLOCATOR_HEADER_WIDE 16

/*
        Four kilobytes, and it is a rounding unit rather than a claim about
        the machine. arm64 is configured with sixteen or sixty four kilobyte
        pages on plenty of real systems, and this number being smaller than
        the true page is harmless in both directions: mmap rounds a length up
        on its own, and munmap rounds a length up to the same true page, so a
        mapping recorded as a multiple of four kilobytes is unmapped exactly
        and completely.

        That holds for mremap too, and mremap is the one that would hurt if it
        did not: rounding a length up to four kilobytes can never cross a
        larger page boundary that the true rounding would not have crossed,
        because the true rounding of a length is itself a multiple of four
        kilobytes and is at least the length, so the four kilobyte rounding
        always lands at or below it and both then round to the same page. A
        length that came out short of the real mapping would make mremap move
        the front of a mapping and leave the tail of it behind, and that is
        the failure this paragraph exists to rule out.

        The only consequence is that malloc_usable_size reports less than the
        kernel really left there on a machine with larger pages, which is the
        safe direction to be wrong in.
*/
#define ALLOCATOR_PAGE 4096

//      The first chunk asked of the kernel, and the largest. Doubling from
//      one to the other means a program that allocates a handful of strings
//      touches 64 KiB of address space and a program that allocates a million
//      of them stops asking after a dozen calls.
#define ALLOCATOR_CHUNK_FIRST (64u << 10)
#define ALLOCATOR_CHUNK_MAX (4u << 20)

/*
        A ceiling on any single size this will consider, so that adding a
        header, an alignment's worth of padding and a page of rounding to it
        cannot wrap. It is a sixteenth of the address space, which is 2^60 on
        these three and 2^28 where positive is thirty two bits wide, and in
        both cases it is far past anything the kernel would map anyway. The
        point is not the number, the point is that every sum below is proven
        not to overflow by one comparison at the top.
*/
#define ALLOCATOR_LIMIT (((positive)-1) >> 4)

/*
        Fifty two shelves. Sixteen, thirty two and forty eight on their own,
        and from sixty four upward each power of two cut into quarters, up to
        a quarter of a megabyte. Everything above that is its own mapping.

        Every one of them is a multiple of sixteen, which is what keeps the
        payloads aligned as the bump pointer walks a chunk, and the table is
        written out rather than computed so that the arithmetic in
        allocator_class_of can be checked against something that is not
        itself.
*/
#define ALLOCATOR_CLASSES 52
#define ALLOCATOR_LARGEST 262144

//      Tags that are not class indexes. Both are just past the shelves, so a
//      single unsigned compare separates a live block of a class from
//      everything else.
#define ALLOCATOR_MAPPED ((positive)ALLOCATOR_CLASSES)
#define ALLOCATOR_SHIFTED ((positive)ALLOCATOR_CLASSES + 1)

/*
        And a whole band of tags above those, one per shelf, that mean "on the
        free list of this shelf right now".

        This is what the link living in the payload buys. awk's allocator puts
        the free list link where the class word is, so a block on the list has
        no tag at all and a second free of the same pointer walks straight
        through and pushes it a second time -- after which two allocations of
        that shelf return the same address and the program has two owners of
        one block and no way to find out. Here the tag is a separate word that
        nothing on the list path needs, so freeing can move it into this band
        and taking can move it back, and a second free finds a tag that is not
        a shelf and does nothing at all.

        It is not a heap checker. It does not notice a pointer into the middle
        of a block, or a write that ran off the end of the block before it, or
        a free of a stack address that happens to have a small number eight
        bytes in front of it. What it does is turn the single most common
        memory bug in C from silent list corruption into nothing happening,
        for one addition on each side.
*/
#define ALLOCATOR_FREED ((positive)ALLOCATOR_CLASSES + 2)

//      mremap's flag word: the kernel may pick a new address rather than
//      failing when the mapping cannot grow where it stands.
#define ALLOCATOR_MREMAP_MAYMOVE 1

//      What posix_memalign answers with. It reports through its return value
//      rather than through errno, which is the one place in the C library
//      where that is true, and it is why this file needs no errno at all.
#define ALLOCATOR_EINVAL 22
#define ALLOCATOR_ENOMEM 12

static const positive allocator_class_size[ALLOCATOR_CLASSES] = {
        16, 32, 48,
        64, 80, 96, 112,
        128, 160, 192, 224,
        256, 320, 384, 448,
        512, 640, 768, 896,
        1024, 1280, 1536, 1792,
        2048, 2560, 3072, 3584,
        4096, 5120, 6144, 7168,
        8192, 10240, 12288, 14336,
        16384, 20480, 24576, 28672,
        32768, 40960, 49152, 57344,
        65536, 81920, 98304, 114688,
        131072, 163840, 196608, 229376,
        262144,
};

//      One head per shelf, holding payload addresses rather than bases. The
//      link to the next free block lives in the payload's first eight bytes,
//      which every class has room for, so a pop is a load and a store and the
//      tag is never disturbed.
//
//      The heads themselves are in library.c beside memory_take, which is the
//      only routine that pops one on the path that matters. What is here is
//      the rest of the family reaching the same object. The two literals that
//      assembly spells out are checked against this file's constants below.
_Static_assert(ALLOCATOR_CLASSES == 52,
               "library.c reserves 52 shelf heads for allocator_free_list");
_Static_assert(ALLOCATOR_LARGEST - ALLOCATOR_HEADER == 262136,
               "library.c's memory_take compares the request against 262136");

//      The base of the next block to be cut from the current chunk, which is
//      always eight modulo sixteen, and how many bytes are left after it.
static p8 address_to allocator_bump;
static positive allocator_bump_left;

//      How large the next chunk asked of the kernel will be. Zero means none
//      has been asked for yet, which is what a program that never allocates
//      pays: three words of bss and no syscall.
static positive allocator_chunk_next;

//      The address of the tag, and of the second word the two wide kinds put
//      in front of it. Written as functions returning the address rather than
//      as macros returning the value so that both reading and writing them
//      read the same at the call site.
static positive address_to allocator_tag(address_any block)
{
        return (positive address_to)block - 1;
}

static positive address_to allocator_extra(address_any block)
{
        return (positive address_to)block - 2;
}

//      The free list link, which is the payload's own first word.
static address_any address_to allocator_link(address_any block)
{
        return (address_any address_to)block;
}

#define allocator_page_round(bytes)                                         \
        (((bytes) + (ALLOCATOR_PAGE - 1)) & ~(positive)(ALLOCATOR_PAGE - 1))

/*
        Which shelf a request of this many bytes -- header included -- belongs
        on, or ALLOCATOR_CLASSES when it belongs on none of them.

        The four smallest are answered by a shift because the quarter cut has
        no meaning below sixty four: a quarter of thirty two is eight and the
        stride would stop being sixteen. Sixteen, thirty two, forty eight and
        sixty four are sixteen apart, so the shelf is how many whole sixteens
        the request needs, which is one subtraction and one shift. What stood
        here was four compares and four branches arriving at the same four
        numbers, and it was the first thing every small allocation ran into.
        It is now ahead of the ceiling test rather than behind it, because a
        small request is the common one and it should reach its answer on one
        compare; a request too large for any shelf pays the extra compare and
        is about to call the kernel anyway.

        Nothing arrives here with a want of zero, which is what makes the
        subtraction safe. The two allocating callers add ALLOCATOR_HEADER
        before asking, so the least either can present is eight, and the
        remainder walk only asks while at least sixteen bytes are left.

        From sixty five upward the answer is arithmetic. Take the position of
        the highest set bit, call it high, so that the request sits in
        [2^high, 2^(high+1)). A quarter of that interval is 2^(high-2), and
        rounding the request up to a whole number of quarters gives a value
        from four to eight. Four through seven are the four shelves of this
        interval; eight is the next interval's first shelf, and the index
        arithmetic below lands on it without a branch, because the four
        shelves of every interval are consecutive.

        Which is the reason the shelves above sixty four are laid out in the
        table in groups of four in the first place.

        The highest set bit comes from top_bit_known rather than from
        bits_leading_zeros. They answer the same question and the assembly one
        is the better instruction on two of the three machines, but it is an
        assembly symbol: the compiler cannot see through a call to one, so
        what stood here was a call through the PLT with a stack frame built
        around it and the argument spilled across it, for a value already in a
        register. top_bit_known is the umbrella's own spelling of the same
        question and it is inline everywhere -- bsr on x86_64, clz on arm64,
        and on a riscv baseline with no Zbb to count leading zeros with, the
        same halving search the riscv bodies in library.c use. Folding it in
        is also what let the whole routine be inlined into allocator_take,
        which it was not before: measured on x86_64 over two million small
        allocations, 158.2 million instructions and 80.0 million cycles
        became 119.7 million and 32.7 million.
*/
static b32 allocator_class_of(positive want)
{
        if (want <= 64)
                return (b32)((want - 1) >> 4);

        if (want > ALLOCATOR_LARGEST)
                return ALLOCATOR_CLASSES;

        b32 high = (b32)top_bit_known(want);
        positive step = (positive)1 << (high - 2);
        positive quarter = (want + step - 1) >> (high - 2);

        return 3 + 4 * (high - 6) + (b32)(quarter - 4);
}

/*
        How many bytes a fresh allocation of this size would actually leave
        usable. realloc asks this rather than comparing sizes, because the
        question it needs answered is not "is the new size smaller" but "would
        a new block be a different block at all" -- and inside one shelf the
        answer is no, whichever direction the size moved.
*/
static positive allocator_fit(positive bytes)
{
        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        if (class < ALLOCATOR_CLASSES)
                return allocator_class_size[class] - ALLOCATOR_HEADER;

        return allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE) -
               ALLOCATOR_HEADER_WIDE;
}

/*
        Spend what is left of the current chunk before abandoning it.

        A chunk ends when the next request does not fit in what remains, and
        what remains at that moment is anything from nothing to one byte short
        of the largest class. Cutting it into the biggest shelves that fit and
        pushing those onto their free lists turns the whole of it back into
        allocations, so the only memory a chunk boundary loses is whatever is
        left under sixteen bytes.

        allocator_class_of rounds up, so the shelf it names for the remainder
        is either exactly the remainder or one too big, and stepping back one
        is enough. The loop is bounded by fifty two iterations because each
        turn takes at least sixteen bytes and each next shelf is no larger
        than the one before.
*/
static fn allocator_spend_remainder(void)
{
        while (allocator_bump_left >= allocator_class_size[0])
        {
                b32 class = allocator_class_of(allocator_bump_left);

                if (class >= ALLOCATOR_CLASSES)
                        class = ALLOCATOR_CLASSES - 1;
                else if (allocator_class_size[class] > allocator_bump_left)
                        class--;

                address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

                address_to allocator_tag(block) = ALLOCATOR_FREED + class;
                address_to allocator_link(block) = allocator_free_list[class];
                allocator_free_list[class] = block;

                allocator_bump += allocator_class_size[class];
                allocator_bump_left -= allocator_class_size[class];
        }
}

/*
        The one place a block comes from.

        fresh, when a caller passes an address for it, comes back true only
        when the payload is known to be untouched kernel memory and therefore
        already zero. That is exactly two cases: a block cut from the bump
        pointer, because a chunk is freshly mapped and the bump pointer only
        ever moves forward over it, and a mapping of its own. A block off a
        free list has been written by whoever had it last and says false, and
        so does a block cut from a chunk's remainder, because pushing it onto
        a free list wrote a link into its payload. calloc is the only caller
        that asks, and the only thing it does with a false is zero the block
        it would otherwise have had to zero anyway.
*/
static address_any allocator_take(positive bytes, bool address_to fresh)
{
        if (fresh)
                address_to fresh = 0;

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        //      Too big for any shelf: its own mapping, and the length written
        //      down in front of the tag because munmap will want it back.
        if (class >= ALLOCATOR_CLASSES)
        {
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive got = (positive)memory(whole);

                //      memory() is the raw trap and returns the kernel's
                //      answer unchanged, so a failure is a small negative
                //      number wearing an unsigned hat.
                if (!got || system_failed(got))
                        return null;

                address_any block = (address_any)(got + ALLOCATOR_HEADER_WIDE);

                address_to allocator_tag(block) = ALLOCATOR_MAPPED;
                address_to allocator_extra(block) = whole;

                if (fresh)
                        address_to fresh = 1;

                return block;
        }

        if (allocator_free_list[class])
        {
                address_any block = allocator_free_list[class];

                allocator_free_list[class] = address_to allocator_link(block);

                //      Back from the freed band to the plain shelf number,
                //      which is what says this block is live.
                address_to allocator_tag(block) = (positive)class;

                return block;
        }

        positive size = allocator_class_size[class];

        if (allocator_bump_left < size)
        {
                allocator_spend_remainder();

                positive chunk = allocator_chunk_next;

                if (!chunk)
                        chunk = ALLOCATOR_CHUNK_FIRST;

                //      A shelf larger than the chunk schedule has reached
                //      gets a chunk of its own size instead, rather than the
                //      schedule being jumped forward for one request.
                if (chunk < size + ALLOCATOR_HEADER)
                        chunk = allocator_page_round(size + ALLOCATOR_HEADER);

                positive got = (positive)memory(chunk);

                if (!got || system_failed(got))
                        return null;

                //      Eight bytes of the page go unused so that the first
                //      base lands eight past a sixteen byte boundary, which
                //      is what puts every payload in the chunk on one.
                allocator_bump = (p8 address_to)(got + ALLOCATOR_HEADER);
                allocator_bump_left = chunk - ALLOCATOR_HEADER;

                allocator_chunk_next = chunk < ALLOCATOR_CHUNK_MAX
                                               ? chunk + chunk
                                               : ALLOCATOR_CHUNK_MAX;

                if (allocator_chunk_next > ALLOCATOR_CHUNK_MAX)
                        allocator_chunk_next = ALLOCATOR_CHUNK_MAX;
        }

        address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

        allocator_bump += size;
        allocator_bump_left -= size;

        address_to allocator_tag(block) = (positive)class;

        if (fresh)
                address_to fresh = 1;

        return block;
}

/*
        malloc.

        A request of zero is a request for a block: the standard allows null
        and allows a pointer, and a pointer is the answer that does not make
        every caller check twice, so zero lands on the sixteen byte shelf like
        anything else under nine bytes and comes back with eight usable bytes
        and a tag free() will recognise. Two calls to malloc(0) return two
        different pointers, which is what a program that uses the pointer as
        an identity expects.
*/
//      What library.c's memory_take jumps to when the shelf could not answer:
//      a request past the largest shelf, an empty shelf, or a size that would
//      wrap. Everything the fast path skipped is redone here, because a slow
//      path that runs once per refill can afford to.
//
//      pub, and it has to be. The only thing that reaches this is a jump
//      inside a top-level __asm__ string, which the compiler treats as text
//      it cannot read: nothing it can see refers to the name. The shell and
//      the image are built with -flto -fwhole-program, where that is licence
//      to delete the body, and the link then fails on an undefined
//      allocator_take_slow. pub carries KEEP -- __attribute__((used)) -- and
//      is what says the reference exists somewhere the compiler is not
//      looking. The same goes for allocator_give_slow below.
pub address_any allocator_take_slow(positive bytes)
{
        return allocator_take(bytes, null);
}

/*
        free.

        Null is a no-op, and that is not a courtesy: the cleanup path of
        almost every function in a C program frees things that may never have
        been allocated, and a free that could not take null would put a test
        around every one of them.

        The tag decides the rest. A mapping goes back to the kernel whole, an
        over-aligned block hands the question to the allocation underneath it,
        and everything else goes onto the free list of the shelf it has said
        it belongs to since it was cut.

        The shelf comparison is deliberately first. It is the only path in a
        warmed malloc/free loop, while shifted and mapped blocks are the rare
        cases. Putting their two equality tests first cost that loop two
        branches and four retired instructions per pair.

        A tag in the freed band means this block is already on a list, so the
        second free of it does nothing. A tag that is none of the above means
        the pointer did not come from here at all, or points into the middle
        of something, or the block in front of it overran and wrote over the
        word. Nothing here can tell those apart and there is no abort to reach
        for, so the block is left exactly as it is. That leaks, and leaking is
        the containment: the alternative is to index the free list array with
        whatever the number happened to be and write a pointer through it.
*/
//      The shelf push is assembly in library.c beside the pop. What is left
//      here is everything a shelf number does not cover, reached by a jump
//      from it: the block is known to carry a tag of ALLOCATOR_CLASSES or
//      more, so the shelf test is not repeated.
pub fn allocator_give_slow(address_any block)
{
        positive tag = address_to allocator_tag(block);

        if (tag == ALLOCATOR_SHIFTED)
        {
                memory_give((address_any)address_to allocator_extra(block));
                return;
        }

        if (tag == ALLOCATOR_MAPPED)
        {
                memory_free((address_any)((positive)block -
                                          ALLOCATOR_HEADER_WIDE),
                            address_to allocator_extra(block));
                return;
        }

        //      A tag in the freed band, or one this allocator did not write.
}

/*
        malloc_usable_size.

        How many bytes are really there, which is at least what was asked for
        and usually more, because a shelf is a rounded size. A program is
        allowed to use all of it. Reporting a shelf's whole payload rather
        than the original request is both the standard behaviour and the
        honest one -- the memory is spent either way -- and it is what lets
        realloc decide in one comparison whether a block needs to move.

        An over-aligned block reports what is left of the allocation
        underneath it after the padding that got it onto its boundary, which
        is the only figure a caller can safely write into.

        As in free, the ordinary shelf is recognized first: asking the usable
        size of a live malloc block is the common call, and needs only the one
        range check before the table lookup.
*/
pub PURE positive memory_usable_size(address_any block)
{
        if (!block)
                return 0;

        positive tag = address_to allocator_tag(block);

        if (tag < ALLOCATOR_CLASSES)
                return allocator_class_size[tag] - ALLOCATOR_HEADER;

        if (tag == ALLOCATOR_SHIFTED)
        {
                positive inner = address_to allocator_extra(block);
                positive lead = (positive)block - inner;
                positive whole = memory_usable_size((address_any)inner);

                return whole > lead ? whole - lead : 0;
        }

        if (tag == ALLOCATOR_MAPPED)
                return address_to allocator_extra(block) - ALLOCATOR_HEADER_WIDE;

        return 0;
}

/*
        calloc.

        Two things beyond malloc. The multiplication has to be checked,
        because calloc(count, size) is the one allocation call in C that takes
        two numbers and multiplies them, and every historical hole of this
        shape has been an unchecked multiply wrapping to a small number and a
        loop then writing count elements into it. The check is the
        multiplication itself, which is where the answer was all along.

        What stood here was count > MAX / size, which is exact and is what
        the interface needs, and which on all three of these machines is a
        sixty four bit division: twenty to forty cycles, not foldable because
        neither operand is known, and in front of every call. The comment
        that came with it said that cost sat on the cold side of a call about
        to touch every byte of the result anyway, and that is false on
        precisely the path the next paragraph is proud of -- a calloc large
        enough to get its own mapping writes nothing at all, so the division
        was the whole of the call.

        The wide half of the product is the same test and is already computed
        by the multiply. __builtin_mul_overflow is mul and seto on x86_64,
        mul and umulh on arm64, mul and mulhu on riscv64, inline on all three
        with no reach into a libgcc helper that a -nostdlib link would have
        no symbol for, which was worth checking before trusting it. The two
        forms agree everywhere including both corners: a size of zero and a
        count of zero each give a product of zero and no overflow, which is
        what the division form arrived at by skipping itself. Measured on
        x86_64 over two million calloc calls, 228.0 million instructions and
        44.7 million cycles became 184.0 million and 38.7 million.

        And the zeroing is skipped exactly when it can be proven unnecessary.
        A block cut from a chunk the kernel has only just handed over, or a
        mapping of its own, is already zero and stays zero until somebody
        writes to it; a block off a free list held somebody else's data ten
        instructions ago. allocator_take knows which of those it did and says
        so. For a large calloc that is the whole cost of the call: the pages
        are not even faulted in until they are read.
*/
pub address_any memory_take_zeroed(positive count, positive size)
{
        positive bytes;

        if (__builtin_mul_overflow(count, size, address_of bytes))
                return null;

        bool fresh = 0;
        address_any block = allocator_take(bytes, address_of fresh);

        if (!block)
                return null;

        if (!fresh && bytes)
                memory_zero(block, bytes);

        return block;
}

/*
        realloc, and its four corners.

        A null block is malloc, because that is what makes a grow-as-you-go
        loop start from nothing without a special first turn. A size of zero
        frees and answers null, which is what glibc does and what every
        program written before C23 deprecated it expects; a program that wants
        the other reading can test the size itself.

        Otherwise the question is whether the block has to move at all, and
        the answer is not "did the size go up". Sizes inside one shelf all get
        the same block, so the test is whether a fresh allocation of the new
        size would have a different usable size than this one already has. If
        it would not, the block stays exactly where it is, and a loop that
        grows a buffer a byte at a time crosses a shelf boundary about fifty
        times over the whole address space instead of copying every turn.

        When it would, the block moves, and min(old, new) bytes come with it.
        The old usable size is the right thing to copy up to rather than the
        old request, which is not written down anywhere: every byte of it is
        this block's and copying a few more of them than the caller ever wrote
        is free.

        The one case that does not move is a mapping still too large for any
        shelf. mremap resizes those in the page tables, which for anything
        over a megabyte is the difference between a syscall and a memcpy, and
        it is allowed to move it, in which case the kernel has already brought
        the contents along. If the kernel refuses -- and it can, there is no
        guarantee here -- the copy underneath catches it.

        A failed allocation while shrinking answers with the original block
        rather than null. It is still there, it is still large enough, and
        nothing was freed; answering null would be true of the new block and a
        lie about the old one, and callers write p = realloc(p, n).
*/
pub address_any memory_resize(address_any block, positive bytes)
{
        if (!block)
                return memory_take(bytes);

        if (!bytes)
        {
                memory_give(block);
                return null;
        }

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        positive usable = memory_usable_size(block);
        positive tag = address_to allocator_tag(block);

        if (allocator_fit(bytes) == usable)
                return block;

#if defined(LINUX)
        //      Linux only, because mremap is a Linux call and the macOS
        //      table beside it has no number to name. Everywhere else the
        //      copy below is the whole of realloc for a mapping, which is
        //      slower and no less correct.
        if (tag == ALLOCATOR_MAPPED &&
            bytes + ALLOCATOR_HEADER > ALLOCATOR_LARGEST)
        {
                positive have = address_to allocator_extra(block);
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive moved = (positive)system_call_4(
                        syscall(mremap),
                        (positive)block - ALLOCATOR_HEADER_WIDE, have, whole,
                        ALLOCATOR_MREMAP_MAYMOVE);

                if (moved && !system_failed(moved))
                {
                        address_any grown =
                                (address_any)(moved + ALLOCATOR_HEADER_WIDE);

                        address_to allocator_tag(grown) = ALLOCATOR_MAPPED;
                        address_to allocator_extra(grown) = whole;

                        return grown;
                }
        }
#endif

        address_any grown = memory_take(bytes);

        if (!grown)
                return bytes > usable ? null : block;

        memory_copy_apart(grown, block, bytes < usable ? bytes : usable);
        memory_give(block);

        return grown;
}

/* The cold half of memory_resize_reserve. On x86 GCC's IPA folding produces
   the smaller image when it may choose the outline boundary itself; on the
   fixed-width targets keeping it cold and out of line avoids cloning this
   allocator path into every editor and getline caller. */
#if X64
#define MEMORY_RESIZE_GROWTH
#else
#define MEMORY_RESIZE_GROWTH COLD __attribute__((noinline))
#endif
static address_any MEMORY_RESIZE_GROWTH memory_resize_growth(
    address_any block, positive have, positive wanted, positive first,
    positive address_to grown)
{
        positive room = memory_growth(have, wanted, first);

        if (!room)
                return null;

        block = memory_resize(block, room);

        if (block)
                address_to grown = room;

        return block;
}
#undef MEMORY_RESIZE_GROWTH

/*
        aligned_alloc, and the shifted block it invents.

        Sixteen and under is already true of every block this allocator hands
        out, so those requests are plain malloc and cost nothing extra. Above
        that the only way to land on a boundary is to ask for enough room to
        walk forward to one: the alignment itself, plus the sixteen bytes the
        walk has to start past so that the header written at the destination
        cannot land on the inner block's own header.

        The block that comes back is tagged shifted and remembers the inner
        pointer, and free, realloc and malloc_usable_size all follow that one
        word back to the real allocation. Which means the padding in front is
        not tracked and not reused -- it is simply part of a larger block that
        will be freed whole.

        realloc of one of these answers with an ordinary sixteen byte aligned
        block, because a resize is a new allocation and nothing in the block
        records what alignment it was originally asked to sit on. glibc does
        the same and C says nothing about the case, but a caller that keeps
        needing the boundary has to ask for it again rather than resize.

        C11 says the size passed here should be a multiple of the alignment.
        glibc does not enforce that and neither does this, because refusing
        would break the many callers that ask for a page aligned buffer of
        exactly the length they have, and because there is no case where
        honouring the request is unsafe.
*/
pub address_any memory_take_aligned(positive alignment, positive bytes)
{
        //      A power of two, and not zero. The and-with-one-less test is
        //      also true of zero, so zero is refused first.
        if (!alignment || (alignment & (alignment - 1)))
                return null;

        if (alignment <= ALLOCATOR_ALIGNMENT)
                return memory_take(bytes);

        if (alignment >= ALLOCATOR_LIMIT ||
            bytes >= ALLOCATOR_LIMIT - alignment - ALLOCATOR_HEADER_WIDE)
                return null;

        address_any inner =
                memory_take(bytes + alignment + ALLOCATOR_HEADER_WIDE);

        if (!inner)
                return null;

        positive walk = (positive)inner + ALLOCATOR_HEADER_WIDE;
        positive landed = (walk + alignment - 1) & ~(alignment - 1);
        address_any block = (address_any)landed;

        address_to allocator_tag(block) = ALLOCATOR_SHIFTED;
        address_to allocator_extra(block) = (positive)inner;

        return block;
}

/*
        posix_memalign.

        The same allocation with the older interface around it: the result
        goes through a pointer and the failure comes back as the errno value
        itself rather than being left in a global. It is stricter than
        aligned_alloc about the alignment -- a power of two AND at least the
        width of a pointer -- and that stricter rule is the standard's, not an
        opinion, so four is refused here and accepted above.

        On failure the caller's pointer is left alone rather than being set to
        null, which is what glibc does and what a caller checking the return
        value will never notice either way.
*/
pub b32 memory_take_aligned_into(address_any address_to result,
                                 positive alignment, positive bytes)
{
        if (!result)
                return ALLOCATOR_EINVAL;

        if (!alignment || (alignment & (alignment - 1)) ||
            alignment < sizeof(address_any))
                return ALLOCATOR_EINVAL;

        address_any block = memory_take_aligned(alignment, bytes);

        if (!block)
                return ALLOCATOR_ENOMEM;

        address_to result = block;
        return 0;
}

/*
        The names C knows these by.

        Aliases rather than wrappers, so malloc and memory_take are one symbol
        at one address and neither costs a jump to reach the other. A program
        may take the address of either and compare them and they will be
        equal, which is the honest answer: they are the same function and the
        prose name is the one it was written under.
*/
//      memory_take is assembly in library.c, and GCC's alias attribute wants
//      a C definition in this translation unit to point at. A .set is the
//      same thing one layer down and does not care how the target was
//      written, which is how library.c spells every other standard name.
__asm__(ASM_ALIAS(malloc, memory_take));

__asm__(ASM_ALIAS(free, memory_give));

pub address_any calloc(positive count, positive size)
        __attribute__((alias("memory_take_zeroed")));

pub address_any realloc(address_any block, positive bytes)
        __attribute__((alias("memory_resize")));

pub address_any aligned_alloc(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

pub b32 posix_memalign(address_any address_to result, positive alignment,
                       positive bytes)
        __attribute__((alias("memory_take_aligned_into")));

pub PURE positive malloc_usable_size(address_any block)
        __attribute__((alias("memory_usable_size")));

//      memalign is aligned_alloc with the arguments in the same order and
//      without C11's multiple-of rule, which this does not enforce anyway, so
//      it is the same function. Programs old enough to call it exist.
pub address_any memalign(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

#endif // !KERNEL_MODE && !WINDOWS

#endif // STANDARD_MODERN_C_STANDARD_ALLOCATOR
#endif // STANDARD_SKIP_ALLOCATOR

#ifndef STANDARD_SKIP_NUMBERS
/* ---- numbers.c ---- */
/*
        Experimental C standard library

        numbers: text into a number, and the decimal one correctly rounded

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_NUMBERS
#define STANDARD_MODERN_C_STANDARD_NUMBERS

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason every file in this directory carries the same three lines:
        core.c includes the umbrella, library.c sets KERNEL_MODE from
        __MODULE__, and a kernel that already has its own errno and its own
        idea of a double must not be handed a second one.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        THE EXISTING INTEGER ENGINE, AND THE STANDARD WRAPPERS

        src/platform/standard.inc holds the integer scanners as assembly at
        three-architecture parity, and attaches the names that cannot report
        an error with ASM_ALIAS:

              abs labs llabs        absolute_whole, absolute_wide
              atoi atol atoll       string_to_whole, string_to_whole_wide

        library.c gives all ten standard names their exact C types. The six
        operations above remain direct assembly aliases. strtol, strtoll,
        strtoul and strtoull are C bodies here because errno is C-library
        state and deliberately is not a dependency of the raw platform
        library. Their pointer casts bridge C's char to this tree's unsigned
        string_address; both are one address in the ABI.

        WHAT THE CHECKED ASSEMBLY ENTRIES ADD

        The raw string_to_number routines keep their original contract: they
        saturate and do not touch errno, so their pure atoi/atol callers stay
        pure. Their checked entries call that exact scanner once and preserve
        its sticky overflow register long enough to store one b32. The four C
        wrappers below turn that bit into ERANGE. A successful conversion does
        not clear an errno it did not set, as C requires, and a spelled-out
        limit is distinguishable from the same clamped answer without walking
        the digits again.
*/
/*
        These ten were once declared here in the house types, and are not any
        more.

        library.c says them first, in C's own spellings -- long labs(long),
        int atoi(const char *) -- and it is right to and this file was not.
        The house and C spellings disagree about nothing at runtime: their
        integers are the same width in the same register on all three targets.
        They disagree about TYPE, which is what a second declaration checks,
        and C's spelling is the one that has to win when a program brings its
        own <stdlib.h> declarations.

        So the declarations are gone and nothing else is. The definitions
        this file owns -- the checked strto* wrappers, strtod and the
        <inttypes.h> spellings below -- use the C declarations in library.c.
*/

long strtol(const char address_to input,
            char address_to address_to stopped, int base)
{
        b32 out_of_range;
        bipolar value = string_to_number_checked(
                (string_address)input, (string_address address_to)stopped,
                base, address_of out_of_range);

        if (out_of_range)
                errno = ERANGE;
        return (long)value;
}

long long strtoll(const char address_to input,
                  char address_to address_to stopped, int base)
{
        return (long long)strtol(input, stopped, base);
}

unsigned long strtoul(const char address_to input,
                      char address_to address_to stopped, int base)
{
        b32 out_of_range;
        positive value = string_to_number_unsigned_checked(
                (string_address)input, (string_address address_to)stopped,
                base, address_of out_of_range);

        if (out_of_range)
                errno = ERANGE;
        return (unsigned long)value;
}

unsigned long long strtoull(const char address_to input,
                            char address_to address_to stopped, int base)
{
        return (unsigned long long)strtoul(input, stopped, base);
}

/*
        The three <inttypes.h> spellings, which are the only integer names in
        this family that had no symbol at all.

        intmax_t and uintmax_t are b64 and p64 in library.c, and they only
        exist when a program has asked for the compatibility spellings, so
        the signatures below are written in the house types those alias --
        bipolar and positive -- and each of these is the wide routine under a
        different name and nothing else. They are
        wrappers rather than macros so that a program can take the address of
        one, which is the same reason math.c gives for its own wrappers, and
        static like everything else here so that an unused one leaves no code
        behind and none of them collides with a program that also links a
        real libc.
*/
static bipolar imaxabs(bipolar value)
{
        return labs(value);
}

static bipolar strtoimax(string_address input, string_address address_to stopped,
                          b32 base)
{
        return strtol((const char address_to)input, (char address_to address_to)stopped, base);
}

static positive strtoumax(string_address input, string_address address_to stopped,
                           b32 base)
{
        return strtoul((const char address_to)input, (char address_to address_to)stopped, base);
}

/*
        AND THE HALF THAT WAS ABSENT: DECIMAL TEXT INTO A BINARY FLOAT

        Everything below is strtod, strtof and strtold, and the whole of the
        work is the word "correctly". A conversion that is merely close is
        four lines -- accumulate the digits into a double and multiply by a
        power of ten -- and it is wrong by up to several units in the last
        place, because each of those operations rounds and the errors
        compound. A conversion that is correct returns, for every input, the
        representable number nearest the exact decimal value the text names,
        with ties going to the even significand, and it has to do that for
        the subnormals, for the boundaries, and for an input with nine
        hundred digits in it.

        THE THREE TIERS, FASTEST FIRST

        1. The exact tier. If the significant digits fit in a p64 with room
           to spare -- nineteen of them or fewer -- and the integer they form
           is small enough to be a double exactly, and the power of ten is
           between minus twenty two and twenty two, where every power of ten
           is itself exactly a double, then the answer is one multiply or one
           divide of two exact operands. IEEE says a single operation is
           correctly rounded, so the answer is correctly rounded, and nothing
           else needs to happen. This is Clinger's fast path from 1990 and it
           takes most of the traffic a real program generates: 1.5, 0.1,
           3.14159, 6.02e23.

        2. The estimating tier, which is Eisel and Lemire's. The significant
           digits go into a p64 and the power of ten comes out of a table of
           128 bit truncated powers; one 64x64 to 128 multiply, and sometimes
           a second, produces the significand together with a proof that the
           bits below it cannot reach the rounding boundary. When the proof
           holds the answer is correctly rounded and the whole conversion was
           two multiplies. When it does not hold -- which is where the value
           sits so near a boundary that 128 bits cannot separate it -- the
           tier declines to answer rather than guessing, and tier three runs.
           That refusal is what makes this safe: an estimator that guessed
           would be wrong rarely, which is the worst frequency to be wrong at.

        3. The exact tier of last resort, which is Clinger's and Gay's idea
           in the shape Go's strconv gives it. The digits go into a register
           of decimal digits, and multiplying or dividing by two is done on
           those decimal digits directly, in long arithmetic, exactly. Scale
           by powers of two until the value sits in [1/2, 1), read off one
           more bit than the significand holds, and round on the decimal
           digits -- where a tie is a real tie, because a decimal register
           that has not overflowed holds the exact value and one that has
           overflowed knows it and says the value is greater than what it
           holds. This is slow -- a few microseconds for a hard input -- and
           it is never wrong.

        The three tiers agree by construction, and the test lane checks that
        rather than trusting it: it runs the same inputs with tiers one and
        two disabled and diffs, so a defect in either fast tier shows up
        without glibc having to be in the loop.

        WHY THE SLOW TIER IS PARAMETERISED AND THE FAST ONES ARE NOT

        binary32, binary64, the eighty bit x87 format and binary128 differ in
        four numbers: how many bits the stored significand has, where the
        exponent field starts, how wide it is, and the bias. Written that way
        the slow tier is one body that serves all four, which is what makes
        strtold cost almost nothing here -- and strtold is the one where
        long double is eighty bit on x86_64 and binary128 on arm64 and
        riscv64, so a body written for one of them would have been wrong on
        the other two. The fast tiers are not parameterised: they are for
        strtod and strtof, the two formats a program actually converts in a
        loop, and strtold goes straight to tier three every time.

        WHAT THE STANDARD ASKS FOR AND IMPLEMENTATIONS FORGET

        Leading whitespace, an optional sign, "inf" and "infinity" and "nan"
        in any mixture of cases, an optional parenthesised sequence after nan,
        hexadecimal floats with an optional binary exponent, an end pointer
        that on failure points at the ORIGINAL string rather than at wherever
        the scan gave up, and ERANGE in errno on overflow and on underflow.
        All of it is here and all of it is in the test lane, because that is
        where implementations diverge from each other far more often than
        they diverge on the arithmetic.

        WHAT THE FLOOR GIVES AND WHAT IT CANNOT

        byte_is_space walks the leading whitespace, byte_is_digit,
        byte_is_hexadecimal and byte_is_alnum classify, byte_to_lower folds
        the exponent letter and the hexadecimal ones, string_length_max gives
        the bounded page-safe length that lets a fixed-width compare run at
        all, string_compare_folded_max is strncasecmp and recognises
        "infinity", "inf" and "nan" without a byte loop of its own, strtoull
        reads the payload inside a NaN's brackets, bits_leading_zeros
        normalises the significand for the estimating tier, and memory_copy
        takes the register aside for the tininess trial.

        What the floor cannot give is the digit walk itself, and the reason is
        worth writing down rather than leaving as an absence. The obvious
        candidate is memory_span_byte, which counts a run of one byte value --
        the leading zeros, say. It does not fit: it wants a size, and a
        NUL-terminated decimal has no size until something has walked it, so
        finding one would cost a string_length over text the parse is about to
        walk anyway. And the run of zeros is not the only thing the pass is
        doing; it is also moving the decimal point, deciding whether the point
        has been seen, and filling the register, all of which have to happen
        in step with the same byte. string_to_positive is the other candidate
        and it answers a different question: it reads backwards from the
        terminator, with no end pointer, no fraction and no exponent. So the
        digit loops are loops, and the accumulation inside them is a serial
        multiply-and-add with a carry that no routine in library.c can hold
        the running value for.
*/
#if decimal_bits == 64

#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")

/*
        A float seen as its bits.

        math.c has unions of the same shape and this does not reach for them:
        they are static to that file, this file may be included without it,
        and six lines duplicated is cheaper than an ordering dependency
        between two families that land in one commit.
*/
typedef union
{
        f32 value;
        p32 bits;
} numbers_narrow_shape;

typedef union
{
        decimal value;
        p64 bits;
} numbers_shape;

typedef union
{
        f128 value;
        p128 bits;
} numbers_extended_shape;

/*
        The four formats, as the four numbers that tell them apart.

        significand_place is where the leading bit of the significand sits,
        which for a format with an implied bit is one past the stored
        fraction and for x87 -- the one format that stores its leading bit --
        is the top of the stored significand. exponent_place is where the
        exponent field begins, and it is the same number except on x87, where
        the explicit bit occupies the position the exponent would otherwise
        start at. Everything else in the slow tier is written in terms of
        those two, so x87 costs one extra field rather than a second body.

        bias is negative, in the sense the exponent field is a biased number:
        the field holds (real exponent - bias), so a bias of -1023 makes the
        field 1023 for an exponent of zero, and a value of exactly `bias`
        means the field is zero, which is what a zero and a subnormal want.

        point_high and point_low are the coarse early outs: a value whose
        decimal point sits above the first or below the second cannot be
        anything but an infinity or a zero, and saying so before the register
        arithmetic starts is what keeps 1e1000000 from being a long loop.
*/
typedef struct
{
        b32 significand_place;
        b32 exponent_place;
        b32 exponent_bits;
        b32 bias;
        b32 point_high;
        b32 point_low;

        //      The window of powers of ten inside which the estimating tier
        //      has to apply round-half-to-even by hand, because the table
        //      entry is exact there and a tie is a real one. Outside it a tie
        //      cannot arise, and the two long double shapes never reach that
        //      tier at all, so they carry a window of nothing.
        b32 round_even_low;
        b32 round_even_high;
} numbers_format;

static const numbers_format numbers_binary32 = {23, 23, 8, -127, 40, -51, -17, 10};
static const numbers_format numbers_binary64 = {52, 52, 11, -1023, 310, -330, -4, 23};

#if __LDBL_MANT_DIG__ == 64
static const numbers_format numbers_extended = {63, 64, 15, -16383, 4934, -4957, 0, 0};
#else
static const numbers_format numbers_extended = {112, 112, 15, -16383, 4934, -4972, 0, 0};
#endif

/*
        The decimal register the slow tier works in.

        Eight hundred and thirty two digits, held as values rather than as
        characters so that nothing subtracts a '0' in the inner loops. The
        number it holds is

              0 . d[0] d[1] ... d[count-1]   times ten to the point

        and `truncated` says that at least one nonzero digit was dropped off
        the end, so the true value is strictly greater than what is held.

        Eight hundred is the number Go arrived at and the reason is worth
        writing down, because the buffer size is a correctness argument and
        not a tuning knob. Dropping digits can only ever change the answer
        when the retained prefix lands exactly on the midpoint between two
        representable numbers, since anywhere else the prefix already decides
        the rounding and a strictly greater value decides it the same way.
        The longest exact midpoint in binary64 is the one nearest the bottom
        of the subnormals and it has 767 significant digits, so a register
        that holds more than that is exact for every input in both binary64
        and binary32 -- and `truncated` makes even a dropped tail correct,
        because a tie plus something is not a tie and rounds up.

        For long double the same argument gives a bigger number: a midpoint
        near the bottom of the binary128 subnormals needs about 11,500
        digits. This does not carry 11,500 digits. So strtold is correctly
        rounded for every input whose significant digits fit here, which is
        every input a program writes, and can round a longer input up where
        round-half-even would have gone down -- one unit in the last place,
        only for a decimal that is exactly a midpoint, only when it is spelled
        with more than 832 significant digits. That is measured in the lane
        and reported rather than claimed away.
*/
#define NUMBERS_DIGIT_MAX 832

//      Sixty is the largest shift the long arithmetic below can take in one
//      pass: the running carry reaches ten times two to the shift, which must
//      stay inside a p64, and ten times two to the sixtieth does.
#define NUMBERS_SHIFT_MAX 60

#define NUMBERS_NONE 0
#define NUMBERS_NUMBER 1
#define NUMBERS_INFINITE 2
#define NUMBERS_NOT_A_NUMBER 3

#define NUMBERS_FINE 0
#define NUMBERS_OVERFLOW 1
#define NUMBERS_UNDERFLOW 2

typedef struct
{
        p8 digits[NUMBERS_DIGIT_MAX];
        b32 count;
        b32 point;
        bool truncated;
        bool negative;

        //      What the text turned out to be, and where it stopped.
        b32 kind;
        string_address stopped;

        //      The first nineteen digits as one integer, which is what the
        //      two fast tiers work from, how many of them there were, and how
        //      many significant digits the text had in total -- the fast
        //      tiers may only run when those last two agree, because a packed
        //      value that is a prefix of the digits is not the number.
        p64 packed;
        b32 packed_count;
        b32 significant;

        //      A hexadecimal float is already binary and skips the decimal
        //      register entirely: the significand goes here with a sticky bit
        //      under it and the exponent counts twos.
        p128 hex_significand;
        b32 hex_exponent;
        bool hex_sticky;
        bool hexadecimal;
} numbers_scan;

/*
        Trimming, which every operation below ends with.

        A register with trailing zeros in it is the same number as one
        without, and the difference matters in exactly one place: the tie
        test asks whether the digit it is looking at is the last one, and a
        trailing zero left behind would make an exact tie look like something
        above a tie. So the zeros come off, and a register that is entirely
        zeros has no point either.
*/
static fn numbers_trim(numbers_scan address_to number)
{
        while (number->count > 0 && number->digits[number->count - 1] == 0)
                number->count--;

        if (number->count == 0)
                number->point = 0;
}

/*
        Halving the register a given number of times, in decimal.

        The digits are walked most significant first with a carry that is the
        remainder of everything above, in a base ten long division by two to
        the shift. The first loop is the one that finds the leading digit of
        the answer: it feeds digits in until the carry is large enough to
        produce a nonzero quotient, and the number of digits it had to eat is
        how far the decimal point moves left. The second loop is the division
        proper, one digit in and one digit out, writing behind the read head
        so nothing needs a second buffer. The third drains the carry, which is
        where a division by two lengthens the number -- and where the register
        can run out and set the truncated flag.

        The carry stays inside a p64 because the loops only ever continue
        while it is below two to the shift, and ten times two to the sixtieth
        is still a p64. That is the whole reason NUMBERS_SHIFT_MAX is sixty.
*/
static fn numbers_shift_right(numbers_scan address_to number, b32 places)
{
        b32 read = 0;
        b32 write = 0;
        positive carry = 0;
        positive mask = ((positive)1 << places) - 1;

        while ((carry >> places) == 0)
        {
                if (read >= number->count)
                {
                        if (carry == 0)
                        {
                                number->count = 0;
                                number->point = 0;
                                return;
                        }

                        while ((carry >> places) == 0)
                        {
                                carry = carry * 10;
                                read++;
                        }
                        break;
                }

                carry = carry * 10 + number->digits[read];
                read++;
        }

        number->point -= read - 1;

        while (read < number->count)
        {
                positive digit = carry >> places;

                carry &= mask;
                number->digits[write] = (p8)digit;
                write++;
                carry = carry * 10 + number->digits[read];
                read++;
        }

        while (carry > 0)
        {
                positive digit = carry >> places;

                carry &= mask;

                if (write < NUMBERS_DIGIT_MAX)
                {
                        number->digits[write] = (p8)digit;
                        write++;
                }
                else if (digit > 0)
                        number->truncated = true;

                carry = carry * 10;
        }

        number->count = write;
        numbers_trim(number);
}

/*
        Doubling the register a given number of times, in decimal.

        Multiplying by a power of two lengthens the number, and the loop that
        does the multiplication has to know by how much before it starts,
        because it writes from the right and needs to know where the right
        end will be. The count of extra digits is the number of digits in two
        to the shift -- except when the leading digits of the register are
        small enough that the product does not reach the next power of ten,
        and "small enough" is decided by comparing them against the decimal
        spelling of five to the shift. That is the classical cheat and it is
        exact: the register holds 0.d and the product 0.d times two to the k
        gains a digit precisely when 0.d is at least five to the k over ten
        to the delta.

        The table below is that pair, delta and the digits of five to the k,
        for every shift up to sixty. It is generated arithmetic rather than
        measured constants -- delta is the digit count of two to the k, and
        the string is five to the k written out -- and the test lane checks
        every entry of it against the register's own long multiplication.
*/
typedef struct
{
        b32 delta;
        const char address_to cutoff;
} numbers_cheat;

static const numbers_cheat numbers_cheats[NUMBERS_SHIFT_MAX + 1] = {
        { 0, ""},
        { 1, "5"},
        { 1, "25"},
        { 1, "125"},
        { 2, "625"},
        { 2, "3125"},
        { 2, "15625"},
        { 3, "78125"},
        { 3, "390625"},
        { 3, "1953125"},
        { 4, "9765625"},
        { 4, "48828125"},
        { 4, "244140625"},
        { 4, "1220703125"},
        { 5, "6103515625"},
        { 5, "30517578125"},
        { 5, "152587890625"},
        { 6, "762939453125"},
        { 6, "3814697265625"},
        { 6, "19073486328125"},
        { 7, "95367431640625"},
        { 7, "476837158203125"},
        { 7, "2384185791015625"},
        { 7, "11920928955078125"},
        { 8, "59604644775390625"},
        { 8, "298023223876953125"},
        { 8, "1490116119384765625"},
        { 9, "7450580596923828125"},
        { 9, "37252902984619140625"},
        { 9, "186264514923095703125"},
        {10, "931322574615478515625"},
        {10, "4656612873077392578125"},
        {10, "23283064365386962890625"},
        {10, "116415321826934814453125"},
        {11, "582076609134674072265625"},
        {11, "2910383045673370361328125"},
        {11, "14551915228366851806640625"},
        {12, "72759576141834259033203125"},
        {12, "363797880709171295166015625"},
        {12, "1818989403545856475830078125"},
        {13, "9094947017729282379150390625"},
        {13, "45474735088646411895751953125"},
        {13, "227373675443232059478759765625"},
        {13, "1136868377216160297393798828125"},
        {14, "5684341886080801486968994140625"},
        {14, "28421709430404007434844970703125"},
        {14, "142108547152020037174224853515625"},
        {15, "710542735760100185871124267578125"},
        {15, "3552713678800500929355621337890625"},
        {15, "17763568394002504646778106689453125"},
        {16, "88817841970012523233890533447265625"},
        {16, "444089209850062616169452667236328125"},
        {16, "2220446049250313080847263336181640625"},
        {16, "11102230246251565404236316680908203125"},
        {17, "55511151231257827021181583404541015625"},
        {17, "277555756156289135105907917022705078125"},
        {17, "1387778780781445675529539585113525390625"},
        {18, "6938893903907228377647697925567626953125"},
        {18, "34694469519536141888238489627838134765625"},
        {18, "173472347597680709441192448139190673828125"},
        {19, "867361737988403547205962240695953369140625"},
};

//      Whether the register's digits, read as the fraction they are, come to
//      less than the cutoff. A register that runs out first is less, because
//      the cutoff has a nonzero digit where the register has nothing.
static bool numbers_below_cutoff(numbers_scan address_to number, const char address_to cutoff)
{
        b32 index;

        for (index = 0; cutoff[index] != 0; index++)
        {
                if (index >= number->count)
                        return true;

                if (number->digits[index] != (p8)(cutoff[index] - '0'))
                        return number->digits[index] < (p8)(cutoff[index] - '0');
        }

        return false;
}

static fn numbers_shift_left(numbers_scan address_to number, b32 places)
{
        b32 delta = numbers_cheats[places].delta;
        b32 read;
        b32 write;
        positive carry = 0;

        if (numbers_below_cutoff(number, numbers_cheats[places].cutoff))
                delta--;

        write = number->count + delta;

        for (read = number->count; read > 0;)
        {
                positive quotient;
                positive remainder;

                read--;
                carry += (positive)number->digits[read] << places;
                quotient = carry / 10;
                remainder = carry - quotient * 10;
                write--;

                if (write >= 0 && write < NUMBERS_DIGIT_MAX)
                        number->digits[write] = (p8)remainder;
                else if (remainder != 0)
                        number->truncated = true;

                carry = quotient;
        }

        while (carry > 0)
        {
                positive quotient = carry / 10;
                positive remainder = carry - quotient * 10;

                write--;

                if (write >= 0 && write < NUMBERS_DIGIT_MAX)
                        number->digits[write] = (p8)remainder;
                else if (remainder != 0)
                        number->truncated = true;

                carry = quotient;
        }

        number->count += delta;

        if (number->count > NUMBERS_DIGIT_MAX)
                number->count = NUMBERS_DIGIT_MAX;

        number->point += delta;
        numbers_trim(number);
}

//      Either direction, in passes of at most sixty, because the shifts this
//      wants are as large as a hundred and fourteen -- one more than the
//      binary128 significand -- and no single pass may exceed the width the
//      carry has room for.
static fn numbers_shift(numbers_scan address_to number, b32 places)
{
        if (number->count == 0)
                return;

        if (places > 0)
        {
                while (places > NUMBERS_SHIFT_MAX)
                {
                        numbers_shift_left(number, NUMBERS_SHIFT_MAX);

                        if (number->count == 0)
                                return;

                        places -= NUMBERS_SHIFT_MAX;
                }

                if (places > 0)
                        numbers_shift_left(number, places);

                return;
        }

        while (places < -NUMBERS_SHIFT_MAX)
        {
                numbers_shift_right(number, NUMBERS_SHIFT_MAX);

                if (number->count == 0)
                        return;

                places += NUMBERS_SHIFT_MAX;
        }

        if (places < 0)
                numbers_shift_right(number, -places);
}

/*
        Whether the digit at a given place rounds the number above it up.

        Three cases and only three. Past the end of the register there is
        nothing to round with, so no. A five that is the very last digit is an
        exact tie, and a tie goes to the even neighbour -- unless the register
        was truncated, in which case the true value is strictly above the
        midpoint and rounds up, which is the whole reason the truncated flag
        is carried. Anything else is decided by the digit alone.
*/
static bool numbers_should_round_up(numbers_scan address_to number, b32 place)
{
        if (place < 0 || place >= number->count)
                return false;

        if (number->digits[place] == 5 && place + 1 == number->count)
        {
                if (number->truncated)
                        return true;

                return place > 0 && (number->digits[place - 1] & 1) != 0;
        }

        return number->digits[place] >= 5;
}

/*
        The register's integer part, rounded.

        The caller has already shifted so that the integer part is the
        significand it wants, which is at most a hundred and thirteen bits and
        so at most thirty five decimal digits. A p128 holds thirty eight, and
        the guard above the loop is there so that a register that somehow
        arrived with a larger point does not wrap silently.
*/
static p128 numbers_rounded_integer(numbers_scan address_to number)
{
        b32 index;
        p128 value = 0;

        if (number->point > 38)
                return ~(p128)0;

        for (index = 0; index < number->point && index < number->count; index++)
                value = value * 10 + number->digits[index];

        for (; index < number->point; index++)
                value = value * 10;

        if (numbers_should_round_up(number, number->point))
                value++;

        return value;
}

/*
        The steps the scaling loop takes, which are how far a decimal point of
        a given size lets the register be shifted in one pass without the
        answer leaving the range the loop is walking it into. Two to the
        twenty seventh is under ten to the ninth, so twenty seven is the step
        once the point is past the end of the table.
*/
static const b32 numbers_step[9] = {1, 3, 6, 9, 13, 16, 19, 23, 26};

static b32 numbers_step_for(b32 point)
{
        if (point >= 9)
                return 27;

        return numbers_step[point];
}

/*
        The slow tier: a decimal register into the bits of a float.

        Scale by powers of two -- exactly, in decimal -- until the value sits
        in [1/2, 1). That is the loop pair: while the point is above zero the
        number is one or more and wants halving, and while the point is at or
        below zero with a leading digit under five the number is under a half
        and wants doubling. Each pass moves by as much as the point allows,
        which is what keeps a value like 1e300 from being three hundred
        separate multiplications.

        Then the exponent is one less than the count of halvings, because a
        significand lives in [1, 2) and the register was walked into [1/2, 1).
        If that exponent is below the smallest the format has, the register is
        halved the rest of the way by hand: that is gradual underflow, and it
        is why the subnormals come out right rather than as a special case.

        One more bit than the stored fraction is then shifted into the integer
        part and read off with rounding, and the only thing that can go wrong
        after that is the rounding carrying into a new bit, which is one
        shift and one more overflow test.

        The last test is the one that separates a normal from a subnormal:
        the leading bit is there or it is not, and if it is not the exponent
        field is zero. For x87, where the leading bit is stored rather than
        implied, that same test reads the stored bit and the same assignment
        is correct, which is the entire reason the format is four numbers.

        WHEN A SMALL ANSWER IS AN UNDERFLOW AND WHEN IT IS ONLY SMALL

        A subnormal result is not by itself an error. IEEE raises underflow
        only when the answer is both tiny AND inexact, and glibc's strtod
        follows that exactly: "0x435p-1073" is a subnormal double and is the
        value written down with nothing lost, so errno stays zero, while
        "5e-324" is a subnormal that had to round and sets ERANGE. That was
        measured rather than assumed -- a first version of this set ERANGE on
        every subnormal and disagreed with glibc on 1,645 of two million
        sweep inputs, every one of them an exactly representable small value,
        and nowhere else.

        Inexactness in the register is one test: once the significand has
        been shifted into the integer part, the conversion lost something if
        and only if a digit is left below the point, or the register was
        truncated on the way in.

        Tininess is decided AFTER rounding, which is one of the two IEEE
        allows and the one x86_64, arm64 and riscv64 all take. The definition
        is precise and it is not the same as "the answer came out subnormal":
        a result is tiny when the value, rounded to the format's FULL
        precision with the exponent range pretended to be unbounded, is still
        below the smallest normal. So a value a hair under the smallest
        normal that would round up to it at full precision was never tiny,
        even though it is below the floor, and no ERANGE is due.

        Four adjacent inputs measured against glibc show every branch of that:

              2.2250738585072011e-308   largest subnormal   ERANGE
              2.2250738585072012e-308   smallest normal     ERANGE
              2.2250738585072013e-308   smallest normal     no ERANGE
              2.2250738585072014e-308   smallest normal     no ERANGE

        The middle two produce the same double and disagree about errno, and
        nothing but a full-precision trial rounding tells them apart: at
        binary64's own spacing the first of them lands one step below the
        smallest normal and the second lands on it.

        That trial can only ever change the answer when the exponent is
        exactly one below the format's floor. Two or more below, the value is
        under half the smallest normal and no rounding at any precision can
        reach it. So the copy of the register the trial needs is taken on one
        narrow band of inputs and nowhere else.

        AND THE ONE PLACE THIS DELIBERATELY DOES NOT MATCH GLIBC

        IEEE 754 permits both answers, and the three machines this builds for
        do not agree about which they give. glibc's strtod follows the local
        hardware: x86_64 and riscv64 detect tininess after rounding, and
        arm64 detects it before. So the four inputs above produce two
        different errno columns on arm64 from the ones they produce on the
        other two, out of the same C library.

        This picks after-rounding on all three, because one answer everywhere
        is what the rest of this project means by parity and because the
        difference is an errno bit rather than a value. It was measured
        rather than waved at: against arm64's own glibc, over two million
        sweep inputs, the disagreement appeared once, and that once was a
        boundary value put in the list on purpose. Defining
        NUMBERS_TININESS_AFTER_ROUNDING to zero takes the other answer, which
        is what a program that wants arm64's glibc bit for bit should do.
*/
#ifndef NUMBERS_TININESS_AFTER_ROUNDING
#define NUMBERS_TININESS_AFTER_ROUNDING 1
#endif
static p128 numbers_assemble(numbers_scan address_to number,
                             const numbers_format address_to shape,
                             b32 address_to condition)
{
        b32 exponent = 0;
        p128 significand = 0;
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 outcome = NUMBERS_FINE;
        bool exact = true;
        bool tiny = false;
        p128 bits;

        if (number->count == 0)
        {
                exponent = shape->bias;
                goto ready;
        }

        if (number->point > shape->point_high)
                goto over;

        if (number->point < shape->point_low)
        {
                exponent = shape->bias;
                outcome = NUMBERS_UNDERFLOW;
                goto ready;
        }

        while (number->point > 0)
        {
                b32 step = numbers_step_for(number->point);

                numbers_shift(number, -step);
                exponent += step;

                if (number->count == 0)
                {
                        exponent = shape->bias;
                        outcome = NUMBERS_UNDERFLOW;
                        goto ready;
                }
        }

        while (number->point < 0 || (number->point == 0 && number->digits[0] < 5))
        {
                b32 step = numbers_step_for(-number->point);

                numbers_shift(number, step);
                exponent -= step;
        }

        exponent--;

        if (exponent < shape->bias + 1)
        {
                b32 back = shape->bias + 1 - exponent;

                tiny = true;

                if (NUMBERS_TININESS_AFTER_ROUNDING && back == 1)
                {
                        numbers_scan trial;

                        memory_copy(address_of trial, number, sizeof trial);
                        numbers_shift(address_of trial, 1 + shape->significand_place);

                        if (numbers_rounded_integer(address_of trial) ==
                            ((p128)2 << shape->significand_place))
                                tiny = false;
                }

                numbers_shift(number, -back);
                exponent += back;
        }

        if (exponent - shape->bias >= limit)
                goto over;

        numbers_shift(number, 1 + shape->significand_place);
        exact = !number->truncated && number->count <= number->point;
        significand = numbers_rounded_integer(number);

        if (significand == ((p128)2 << shape->significand_place))
        {
                significand >>= 1;
                exponent++;

                if (exponent - shape->bias >= limit)
                        goto over;
        }

        if ((significand & ((p128)1 << shape->significand_place)) == 0)
                exponent = shape->bias;

        if (tiny && !exact)
                outcome = NUMBERS_UNDERFLOW;

        goto ready;

over:
        significand = 0;
        exponent = limit + shape->bias;
        outcome = NUMBERS_OVERFLOW;

        //      x87 stores the leading bit of its significand, and an infinity
        //      with that bit clear is a shape the hardware calls invalid.
        if (shape->exponent_place > shape->significand_place)
                significand = (p128)1 << shape->significand_place;

ready:
        bits = significand & ((((p128)1 << shape->exponent_place) - 1));
        bits |= (p128)(p64)((exponent - shape->bias) & limit) << shape->exponent_place;

        if (number->negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        if (condition)
                address_to condition = outcome;

        return bits;
}

/*
        The other rounding, which is binary all the way down.

        A hexadecimal float never becomes a decimal register: the text is
        already a significand and a power of two, so the only work is to move
        the leading bit where the format wants it and round off whatever falls
        below. Sticky is the flag that says something nonzero fell off the
        bottom of the significand while it was being read, which is what turns
        a would-be tie into a value above the midpoint.

        The subnormal case is the same shift with a smaller target: instead of
        putting the leading bit at the top of the significand, put it as far
        down as the smallest exponent forces, and let the rounding happen
        there. A carry out of that lands exactly on the smallest normal, which
        is the right answer and needs no case of its own -- so the carry
        correction below only fires when the significand really has grown past
        the top of the format.
*/
static b32 numbers_top_bit(p128 value)
{
        p64 high = (p64)(value >> 64);

        if (high != 0)
                return 127 - (b32)bits_leading_zeros(high);

        return 63 - (b32)bits_leading_zeros((p64)value);
}

static p128 numbers_round_binary(numbers_scan address_to number,
                                 const numbers_format address_to shape,
                                 b32 address_to condition)
{
        p128 significand = number->hex_significand;
        bool sticky = number->hex_sticky;
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 outcome = NUMBERS_FINE;
        bool exact = true;
        bool tiny = false;
        b32 top;
        b32 leading;
        b32 wanted;
        b32 move;
        b32 scale;
        b32 field;
        p128 bits;

        if (significand == 0)
        {
                scale = 0;
                field = 0;
                goto ready;
        }

        exact = !sticky;

        top = numbers_top_bit(significand);
        leading = number->hex_exponent + top;
        wanted = shape->significand_place;

        if (leading < shape->bias + 1)
        {
                tiny = true;
                wanted = shape->significand_place - (shape->bias + 1 - leading);
        }

        move = wanted - top;
        scale = leading - wanted;

        if (move >= 0)
        {
                significand <<= move;
        }
        else if (-move >= 128)
        {
                //      Every bit of the significand is below the last place
                //      the format has, so the answer is a zero that knows it
                //      was not one.
                significand = 0;
                outcome = NUMBERS_UNDERFLOW;
                scale = 0;
                field = 0;
                goto ready;

        }
        else
        {
                b32 back = -move;
                p128 half = (p128)1 << (back - 1);
                p128 dropped = significand & (((p128)1 << back) - 1);

                significand >>= back;

                if (dropped != 0)
                        exact = false;

                if (dropped > half)
                        significand++;
                else if (dropped == half && (sticky || (significand & 1) != 0))
                        significand++;
        }

        if (significand >= ((p128)2 << shape->significand_place))
        {
                significand >>= 1;
                scale++;
        }

        if ((significand & ((p128)1 << shape->significand_place)) == 0)
        {
                field = 0;
        }
        else
        {
                field = scale + shape->significand_place - shape->bias;

                if (field >= limit)
                {
                        significand = 0;
                        field = limit;

                        if (shape->exponent_place > shape->significand_place)
                                significand = (p128)1 << shape->significand_place;

                        outcome = NUMBERS_OVERFLOW;
                        tiny = false;
                }
        }

        //      Tiny and inexact is an underflow; tiny and exact is only a
        //      subnormal, and a subnormal is a number like any other.
        if (tiny && !exact)
                outcome = NUMBERS_UNDERFLOW;

ready:
        bits = significand & ((((p128)1 << shape->exponent_place) - 1));
        bits |= (p128)(p64)field << shape->exponent_place;

        if (number->negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        if (condition)
                address_to condition = outcome;

        return bits;
}

/*
        Reading the text, which is the half of strtod the standard spends its
        words on.

        Leading whitespace, an optional sign, and then one of four things: a
        name, a hexadecimal float, a decimal float, or nothing at all. The
        three that convert leave the end pointer past what they took; the
        fourth leaves it at the ORIGINAL string, sign and whitespace
        included, which is the corner that separates a library that read the
        standard from one that did not.

        Nothing here reads past the terminator. Every load is either the
        first byte of what is left or a byte whose predecessor has already
        been found not to be the terminator, and the two fixed comparisons
        against "infinity" and "nan" are preceded by a bounded length so that
        a three byte string at the end of a page is never compared eight
        bytes wide. string_length_max with a literal bound folds to
        straight-line code through the umbrella's specializer, so the safety
        costs no call.

        The clamps on the three counters are not decoration. A string may
        carry a billion leading zeros after the point, or an exponent of ten
        to the ninth, and the register's decimal point is a b32. Every clamp
        below saturates far outside the range any format can represent, so
        the coarse early outs in the assembly step answer correctly for
        anything that reaches them.
*/
static p8 numbers_infinity_text[] = "infinity";
static p8 numbers_short_infinity_text[] = "inf";
static p8 numbers_not_a_number_text[] = "nan";

#define NUMBERS_POINT_CLAMP 1000000000
#define NUMBERS_EXPONENT_CLAMP 100000000

static b32 numbers_hex_value(b32 byte)
{
        if (byte_is_digit(byte))
                return byte - '0';

        return byte_to_lower(byte) - 'a' + 10;
}

static inline INLINE fn numbers_read_exponent(string_address address_to scan,
                                               b32 address_to exponent)
{
        string_address digits = address_to scan + 1;
        b32 sign = 1;
        b32 value = 0;

        if (address_to digits == '+')
                digits++;
        else if (address_to digits == '-')
        {
                sign = -1;
                digits++;
        }

        if (!byte_is_digit(address_to digits))
                return;

        while (byte_is_digit(address_to digits))
        {
                if (value < NUMBERS_EXPONENT_CLAMP)
                        value = value * 10 + (address_to digits - '0');

                digits++;
        }

        address_to exponent += sign * value;
        address_to scan = digits;
}

static fn numbers_read_hexadecimal(numbers_scan address_to number, string_address start)
{
        string_address scan = start + 2;
        p128 significand = 0;
        b32 exponent = 0;
        bool sticky = false;
        bool seen = false;
        bool dotted = false;

        while (true)
        {
                b32 byte = address_to scan;
                b32 value;

                if (byte == '.')
                {
                        if (dotted)
                                break;

                        dotted = true;
                        scan++;
                        continue;
                }

                if (!byte_is_hexadecimal(byte))
                        break;

                value = numbers_hex_value(byte);
                seen = true;

                if (significand < ((p128)1 << 124))
                {
                        significand = (significand << 4) | (p128)(p64)value;

                        if (dotted && exponent > -NUMBERS_POINT_CLAMP)
                                exponent -= 4;
                }
                else
                {
                        if (value != 0)
                                sticky = true;

                        if (!dotted && exponent < NUMBERS_POINT_CLAMP)
                                exponent += 4;
                }

                scan++;
        }

        //      "0x" with nothing usable behind it is not a failed conversion.
        //      The subject sequence is the "0", the answer is zero, and the
        //      end pointer lands on the "x" -- which is what strtol does with
        //      the same text and for the same reason.
        if (!seen)
        {
                number->kind = NUMBERS_NUMBER;
                number->count = 0;
                number->stopped = start + 1;
                return;
        }

        if (byte_to_lower(address_to scan) == 'p')
                numbers_read_exponent(address_of scan, address_of exponent);

        number->kind = NUMBERS_NUMBER;
        number->hexadecimal = true;
        number->hex_significand = significand;
        number->hex_exponent = exponent;
        number->hex_sticky = sticky;
        number->stopped = scan;
}

static bool numbers_read(string_address input, numbers_scan address_to number)
{
        string_address scan = input;
        positive available;
        bool seen_digit = false;
        bool dotted = false;
        b32 whole_digits = 0;
        b32 hidden_zeros = 0;
        bool leading = true;

        number->count = 0;
        number->point = 0;
        number->truncated = false;
        number->negative = false;
        number->kind = NUMBERS_NONE;
        number->stopped = input;
        number->packed = 0;
        number->packed_count = 0;
        number->significant = 0;
        number->hex_significand = 0;
        number->hex_exponent = 0;
        number->hex_sticky = false;
        number->hexadecimal = false;

        scan += string_span_of_set(scan, " \t\n\r\v\f");

        if (address_to scan == '+')
                scan++;
        else if (address_to scan == '-')
        {
                number->negative = true;
                scan++;
        }

        // Only an i/n prefix can name infinity or NaN. Decimal input
        // needs neither a preliminary length scan nor word comparisons.
        available = ((scan[0] | 32) == 'i' || (scan[0] | 32) == 'n')
                        ? string_length_max(scan, 8) : 0;

        if (available >= 8 &&
            string_compare_folded_max(scan, numbers_infinity_text, 8) == 0)
        {
                number->kind = NUMBERS_INFINITE;
                number->stopped = scan + 8;
                return true;
        }

        if (available >= 3 &&
            string_compare_folded_max(scan, numbers_short_infinity_text, 3) == 0)
        {
                number->kind = NUMBERS_INFINITE;
                number->stopped = scan + 3;
                return true;
        }

        if (available >= 3 &&
            string_compare_folded_max(scan, numbers_not_a_number_text, 3) == 0)
        {
                string_address after = scan + 3;

                //      The optional n-char-sequence. C spells it as letters,
                //      digits and underscores between brackets, and it is
                //      part of the subject sequence whether or not anything
                //      can be made of it -- so "nan(zz)" converts and stops
                //      after the bracket, which is where glibc stops too.
                //      What it MEANS is implementation defined, and glibc
                //      reads it as a number and puts it in the significand,
                //      so this hands it to strtoull and uses the answer when
                //      the whole sequence was a number.
                if (address_to after == '(')
                {
                        string_address inside = after + 1;
                        string_address walk = inside +
                                string_span(inside, string_set_name);

                        if (address_to walk == ')')
                        {
                                string_address closing = null;
                                positive payload = strtoull((const char address_to)inside, (char address_to address_to)address_of closing, 0);

                                if (closing == walk)
                                        number->packed = payload;

                                after = walk + 1;
                        }
                }

                number->kind = NUMBERS_NOT_A_NUMBER;
                number->stopped = after;
                return true;
        }

        if (address_to scan == '0' && byte_to_lower(scan[1]) == 'x')
        {
                numbers_read_hexadecimal(number, scan);
                return number->kind != NUMBERS_NONE;
        }

        while (true)
        {
                b32 byte = address_to scan;
                b32 value;

                if (byte == '.')
                {
                        if (dotted)
                                break;

                        dotted = true;
                        scan++;
                        continue;
                }

                if (!byte_is_digit(byte))
                        break;

                seen_digit = true;
                value = byte - '0';

                if (leading && value == 0)
                {
                        if (dotted && hidden_zeros < NUMBERS_POINT_CLAMP)
                                hidden_zeros++;

                        scan++;
                        continue;
                }

                leading = false;

                if (number->count < NUMBERS_DIGIT_MAX)
                {
                        number->digits[number->count] = (p8)value;
                        number->count++;
                }
                else if (value != 0)
                        number->truncated = true;

                if (number->packed_count < 19)
                {
                        number->packed = number->packed * 10 + (p64)(p32)value;
                        number->packed_count++;
                }

                if (number->significant < NUMBERS_POINT_CLAMP)
                        number->significant++;

                if (!dotted && whole_digits < NUMBERS_POINT_CLAMP)
                        whole_digits++;

                scan++;
        }

        if (!seen_digit)
        {
                number->kind = NUMBERS_NONE;
                number->stopped = input;
                return false;
        }

        number->point = whole_digits - hidden_zeros;

        if (byte_to_lower(address_to scan) == 'e')
                numbers_read_exponent(address_of scan, address_of number->point);

        numbers_trim(number);
        number->kind = NUMBERS_NUMBER;
        number->stopped = scan;

        return true;
}

/*
        THE ESTIMATING TIER, WHICH IS EISEL AND LEMIRE'S

        The exact tier above answers when the significand fits fifty three
        bits and the power of ten is one of the forty five that are exactly
        doubles. Outside that -- "1e-30", or twenty significant digits, or
        anything past ten to the twenty second -- there is still a way to be
        both fast and correct, and it is this one.

        The idea is to hold ten to the q as a 128 bit binary approximation
        rather than as a double, multiply the significand by it in 128 bits,
        and then ask whether the bits that were thrown away could possibly
        have reached the rounding boundary. If they could not, the answer is
        proved correctly rounded and the whole conversion was one or two
        multiplies. If they could, the tier says nothing at all and the
        decimal register runs. That refusal is the load-bearing part: an
        estimator that answered anyway would be wrong on a vanishing fraction
        of inputs, which is the worst possible frequency to be wrong at,
        because no test that samples would ever see it.

        The table is six hundred and fifty one 128 bit values, ten to the
        minus three hundred and forty second through ten to the three hundred
        and eighth, each normalised so its top bit is set. They are
        TRUNCATIONS and not roundings, and the difference matters: the proof
        that the reject test is sufficient assumes the stored value is at or
        below the true one. The generator writes each entry twice by two
        different routes and asserts they agree, and asserts the bracketing
        inequality that says the entry is the truncation, before the entry is
        allowed into the file at all.

        TWO PLACES THIS DELIBERATELY DIFFERS FROM THE PUBLISHED ALGORITHM

        The published version handles subnormal results and overflow inside
        itself. This one refuses them: if the exponent it computes is at or
        below zero, or at or above the saturated field, the tier declines and
        the decimal register runs instead. That is not a shortcut, it is the
        errno contract -- underflow has to answer whether the value was tiny
        AFTER rounding at full precision and whether it was inexact, and
        neither question is one this tier has the information to answer. The
        register does, so the register takes those inputs. They are rare and
        they are the ones where being slow costs nothing.

        And where the digits ran past nineteen, the tier is asked twice --
        once with the truncated significand and once with one more -- and only
        answers if both come out the same float. Rounding is monotonic, the
        true value lies between those two, so two equal answers is a proof.
*/
#define NUMBERS_SMALLEST_POWER (-342)
#define NUMBERS_LARGEST_POWER 308

static const p64 numbers_power_of_five[NUMBERS_LARGEST_POWER -
                                       NUMBERS_SMALLEST_POWER + 1][2] = {
        {0x113faa2906a13b40ULL, 0xeef453d6923bd65aULL},
        {0x4ac7ca59a424c508ULL, 0x9558b4661b6565f8ULL},
        {0x5d79bcf00d2df64aULL, 0xbaaee17fa23ebf76ULL},
        {0xf4d82c2c107973ddULL, 0xe95a99df8ace6f53ULL},
        {0x79071b9b8a4be86aULL, 0x91d8a02bb6c10594ULL},
        {0x9748e2826cdee285ULL, 0xb64ec836a47146f9ULL},
        {0xfd1b1b2308169b26ULL, 0xe3e27a444d8d98b7ULL},
        {0xfe30f0f5e50e20f8ULL, 0x8e6d8c6ab0787f72ULL},
        {0xbdbd2d335e51a936ULL, 0xb208ef855c969f4fULL},
        {0xad2c788035e61383ULL, 0xde8b2b66b3bc4723ULL},
        {0x4c3bcb5021afcc32ULL, 0x8b16fb203055ac76ULL},
        {0xdf4abe242a1bbf3eULL, 0xaddcb9e83c6b1793ULL},
        {0xd71d6dad34a2af0eULL, 0xd953e8624b85dd78ULL},
        {0x8672648c40e5ad69ULL, 0x87d4713d6f33aa6bULL},
        {0x680efdaf511f18c3ULL, 0xa9c98d8ccb009506ULL},
        {0x0212bd1b2566def3ULL, 0xd43bf0effdc0ba48ULL},
        {0x014bb630f7604b58ULL, 0x84a57695fe98746dULL},
        {0x419ea3bd35385e2eULL, 0xa5ced43b7e3e9188ULL},
        {0x52064cac828675baULL, 0xcf42894a5dce35eaULL},
        {0x7343efebd1940994ULL, 0x818995ce7aa0e1b2ULL},
        {0x1014ebe6c5f90bf9ULL, 0xa1ebfb4219491a1fULL},
        {0xd41a26e077774ef7ULL, 0xca66fa129f9b60a6ULL},
        {0x8920b098955522b5ULL, 0xfd00b897478238d0ULL},
        {0x55b46e5f5d5535b1ULL, 0x9e20735e8cb16382ULL},
        {0xeb2189f734aa831eULL, 0xc5a890362fddbc62ULL},
        {0xa5e9ec7501d523e5ULL, 0xf712b443bbd52b7bULL},
        {0x47b233c92125366fULL, 0x9a6bb0aa55653b2dULL},
        {0x999ec0bb696e840bULL, 0xc1069cd4eabe89f8ULL},
        {0xc00670ea43ca250eULL, 0xf148440a256e2c76ULL},
        {0x380406926a5e5729ULL, 0x96cd2a865764dbcaULL},
        {0xc605083704f5ecf3ULL, 0xbc807527ed3e12bcULL},
        {0xf7864a44c633682fULL, 0xeba09271e88d976bULL},
        {0x7ab3ee6afbe0211eULL, 0x93445b8731587ea3ULL},
        {0x5960ea05bad82965ULL, 0xb8157268fdae9e4cULL},
        {0x6fb92487298e33beULL, 0xe61acf033d1a45dfULL},
        {0xa5d3b6d479f8e057ULL, 0x8fd0c16206306babULL},
        {0x8f48a4899877186dULL, 0xb3c4f1ba87bc8696ULL},
        {0x331acdabfe94de88ULL, 0xe0b62e2929aba83cULL},
        {0x9ff0c08b7f1d0b15ULL, 0x8c71dcd9ba0b4925ULL},
        {0x07ecf0ae5ee44ddaULL, 0xaf8e5410288e1b6fULL},
        {0xc9e82cd9f69d6151ULL, 0xdb71e91432b1a24aULL},
        {0xbe311c083a225cd3ULL, 0x892731ac9faf056eULL},
        {0x6dbd630a48aaf407ULL, 0xab70fe17c79ac6caULL},
        {0x092cbbccdad5b109ULL, 0xd64d3d9db981787dULL},
        {0x25bbf56008c58ea6ULL, 0x85f0468293f0eb4eULL},
        {0xaf2af2b80af6f24fULL, 0xa76c582338ed2621ULL},
        {0x1af5af660db4aee2ULL, 0xd1476e2c07286faaULL},
        {0x50d98d9fc890ed4eULL, 0x82cca4db847945caULL},
        {0xe50ff107bab528a1ULL, 0xa37fce126597973cULL},
        {0x1e53ed49a96272c9ULL, 0xcc5fc196fefd7d0cULL},
        {0x25e8e89c13bb0f7bULL, 0xff77b1fcbebcdc4fULL},
        {0x77b191618c54e9adULL, 0x9faacf3df73609b1ULL},
        {0xd59df5b9ef6a2418ULL, 0xc795830d75038c1dULL},
        {0x4b0573286b44ad1eULL, 0xf97ae3d0d2446f25ULL},
        {0x4ee367f9430aec33ULL, 0x9becce62836ac577ULL},
        {0x229c41f793cda740ULL, 0xc2e801fb244576d5ULL},
        {0x6b43527578c11110ULL, 0xf3a20279ed56d48aULL},
        {0x830a13896b78aaaaULL, 0x9845418c345644d6ULL},
        {0x23cc986bc656d554ULL, 0xbe5691ef416bd60cULL},
        {0x2cbfbe86b7ec8aa9ULL, 0xedec366b11c6cb8fULL},
        {0x7bf7d71432f3d6aaULL, 0x94b3a202eb1c3f39ULL},
        {0xdaf5ccd93fb0cc54ULL, 0xb9e08a83a5e34f07ULL},
        {0xd1b3400f8f9cff69ULL, 0xe858ad248f5c22c9ULL},
        {0x23100809b9c21fa2ULL, 0x91376c36d99995beULL},
        {0xabd40a0c2832a78bULL, 0xb58547448ffffb2dULL},
        {0x16c90c8f323f516dULL, 0xe2e69915b3fff9f9ULL},
        {0xae3da7d97f6792e4ULL, 0x8dd01fad907ffc3bULL},
        {0x99cd11cfdf41779dULL, 0xb1442798f49ffb4aULL},
        {0x40405643d711d584ULL, 0xdd95317f31c7fa1dULL},
        {0x482835ea666b2573ULL, 0x8a7d3eef7f1cfc52ULL},
        {0xda3243650005eed0ULL, 0xad1c8eab5ee43b66ULL},
        {0x90bed43e40076a83ULL, 0xd863b256369d4a40ULL},
        {0x5a7744a6e804a292ULL, 0x873e4f75e2224e68ULL},
        {0x711515d0a205cb37ULL, 0xa90de3535aaae202ULL},
        {0x0d5a5b44ca873e04ULL, 0xd3515c2831559a83ULL},
        {0xe858790afe9486c3ULL, 0x8412d9991ed58091ULL},
        {0x626e974dbe39a873ULL, 0xa5178fff668ae0b6ULL},
        {0xfb0a3d212dc81290ULL, 0xce5d73ff402d98e3ULL},
        {0x7ce66634bc9d0b9aULL, 0x80fa687f881c7f8eULL},
        {0x1c1fffc1ebc44e81ULL, 0xa139029f6a239f72ULL},
        {0xa327ffb266b56221ULL, 0xc987434744ac874eULL},
        {0x4bf1ff9f0062baa9ULL, 0xfbe9141915d7a922ULL},
        {0x6f773fc3603db4aaULL, 0x9d71ac8fada6c9b5ULL},
        {0xcb550fb4384d21d4ULL, 0xc4ce17b399107c22ULL},
        {0x7e2a53a146606a49ULL, 0xf6019da07f549b2bULL},
        {0x2eda7444cbfc426eULL, 0x99c102844f94e0fbULL},
        {0xfa911155fefb5309ULL, 0xc0314325637a1939ULL},
        {0x793555ab7eba27cbULL, 0xf03d93eebc589f88ULL},
        {0x4bc1558b2f3458dfULL, 0x96267c7535b763b5ULL},
        {0x9eb1aaedfb016f17ULL, 0xbbb01b9283253ca2ULL},
        {0x465e15a979c1caddULL, 0xea9c227723ee8bcbULL},
        {0x0bfacd89ec191ecaULL, 0x92a1958a7675175fULL},
        {0xcef980ec671f667cULL, 0xb749faed14125d36ULL},
        {0x82b7e12780e7401bULL, 0xe51c79a85916f484ULL},
        {0xd1b2ecb8b0908811ULL, 0x8f31cc0937ae58d2ULL},
        {0x861fa7e6dcb4aa16ULL, 0xb2fe3f0b8599ef07ULL},
        {0x67a791e093e1d49bULL, 0xdfbdcece67006ac9ULL},
        {0xe0c8bb2c5c6d24e1ULL, 0x8bd6a141006042bdULL},
        {0x58fae9f773886e19ULL, 0xaecc49914078536dULL},
        {0xaf39a475506a899fULL, 0xda7f5bf590966848ULL},
        {0x6d8406c952429604ULL, 0x888f99797a5e012dULL},
        {0xc8e5087ba6d33b84ULL, 0xaab37fd7d8f58178ULL},
        {0xfb1e4a9a90880a65ULL, 0xd5605fcdcf32e1d6ULL},
        {0x5cf2eea09a550680ULL, 0x855c3be0a17fcd26ULL},
        {0xf42faa48c0ea481fULL, 0xa6b34ad8c9dfc06fULL},
        {0xf13b94daf124da27ULL, 0xd0601d8efc57b08bULL},
        {0x76c53d08d6b70859ULL, 0x823c12795db6ce57ULL},
        {0x54768c4b0c64ca6fULL, 0xa2cb1717b52481edULL},
        {0xa9942f5dcf7dfd0aULL, 0xcb7ddcdda26da268ULL},
        {0xd3f93b35435d7c4dULL, 0xfe5d54150b090b02ULL},
        {0xc47bc5014a1a6db0ULL, 0x9efa548d26e5a6e1ULL},
        {0x359ab6419ca1091cULL, 0xc6b8e9b0709f109aULL},
        {0xc30163d203c94b63ULL, 0xf867241c8cc6d4c0ULL},
        {0x79e0de63425dcf1eULL, 0x9b407691d7fc44f8ULL},
        {0x985915fc12f542e5ULL, 0xc21094364dfb5636ULL},
        {0x3e6f5b7b17b2939eULL, 0xf294b943e17a2bc4ULL},
        {0xa705992ceecf9c43ULL, 0x979cf3ca6cec5b5aULL},
        {0x50c6ff782a838354ULL, 0xbd8430bd08277231ULL},
        {0xa4f8bf5635246429ULL, 0xece53cec4a314ebdULL},
        {0x871b7795e136be9aULL, 0x940f4613ae5ed136ULL},
        {0x28e2557b59846e40ULL, 0xb913179899f68584ULL},
        {0x331aeada2fe589d0ULL, 0xe757dd7ec07426e5ULL},
        {0x3ff0d2c85def7622ULL, 0x9096ea6f3848984fULL},
        {0x0fed077a756b53aaULL, 0xb4bca50b065abe63ULL},
        {0xd3e8495912c62895ULL, 0xe1ebce4dc7f16dfbULL},
        {0x64712dd7abbbd95dULL, 0x8d3360f09cf6e4bdULL},
        {0xbd8d794d96aacfb4ULL, 0xb080392cc4349decULL},
        {0xecf0d7a0fc5583a1ULL, 0xdca04777f541c567ULL},
        {0xf41686c49db57245ULL, 0x89e42caaf9491b60ULL},
        {0x311c2875c522ced6ULL, 0xac5d37d5b79b6239ULL},
        {0x7d633293366b828cULL, 0xd77485cb25823ac7ULL},
        {0xae5dff9c02033198ULL, 0x86a8d39ef77164bcULL},
        {0xd9f57f830283fdfdULL, 0xa8530886b54dbdebULL},
        {0xd072df63c324fd7cULL, 0xd267caa862a12d66ULL},
        {0x4247cb9e59f71e6eULL, 0x8380dea93da4bc60ULL},
        {0x52d9be85f074e609ULL, 0xa46116538d0deb78ULL},
        {0x67902e276c921f8cULL, 0xcd795be870516656ULL},
        {0x00ba1cd8a3db53b7ULL, 0x806bd9714632dff6ULL},
        {0x80e8a40eccd228a5ULL, 0xa086cfcd97bf97f3ULL},
        {0x6122cd128006b2ceULL, 0xc8a883c0fdaf7df0ULL},
        {0x796b805720085f82ULL, 0xfad2a4b13d1b5d6cULL},
        {0xcbe3303674053bb1ULL, 0x9cc3a6eec6311a63ULL},
        {0xbedbfc4411068a9dULL, 0xc3f490aa77bd60fcULL},
        {0xee92fb5515482d45ULL, 0xf4f1b4d515acb93bULL},
        {0x751bdd152d4d1c4bULL, 0x991711052d8bf3c5ULL},
        {0xd262d45a78a0635eULL, 0xbf5cd54678eef0b6ULL},
        {0x86fb897116c87c35ULL, 0xef340a98172aace4ULL},
        {0xd45d35e6ae3d4da1ULL, 0x9580869f0e7aac0eULL},
        {0x8974836059cca10aULL, 0xbae0a846d2195712ULL},
        {0x2bd1a438703fc94cULL, 0xe998d258869facd7ULL},
        {0x7b6306a34627ddd0ULL, 0x91ff83775423cc06ULL},
        {0x1a3bc84c17b1d543ULL, 0xb67f6455292cbf08ULL},
        {0x20caba5f1d9e4a94ULL, 0xe41f3d6a7377eecaULL},
        {0x547eb47b7282ee9dULL, 0x8e938662882af53eULL},
        {0xe99e619a4f23aa44ULL, 0xb23867fb2a35b28dULL},
        {0x6405fa00e2ec94d5ULL, 0xdec681f9f4c31f31ULL},
        {0xde83bc408dd3dd05ULL, 0x8b3c113c38f9f37eULL},
        {0x9624ab50b148d446ULL, 0xae0b158b4738705eULL},
        {0x3badd624dd9b0958ULL, 0xd98ddaee19068c76ULL},
        {0xe54ca5d70a80e5d7ULL, 0x87f8a8d4cfa417c9ULL},
        {0x5e9fcf4ccd211f4dULL, 0xa9f6d30a038d1dbcULL},
        {0x7647c32000696720ULL, 0xd47487cc8470652bULL},
        {0x29ecd9f40041e074ULL, 0x84c8d4dfd2c63f3bULL},
        {0xf468107100525891ULL, 0xa5fb0a17c777cf09ULL},
        {0x7182148d4066eeb5ULL, 0xcf79cc9db955c2ccULL},
        {0xc6f14cd848405531ULL, 0x81ac1fe293d599bfULL},
        {0xb8ada00e5a506a7dULL, 0xa21727db38cb002fULL},
        {0xa6d90811f0e4851dULL, 0xca9cf1d206fdc03bULL},
        {0x908f4a166d1da664ULL, 0xfd442e4688bd304aULL},
        {0x9a598e4e043287ffULL, 0x9e4a9cec15763e2eULL},
        {0x40eff1e1853f29feULL, 0xc5dd44271ad3cdbaULL},
        {0xd12bee59e68ef47dULL, 0xf7549530e188c128ULL},
        {0x82bb74f8301958cfULL, 0x9a94dd3e8cf578b9ULL},
        {0xe36a52363c1faf02ULL, 0xc13a148e3032d6e7ULL},
        {0xdc44e6c3cb279ac2ULL, 0xf18899b1bc3f8ca1ULL},
        {0x29ab103a5ef8c0baULL, 0x96f5600f15a7b7e5ULL},
        {0x7415d448f6b6f0e8ULL, 0xbcb2b812db11a5deULL},
        {0x111b495b3464ad22ULL, 0xebdf661791d60f56ULL},
        {0xcab10dd900beec35ULL, 0x936b9fcebb25c995ULL},
        {0x3d5d514f40eea743ULL, 0xb84687c269ef3bfbULL},
        {0x0cb4a5a3112a5113ULL, 0xe65829b3046b0afaULL},
        {0x47f0e785eaba72acULL, 0x8ff71a0fe2c2e6dcULL},
        {0x59ed216765690f57ULL, 0xb3f4e093db73a093ULL},
        {0x306869c13ec3532dULL, 0xe0f218b8d25088b8ULL},
        {0x1e414218c73a13fcULL, 0x8c974f7383725573ULL},
        {0xe5d1929ef90898fbULL, 0xafbd2350644eeacfULL},
        {0xdf45f746b74abf3aULL, 0xdbac6c247d62a583ULL},
        {0x6b8bba8c328eb784ULL, 0x894bc396ce5da772ULL},
        {0x066ea92f3f326565ULL, 0xab9eb47c81f5114fULL},
        {0xc80a537b0efefebeULL, 0xd686619ba27255a2ULL},
        {0xbd06742ce95f5f37ULL, 0x8613fd0145877585ULL},
        {0x2c48113823b73705ULL, 0xa798fc4196e952e7ULL},
        {0xf75a15862ca504c6ULL, 0xd17f3b51fca3a7a0ULL},
        {0x9a984d73dbe722fcULL, 0x82ef85133de648c4ULL},
        {0xc13e60d0d2e0ebbbULL, 0xa3ab66580d5fdaf5ULL},
        {0x318df905079926a9ULL, 0xcc963fee10b7d1b3ULL},
        {0xfdf17746497f7053ULL, 0xffbbcfe994e5c61fULL},
        {0xfeb6ea8bedefa634ULL, 0x9fd561f1fd0f9bd3ULL},
        {0xfe64a52ee96b8fc1ULL, 0xc7caba6e7c5382c8ULL},
        {0x3dfdce7aa3c673b1ULL, 0xf9bd690a1b68637bULL},
        {0x06bea10ca65c084fULL, 0x9c1661a651213e2dULL},
        {0x486e494fcff30a63ULL, 0xc31bfa0fe5698db8ULL},
        {0x5a89dba3c3efccfbULL, 0xf3e2f893dec3f126ULL},
        {0xf89629465a75e01dULL, 0x986ddb5c6b3a76b7ULL},
        {0xf6bbb397f1135824ULL, 0xbe89523386091465ULL},
        {0x746aa07ded582e2dULL, 0xee2ba6c0678b597fULL},
        {0xa8c2a44eb4571cddULL, 0x94db483840b717efULL},
        {0x92f34d62616ce414ULL, 0xba121a4650e4ddebULL},
        {0x77b020baf9c81d18ULL, 0xe896a0d7e51e1566ULL},
        {0x0ace1474dc1d122fULL, 0x915e2486ef32cd60ULL},
        {0x0d819992132456bbULL, 0xb5b5ada8aaff80b8ULL},
        {0x10e1fff697ed6c6aULL, 0xe3231912d5bf60e6ULL},
        {0xca8d3ffa1ef463c2ULL, 0x8df5efabc5979c8fULL},
        {0xbd308ff8a6b17cb3ULL, 0xb1736b96b6fd83b3ULL},
        {0xac7cb3f6d05ddbdfULL, 0xddd0467c64bce4a0ULL},
        {0x6bcdf07a423aa96cULL, 0x8aa22c0dbef60ee4ULL},
        {0x86c16c98d2c953c7ULL, 0xad4ab7112eb3929dULL},
        {0xe871c7bf077ba8b8ULL, 0xd89d64d57a607744ULL},
        {0x11471cd764ad4973ULL, 0x87625f056c7c4a8bULL},
        {0xd598e40d3dd89bd0ULL, 0xa93af6c6c79b5d2dULL},
        {0x4aff1d108d4ec2c4ULL, 0xd389b47879823479ULL},
        {0xcedf722a585139bbULL, 0x843610cb4bf160cbULL},
        {0xc2974eb4ee658829ULL, 0xa54394fe1eedb8feULL},
        {0x733d226229feea33ULL, 0xce947a3da6a9273eULL},
        {0x0806357d5a3f5260ULL, 0x811ccc668829b887ULL},
        {0xca07c2dcb0cf26f8ULL, 0xa163ff802a3426a8ULL},
        {0xfc89b393dd02f0b6ULL, 0xc9bcff6034c13052ULL},
        {0xbbac2078d443ace3ULL, 0xfc2c3f3841f17c67ULL},
        {0xd54b944b84aa4c0eULL, 0x9d9ba7832936edc0ULL},
        {0x0a9e795e65d4df12ULL, 0xc5029163f384a931ULL},
        {0x4d4617b5ff4a16d6ULL, 0xf64335bcf065d37dULL},
        {0x504bced1bf8e4e46ULL, 0x99ea0196163fa42eULL},
        {0xe45ec2862f71e1d7ULL, 0xc06481fb9bcf8d39ULL},
        {0x5d767327bb4e5a4dULL, 0xf07da27a82c37088ULL},
        {0x3a6a07f8d510f870ULL, 0x964e858c91ba2655ULL},
        {0x890489f70a55368cULL, 0xbbe226efb628afeaULL},
        {0x2b45ac74ccea842fULL, 0xeadab0aba3b2dbe5ULL},
        {0x3b0b8bc90012929eULL, 0x92c8ae6b464fc96fULL},
        {0x09ce6ebb40173745ULL, 0xb77ada0617e3bbcbULL},
        {0xcc420a6a101d0516ULL, 0xe55990879ddcaabdULL},
        {0x9fa946824a12232eULL, 0x8f57fa54c2a9eab6ULL},
        {0x47939822dc96abfaULL, 0xb32df8e9f3546564ULL},
        {0x59787e2b93bc56f8ULL, 0xdff9772470297ebdULL},
        {0x57eb4edb3c55b65bULL, 0x8bfbea76c619ef36ULL},
        {0xede622920b6b23f2ULL, 0xaefae51477a06b03ULL},
        {0xe95fab368e45eceeULL, 0xdab99e59958885c4ULL},
        {0x11dbcb0218ebb415ULL, 0x88b402f7fd75539bULL},
        {0xd652bdc29f26a11aULL, 0xaae103b5fcd2a881ULL},
        {0x4be76d3346f04960ULL, 0xd59944a37c0752a2ULL},
        {0x6f70a4400c562ddcULL, 0x857fcae62d8493a5ULL},
        {0xcb4ccd500f6bb953ULL, 0xa6dfbd9fb8e5b88eULL},
        {0x7e2000a41346a7a8ULL, 0xd097ad07a71f26b2ULL},
        {0x8ed400668c0c28c9ULL, 0x825ecc24c873782fULL},
        {0x728900802f0f32fbULL, 0xa2f67f2dfa90563bULL},
        {0x4f2b40a03ad2ffbaULL, 0xcbb41ef979346bcaULL},
        {0xe2f610c84987bfa9ULL, 0xfea126b7d78186bcULL},
        {0x0dd9ca7d2df4d7caULL, 0x9f24b832e6b0f436ULL},
        {0x91503d1c79720dbcULL, 0xc6ede63fa05d3143ULL},
        {0x75a44c6397ce912bULL, 0xf8a95fcf88747d94ULL},
        {0xc986afbe3ee11abbULL, 0x9b69dbe1b548ce7cULL},
        {0xfbe85badce996169ULL, 0xc24452da229b021bULL},
        {0xfae27299423fb9c4ULL, 0xf2d56790ab41c2a2ULL},
        {0xdccd879fc967d41bULL, 0x97c560ba6b0919a5ULL},
        {0x5400e987bbc1c921ULL, 0xbdb6b8e905cb600fULL},
        {0x290123e9aab23b69ULL, 0xed246723473e3813ULL},
        {0xf9a0b6720aaf6522ULL, 0x9436c0760c86e30bULL},
        {0xf808e40e8d5b3e6aULL, 0xb94470938fa89bceULL},
        {0xb60b1d1230b20e05ULL, 0xe7958cb87392c2c2ULL},
        {0xb1c6f22b5e6f48c3ULL, 0x90bd77f3483bb9b9ULL},
        {0x1e38aeb6360b1af4ULL, 0xb4ecd5f01a4aa828ULL},
        {0x25c6da63c38de1b1ULL, 0xe2280b6c20dd5232ULL},
        {0x579c487e5a38ad0fULL, 0x8d590723948a535fULL},
        {0x2d835a9df0c6d852ULL, 0xb0af48ec79ace837ULL},
        {0xf8e431456cf88e66ULL, 0xdcdb1b2798182244ULL},
        {0x1b8e9ecb641b5900ULL, 0x8a08f0f8bf0f156bULL},
        {0xe272467e3d222f40ULL, 0xac8b2d36eed2dac5ULL},
        {0x5b0ed81dcc6abb10ULL, 0xd7adf884aa879177ULL},
        {0x98e947129fc2b4eaULL, 0x86ccbb52ea94baeaULL},
        {0x3f2398d747b36225ULL, 0xa87fea27a539e9a5ULL},
        {0x8eec7f0d19a03aaeULL, 0xd29fe4b18e88640eULL},
        {0x1953cf68300424adULL, 0x83a3eeeef9153e89ULL},
        {0x5fa8c3423c052dd8ULL, 0xa48ceaaab75a8e2bULL},
        {0x3792f412cb06794eULL, 0xcdb02555653131b6ULL},
        {0xe2bbd88bbee40bd1ULL, 0x808e17555f3ebf11ULL},
        {0x5b6aceaeae9d0ec5ULL, 0xa0b19d2ab70e6ed6ULL},
        {0xf245825a5a445276ULL, 0xc8de047564d20a8bULL},
        {0xeed6e2f0f0d56713ULL, 0xfb158592be068d2eULL},
        {0x55464dd69685606cULL, 0x9ced737bb6c4183dULL},
        {0xaa97e14c3c26b887ULL, 0xc428d05aa4751e4cULL},
        {0xd53dd99f4b3066a9ULL, 0xf53304714d9265dfULL},
        {0xe546a8038efe402aULL, 0x993fe2c6d07b7fabULL},
        {0xde98520472bdd034ULL, 0xbf8fdb78849a5f96ULL},
        {0x963e66858f6d4441ULL, 0xef73d256a5c0f77cULL},
        {0xdde7001379a44aa9ULL, 0x95a8637627989aadULL},
        {0x5560c018580d5d53ULL, 0xbb127c53b17ec159ULL},
        {0xaab8f01e6e10b4a7ULL, 0xe9d71b689dde71afULL},
        {0xcab3961304ca70e9ULL, 0x9226712162ab070dULL},
        {0x3d607b97c5fd0d23ULL, 0xb6b00d69bb55c8d1ULL},
        {0x8cb89a7db77c506bULL, 0xe45c10c42a2b3b05ULL},
        {0x77f3608e92adb243ULL, 0x8eb98a7a9a5b04e3ULL},
        {0x55f038b237591ed4ULL, 0xb267ed1940f1c61cULL},
        {0x6b6c46dec52f6689ULL, 0xdf01e85f912e37a3ULL},
        {0x2323ac4b3b3da016ULL, 0x8b61313bbabce2c6ULL},
        {0xabec975e0a0d081bULL, 0xae397d8aa96c1b77ULL},
        {0x96e7bd358c904a22ULL, 0xd9c7dced53c72255ULL},
        {0x7e50d64177da2e55ULL, 0x881cea14545c7575ULL},
        {0xdde50bd1d5d0b9eaULL, 0xaa242499697392d2ULL},
        {0x955e4ec64b44e865ULL, 0xd4ad2dbfc3d07787ULL},
        {0xbd5af13bef0b113fULL, 0x84ec3c97da624ab4ULL},
        {0xecb1ad8aeacdd58fULL, 0xa6274bbdd0fadd61ULL},
        {0x67de18eda5814af3ULL, 0xcfb11ead453994baULL},
        {0x80eacf948770ced8ULL, 0x81ceb32c4b43fcf4ULL},
        {0xa1258379a94d028eULL, 0xa2425ff75e14fc31ULL},
        {0x096ee45813a04331ULL, 0xcad2f7f5359a3b3eULL},
        {0x8bca9d6e188853fdULL, 0xfd87b5f28300ca0dULL},
        {0x775ea264cf55347eULL, 0x9e74d1b791e07e48ULL},
        {0x95364afe032a819eULL, 0xc612062576589ddaULL},
        {0x3a83ddbd83f52205ULL, 0xf79687aed3eec551ULL},
        {0xc4926a9672793543ULL, 0x9abe14cd44753b52ULL},
        {0x75b7053c0f178294ULL, 0xc16d9a0095928a27ULL},
        {0x5324c68b12dd6339ULL, 0xf1c90080baf72cb1ULL},
        {0xd3f6fc16ebca5e04ULL, 0x971da05074da7beeULL},
        {0x88f4bb1ca6bcf585ULL, 0xbce5086492111aeaULL},
        {0x2b31e9e3d06c32e6ULL, 0xec1e4a7db69561a5ULL},
        {0x3aff322e62439fd0ULL, 0x9392ee8e921d5d07ULL},
        {0x09befeb9fad487c3ULL, 0xb877aa3236a4b449ULL},
        {0x4c2ebe687989a9b4ULL, 0xe69594bec44de15bULL},
        {0x0f9d37014bf60a11ULL, 0x901d7cf73ab0acd9ULL},
        {0x538484c19ef38c95ULL, 0xb424dc35095cd80fULL},
        {0x2865a5f206b06fbaULL, 0xe12e13424bb40e13ULL},
        {0xf93f87b7442e45d4ULL, 0x8cbccc096f5088cbULL},
        {0xf78f69a51539d749ULL, 0xafebff0bcb24aafeULL},
        {0xb573440e5a884d1cULL, 0xdbe6fecebdedd5beULL},
        {0x31680a88f8953031ULL, 0x89705f4136b4a597ULL},
        {0xfdc20d2b36ba7c3eULL, 0xabcc77118461cefcULL},
        {0x3d32907604691b4dULL, 0xd6bf94d5e57a42bcULL},
        {0xa63f9a49c2c1b110ULL, 0x8637bd05af6c69b5ULL},
        {0x0fcf80dc33721d54ULL, 0xa7c5ac471b478423ULL},
        {0xd3c36113404ea4a9ULL, 0xd1b71758e219652bULL},
        {0x645a1cac083126eaULL, 0x83126e978d4fdf3bULL},
        {0x3d70a3d70a3d70a4ULL, 0xa3d70a3d70a3d70aULL},
        {0xcccccccccccccccdULL, 0xccccccccccccccccULL},
        {0x0000000000000000ULL, 0x8000000000000000ULL},
        {0x0000000000000000ULL, 0xa000000000000000ULL},
        {0x0000000000000000ULL, 0xc800000000000000ULL},
        {0x0000000000000000ULL, 0xfa00000000000000ULL},
        {0x0000000000000000ULL, 0x9c40000000000000ULL},
        {0x0000000000000000ULL, 0xc350000000000000ULL},
        {0x0000000000000000ULL, 0xf424000000000000ULL},
        {0x0000000000000000ULL, 0x9896800000000000ULL},
        {0x0000000000000000ULL, 0xbebc200000000000ULL},
        {0x0000000000000000ULL, 0xee6b280000000000ULL},
        {0x0000000000000000ULL, 0x9502f90000000000ULL},
        {0x0000000000000000ULL, 0xba43b74000000000ULL},
        {0x0000000000000000ULL, 0xe8d4a51000000000ULL},
        {0x0000000000000000ULL, 0x9184e72a00000000ULL},
        {0x0000000000000000ULL, 0xb5e620f480000000ULL},
        {0x0000000000000000ULL, 0xe35fa931a0000000ULL},
        {0x0000000000000000ULL, 0x8e1bc9bf04000000ULL},
        {0x0000000000000000ULL, 0xb1a2bc2ec5000000ULL},
        {0x0000000000000000ULL, 0xde0b6b3a76400000ULL},
        {0x0000000000000000ULL, 0x8ac7230489e80000ULL},
        {0x0000000000000000ULL, 0xad78ebc5ac620000ULL},
        {0x0000000000000000ULL, 0xd8d726b7177a8000ULL},
        {0x0000000000000000ULL, 0x878678326eac9000ULL},
        {0x0000000000000000ULL, 0xa968163f0a57b400ULL},
        {0x0000000000000000ULL, 0xd3c21bcecceda100ULL},
        {0x0000000000000000ULL, 0x84595161401484a0ULL},
        {0x0000000000000000ULL, 0xa56fa5b99019a5c8ULL},
        {0x0000000000000000ULL, 0xcecb8f27f4200f3aULL},
        {0x4000000000000000ULL, 0x813f3978f8940984ULL},
        {0x5000000000000000ULL, 0xa18f07d736b90be5ULL},
        {0xa400000000000000ULL, 0xc9f2c9cd04674edeULL},
        {0x4d00000000000000ULL, 0xfc6f7c4045812296ULL},
        {0xf020000000000000ULL, 0x9dc5ada82b70b59dULL},
        {0x6c28000000000000ULL, 0xc5371912364ce305ULL},
        {0xc732000000000000ULL, 0xf684df56c3e01bc6ULL},
        {0x3c7f400000000000ULL, 0x9a130b963a6c115cULL},
        {0x4b9f100000000000ULL, 0xc097ce7bc90715b3ULL},
        {0x1e86d40000000000ULL, 0xf0bdc21abb48db20ULL},
        {0x1314448000000000ULL, 0x96769950b50d88f4ULL},
        {0x17d955a000000000ULL, 0xbc143fa4e250eb31ULL},
        {0x5dcfab0800000000ULL, 0xeb194f8e1ae525fdULL},
        {0x5aa1cae500000000ULL, 0x92efd1b8d0cf37beULL},
        {0xf14a3d9e40000000ULL, 0xb7abc627050305adULL},
        {0x6d9ccd05d0000000ULL, 0xe596b7b0c643c719ULL},
        {0xe4820023a2000000ULL, 0x8f7e32ce7bea5c6fULL},
        {0xdda2802c8a800000ULL, 0xb35dbf821ae4f38bULL},
        {0xd50b2037ad200000ULL, 0xe0352f62a19e306eULL},
        {0x4526f422cc340000ULL, 0x8c213d9da502de45ULL},
        {0x9670b12b7f410000ULL, 0xaf298d050e4395d6ULL},
        {0x3c0cdd765f114000ULL, 0xdaf3f04651d47b4cULL},
        {0xa5880a69fb6ac800ULL, 0x88d8762bf324cd0fULL},
        {0x8eea0d047a457a00ULL, 0xab0e93b6efee0053ULL},
        {0x72a4904598d6d880ULL, 0xd5d238a4abe98068ULL},
        {0x47a6da2b7f864750ULL, 0x85a36366eb71f041ULL},
        {0x999090b65f67d924ULL, 0xa70c3c40a64e6c51ULL},
        {0xfff4b4e3f741cf6dULL, 0xd0cf4b50cfe20765ULL},
        {0xbff8f10e7a8921a4ULL, 0x82818f1281ed449fULL},
        {0xaff72d52192b6a0dULL, 0xa321f2d7226895c7ULL},
        {0x9bf4f8a69f764490ULL, 0xcbea6f8ceb02bb39ULL},
        {0x02f236d04753d5b4ULL, 0xfee50b7025c36a08ULL},
        {0x01d762422c946590ULL, 0x9f4f2726179a2245ULL},
        {0x424d3ad2b7b97ef5ULL, 0xc722f0ef9d80aad6ULL},
        {0xd2e0898765a7deb2ULL, 0xf8ebad2b84e0d58bULL},
        {0x63cc55f49f88eb2fULL, 0x9b934c3b330c8577ULL},
        {0x3cbf6b71c76b25fbULL, 0xc2781f49ffcfa6d5ULL},
        {0x8bef464e3945ef7aULL, 0xf316271c7fc3908aULL},
        {0x97758bf0e3cbb5acULL, 0x97edd871cfda3a56ULL},
        {0x3d52eeed1cbea317ULL, 0xbde94e8e43d0c8ecULL},
        {0x4ca7aaa863ee4bddULL, 0xed63a231d4c4fb27ULL},
        {0x8fe8caa93e74ef6aULL, 0x945e455f24fb1cf8ULL},
        {0xb3e2fd538e122b44ULL, 0xb975d6b6ee39e436ULL},
        {0x60dbbca87196b616ULL, 0xe7d34c64a9c85d44ULL},
        {0xbc8955e946fe31cdULL, 0x90e40fbeea1d3a4aULL},
        {0x6babab6398bdbe41ULL, 0xb51d13aea4a488ddULL},
        {0xc696963c7eed2dd1ULL, 0xe264589a4dcdab14ULL},
        {0xfc1e1de5cf543ca2ULL, 0x8d7eb76070a08aecULL},
        {0x3b25a55f43294bcbULL, 0xb0de65388cc8ada8ULL},
        {0x49ef0eb713f39ebeULL, 0xdd15fe86affad912ULL},
        {0x6e3569326c784337ULL, 0x8a2dbf142dfcc7abULL},
        {0x49c2c37f07965404ULL, 0xacb92ed9397bf996ULL},
        {0xdc33745ec97be906ULL, 0xd7e77a8f87daf7fbULL},
        {0x69a028bb3ded71a3ULL, 0x86f0ac99b4e8dafdULL},
        {0xc40832ea0d68ce0cULL, 0xa8acd7c0222311bcULL},
        {0xf50a3fa490c30190ULL, 0xd2d80db02aabd62bULL},
        {0x792667c6da79e0faULL, 0x83c7088e1aab65dbULL},
        {0x577001b891185938ULL, 0xa4b8cab1a1563f52ULL},
        {0xed4c0226b55e6f86ULL, 0xcde6fd5e09abcf26ULL},
        {0x544f8158315b05b4ULL, 0x80b05e5ac60b6178ULL},
        {0x696361ae3db1c721ULL, 0xa0dc75f1778e39d6ULL},
        {0x03bc3a19cd1e38e9ULL, 0xc913936dd571c84cULL},
        {0x04ab48a04065c723ULL, 0xfb5878494ace3a5fULL},
        {0x62eb0d64283f9c76ULL, 0x9d174b2dcec0e47bULL},
        {0x3ba5d0bd324f8394ULL, 0xc45d1df942711d9aULL},
        {0xca8f44ec7ee36479ULL, 0xf5746577930d6500ULL},
        {0x7e998b13cf4e1ecbULL, 0x9968bf6abbe85f20ULL},
        {0x9e3fedd8c321a67eULL, 0xbfc2ef456ae276e8ULL},
        {0xc5cfe94ef3ea101eULL, 0xefb3ab16c59b14a2ULL},
        {0xbba1f1d158724a12ULL, 0x95d04aee3b80ece5ULL},
        {0x2a8a6e45ae8edc97ULL, 0xbb445da9ca61281fULL},
        {0xf52d09d71a3293bdULL, 0xea1575143cf97226ULL},
        {0x593c2626705f9c56ULL, 0x924d692ca61be758ULL},
        {0x6f8b2fb00c77836cULL, 0xb6e0c377cfa2e12eULL},
        {0x0b6dfb9c0f956447ULL, 0xe498f455c38b997aULL},
        {0x4724bd4189bd5eacULL, 0x8edf98b59a373fecULL},
        {0x58edec91ec2cb657ULL, 0xb2977ee300c50fe7ULL},
        {0x2f2967b66737e3edULL, 0xdf3d5e9bc0f653e1ULL},
        {0xbd79e0d20082ee74ULL, 0x8b865b215899f46cULL},
        {0xecd8590680a3aa11ULL, 0xae67f1e9aec07187ULL},
        {0xe80e6f4820cc9495ULL, 0xda01ee641a708de9ULL},
        {0x3109058d147fdcddULL, 0x884134fe908658b2ULL},
        {0xbd4b46f0599fd415ULL, 0xaa51823e34a7eedeULL},
        {0x6c9e18ac7007c91aULL, 0xd4e5e2cdc1d1ea96ULL},
        {0x03e2cf6bc604ddb0ULL, 0x850fadc09923329eULL},
        {0x84db8346b786151cULL, 0xa6539930bf6bff45ULL},
        {0xe612641865679a63ULL, 0xcfe87f7cef46ff16ULL},
        {0x4fcb7e8f3f60c07eULL, 0x81f14fae158c5f6eULL},
        {0xe3be5e330f38f09dULL, 0xa26da3999aef7749ULL},
        {0x5cadf5bfd3072cc5ULL, 0xcb090c8001ab551cULL},
        {0x73d9732fc7c8f7f6ULL, 0xfdcb4fa002162a63ULL},
        {0x2867e7fddcdd9afaULL, 0x9e9f11c4014dda7eULL},
        {0xb281e1fd541501b8ULL, 0xc646d63501a1511dULL},
        {0x1f225a7ca91a4226ULL, 0xf7d88bc24209a565ULL},
        {0x3375788de9b06958ULL, 0x9ae757596946075fULL},
        {0x0052d6b1641c83aeULL, 0xc1a12d2fc3978937ULL},
        {0xc0678c5dbd23a49aULL, 0xf209787bb47d6b84ULL},
        {0xf840b7ba963646e0ULL, 0x9745eb4d50ce6332ULL},
        {0xb650e5a93bc3d898ULL, 0xbd176620a501fbffULL},
        {0xa3e51f138ab4cebeULL, 0xec5d3fa8ce427affULL},
        {0xc66f336c36b10137ULL, 0x93ba47c980e98cdfULL},
        {0xb80b0047445d4184ULL, 0xb8a8d9bbe123f017ULL},
        {0xa60dc059157491e5ULL, 0xe6d3102ad96cec1dULL},
        {0x87c89837ad68db2fULL, 0x9043ea1ac7e41392ULL},
        {0x29babe4598c311fbULL, 0xb454e4a179dd1877ULL},
        {0xf4296dd6fef3d67aULL, 0xe16a1dc9d8545e94ULL},
        {0x1899e4a65f58660cULL, 0x8ce2529e2734bb1dULL},
        {0x5ec05dcff72e7f8fULL, 0xb01ae745b101e9e4ULL},
        {0x76707543f4fa1f73ULL, 0xdc21a1171d42645dULL},
        {0x6a06494a791c53a8ULL, 0x899504ae72497ebaULL},
        {0x0487db9d17636892ULL, 0xabfa45da0edbde69ULL},
        {0x45a9d2845d3c42b6ULL, 0xd6f8d7509292d603ULL},
        {0x0b8a2392ba45a9b2ULL, 0x865b86925b9bc5c2ULL},
        {0x8e6cac7768d7141eULL, 0xa7f26836f282b732ULL},
        {0x3207d795430cd926ULL, 0xd1ef0244af2364ffULL},
        {0x7f44e6bd49e807b8ULL, 0x8335616aed761f1fULL},
        {0x5f16206c9c6209a6ULL, 0xa402b9c5a8d3a6e7ULL},
        {0x36dba887c37a8c0fULL, 0xcd036837130890a1ULL},
        {0xc2494954da2c9789ULL, 0x802221226be55a64ULL},
        {0xf2db9baa10b7bd6cULL, 0xa02aa96b06deb0fdULL},
        {0x6f92829494e5acc7ULL, 0xc83553c5c8965d3dULL},
        {0xcb772339ba1f17f9ULL, 0xfa42a8b73abbf48cULL},
        {0xff2a760414536efbULL, 0x9c69a97284b578d7ULL},
        {0xfef5138519684abaULL, 0xc38413cf25e2d70dULL},
        {0x7eb258665fc25d69ULL, 0xf46518c2ef5b8cd1ULL},
        {0xef2f773ffbd97a61ULL, 0x98bf2f79d5993802ULL},
        {0xaafb550ffacfd8faULL, 0xbeeefb584aff8603ULL},
        {0x95ba2a53f983cf38ULL, 0xeeaaba2e5dbf6784ULL},
        {0xdd945a747bf26183ULL, 0x952ab45cfa97a0b2ULL},
        {0x94f971119aeef9e4ULL, 0xba756174393d88dfULL},
        {0x7a37cd5601aab85dULL, 0xe912b9d1478ceb17ULL},
        {0xac62e055c10ab33aULL, 0x91abb422ccb812eeULL},
        {0x577b986b314d6009ULL, 0xb616a12b7fe617aaULL},
        {0xed5a7e85fda0b80bULL, 0xe39c49765fdf9d94ULL},
        {0x14588f13be847307ULL, 0x8e41ade9fbebc27dULL},
        {0x596eb2d8ae258fc8ULL, 0xb1d219647ae6b31cULL},
        {0x6fca5f8ed9aef3bbULL, 0xde469fbd99a05fe3ULL},
        {0x25de7bb9480d5854ULL, 0x8aec23d680043beeULL},
        {0xaf561aa79a10ae6aULL, 0xada72ccc20054ae9ULL},
        {0x1b2ba1518094da04ULL, 0xd910f7ff28069da4ULL},
        {0x90fb44d2f05d0842ULL, 0x87aa9aff79042286ULL},
        {0x353a1607ac744a53ULL, 0xa99541bf57452b28ULL},
        {0x42889b8997915ce8ULL, 0xd3fa922f2d1675f2ULL},
        {0x69956135febada11ULL, 0x847c9b5d7c2e09b7ULL},
        {0x43fab9837e699095ULL, 0xa59bc234db398c25ULL},
        {0x94f967e45e03f4bbULL, 0xcf02b2c21207ef2eULL},
        {0x1d1be0eebac278f5ULL, 0x8161afb94b44f57dULL},
        {0x6462d92a69731732ULL, 0xa1ba1ba79e1632dcULL},
        {0x7d7b8f7503cfdcfeULL, 0xca28a291859bbf93ULL},
        {0x5cda735244c3d43eULL, 0xfcb2cb35e702af78ULL},
        {0x3a0888136afa64a7ULL, 0x9defbf01b061adabULL},
        {0x088aaa1845b8fdd0ULL, 0xc56baec21c7a1916ULL},
        {0x8aad549e57273d45ULL, 0xf6c69a72a3989f5bULL},
        {0x36ac54e2f678864bULL, 0x9a3c2087a63f6399ULL},
        {0x84576a1bb416a7ddULL, 0xc0cb28a98fcf3c7fULL},
        {0x656d44a2a11c51d5ULL, 0xf0fdf2d3f3c30b9fULL},
        {0x9f644ae5a4b1b325ULL, 0x969eb7c47859e743ULL},
        {0x873d5d9f0dde1feeULL, 0xbc4665b596706114ULL},
        {0xa90cb506d155a7eaULL, 0xeb57ff22fc0c7959ULL},
        {0x09a7f12442d588f2ULL, 0x9316ff75dd87cbd8ULL},
        {0x0c11ed6d538aeb2fULL, 0xb7dcbf5354e9beceULL},
        {0x8f1668c8a86da5faULL, 0xe5d3ef282a242e81ULL},
        {0xf96e017d694487bcULL, 0x8fa475791a569d10ULL},
        {0x37c981dcc395a9acULL, 0xb38d92d760ec4455ULL},
        {0x85bbe253f47b1417ULL, 0xe070f78d3927556aULL},
        {0x93956d7478ccec8eULL, 0x8c469ab843b89562ULL},
        {0x387ac8d1970027b2ULL, 0xaf58416654a6babbULL},
        {0x06997b05fcc0319eULL, 0xdb2e51bfe9d0696aULL},
        {0x441fece3bdf81f03ULL, 0x88fcf317f22241e2ULL},
        {0xd527e81cad7626c3ULL, 0xab3c2fddeeaad25aULL},
        {0x8a71e223d8d3b074ULL, 0xd60b3bd56a5586f1ULL},
        {0xf6872d5667844e49ULL, 0x85c7056562757456ULL},
        {0xb428f8ac016561dbULL, 0xa738c6bebb12d16cULL},
        {0xe13336d701beba52ULL, 0xd106f86e69d785c7ULL},
        {0xecc0024661173473ULL, 0x82a45b450226b39cULL},
        {0x27f002d7f95d0190ULL, 0xa34d721642b06084ULL},
        {0x31ec038df7b441f4ULL, 0xcc20ce9bd35c78a5ULL},
        {0x7e67047175a15271ULL, 0xff290242c83396ceULL},
        {0x0f0062c6e984d386ULL, 0x9f79a169bd203e41ULL},
        {0x52c07b78a3e60868ULL, 0xc75809c42c684dd1ULL},
        {0xa7709a56ccdf8a82ULL, 0xf92e0c3537826145ULL},
        {0x88a66076400bb691ULL, 0x9bbcc7a142b17ccbULL},
        {0x6acff893d00ea435ULL, 0xc2abf989935ddbfeULL},
        {0x0583f6b8c4124d43ULL, 0xf356f7ebf83552feULL},
        {0xc3727a337a8b704aULL, 0x98165af37b2153deULL},
        {0x744f18c0592e4c5cULL, 0xbe1bf1b059e9a8d6ULL},
        {0x1162def06f79df73ULL, 0xeda2ee1c7064130cULL},
        {0x8addcb5645ac2ba8ULL, 0x9485d4d1c63e8be7ULL},
        {0x6d953e2bd7173692ULL, 0xb9a74a0637ce2ee1ULL},
        {0xc8fa8db6ccdd0437ULL, 0xe8111c87c5c1ba99ULL},
        {0x1d9c9892400a22a2ULL, 0x910ab1d4db9914a0ULL},
        {0x2503beb6d00cab4bULL, 0xb54d5e4a127f59c8ULL},
        {0x2e44ae64840fd61dULL, 0xe2a0b5dc971f303aULL},
        {0x5ceaecfed289e5d2ULL, 0x8da471a9de737e24ULL},
        {0x7425a83e872c5f47ULL, 0xb10d8e1456105dadULL},
        {0xd12f124e28f77719ULL, 0xdd50f1996b947518ULL},
        {0x82bd6b70d99aaa6fULL, 0x8a5296ffe33cc92fULL},
        {0x636cc64d1001550bULL, 0xace73cbfdc0bfb7bULL},
        {0x3c47f7e05401aa4eULL, 0xd8210befd30efa5aULL},
        {0x65acfaec34810a71ULL, 0x8714a775e3e95c78ULL},
        {0x7f1839a741a14d0dULL, 0xa8d9d1535ce3b396ULL},
        {0x1ede48111209a050ULL, 0xd31045a8341ca07cULL},
        {0x934aed0aab460432ULL, 0x83ea2b892091e44dULL},
        {0xf81da84d5617853fULL, 0xa4e4b66b68b65d60ULL},
        {0x36251260ab9d668eULL, 0xce1de40642e3f4b9ULL},
        {0xc1d72b7c6b426019ULL, 0x80d2ae83e9ce78f3ULL},
        {0xb24cf65b8612f81fULL, 0xa1075a24e4421730ULL},
        {0xdee033f26797b627ULL, 0xc94930ae1d529cfcULL},
        {0x169840ef017da3b1ULL, 0xfb9b7cd9a4a7443cULL},
        {0x8e1f289560ee864eULL, 0x9d412e0806e88aa5ULL},
        {0xf1a6f2bab92a27e2ULL, 0xc491798a08a2ad4eULL},
        {0xae10af696774b1dbULL, 0xf5b5d7ec8acb58a2ULL},
        {0xacca6da1e0a8ef29ULL, 0x9991a6f3d6bf1765ULL},
        {0x17fd090a58d32af3ULL, 0xbff610b0cc6edd3fULL},
        {0xddfc4b4cef07f5b0ULL, 0xeff394dcff8a948eULL},
        {0x4abdaf101564f98eULL, 0x95f83d0a1fb69cd9ULL},
        {0x9d6d1ad41abe37f1ULL, 0xbb764c4ca7a4440fULL},
        {0x84c86189216dc5edULL, 0xea53df5fd18d5513ULL},
        {0x32fd3cf5b4e49bb4ULL, 0x92746b9be2f8552cULL},
        {0x3fbc8c33221dc2a1ULL, 0xb7118682dbb66a77ULL},
        {0x0fabaf3feaa5334aULL, 0xe4d5e82392a40515ULL},
        {0x29cb4d87f2a7400eULL, 0x8f05b1163ba6832dULL},
        {0x743e20e9ef511012ULL, 0xb2c71d5bca9023f8ULL},
        {0x914da9246b255416ULL, 0xdf78e4b2bd342cf6ULL},
        {0x1ad089b6c2f7548eULL, 0x8bab8eefb6409c1aULL},
        {0xa184ac2473b529b1ULL, 0xae9672aba3d0c320ULL},
        {0xc9e5d72d90a2741eULL, 0xda3c0f568cc4f3e8ULL},
        {0x7e2fa67c7a658892ULL, 0x8865899617fb1871ULL},
        {0xddbb901b98feeab7ULL, 0xaa7eebfb9df9de8dULL},
        {0x552a74227f3ea565ULL, 0xd51ea6fa85785631ULL},
        {0xd53a88958f87275fULL, 0x8533285c936b35deULL},
        {0x8a892abaf368f137ULL, 0xa67ff273b8460356ULL},
        {0x2d2b7569b0432d85ULL, 0xd01fef10a657842cULL},
        {0x9c3b29620e29fc73ULL, 0x8213f56a67f6b29bULL},
        {0x8349f3ba91b47b8fULL, 0xa298f2c501f45f42ULL},
        {0x241c70a936219a73ULL, 0xcb3f2f7642717713ULL},
        {0xed238cd383aa0110ULL, 0xfe0efb53d30dd4d7ULL},
        {0xf4363804324a40aaULL, 0x9ec95d1463e8a506ULL},
        {0xb143c6053edcd0d5ULL, 0xc67bb4597ce2ce48ULL},
        {0xdd94b7868e94050aULL, 0xf81aa16fdc1b81daULL},
        {0xca7cf2b4191c8326ULL, 0x9b10a4e5e9913128ULL},
        {0xfd1c2f611f63a3f0ULL, 0xc1d4ce1f63f57d72ULL},
        {0xbc633b39673c8cecULL, 0xf24a01a73cf2dccfULL},
        {0xd5be0503e085d813ULL, 0x976e41088617ca01ULL},
        {0x4b2d8644d8a74e18ULL, 0xbd49d14aa79dbc82ULL},
        {0xddf8e7d60ed1219eULL, 0xec9c459d51852ba2ULL},
        {0xcabb90e5c942b503ULL, 0x93e1ab8252f33b45ULL},
        {0x3d6a751f3b936243ULL, 0xb8da1662e7b00a17ULL},
        {0x0cc512670a783ad4ULL, 0xe7109bfba19c0c9dULL},
        {0x27fb2b80668b24c5ULL, 0x906a617d450187e2ULL},
        {0xb1f9f660802dedf6ULL, 0xb484f9dc9641e9daULL},
        {0x5e7873f8a0396973ULL, 0xe1a63853bbd26451ULL},
        {0xdb0b487b6423e1e8ULL, 0x8d07e33455637eb2ULL},
        {0x91ce1a9a3d2cda62ULL, 0xb049dc016abc5e5fULL},
        {0x7641a140cc7810fbULL, 0xdc5c5301c56b75f7ULL},
        {0xa9e904c87fcb0a9dULL, 0x89b9b3e11b6329baULL},
        {0x546345fa9fbdcd44ULL, 0xac2820d9623bf429ULL},
        {0xa97c177947ad4095ULL, 0xd732290fbacaf133ULL},
        {0x49ed8eabcccc485dULL, 0x867f59a9d4bed6c0ULL},
        {0x5c68f256bfff5a74ULL, 0xa81f301449ee8c70ULL},
        {0x73832eec6fff3111ULL, 0xd226fc195c6a2f8cULL},
        {0xc831fd53c5ff7eabULL, 0x83585d8fd9c25db7ULL},
        {0xba3e7ca8b77f5e55ULL, 0xa42e74f3d032f525ULL},
        {0x28ce1bd2e55f35ebULL, 0xcd3a1230c43fb26fULL},
        {0x7980d163cf5b81b3ULL, 0x80444b5e7aa7cf85ULL},
        {0xd7e105bcc332621fULL, 0xa0555e361951c366ULL},
        {0x8dd9472bf3fefaa7ULL, 0xc86ab5c39fa63440ULL},
        {0xb14f98f6f0feb951ULL, 0xfa856334878fc150ULL},
        {0x6ed1bf9a569f33d3ULL, 0x9c935e00d4b9d8d2ULL},
        {0x0a862f80ec4700c8ULL, 0xc3b8358109e84f07ULL},
        {0xcd27bb612758c0faULL, 0xf4a642e14c6262c8ULL},
        {0x8038d51cb897789cULL, 0x98e7e9cccfbd7dbdULL},
        {0xe0470a63e6bd56c3ULL, 0xbf21e44003acdd2cULL},
        {0x1858ccfce06cac74ULL, 0xeeea5d5004981478ULL},
        {0x0f37801e0c43ebc8ULL, 0x95527a5202df0ccbULL},
        {0xd30560258f54e6baULL, 0xbaa718e68396cffdULL},
        {0x47c6b82ef32a2069ULL, 0xe950df20247c83fdULL},
        {0x4cdc331d57fa5441ULL, 0x91d28b7416cdd27eULL},
        {0xe0133fe4adf8e952ULL, 0xb6472e511c81471dULL},
        {0x58180fddd97723a6ULL, 0xe3d8f9e563a198e5ULL},
        {0x570f09eaa7ea7648ULL, 0x8e679c2f5e44ff8fULL},
};

//      floor(q * log2(10)) + 63, with the logarithm carried as a fixed point
//      fraction: 217706 over 65536 is log2(10) to more places than the range
//      of q here can tell apart, and the multiply cannot overflow because q
//      is between -342 and 308.
//
//      The shift has to be the arithmetic one, because floor and not truncate
//      is what is wanted for a negative q. gcc defines a right shift of a
//      negative signed value that way on every target, which is the same
//      guarantee the rest of this library already builds on.
static b32 numbers_binary_power(b32 power)
{
        return ((217706 * power) >> 16) + 63;
}

static p64 numbers_multiply_high(p64 left, p64 right, p64 address_to low)
{
        p128 product = (p128)left * (p128)right;

        address_to low = (p64)product;

        return (p64)(product >> 64);
}

static bool numbers_estimate(p64 significand, b32 power,
                             const numbers_format address_to shape,
                             p64 address_to answer)
{
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 precision = shape->significand_place + 3;
        b32 lead;
        b32 upper;
        b32 exponent;
        b32 place;
        p64 high;
        p64 low;
        p64 mask;
        p64 mantissa;

        if (significand == 0)
                return false;

        if (power < NUMBERS_SMALLEST_POWER || power > NUMBERS_LARGEST_POWER)
                return false;

        lead = bits_leading_zeros(significand);
        significand <<= lead;

        mask = ~(p64)0 >> precision;
        high = numbers_multiply_high(
                significand,
                numbers_power_of_five[power - NUMBERS_SMALLEST_POWER][1],
                address_of low);

        //      The top of the product is all ones under the mask, so the
        //      bits below it can still carry into the answer and the second
        //      half of the table entry has to be brought in.
        if ((high & mask) == mask)
        {
                p64 second_low;
                p64 second_high = numbers_multiply_high(
                        significand,
                        numbers_power_of_five[power - NUMBERS_SMALLEST_POWER][0],
                        address_of second_low);

                low += second_high;

                if (second_high > low)
                        high++;
        }

        //      Still saturated after both halves: 128 bits could not separate
        //      this value from the boundary. The powers between ten to the
        //      minus twenty seventh and ten to the fifty fifth are exact in
        //      the table, so there is nothing to separate there and the
        //      answer stands; everywhere else the tier declines.
        if (low == ~(p64)0 && (power < -27 || power > 55))
                return false;

        upper = (b32)(high >> 63);
        place = upper + 64 - shape->significand_place - 3;
        mantissa = high >> place;
        exponent = numbers_binary_power(power) + upper - lead - shape->bias;

        //      Subnormal, zero or infinite answers go to the decimal register,
        //      which is the only tier that can decide the errno question.
        if (exponent <= 0 || exponent >= limit)
                return false;

        //      The one case where round-half-to-even has to be applied by
        //      hand: the discarded half is exactly a half, the product was
        //      exact, and the bit above it is odd.
        if (low <= 1 && power >= shape->round_even_low &&
            power <= shape->round_even_high && (mantissa & 3) == 1 &&
            (mantissa << place) == high)
                mantissa &= ~(p64)1;

        mantissa += mantissa & 1;
        mantissa >>= 1;

        if (mantissa >= ((p64)2 << shape->significand_place))
        {
                mantissa = (p64)1 << shape->significand_place;
                exponent++;

                if (exponent >= limit)
                        return false;
        }

        mantissa &= ~((p64)1 << shape->significand_place);

        address_to answer = ((p64)(p32)exponent << shape->exponent_place) | mantissa;

        return true;
}

/*
        The tier as the conversion calls it: one estimate when every digit
        fitted, two when they did not, and a refusal the moment anything is
        not certain.
*/
static bool numbers_estimated(numbers_scan address_to number,
                              const numbers_format address_to shape,
                              p64 address_to answer)
{
        b32 power = number->point - number->packed_count;
        p64 first;

        if (number->count == 0)
                return false;

        if (!numbers_estimate(number->packed, power, shape, address_of first))
                return false;

        if (number->significant != number->packed_count)
        {
                p64 second;

                if (!numbers_estimate(number->packed + 1, power, shape,
                                      address_of second))
                        return false;

                if (first != second)
                        return false;
        }

        if (number->negative)
                first |= (p64)1 << (shape->exponent_place + shape->exponent_bits);

        address_to answer = first;

        return true;
}

/*
        The exact tier, which is Clinger's.

        Two conditions and one operation. The significant digits must all
        have fitted in the p64 -- nineteen of them at most, and none dropped
        -- and the integer they form must be small enough to be a double with
        nothing lost, which is two to the fifty third. The power of ten must
        be one of the ones that is itself exactly a double, which runs from
        ten to the minus twenty second to ten to the twenty second because
        beyond that a power of ten needs more than fifty three bits.

        Given both, the answer is one multiply or one divide of two exact
        operands, and IEEE 754 says a single operation returns the correctly
        rounded result of the exact answer. There is nothing to check
        afterwards and nothing that can be off by a bit.

        The narrow one is the same argument in twenty four bits, and it is
        computed in f32 rather than in double and narrowed: a double result
        rounded again to a float rounds twice, and two roundings are not one.
*/
static const decimal numbers_power_of_ten[23] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static const f32 numbers_narrow_power_of_ten[11] = {
        1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

#define NUMBERS_EXACT(name, type, bits, limit, powers)                       \
        static bool name(numbers_scan address_to number,                    \
                         type address_to answer)                             \
        {                                                                    \
                if (number->truncated ||                                    \
                    number->significant != number->packed_count ||          \
                    number->packed > ((p64)1 << (bits)))                     \
                        return false;                                        \
                                                                             \
                b32 power = number->point - number->significant;             \
                                                                             \
                if (power > (limit) || power < -(limit))                     \
                        return false;                                        \
                                                                             \
                type value = (type)number->packed;                           \
                                                                             \
                if (power > 0)                                               \
                        value *= (powers)[power];                             \
                else if (power < 0)                                          \
                        value /= (powers)[-power];                            \
                                                                             \
                address_to answer = number->negative ? -value : value;       \
                return true;                                                 \
        }

NUMBERS_EXACT(numbers_exact_double, decimal, 53, 22, numbers_power_of_ten)
NUMBERS_EXACT(numbers_exact_narrow, f32, 24, 10, numbers_narrow_power_of_ten)
#undef NUMBERS_EXACT

/*
        The two answers that are not numbers, built out of the format rather
        than out of a constant, so that one body serves all four widths.

        An infinity is the exponent field saturated over a zero significand.
        A quiet NaN is that same exponent over a significand whose top bit is
        set, and glibc puts the parenthesised n-char-sequence in the bits
        below it, so this does too -- strtoull read the sequence during the
        scan and left it in `packed`. x87 is the one format where the leading
        bit of the significand is stored rather than implied, and a NaN there
        has to carry it, which is the one line the other three do not need.
*/
static p128 numbers_special(const numbers_format address_to shape, bool negative,
                            b32 kind, p64 payload)
{
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        p128 bits = 0;

        if (kind == NUMBERS_NOT_A_NUMBER)
        {
                bits = (p128)1 << (shape->significand_place - 1);
                bits |= (p128)payload &
                        ((((p128)1 << (shape->significand_place - 1)) - 1));
        }

        //      x87 stores the leading bit of its significand rather than
        //      implying it, and an eighty bit infinity or NaN with that bit
        //      clear is one of the shapes the hardware calls invalid. glibc
        //      sets it and so does this; the other three formats have no such
        //      bit to set.
        if (shape->exponent_place > shape->significand_place)
                bits |= (p128)1 << shape->significand_place;

        bits |= (p128)(p64)limit << shape->exponent_place;

        if (negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        return bits;
}

/*
        The three conversions themselves, and the one shape they share.

        Read the text; hand back the end pointer whatever happened; answer a
        name or a hexadecimal float or a decimal one; and set ERANGE when the
        answer was pushed to an infinity or squeezed down into the subnormals
        or to zero. errno is only ever written, never cleared -- C says a
        library function may set errno and may not clear one it did not set,
        and a caller that wants to know clears it first.

        NUMBERS_SLOW_TIER_ONLY exists for the test lane and for nothing else.
        Defined, both fast tiers are compiled out and every conversion goes
        through the decimal register, so the lane can run identical inputs
        through both and diff -- which is how a defect in a fast tier gets
        found without a reference library being in the loop.
*/
#ifndef NUMBERS_SLOW_TIER_ONLY
#define NUMBERS_FAST(type, exact, format, bits_type)                         \
        do                                                                   \
        {                                                                    \
                type quick;                                                  \
                p64 estimate;                                                \
                                                                             \
                if (exact(address_of number, address_of quick))              \
                        return quick;                                        \
                if (numbers_estimated(address_of number, address_of format,  \
                                      address_of estimate))                  \
                {                                                            \
                        shape.bits = (bits_type)estimate;                     \
                        return shape.value;                                  \
                }                                                            \
        } while (0)
#else
#define NUMBERS_FAST(type, exact, format, bits_type) do { } while (0)
#endif

#define NUMBERS_TO(name, type, shape_type, format, bits_type, fast)   \
        static type name(string_address input,                              \
                         string_address address_to stopped)                  \
        {                                                                    \
                numbers_scan number;                                         \
                shape_type shape;                                            \
                b32 condition = NUMBERS_FINE;                                \
                                                                             \
                numbers_read(input, address_of number);                      \
                if (stopped)                                                 \
                        address_to stopped = number.stopped;                 \
                if (number.kind == NUMBERS_NONE)                             \
                {                                                            \
                        shape.bits = 0;                                      \
                        return shape.value;                                  \
                }                                                            \
                if (number.kind == NUMBERS_INFINITE ||                       \
                    number.kind == NUMBERS_NOT_A_NUMBER)                     \
                {                                                            \
                        shape.bits = (bits_type)numbers_special(             \
                            address_of format, number.negative, number.kind,  \
                            number.packed);                                  \
                        return shape.value;                                  \
                }                                                            \
                if (number.hexadecimal)                                      \
                        shape.bits = (bits_type)numbers_round_binary(        \
                            address_of number, address_of format,            \
                            address_of condition);                           \
                else                                                         \
                {                                                            \
                        fast;                                                \
                        shape.bits = (bits_type)numbers_assemble(            \
                            address_of number, address_of format,            \
                            address_of condition);                           \
                }                                                            \
                if (condition != NUMBERS_FINE)                               \
                        errno = ERANGE;                                       \
                return shape.value;                                          \
        }

/*
        The short decimal is read by library.c's string_to_decimal_short,
        which is assembly on all three machines and says there why. What is
        left here is the general path and the choice between them.
*/
NUMBERS_TO(string_to_decimal_general, decimal, numbers_shape, numbers_binary64,
           p64, NUMBERS_FAST(decimal, numbers_exact_double, numbers_binary64,
                             p64))

static decimal string_to_decimal(string_address input,
                                 string_address address_to stopped)
{
        decimal quick;

        if (string_to_decimal_short(input, stopped, address_of quick))
                return quick;

        return string_to_decimal_general(input, stopped);
}
NUMBERS_TO(string_to_narrow, f32, numbers_narrow_shape, numbers_binary32, p32,
           NUMBERS_FAST(f32, numbers_exact_narrow, numbers_binary32, p32))

/*
        long double, which is eighty bit x87 on x86_64 and binary128 on arm64
        and riscv64, and is the format the fast tiers deliberately skip.

        There is no exact tier here and no estimating one: both are written
        for a significand that fits a p64 with room over it, and neither
        eighty nor a hundred and thirteen bits does. Every long double
        conversion therefore runs the decimal register, which is correct and
        is measured in microseconds rather than nanoseconds. That is the right
        trade for a format a program converts once at startup and never in a
        loop, and it is what let this format cost four numbers rather than a
        second implementation.

        Nothing here does arithmetic on a long double. Every value is
        assembled as bits and read out through a union, which is not only
        faster but necessary: a -nostdlib link has no libgcc under it, and a
        single comparison of two binary128 values on arm64 or riscv64 is a
        call to __letf2 that would not resolve.
*/
NUMBERS_TO(string_to_extended, f128, numbers_extended_shape, numbers_extended,
           p128, (void)0)
#undef NUMBERS_TO
#undef NUMBERS_FAST

/*
        The standard names.

        One line wrappers rather than macros, for the reason math.c gives:
        a program can take the address of one, and a local variable called
        strtod does not silently become a call. atof is here rather than in
        standard.inc beside atoi because it is strtod with the end pointer
        thrown away, and strtod is C.
*/
//      The end pointer is a char ** and not a string_address address_to,
//      which is the house spelling everything below the standard names uses.
//      library.c settled that question for the ninety eight names it covers
//      and these three are the same kind of name reached the same way: a
//      program that brings its own <stdlib.h> line for strtod must compile
//      against this one, and it cannot if the tree says the end pointer is
//      an unsigned char **. The cast inside is the whole of the difference;
//      the two types are one width in one register.
static decimal strtod(const char address_to input, char address_to address_to stopped)
{
        return string_to_decimal((string_address)input, (string_address address_to)stopped);
}

static f32 strtof(const char address_to input, char address_to address_to stopped)
{
        return string_to_narrow((string_address)input, (string_address address_to)stopped);
}

static f128 strtold(const char address_to input, char address_to address_to stopped)
{
        return string_to_extended((string_address)input, (string_address address_to)stopped);
}

static decimal atof(string_address input)
{
        return string_to_decimal(input, null);
}

#pragma GCC pop_options

#endif // decimal_bits == 64

#endif // !KERNEL_MODE && !STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_NUMBERS
#endif // STANDARD_SKIP_NUMBERS

#ifndef STANDARD_SKIP_STDLIB
/* ---- stdlib.c ---- */
/*
        Experimental C standard library

        stdlib: leaving, the environment, sorting, searching, division that
        keeps its remainder, and a generator

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_STDLIB
#define STANDARD_MODERN_C_STANDARD_STDLIB

/*
        Guarded out of the kernel build and out of a no-platform build. core.c
        includes this umbrella and library.c sets KERNEL_MODE from __MODULE__,
        so without this the module would pull in a second struct stat, a second
        open and a second errno beside the ones <linux/...> already declares.
        The three families that shipped without this guard were each correct in
        isolation and wrong together; the ones that had it were right.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        This is ordinary C, and it is here rather than in library.c for the
        reason netlink.c gives: library.c and its includes hold declarations
        and assembly and nothing else, and that is checked.

        Nothing below is a hardware floor. An allocator has policy; a sort has
        a strategy; an environment has ownership. Every one of those is a
        decision made once and read the same way on all three architectures,
        and writing it three times in assembly would produce three chances to
        get the policy wrong and no chance to get the machine more right. The
        two places where the machine is actually involved -- the trap that
        ends the process, and the trap that raises a signal at it -- are calls
        into routines library.c already owns.

        The whole family is prefixed stdlib_ where it is ours, and carries the
        C name where C has one.
*/

/*
        The names C spells for itself, defended against a second definition.

        Another family in this tree may reach for div_t or RAND_MAX first, and
        a typedef cannot be tested for the way a macro can, so each one gets a
        guard macro of its own that says the shape has been laid down.
*/
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif

#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

#ifndef STANDARD_DIVISION_TYPES
#define STANDARD_DIVISION_TYPES

typedef struct
{
        b32 quot;
        b32 rem;
} div_t;

typedef struct
{
        b64 quot;
        b64 rem;
} ldiv_t;

typedef struct
{
        b64 quot;
        b64 rem;
} lldiv_t;

#endif // STANDARD_DIVISION_TYPES

/*
        A bump arena, deliberately not the allocator.

        Everything this file needs to own is environment text: a NAME=value
        string that outlives the call that made it, and the vector of pointers
        that indexes them. The lifetime is the process, the churn is a handful
        of entries in the life of most programs, and the total is measured in
        hundreds of bytes.

        Aligned slices avoid per-entry allocator metadata and calls. Old
        strings remain valid after unsetenv or replacement; chunks are kept
        until process exit, including their unused tails.

        Sixty four kilobytes at a time because that is one mmap for an
        environment far larger than any program here will build, and a program
        that sets a variable in a loop gets geometric behaviour rather than a
        syscall per set.
*/
#define STDLIB_ARENA_CHUNK 65536
#define STDLIB_ARENA_ALIGN 16

static p8 address_to stdlib_arena_next = null;
static positive stdlib_arena_left = 0;

/* Standalone users of this family have no errno dependency. With the error
   family present, every failed environment operation reports its cause. */
#ifdef STANDARD_MODERN_C_STANDARD_ERROR
#define stdlib_environment_failure(result) error_result(result)
#else
#define stdlib_environment_failure(result) (-1)
#endif

static address_any stdlib_arena_take(positive size)
{
        address_any given;

        if (size > (positive)-1 - (STDLIB_ARENA_ALIGN - 1))
                return ((fn)stdlib_environment_failure(-ENOMEM), null);
        size = (size + (STDLIB_ARENA_ALIGN - 1)) & ~(positive)(STDLIB_ARENA_ALIGN - 1);

        if (size > stdlib_arena_left)
        {
                positive want = memory_growth(0, size, STDLIB_ARENA_CHUNK);
                p8 address_to block;

                if (want == 0)
                        return ((fn)stdlib_environment_failure(-ENOMEM), null);

                block = (p8 address_to)memory(want);

                if (is_null(block) || system_failed(block))
                        return ((fn)stdlib_environment_failure(-ENOMEM), null);

                //      Whatever was left of the previous chunk is abandoned.
                //      It is at most one allocation's worth and chasing it
                //      would need a free list this file has no other use for.
                stdlib_arena_next = block;
                stdlib_arena_left = want;
        }

        given = stdlib_arena_next;
        stdlib_arena_next += size;
        stdlib_arena_left -= size;

        return given;
}

/*
        The environment, and why it has to be copied before it can be changed.

        The kernel hands a process its environment on the stack it starts on:
        a vector of pointers ending in null, sitting above argv, in memory
        that cannot be extended because the argument strings are immediately
        after it. program_environment_list() returns that vector. Reading it
        is fine forever. Adding an entry to it is not possible at all.

        So the first call that could change anything copies the vector -- the
        pointers, not the strings, which are perfectly good where they are --
        into arena memory with room to grow, and every call after that reads
        and writes the copy. getenv reads the copy too, which is the part that
        is easy to get backwards: a getenv still looking at the kernel vector
        would never see a setenv, and would look right in every test that did
        not do both.

        string_get_environment is the library's own lookup and it is the one
        used here, against our vector rather than the kernel's. It compares a
        name against an entry where the entry starts and stops at the equals,
        which is exactly the match this needs.
*/
#define STDLIB_ENVIRONMENT_SLACK 8

static string_address address_to stdlib_environment_vector = null;
static positive stdlib_environment_count = 0;
static positive stdlib_environment_room = 0;

/*
        environ, which is this vector under the name every program knows.

        Every other C name in this tree is attached to a prose one by an alias
        or by a wrapper, and neither is available here, because this is an
        object rather than a routine: the name has to be the object.

        It was left out until now, and the reason has just stopped being true.
        Nothing ran before main, so nothing could point this anywhere, and a
        program written the ordinary way -- for (walk = environ; *walk; walk++)
        -- would have dereferenced a null on its first line. That is worse than
        not offering the name at all, which is why it was not offered. The
        umbrella's startup shim is the C startup that was missing, it runs
        before main, and it publishes the kernel's own vector here, so the loop
        above works from the first line of the first program that writes it.

        Publishing is not one act. Two calls in this file install a new vector:
        the copy stdlib_environment_own takes before anything can be changed,
        and the growth that runs when that copy fills up. Both end by
        publishing, so a program that reads environ, calls setenv and reads it
        again is looking at the vector setenv is actually using rather than at
        a pointer that went stale the first time the environment was written
        to. unsetenv and clearenv move entries about inside a vector they were
        already given and leave its address alone, so they publish nothing,
        which is correct rather than an omission.

        What is deliberately not followed is a program assigning to environ
        itself. POSIX allows it, and it is how a caller replaces a whole
        environment in one move; getenv here would go on reading the vector
        this file owns and would not see it. Following it would put a second
        pointer comparison in every lookup, paid for by everybody, against the
        chance that one caller did the one thing almost nobody does. It is
        written down instead.
*/
string_address address_to environ = null;

//      Take a private copy of the vector, once. False means the arena could
//      not give us the room, and every caller falls back to read-only
//      behaviour against the kernel's own vector rather than losing entries.
static bool stdlib_environment_own(void)
{
        string_address address_to kernel;
        string_address address_to made;
        positive count = 0;
        positive room;

        if (!is_null(stdlib_environment_vector))
                return true;

        kernel = program_environment_list();

        if (!is_null(kernel))
                while (!is_null(kernel[count]))
                        count++;

        if (count > positive_max / sizeof(string_address) -
                        STDLIB_ENVIRONMENT_SLACK - 1)
                return false;
        room = count + STDLIB_ENVIRONMENT_SLACK;
        made = (string_address address_to)stdlib_arena_take((room + 1) *
                                                            sizeof(string_address));

        if (is_null(made))
                return false;

        //      The pointers and not the strings, which stay where the
        //      kernel put them. memory_copy_apart is the non-overlapping
        //      copy and these two are a fresh arena slice and the kernel's
        //      own vector, which cannot be the same memory.
        if (count > 0)
                memory_copy_apart(made, kernel, count * sizeof(string_address));

        made[count] = null;

        stdlib_environment_vector = made;
        stdlib_environment_count = count;
        stdlib_environment_room = room;

        environ = stdlib_environment_vector;

        return true;
}

//      Room for one more entry beside the null that ends the vector. Growth
//      abandons the old vector in the arena; it is a few dozen bytes and the
//      alternative is a free() this file has decided not to require.
static bool stdlib_environment_grow(void)
{
        string_address address_to made;
        positive room;

        if (stdlib_environment_count < stdlib_environment_room)
                return true;
        if (stdlib_environment_count == positive_max)
                return false;
        room = memory_growth(stdlib_environment_room,
                             stdlib_environment_count + 1,
                             STDLIB_ENVIRONMENT_SLACK);
        if (!room || room >= positive_max / sizeof(string_address))
                return false;
        made = (string_address address_to)stdlib_arena_take((room + 1) *
                                                            sizeof(string_address));

        if (is_null(made))
                return false;

        /* The new arena slice cannot overlap the live vector. Copy its
           pointers and terminating null through the architecture's bulk
           core instead of retiring a load, store, increment and branch for
           every entry here. */
        memory_copy_apart(made, stdlib_environment_vector,
                          (stdlib_environment_count + 1) *
                              sizeof(string_address));

        stdlib_environment_vector = made;
        stdlib_environment_room = room;

        environ = stdlib_environment_vector;

        return true;
}

//      The index of the entry whose key is exactly this name, or -1. It is
//      library.common.c's NAME= question asked of every entry in turn. The
//      older spelling here compared first and
//      looked for the equals afterwards, which answers differently for a name
//      that carries an equals of its own -- the very case this comment says
//      can never be found -- and which reads `length` bytes of an entry whose
//      key is shorter than that before deciding it does not match.
static PURE bipolar stdlib_environment_find(string_address name, positive length)
{
        positive index;

        for (index = 0; index < stdlib_environment_count; index++)
                if (environment_key_is(stdlib_environment_vector[index],
                                       name, length))
                        return (bipolar)index;

        return -1;
}

//      A name is a name only if it is not empty and holds no equals: the
//      equals is the separator, and a name carrying one would produce an
//      entry nothing could ever look up again.
static bool stdlib_environment_name_valid(string_address name, positive address_to length)
{
        if (is_null(name))
                return false;

        string_address stop = string_first_of_or_end(name, '=');

        if (stop == name || stop[0] == '=')
                return false;

        *length = (positive)(stop - name);
        return true;
}

//      The live vector, kernel's or ours, for anyone who needs to hand a whole
//      environment to execve. This is what environ would have been; see the
//      note at system() below for why the variable itself is not defined here.
string_address address_to stdlib_environment_list(void)
{
        if (stdlib_environment_own())
                return stdlib_environment_vector;

        return program_environment_list();
}

string_address getenv(string_address name)
{
        if (is_null(name) || name[0] == end)
                return null;

        if (stdlib_environment_own())
                return string_get_environment(stdlib_environment_vector, name);

        return string_get_environment(program_environment_list(), name);
}

static inline INLINE fn stdlib_environment_install(bipolar found,
                                                   string_address entry)
{
        if (found >= 0)
        {
                stdlib_environment_vector[found] = entry;
                return;
        }

        stdlib_environment_vector[stdlib_environment_count++] = entry;
        stdlib_environment_vector[stdlib_environment_count] = null;
}

/*
        setenv copies both halves into storage it owns, because the caller is
        entitled to free or overwrite either one the instant this returns.

        Zero for overwrite means "only if it is not already there", and the
        already-there case is a success that changes nothing rather than a
        refusal: a program that sets a default and then reads it back gets the
        value that was already in the environment, which is the point.
*/
b32 setenv(string_address name, string_address value, b32 overwrite)
{
        positive name_length;
        positive value_length;
        bipolar found;
        p8 address_to entry;

        if (!stdlib_environment_name_valid(name, address_of name_length))
                return stdlib_environment_failure(-EINVAL);

        if (is_null(value))
                value = (string_address) "";

        if (!stdlib_environment_own() || !stdlib_environment_grow())
                return stdlib_environment_failure(-ENOMEM);

        found = stdlib_environment_find(name, name_length);

        if (found >= 0 && !overwrite)
                return 0;

        value_length = string_length(value);
        if (name_length > positive_max - 2 ||
            value_length > positive_max - name_length - 2)
                return stdlib_environment_failure(-ENOMEM);
        entry = (p8 address_to)stdlib_arena_take(name_length + value_length + 2);

        if (is_null(entry))
                return stdlib_environment_failure(-ENOMEM);

        memory_copy_apart(entry, name, name_length);
        entry[name_length] = '=';
        memory_copy_apart(entry + name_length + 1, value, value_length);
        entry[name_length + 1 + value_length] = end;

        stdlib_environment_install(found, entry);
        return 0;
}

/*
        unsetenv removes every entry with this key, not just the first.

        A duplicate key in the vector is not something this file can create,
        but it is something a kernel or an exec'ing parent can hand us, and
        removing one of two would leave a variable that getenv still finds
        after it has been unset. The loop is over an environment of a few
        dozen entries and runs once.
*/
b32 unsetenv(string_address name)
{
        positive name_length;
        positive index = 0;

        if (!stdlib_environment_name_valid(name, address_of name_length))
                return stdlib_environment_failure(-EINVAL);

        if (!stdlib_environment_own())
                return stdlib_environment_failure(-ENOMEM);

        while (index < stdlib_environment_count)
        {
                if (environment_key_is(stdlib_environment_vector[index],
                                       name, name_length))
                {
                        //      Everything above the entry moves down one
                        //      place, the null that ends the vector included,
                        //      which is why the count is the distance to the
                        //      end and not one less. memory_copy is memmove
                        //      -- library.c aliases both names onto it -- so
                        //      the overlap is the routine's business.
                        memory_copy(stdlib_environment_vector + index,
                                    stdlib_environment_vector + index + 1,
                                    (stdlib_environment_count - index) *
                                            sizeof(string_address));

                        stdlib_environment_count--;
                        continue;
                }

                index++;
        }

        return 0;
}

/*
        putenv keeps the caller's string, and that is not an oversight.

        POSIX says the string becomes part of the environment: a later write
        through the caller's own pointer changes the variable, and freeing it
        while it is still installed is the caller's bug rather than ours. It
        is the one entry point here that stores something the arena did not
        make, which is why setenv is the one to reach for and this exists for
        the code that already expects it.

        A string with no equals in it is a request to remove that name, which
        is what glibc does and what a caller writing putenv("TZ") means.
*/
b32 putenv(string_address entry)
{
        positive name_length;
        string_address stop;
        bipolar found;

        if (is_null(entry))
                return stdlib_environment_failure(-EINVAL);

        //      The same one scan the name check above uses. Where it stopped
        //      is the key's length and what it stopped on says whether there
        //      is a value at all.
        stop = string_first_of_or_end(entry, '=');

        if (stop[0] != '=')
                return unsetenv(entry);

        name_length = (positive)(stop - entry);

        if (name_length == 0)
                return stdlib_environment_failure(-EINVAL);

        if (!stdlib_environment_own() || !stdlib_environment_grow())
                return stdlib_environment_failure(-ENOMEM);

        found = stdlib_environment_find(entry, name_length);

        stdlib_environment_install(found, entry);
        return 0;
}

/*
        An environment with nothing in it is still a vector with a null in it,
        because everything that walks one stops on that null and not on a
        pointer that was never written.

        The entries it drops are abandoned rather than reclaimed, like every
        other drop here. Nothing leaks that the arena made on the first call,
        because those pointers were the kernel's own strings, but a program
        that clears and refills in a loop grows the arena without bound. That
        is the same missing free() the arena's own note is about and it goes
        away with it.
*/
b32 clearenv(void)
{
        if (!stdlib_environment_own())
                return stdlib_environment_failure(-ENOMEM);

        stdlib_environment_count = 0;
        stdlib_environment_vector[0] = null;

        return 0;
}

#undef stdlib_environment_failure

/*
        Leaving.

        library.c already defines exit, and what it defines is the trap: it
        puts the code in the argument register and calls the kernel, and
        nothing in the process runs afterwards. That is precisely _Exit, and
        it is what _start calls with main's return value.

        C's exit is a different function that happens to share the spelling --
        it runs the atexit handlers in reverse and flushes what is buffered
        before it traps. It cannot be named exit here, because the assembly
        symbol already is, and the C name is attached by a macro instead.

        That macro used to be opt-in, and what it was staying away from was
        real. The shell in this same tree includes this umbrella and calls exit
        inside forked children, most of them after a failed execve. A child
        inherits the parent's four-kilobyte log buffer with whatever the parent
        had not flushed still in it, and a child that flushed would print the
        parent's pending output a second time, on every failed command. glibc
        has exactly that bug and answers it with folklore -- flush before you
        fork -- which is advice given to callers rather than a mechanism.

        The mechanism is below, in stdlib_buffers_are_ours, and when output is
        pending it costs one getpid on the one path in a process's life that
        ends the process. With it the macro is the default and the divergence
        is closed at both ends:
        printf then exit prints, which is what every C program on earth
        expects and what this library got wrong, and printf then fork then
        exit in the child prints nothing extra, which is better than what
        glibc does. STANDARD_EXIT_KEEPS_TRAP goes back the other way for a
        caller that wants the bare trap under the C spelling.

        stdlib_exit is the function under either name and can always be called
        directly.

        Handlers are popped before they are called, so a handler that calls
        exit again finishes the list rather than starting it over, and a
        handler that registers another one during the walk gets it run: both
        are undefined behaviour in the standard and both are cheaper to make
        harmless than to detect.

        The flush hook exists because streams belong to another family. exit
        must close what fopen opened, and this file must not name fopen. A
        null function pointer that the stream family fills in costs one load
        and one test at the only moment in a process's life where nothing is
        in a hurry.
*/
#define STDLIB_EXIT_HANDLERS 64

typedef fn(address_to stdlib_exit_handler)(void);

static stdlib_exit_handler stdlib_exit_list[STDLIB_EXIT_HANDLERS];
static positive stdlib_exit_count = 0;

static stdlib_exit_handler stdlib_quick_list[STDLIB_EXIT_HANDLERS];
static positive stdlib_quick_count = 0;

//      Set by the stream family, if there is one linked. Called after the
//      atexit handlers and before the trap.
fn(address_to stdlib_exit_flush_hook)(void) = null;

/*
        Whose bytes are in the buffer.

        A buffer is a promise to write something later, and a fork copies the
        promise without copying the obligation. After one there are two
        processes each holding the same unwritten bytes, and each of them
        leaving through a path that flushes writes those bytes once, so they
        are written twice. That is the double print, and it is not a shell
        problem: it is what buffered output plus fork means, here and in glibc
        alike.

        So the flush asks whose bytes these are before it writes them, and the
        question is asked of the buffer rather than of the process. A stream
        records whose process filled it at the moment it went from empty to
        holding something -- the owner field in struct stream -- and the flush
        at exit compares that against the identity below. A child of a process
        that flushed before it forked starts with an empty buffer, stamps its
        own writes with itself, and its output goes out at exit exactly as a
        program with no fork in it would expect. A child that inherited bytes
        it never wrote finds somebody else's stamp and leaves them where they
        are; the parent is still holding the same bytes and will write them
        itself.

        The identity is one getpid, 32ns on the machine this was written on,
        and a stream asks for it only when its buffer goes from empty to
        holding something -- once per four kilobytes for a program writing
        steadily. Through fwrite to a real file, in milliseconds, lower is
        quicker:

              chunk    rounds    without      with
                 64   2000000       20.9      22.0
               4096    200000      105.3     109.8

        Four to five percent, and the four kilobyte column is exactly the two
        hundred thousand getpids it makes. Against /dev/null the same stamp is
        fifteen percent at sixty four bytes and thirty eight at four thousand,
        which is worth printing because it is the shape of the cost rather
        than the size of it: what is bought is one syscall per buffer, and it
        only looks large standing beside the cheapest syscall Linux has.

        A page mapped MADV_WIPEONFORK answers the same question with a byte
        load and no syscall at all. It was tried, it works, and it is not here:
        it is an mmap, an madvise, a generation counter and a fallback for the
        kernels that refuse, to recover four percent on a path nothing in this
        tree writes through -- the shell's output goes to the log buffer and
        not to a stream, and every consumer that would pay the four percent is
        hypothetical.

        The log buffer in any.inc is coarser, because it has to be. It is
        assembly, and library.c holds assembly and declarations and nothing
        else, so there is nowhere in it to keep a stamp and no C to keep one
        from. What is available is the identity recorded at startup, which
        answers a weaker question -- is this the process the program started
        as -- and that is what gates the log flush below. A forked child's
        implicit flush therefore leaves the log buffer alone whether or not
        the bytes in it are its own. That is exactly the behaviour every such
        child had before this file learned to flush at all, so nothing that
        used to arrive stops arriving, and an explicit log_flush in a child
        still works and is what the shell in this tree does at all twenty of
        its exit sites and before all of its clones.

        The residue, named rather than hidden: a child that writes bytes of
        its own into a stream buffer that already held its parent's loses its
        own along with the inherited ones, because the two are in one buffer
        with nothing between them to tell them apart. glibc writes both and
        then writes the parent's again when the parent exits. Neither answer
        is right; this one never writes anything twice and never invents
        output that was already written, and the way to have both halves is
        the one POSIX has always given -- flush before the fork, or leave
        through _exit.

        A recorded identity of zero means no shim ran -- a kernel build, or a
        translation unit that asked for no platform under it -- and then the
        buffers can only be ours, because nothing in such a build forked.
*/
static positive stdlib_process_at_start = 0;

positive stdlib_process_identity(void)
{
        return (positive)system_call(syscall(getpid));
}

static bool stdlib_buffers_are_ours(void)
{
        if (stdlib_process_at_start == 0)
                return true;

        return stdlib_process_identity() == stdlib_process_at_start;
}

/*
        What the startup shim in the umbrella calls before main, and the only
        thing in this file whose ordering matters.

        Two acts, neither of which can be a static initialiser and both of
        which a program can observe on its very first line: the identity the
        log flush above compares against, and the environment vector under its
        C name.

        The vector published here is the kernel's own and not a copy.
        stdlib_environment_own has not run and cannot have -- nothing has
        called setenv, because nothing has run -- and calling it here to be
        tidy would make every program that includes this umbrella pay for an
        allocation and a copy of its whole environment at startup, in order to
        publish a pointer that reading alone never needed. The moment anything
        does take that copy, that call publishes the copy over this one.
*/
fn stdlib_program_starting(void)
{
#ifdef LINUX
        stdlib_process_at_start = program_initial_identity();
#else
        stdlib_process_at_start = stdlib_process_identity();
#endif

        environ = program_environment_list();
}

static inline INLINE b32 stdlib_handler_add(stdlib_exit_handler handler,
                                            stdlib_exit_handler list[],
                                            positive address_to count)
{
        if (is_null(handler) || address_to count >= STDLIB_EXIT_HANDLERS)
                return -1;

        list[address_to count] = handler;
        address_to count += 1;

        return 0;
}

b32 atexit(stdlib_exit_handler handler)
{
        return stdlib_handler_add(handler, stdlib_exit_list,
                                  address_of stdlib_exit_count);
}

b32 at_quick_exit(stdlib_exit_handler handler)
{
        return stdlib_handler_add(handler, stdlib_quick_list,
                                  address_of stdlib_quick_count);
}

DEAD_END fn stdlib_exit(b32 code)
{
        while (stdlib_exit_count > 0)
        {
                stdlib_exit_count--;
                stdlib_exit_list[stdlib_exit_count]();
        }

        //      The hook decides per stream, because a stream knows which
        //      process filled it. It is called after the handlers rather than
        //      before them, so that anything a handler wrote is included.
        if (!is_null(stdlib_exit_flush_hook))
                stdlib_exit_flush_hook();

        //      An empty log has neither bytes nor an owner to decide. Test
        //      that published state before asking the kernel whose process
        //      this is, so a silent program does not pay a getpid syscall on
        //      its way out. A nonempty log keeps the fork-safe ownership
        //      decision described above unchanged.
        if (log_writer_buffer_length != 0 && stdlib_buffers_are_ours())
                log_flush();

        exit(code);

        __builtin_unreachable();
}

//      quick_exit deliberately does not flush. C11 added it for the program
//      that has decided its buffered state is not worth writing out, and a
//      flush here would take that choice away from the only caller who ever
//      asks for it.
DEAD_END fn quick_exit(b32 code)
{
        while (stdlib_quick_count > 0)
        {
                stdlib_quick_count--;
                stdlib_quick_list[stdlib_quick_count]();
        }

        exit(code);

        __builtin_unreachable();
}

#ifndef STANDARD_NO_UNDERSCORE_EXIT
DEAD_END fn _Exit(b32 code)
{
        exit(code);

        __builtin_unreachable();
}
#endif

#ifndef STANDARD_NO_UNDERSCORE_EXIT
DEAD_END fn _exit(b32 code) __attribute__((alias("_Exit")));
#endif

/*
        abort, which has to win.

        The standard says abort terminates, and it says so without an escape:
        a SIGABRT handler that returns does not get to keep the process
        running. So this raises it three times over, each attempt weaker in
        what it trusts and stronger in what it forces.

        First the signal is unblocked and raised with whatever disposition the
        program installed, so a handler that wants to write a message gets to.
        If that returns -- because the handler returned, or because SIGABRT
        was ignored and the raise did nothing at all -- the disposition is
        reset to the default and it is raised again, and the default action
        for SIGABRT is to die.  If even that returns, which needs the kernel
        to have refused both calls, the process leaves by the trap with 127.

        The sigaction handed to the kernel is four zeroed words and it is the
        same four words on every architecture here, which is worth saying
        because the struct is not. x86_64 carries a restorer pointer between
        the flags and the mask and arm64 and riscv64 do not, so the mask lands
        at a different offset on each. Every field is zero -- SIG_DFL is zero,
        no flags, an empty mask -- so a zeroed buffer is the right buffer
        under both layouts, and there is no need to write the struct twice.
        A restorer would be needed to install a real handler on x86_64, and
        this never installs one, which is exactly why it does not need one.

        tgkill rather than kill so the signal is delivered to this thread and
        cannot be taken by another one that has SIGABRT blocked.
*/
#ifndef SIGABRT
#define SIGABRT 6
#endif

#define STDLIB_SIGNAL_UNBLOCK 1
#define STDLIB_SIGNAL_SET_BYTES 8

static fn stdlib_signal_unblock(b32 number)
{
        positive mask = (positive)1 << (number - 1);

        system_signal_mask(STDLIB_SIGNAL_UNBLOCK, address_of mask, 0,
                           STDLIB_SIGNAL_SET_BYTES);
}

/* The raw default-action shape is shared by abort and namespace launchers;
   it is four zero words on every supported kernel ABI. */
static inline INLINE fn process_signal_default(b32 number)
{
        system_signal_install(number, 0, 0, 0, null);
}

static fn stdlib_signal_raise(b32 number)
{
        bipolar group = system_call(syscall(getpid));
        bipolar thread = system_call(syscall(gettid));

        system_call_3(syscall(tgkill), (positive)group, (positive)thread,
                      (positive)number);
}

DEAD_END fn abort(void)
{
        stdlib_signal_unblock(SIGABRT);
        stdlib_signal_raise(SIGABRT);

        process_signal_default(SIGABRT);
        stdlib_signal_unblock(SIGABRT);
        stdlib_signal_raise(SIGABRT);

        exit(127);

        __builtin_unreachable();
}

/*
        qsort, and why it is an introsort.

        The three candidates were a plain quicksort with median of three, a
        heapsort, and an introsort that is the first until it stops going
        well and the second afterwards.

        Plain quicksort loses on adversarial input, and adversarial input is
        not hypothetical here: the organ pipe -- up to the middle and back
        down again -- is a shape real data takes, it defeats median of three,
        and it turns n log n into n squared with no warning and no bound.
        Heapsort alone never does that but it pays for the guarantee
        everywhere: its comparisons are more numerous and its memory access
        pattern jumps by powers of two, which costs cache misses a partition
        scan does not have. Introsort is the first one's speed with the
        second one's ceiling, at the price of counting the recursion depth,
        and the counter is one decrement per partition.

        The depth limit is twice the base two logarithm of the count. A well
        behaved input halves at every level and finishes in log n, so the
        limit is never reached and heapsort is never entered; an input that
        splits badly enough to spend twice that budget was never going to
        finish as a quicksort, and the switch happens while the partition is
        still large enough for heapsort's guarantee to matter.

        The pivot is chosen by Tukey's ninther once the range is over forty
        elements and by the median of three below that, and the reason is the
        organ pipe: an array that climbs to the middle and comes back down.
        Median of three reads the first, the middle and the last of that and
        finds the minimum, the maximum and the minimum again, so it picks the
        minimum -- the worst pivot available -- and peels one element per
        partition until the depth limit fires and heapsort finishes the job.
        Measured, on two hundred thousand elements, that cost 11.4 million
        comparisons against glibc's 1.9 and was six times slower in time. The
        ninther takes the median of three medians of three, spread across the
        range, and cannot be fooled by a shape that only has three points on
        it. The same input costs 4.5 million comparisons with it.

        The scan is Hoare's, with the pivot parked at the front and both
        halves walking inward. The right scan stops on the pivot itself and
        needs no bound; the left scan carries one, because the ninther's pick
        can be larger than the last element and there is then nothing at the
        far end to stop it. That is one predictable compare per step of one of
        the two loops, against an indirect call in the same step, and buying
        the sentinel back would mean giving up the ninther.

        Both scans stop on an element equal to the pivot rather than stepping
        over it, which is what makes the all-equal input split down the middle
        instead of degenerating. Scanning with <= instead of < is the classic
        mistake here and it turns every run of equal keys into the worst case.

        Small ranges are finished by insertion sort. The cutoff is twelve,
        which is where a partition costs more in bookkeeping than the
        insertion it would save.

        On the retpoline question this codebase raises: ASM_CALL in library.c
        routes an indirect call through __x86_indirect_thunk_ only in a kernel
        build on x86_64 with the retpoline mitigation configured, and that is
        assembly asking for a thunk the compiler would have supplied on its
        own. qsort is C, so the comparator call is emitted by the compiler and
        obeys whatever -mindirect-branch the build asks for; nothing here has
        to ask. What the indirect call does change is the shape of the answer
        rather than the choice of algorithm: the comparator is by far the most
        expensive thing in the loop, so the sort is worth measuring by
        comparison count and not by swap count, and that is what picked
        Sedgewick's partition and the insertion cutoff. The measurement of the
        call's cost is in the notes with the benchmark.

*/
#ifndef STDLIB_SORT_SMALL
#define STDLIB_SORT_SMALL 12
#endif

#define STDLIB_SORT_NINTHER 40

typedef b32(address_to stdlib_compare)(address_any left, address_any right);
typedef b32(address_to stdlib_compare_context)(address_any left, address_any right,
                                               address_any context);

typedef struct
{
        stdlib_compare_context compare;
        address_any context;
        positive size;
} stdlib_sort_plan;

static bool stdlib_sort_before(p8 address_to left, p8 address_to right,
                               stdlib_sort_plan address_to plan)
{
        return plan->compare(left, right, plan->context) < 0;
}

//      Which of the three belongs in the middle. Returned as a position
//      rather than a value, because an element here is an opaque run of bytes
//      of a width only the caller knows.
static p8 address_to stdlib_sort_median(p8 address_to first, p8 address_to second,
                                        p8 address_to third,
                                        stdlib_sort_plan address_to plan)
{
        if (stdlib_sort_before(first, second, plan))
        {
                if (stdlib_sort_before(second, third, plan))
                        return second;

                return stdlib_sort_before(first, third, plan) ? third : first;
        }

        if (stdlib_sort_before(first, third, plan))
                return first;

        return stdlib_sort_before(second, third, plan) ? third : second;
}

static fn stdlib_sort_insertion(p8 address_to base, positive count,
                                stdlib_sort_plan address_to plan)
{
        positive size = plan->size;
        positive index;

        for (index = 1; index < count; index++)
        {
                p8 address_to walk = base + index * size;

                while (walk > base && stdlib_sort_before(walk, walk - size, plan))
                {
                        memory_exchange_apart(walk, walk - size, size);
                        walk -= size;
                }
        }
}

//      Sift one element down a heap whose root is at index zero. The child
//      chosen is the larger of the two, so the element that rises is the one
//      that belongs above the other.
static fn stdlib_sort_sift(p8 address_to base, positive root, positive count,
                           stdlib_sort_plan address_to plan)
{
        positive size = plan->size;

        for (;;)
        {
                positive child = root * 2 + 1;

                if (child >= count)
                        return;

                if (child + 1 < count &&
                    stdlib_sort_before(base + child * size,
                                       base + (child + 1) * size, plan))
                        child++;

                if (!stdlib_sort_before(base + root * size, base + child * size, plan))
                        return;

                memory_exchange_apart(base + root * size, base + child * size,
                                      size);
                root = child;
        }
}

static fn stdlib_sort_heap(p8 address_to base, positive count,
                           stdlib_sort_plan address_to plan)
{
        positive size = plan->size;
        positive build = count / 2;

        while (build > 0)
        {
                build--;
                stdlib_sort_sift(base, build, count, plan);
        }

        while (count > 1)
        {
                count--;
                memory_exchange_apart(base, base + count * size, size);
                stdlib_sort_sift(base, 0, count, plan);
        }
}

/*
        The quicksort half. It recurses into the smaller partition and loops
        on the larger, which bounds the stack at the base two logarithm of the
        count no matter how badly the splits go -- the same reason the depth
        counter is about work and not about safety.
*/
static fn stdlib_sort_range(p8 address_to base, positive count,
                            stdlib_sort_plan address_to plan, positive budget)
{
        positive size = plan->size;

        while (count > STDLIB_SORT_SMALL)
        {
                p8 address_to low;
                p8 address_to high;
                p8 address_to middle;
                p8 address_to pivot;
                p8 address_to left;
                p8 address_to right;
                positive taken;

                if (budget == 0)
                {
                        stdlib_sort_heap(base, count, plan);
                        return;
                }

                budget--;

                low = base;
                high = base + (count - 1) * size;
                middle = base + (count / 2) * size;

                if (count > STDLIB_SORT_NINTHER)
                {
                        positive step = (count / 8) * size;

                        pivot = stdlib_sort_median(
                                stdlib_sort_median(low, low + step,
                                                   low + step * 2, plan),
                                stdlib_sort_median(middle - step, middle,
                                                   middle + step, plan),
                                stdlib_sort_median(high - step * 2, high - step,
                                                   high, plan),
                                plan);
                }
                else
                {
                        pivot = stdlib_sort_median(low, middle, high, plan);
                }

                //      The pivot is parked at the front, where the right scan
                //      finds it and stops. Nothing stands at the other end,
                //      which is what the bound in the left scan is for.
                memory_exchange_apart(pivot, low, size);
                pivot = low;

                left = low;
                right = high + size;

                for (;;)
                {
                        do
                                left += size;
                        while (left <= high && stdlib_sort_before(left, pivot, plan));

                        do
                                right -= size;
                        while (stdlib_sort_before(pivot, right, plan));

                        if (left >= right)
                                break;

                        memory_exchange_apart(left, right, size);
                }

                memory_exchange_apart(low, right, size);

                taken = (positive)(right - base) / size;

                if (taken < count - taken - 1)
                {
                        stdlib_sort_range(base, taken, plan, budget);
                        base = right + size;
                        count = count - taken - 1;
                }
                else
                {
                        stdlib_sort_range(right + size, count - taken - 1, plan,
                                          budget);
                        count = taken;
                }
        }

        stdlib_sort_insertion(base, count, plan);
}

fn qsort_r(address_any base, positive count, positive size,
           stdlib_compare_context compare, address_any context)
{
        stdlib_sort_plan plan;
        positive budget;

        if (is_null(base) || is_null(compare) || size == 0 || count < 2)
                return;

        plan.compare = compare;
        plan.context = context;
        plan.size = size;

        //      Inline bsr/clz; the RISC-V floor owns its multi-step search.
#if RISCV64
        budget = 63 - (positive)bits_leading_zeros(count);
#else
        budget = (positive)top_bit_known(count);
#endif

        stdlib_sort_range((p8 address_to)base, count, address_of plan, budget * 2);
}

/*
        qsort_r's comparator takes a context and qsort's does not. Holding the
        plain pointer in that context keeps both function types honest; the
        forwarder is one load and a tail call on all three architectures.
*/
typedef struct
{
        stdlib_compare plain;
} stdlib_compare_holder;

static b32 stdlib_compare_forward(address_any left, address_any right,
                                  address_any context)
{
        return ((stdlib_compare_holder address_to)context)->plain(left, right);
}

fn qsort(address_any base, positive count, positive size, stdlib_compare compare)
{
        stdlib_compare_holder holder;

        //      qsort_r refuses a null comparator, and passing the cast one
        //      straight down used to be how that refusal was reached. The
        //      forwarder is never null, so the same guard has to be here or
        //      a null comparator becomes a call through null instead of a
        //      sort that declines. The test that caught this passes exactly
        //      that, together with a zero count, a zero width and a null
        //      base, and it is the only reason this line exists.
        if (is_null(compare))
                return;

        holder.plain = compare;

        qsort_r(base, count, size, stdlib_compare_forward, address_of holder);
}

/*
        bsearch, which compares key against element and never the other way
        round. The order matters: a comparator written for qsort is often
        asymmetric in what it accepts, and the C standard fixes the key as the
        left argument.

        The narrowing is by half of the remaining count rather than by a
        midpoint index, so nothing here can overflow on a table larger than
        half the address space, and the empty table falls out of the loop
        condition rather than needing a case of its own.
*/
address_any bsearch(address_any key, address_any base, positive count,
                    positive size, stdlib_compare compare)
{
        p8 address_to table = (p8 address_to)base;

        if (is_null(key) || is_null(base) || is_null(compare) || size == 0)
                return null;

        while (count > 0)
        {
                positive half = count / 2;
                p8 address_to middle = table + half * size;
                b32 order = compare(key, middle);

                if (order == 0)
                        return middle;

                if (order > 0)
                {
                        table = middle + size;
                        count = count - half - 1;
                        continue;
                }

                count = half;
        }

        return null;
}

/*
        Division that keeps both halves.

        C guarantees the quotient truncates toward zero and that the remainder
        has the sign of the numerator, and both of those have been true of the
        language's own operators since C99, so the whole of this is one divide
        instruction on every architecture here -- the compiler emits the pair
        from a single division on x86_64, and a divide followed by a multiply
        and subtract on arm64 and riscv64, which have no remainder instruction.
        Writing it by hand would produce the same instructions and would have
        to be written three times to say so.
*/
#define STDLIB_DIVIDE(name, type, result)                                    \
        CONST result name(type numerator, type denominator)                  \
        {                                                                    \
                result answer = {numerator / denominator,                    \
                                 numerator % denominator};                   \
                return answer;                                               \
        }

STDLIB_DIVIDE(div, b32, div_t)
STDLIB_DIVIDE(ldiv, b64, ldiv_t)
STDLIB_DIVIDE(lldiv, b64, lldiv_t)
#undef STDLIB_DIVIDE

/*
        rand, and the decision to be bit for bit what glibc is.

        glibc's rand and random are the same generator -- measured, not
        assumed: srand(n) and srandom(n) produce identical sequences for every
        seed tried, and an unseeded random() produces the sequence srandom(1)
        produces. So one implementation carries all four names, and the
        verification is not "it looks random" but "it is the same numbers",
        which is a test that either passes or points at a line.

        The generator is an additive feedback one: thirty one words of state,
        the word three ahead added into the word being read, and the sum's top
        thirty one bits handed back. Its period is 2^31 - 1 times 2^30 or
        thereabouts, which is enormous, and its low bits are the ones an
        additive generator is weakest in, which is why the answer is the sum
        shifted right rather than the sum masked.

        The seeding is the Park-Miller multiplicative generator run thirty
        times to fill the state, written with Schrage's factoring so the
        intermediate never leaves thirty one bits -- 16807 times a value up to
        2^31 would need 46 bits, and the original this reproduces was written
        for a machine that did not have them. Then three hundred and ten
        values are drawn and thrown away, which is ten times the state size,
        so the first number a caller sees does not carry the shape of the
        seed.

        None of this is cryptographic and none of it should be used as if it
        were. A program that needs unpredictable bytes wants getrandom, which
        is a syscall and not this.
*/
#define STDLIB_RANDOM_DEGREE 31
#define STDLIB_RANDOM_SEPARATION 3
#define STDLIB_RANDOM_WARMUP (STDLIB_RANDOM_DEGREE * 10)

#define STDLIB_RANDOM_MODULUS 2147483647
#define STDLIB_RANDOM_MULTIPLIER 16807
#define STDLIB_RANDOM_QUOTIENT 127773
#define STDLIB_RANDOM_REMAINDER 2836

/* The state after srandom(1)'s 310 warm-up draws.  C requires no particular
   default seed; glibc specifies one, and it is seed one.  Keeping that exact
   post-warm state in the image makes the first draw identical without making
   every later draw test whether the first one has happened.  Regenerating
   these words through srandom(1) produces the same array and leaves the two
   indices at 3 and 0 because 310 is ten complete turns of the ring. */
static b32 stdlib_random_state[STDLIB_RANDOM_DEGREE] = {
        -1726662223, 379960547, 1735697613, 1040273694, 1313901226,
        1627687941, -179304937, -2073333483, 1780058412, -1989503057,
        -615974602, 344556628, 939512070, -1249116260, 1507946756,
        -812545463, 154635395, 1388815473, -1926676823, 525320961,
        -1009028674, 968117788, -123449607, 1284210865, 435012392,
        -2017506339, -911064859, -370259173, 1132637927, 1398500161,
        -205601318};
static positive stdlib_random_front = STDLIB_RANDOM_SEPARATION;
static positive stdlib_random_rear = 0;

b32 random(void)
{
        p32 sum = (p32)stdlib_random_state[stdlib_random_front] +
                  (p32)stdlib_random_state[stdlib_random_rear];

        stdlib_random_state[stdlib_random_front] = (b32)sum;

        stdlib_random_front++;

        if (stdlib_random_front == STDLIB_RANDOM_DEGREE)
                stdlib_random_front = 0;

        stdlib_random_rear++;

        if (stdlib_random_rear == STDLIB_RANDOM_DEGREE)
                stdlib_random_rear = 0;

        return (b32)(sum >> 1);
}

fn srandom(p32 seed)
{
        positive index;

        //      Zero would make the multiplicative seeding produce nothing but
        //      zero forever, so it is folded onto one, which is why srand(0)
        //      and srand(1) give the same sequence in every libc.
        if (seed == 0)
                seed = 1;

        stdlib_random_state[0] = (b32)seed;

        for (index = 1; index < STDLIB_RANDOM_DEGREE; index++)
        {
                b64 previous = stdlib_random_state[index - 1];
                b64 high = previous / STDLIB_RANDOM_QUOTIENT;
                b64 low = previous % STDLIB_RANDOM_QUOTIENT;
                b64 word = STDLIB_RANDOM_MULTIPLIER * low -
                           STDLIB_RANDOM_REMAINDER * high;

                if (word < 0)
                        word += STDLIB_RANDOM_MODULUS;

                stdlib_random_state[index] = (b32)word;
        }

        stdlib_random_front = STDLIB_RANDOM_SEPARATION;
        stdlib_random_rear = 0;
        for (index = 0; index < STDLIB_RANDOM_WARMUP; index++)
                random();
}

fn srand(p32 seed) __attribute__((alias("srandom")));
b32 rand(void) __attribute__((alias("random")));

/*
        rand_r, which is a different generator entirely and has to be.

        It has no state of its own, so it cannot be the one above: the caller
        holds a single thirty two bit word and that is the whole of what it
        gets to remember. The answer is built out of three steps of a linear
        congruential generator, eleven bits then ten then ten, taken from the
        middle of each step because the low bits of an LCG with a power of two
        modulus cycle with a period as short as two. This is the sequence
        glibc produces, checked against it.
*/
b32 rand_r(p32 address_to seed)
{
        p32 next = *seed;
        b32 answer;

        next = next * 1103515245 + 12345;
        answer = (b32)((next / 65536) % 2048);

        next = next * 1103515245 + 12345;
        answer <<= 10;
        answer ^= (b32)((next / 65536) % 1024);

        next = next * 1103515245 + 12345;
        answer <<= 10;
        answer ^= (b32)((next / 65536) % 1024);

        *seed = next;

        return answer;
}

/*
        system, which is worth having and is not what POSIX describes.

        What it does have is the shape callers rely on: a shell is started on
        the command, the caller waits for it, and the raw wait status comes
        back, which wait_status_code turns into the number a shell would have
        reported. A null command asks whether there is a shell at all, and the
        answer is taken from whether /bin/sh can be executed rather than
        assumed.

        There is no fork syscall on arm64 or riscv64 -- the asm-generic table
        never had one -- so the child comes from clone with SIGCHLD and no new
        stack, which is what fork is underneath and what every other spawn in
        this tree already calls. With every argument but the flags zero, the
        two argument orders the architectures disagree about cannot be told
        apart.

        What is missing against POSIX is the signal handling: system is
        supposed to ignore SIGINT and SIGQUIT in the parent for the duration
        and block SIGCHLD, so that interrupting the child does not also
        interrupt the caller. Installing a disposition and restoring it needs
        the sigaction round trip that abort above went out of its way not to
        need, and doing it half way would be worse than not doing it. A caller
        that cares should spawn the child itself.
*/
#define STDLIB_SHELL "/bin/sh"

//      access(2)'s X_OK, which is not open(2)'s FILE_EXECUTE.
#define STDLIB_ACCESS_EXECUTE 1

b32 system(string_address command)
{
        string_address words[4];
        bipolar child;
        positive raw = 0;

        if (is_null(command))
        {
                //      Is the shell there and runnable. The number is
                //      access(2)'s own X_OK and not FILE_EXECUTE, which is
                //      open(2)'s flag of the same name and a different value:
                //      asking faccessat for 010 asks about a permission bit
                //      that does not exist and is answered EINVAL. library.c
                //      has the same warning beside memory() about the mmap
                //      flags, and it is the same mistake.
                return system_access_at(AT_FDCWD, STDLIB_SHELL,
                                        STDLIB_ACCESS_EXECUTE) == 0;
        }

        words[0] = (string_address)STDLIB_SHELL;
        words[1] = (string_address) "-c";
        words[2] = command;
        words[3] = null;

        //      Anything this process has buffered belongs to this process.
        //      The child would inherit the buffer and write it out a second
        //      time, so it is written out once, here, before the fork.
        log_flush();

        child = system_fork();

        if (child < 0)
                return -1;

        if (child == 0)
        {
                system_execute((address_any)STDLIB_SHELL, (address_any)words,
                               (address_any)stdlib_environment_list());

                //      execve only returns when it failed, and the shell's
                //      own answer for a command it could not run is 127.
                exit(127);
        }

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
                return -1;

        return (b32)raw;
}

/*
        The C spelling of exit.

        Function-like on purpose, so it expands only where exit is being
        called: the assembly symbol of the same name keeps its address, a bare
        exit still takes it, and every call written above this line in the
        umbrella's include order still means the trap -- error.c's _exit and
        _Exit, abort and quick_exit above, and the execve child inside
        system(), each of which has to mean the trap and nothing else.

        STANDARD_EXIT_KEEPS_TRAP is the way back out, for a caller who wants
        the bare trap under this spelling. Nothing in this tree defines it;
        the guard in stdlib_exit is what made needing it unnecessary.
*/
#ifndef STANDARD_EXIT_KEEPS_TRAP
#define exit(code) stdlib_exit(code)
#endif

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_STDLIB
#endif // STANDARD_SKIP_STDLIB

#ifndef STANDARD_SKIP_CLOCK
/* ---- clock.c ---- */
/*
        Experimental C standard library

        <time.h>: a clock read from the kernel, and a calendar with no tables

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_CLOCK
#define STANDARD_MODERN_C_STANDARD_CLOCK

/*
        Two builds this family deliberately does not join.

        A kernel build already has a calendar and already has these names:
        <linux/time.h> declares struct tm and time64_to_tm and mktime64, and a
        second struct tm in the same translation unit is not a conflict of
        opinion, it is a compile error. A module also has no business reaching
        for clock_gettime through a syscall trap -- ktime_get is what it wants
        -- so there is nothing here it could use even if the names were free.

        A no-platform build has had platform/syscall.inc compiled out from
        under it, so syscall(clock_gettime) is not a number and there is no
        kernel to ask the time of. The calendar arithmetic would still be
        valid there, but half a <time.h> that cannot say what o'clock it is
        would be a worse thing to offer than none.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        This is ordinary C on purpose.

        library.c and everything it includes holds declarations and assembly
        and nothing else, which is checked. Almost nothing here is a floor a
        machine could do better: the whole of <time.h> below the two syscalls
        is integer arithmetic over a calendar that Rome and then Pope Gregory
        chose, and no processor has an instruction for either of them. Writing
        it once as C is what lets the same bytes be tested on all three
        machines instead of being written three times and drifting.

        What it does need from the library is small and already there:
        system_call_2 for the two clock traps, memory_copy_apart for the
        pieces strftime assembles, positive_into_padded for a zero-filled
        field, and string_length. Nothing here calls printf or snprintf, so
        this file can be merged before or after the formatting family and in
        either order with the rest.

        Timezones, said plainly: this system has no zoneinfo, nothing reads
        /etc/localtime, and localtime is gmtime. Every broken-down time this
        file produces carries tm_isdst zero, tm_gmtoff zero, and tm_zone
        "UTC", because that is what it actually computed -- not because the
        machine happens to be in London. A program that needs a real local
        time will need a TZif parser; until one exists the local spellings
        share the UTC entries below.
*/

/*
        The clock identifiers the kernel takes in its first argument. These
        are the same three numbers on x86_64, arm64 and riscv64 because they
        are not a syscall number, they are a kernel-wide enumeration.

        Guarded one at a time because src/sh/system.c and src/sh/net.c each
        define a CLOCK_MONOTONIC for themselves and include this umbrella; an
        identical redefinition is legal but only if the spelling matches, and
        a guard is cheaper than requiring it to.
*/
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif

#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

/*
        clock() counts in these, and the number is a fiction the standard
        fixed at a million on POSIX. It is not the resolution of anything.
*/
#define CLOCKS_PER_SEC ((clock_t)1000000)

#define CLOCK_SECONDS_PER_DAY 86400

typedef bipolar time_t;
typedef bipolar clock_t;
typedef bipolar suseconds_t;
typedef b32 clockid_t;

/*
        gettimeofday's pair, which is the older of the two shapes and the one
        BSD left behind. Signed, both fields, unlike library.c's timespec:
        that one is a duration handed to nanosleep and never negative, this
        one is a point on a line that started in 1970 and has an outside.
*/
typedef struct timeval
{
        b64 tv_sec;
        b64 tv_usec;
} timeval;

/*
        The broken-down time, in the layout every program that has ever read
        a struct tm expects, including the two fields that are not in C but
        are in glibc and in BSD and are therefore in practice mandatory.

        The field names are not prose and will not become prose. They are the
        names a caller writes, they are fixed by thirty years of source, and
        renaming them would mean this structure is not struct tm any more.

        tm_year counts from 1900 and tm_mon from zero, which are the two
        traps in the whole of <time.h>. tm_yday is zero for the first of
        January. tm_isdst is always zero here; see the note about zoneinfo.
*/
typedef struct tm
{
        b32 tm_sec;
        b32 tm_min;
        b32 tm_hour;
        b32 tm_mday;
        b32 tm_mon;
        b32 tm_year;
        b32 tm_wday;
        b32 tm_yday;
        b32 tm_isdst;
        b64 tm_gmtoff;
        const char address_to tm_zone;
} tm;

static const char address_to clock_zone_name = "UTC";

static const char address_to clock_weekday_short[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static const char address_to clock_weekday_long[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"};
static const p8 clock_weekday_long_length[7] = {6, 6, 7, 9, 8, 6, 8};

static const char address_to clock_month_short[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char address_to clock_month_long[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
static const p8 clock_month_long_length[12] = {
        7, 8, 5, 5, 3, 4, 4, 6, 9, 7, 8, 8};

/*
        The two halves of a twelve hour clock. A table rather than a pair of
        literals inside %p, because strptime has to match the same two words
        the way it matches a weekday name -- longest first and without regard
        to case -- and two spellings of the same two words would be two places
        to get them wrong. %P prints these folded to lower case rather than
        keeping a second table beside this one.
*/
static const char address_to clock_half_day[2] = {"AM", "PM"};

/*
        What glibc prints where the index is out of range, rather than reading
        past the end of its own table. A hand-filled struct tm with tm_wday of
        nine is not an error strftime is allowed to report, so it prints this.
*/
static const char address_to clock_unknown_name = "?";

/*
        Division that rounds toward minus infinity, which is what a calendar
        means by "which day is this second in" and is not what C's / does.

        C truncates toward zero, so -1 / 86400 is 0 and the second before the
        epoch lands in the same day as the second after it. Every date before
        1970 depends on this correction and nothing after 1970 ever reaches
        it, which is exactly the shape of bug that survives a test suite that
        only looks at the present.
*/
static bipolar clock_floor_divide(bipolar value, bipolar divisor)
{
        bipolar quotient = value / divisor;

        if (value % divisor != 0 && (value < 0) != (divisor < 0))
                quotient--;

        return quotient;
}

/*
        The calendar, closed form, no table of month lengths and no loop over
        years. This is Howard Hinnant's days_from_civil and civil_from_days,
        from "chrono-Compatible Low-Level Date Algorithms",
        howardhinnant.github.io/date_algorithms.html, which he placed in the
        public domain and which is the arithmetic underneath C++'s
        <chrono> calendar. It is transcribed here rather than reinvented
        because a proof exists for it over the whole range of a 64-bit day
        count and would not exist for a second attempt at the same idea.

        The trick, and the only thing worth understanding to read the rest:
        the internal year begins on the first of March, not the first of
        January. That moves the leap day to the very end of the year, where
        it stops being an insertion the arithmetic has to step over, and it
        makes the twelve month lengths from March round to February a single
        almost regular sequence -- 31 30 31 30 31 31 30 31 30 31 31
        28 -- whose running total is exactly (153 * month + 2) / 5. That one
        expression is the whole of the month table. The +-3/+9 and the two
        `month <= 2` adjustments are just the shift into and out of that
        March-first year.

        The constants:

            146097  days in four hundred years, which is the length of the
                    Gregorian cycle: 400 * 365 + 97 leap days, since a year
                    divisible by 4 is a leap year unless it is divisible by
                    100 unless it is divisible by 400. An "era" below is one
                    of these four-century blocks.
             36524  days in one hundred years, 100 * 365 + 24
              1460  days in four years without the century rule, 4 * 365
            719468  days from 0000-03-01 to 1970-01-01. The epoch is not the
                    origin of the arithmetic; the start of an era is, and
                    this is the offset between them.

        Both directions are branch-free apart from the sign guards, which
        exist for the same reason clock_floor_divide does: era is a floored
        quotient and C's / is not, so the negative side is nudged down by one
        divisor's worth before the truncation happens.

        Preconditions, from the source: month is 1 through 12, day is 1
        through the length of that month, and year is the proleptic Gregorian
        year -- the Gregorian rules run backwards through 1582 and through
        zero, with year 0 being 1 BC and being a leap year. Both directions
        are exact for any year that fits in a signed 64-bit value, which is
        very much more than any caller here will ask for.
*/
static bipolar clock_days_from_civil(bipolar year, bipolar month, bipolar day)
{
        bipolar era;
        bipolar year_of_era;
        bipolar day_of_year;
        bipolar day_of_era;

        year -= month <= 2;

        era = (year >= 0 ? year : year - 399) / 400;
        year_of_era = year - era * 400;
        day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                     day_of_year;

        return era * 146097 + day_of_era - 719468;
}

static fn clock_civil_from_days(bipolar days, bipolar address_to year,
                                bipolar address_to month,
                                bipolar address_to day)
{
        bipolar era;
        bipolar day_of_era;
        bipolar year_of_era;
        bipolar shifted_year;
        bipolar day_of_year;
        bipolar shifted_month;
        bipolar civil_month;

        days += 719468;

        era = (days >= 0 ? days : days - 146096) / 146097;
        day_of_era = days - era * 146097;
        year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                       day_of_era / 146096) /
                      365;
        shifted_year = year_of_era + era * 400;
        day_of_year = day_of_era -
                      (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        shifted_month = (5 * day_of_year + 2) / 153;

        civil_month = shifted_month + (shifted_month < 10 ? 3 : -9);

        address_to day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
        address_to month = civil_month;
        address_to year = shifted_year + (civil_month <= 2);
}

/*
        The day of the week, straight from the day count and with no calendar
        in the way. 1970-01-01 was a Thursday, so day zero is weekday four,
        and the second form is C's truncating remainder corrected for the days
        before day minus four rather than a separate floored modulo.
*/
static bipolar clock_weekday_from_days(bipolar days)
{
        return days >= -4 ? (days + 4) % 7 : (days + 5) % 7 + 6;
}

/*
        The whole of gmtime, once the calendar exists.

        Returns false when the year does not fit in tm_year, which is an int
        and therefore runs out around the year two billion while a 64-bit
        time_t does not run out until the year three hundred billion. glibc
        answers a null pointer and EOVERFLOW there; this sets the same error
        and its callers turn false into their failure result.
*/
static bool clock_break_down(bipolar seconds, tm address_to broken)
{
        bipolar days = clock_floor_divide(seconds, CLOCK_SECONDS_PER_DAY);
        bipolar rest = seconds % CLOCK_SECONDS_PER_DAY;
        bipolar year;
        bipolar month;
        bipolar day;
        bipolar year_field;

        if (rest < 0)
                rest += CLOCK_SECONDS_PER_DAY;

        clock_civil_from_days(days, address_of year, address_of month,
                              address_of day);

        year_field = year - 1900;

        if (year_field > 2147483647 || year_field < -2147483647 - 1)
        {
                errno = EOVERFLOW;
                return false;
        }

        broken->tm_sec = (b32)(rest % 60);
        broken->tm_min = (b32)((rest / 60) % 60);
        broken->tm_hour = (b32)(rest / 3600);
        broken->tm_mday = (b32)day;
        broken->tm_mon = (b32)(month - 1);
        broken->tm_year = (b32)year_field;
        broken->tm_wday = (b32)clock_weekday_from_days(days);
        broken->tm_yday = (b32)(days - clock_days_from_civil(year, 1, 1));
        broken->tm_isdst = 0;
        broken->tm_gmtoff = 0;
        broken->tm_zone = clock_zone_name;

        return true;
}

/*
        The two traps. Both take a pointer the kernel fills in, both answer
        zero or a negative errno, and neither exists under a different number
        on the three machines: riscv64 shares arm64's asm-generic table, which
        is why syscall(clock_gettime) resolves everywhere.

        There is deliberately no use of a `time` syscall. x86_64 has one,
        number 201, and arm64 and riscv64 have never had one -- it is one of
        the calls the asm-generic ABI dropped because clock_gettime already
        answers it. Building time() on the trap that exists on one machine out
        of three is how a family passes its own test and fails on the others.
*/
b32 clock_gettime(clockid_t which, timespec address_to into)
{
        return error_result((bipolar)system_call_2(
            syscall(clock_gettime), (positive)which, (positive)into));
}

/*
        The seconds field of library.c's timespec is p64 and therefore
        unsigned, because that structure's job in this tree so far has been to
        carry a sleep duration into nanosleep and a duration is never
        negative. A wall clock reading is not a duration: it has an outside,
        and every second before 1970 is negative in it. So every read is cast
        to bipolar at the boundary, here, once, rather than by each caller --
        an unsigned second sliding into clock_break_down would make the
        `days >= 0` guard vacuously true and quietly hand back a date in the
        year 584 billion for every timestamp before the epoch.
*/
static bipolar clock_now(b32 which)
{
        timespec stamp = {0, 0};

        if (clock_gettime(which, address_of stamp) < 0)
                return -1;

        return (bipolar)stamp.tv_sec;
}

// The number of seconds since 1970-01-01 00:00:00 UTC, ignoring leap seconds
// as POSIX requires. The argument may be null, and is written when it is not.
time_t time(time_t address_to into)
{
        time_t now = (time_t)clock_now(CLOCK_REALTIME);

        if (!is_null(into))
                address_to into = now;

        return now;
}

/*
        Processor time this process has burned, in CLOCKS_PER_SEC units.

        Not get_cpu_time, which is the machine's free-running cycle counter
        and answers a different question: that one counts wall time in units
        nobody has calibrated, this one counts the time the scheduler actually
        gave this process and stops while it is blocked. A program measuring
        itself wants this; a program measuring an instruction sequence wants
        get_cpu_time.
*/
clock_t clock(void)
{
        timespec stamp = {0, 0};

        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, address_of stamp) < 0)
                return (clock_t)-1;

        return (clock_t)stamp.tv_sec * CLOCKS_PER_SEC +
               (clock_t)(stamp.tv_nsec / 1000);
}

/* One monotonic nanosecond clock for every polling/backoff state machine in
   the combined shell.  A failed clock safely reads as zero for all of them. */
static HOT positive clock_monotonic_nanoseconds()
{
        timespec now = {0, 0};

        if (clock_gettime(CLOCK_MONOTONIC, address_of now) < 0)
                return 0;

        return (positive)now.tv_sec * 1000000000 + (positive)now.tv_nsec;
}

b32 clock_getres(clockid_t which, timespec address_to into)
{
        return error_result((bipolar)system_call_2(
            syscall(clock_getres), (positive)which, (positive)into));
}

/*
        The older shape, built on the newer trap rather than on its own.

        gettimeofday has its own syscall on all three machines, but it answers
        a coarser reading of the same clock and needs its own structure laid
        out for it; going through clock_gettime and dividing is one division
        and keeps one path. The second argument is the vestigial timezone
        pointer, which POSIX marked obsolete and which is ignored here as it
        is everywhere else.
*/
b32 gettimeofday(timeval address_to into, address_any zone)
{
        timespec stamp = {0, 0};

        (void)zone;

        if (clock_gettime(CLOCK_REALTIME, address_of stamp) < 0)
                return -1;

        if (!is_null(into))
        {
                into->tv_sec = (b64)stamp.tv_sec;
                into->tv_usec = (b64)(stamp.tv_nsec / 1000);
        }

        return 0;
}

// Seconds between two points, as a decimal because the standard says so and
// because the difference of two 64-bit times does not always fit in one.
CONST decimal difftime(time_t later, time_t earlier)
{
        // Subtract before rounding. Unsigned magnitudes cover the full
        // signed time_t span without overflowing or losing nearby seconds.
        return later >= earlier
                   ? (decimal)((p64)later - (p64)earlier)
                   : -(decimal)((p64)earlier - (p64)later);
}

/*
        The way back: a broken-down time to a count of seconds, and the
        broken-down time normalised in place while we are there.

        Normalising is not a nicety, it is most of what mktime is for. A
        caller that wants "thirty days from now" adds thirty to tm_mday and
        calls this, and a caller reading a date out of a file hands over
        whatever was in the file. So nothing here checks a range: the month is
        floored into a year and a remainder, the day of the month is added to
        the first of that month as a plain offset and is allowed to be zero or
        negative or four hundred, and the hours, minutes and seconds are
        multiplied out and added without a care for whether any of them is
        under sixty. Every one of those out-of-range values lands on the right
        second, and clock_break_down then writes the in-range spelling of that
        second back through the caller's pointer along with tm_wday and
        tm_yday, which is what the standard requires and what a round-trip
        test through gmtime can never see, because gmtime only ever hands back
        a structure that was already normal.

        tm_isdst is ignored rather than consulted. There is no daylight saving
        without a timezone database, so there is no ambiguous hour to resolve.
*/
time_t timegm(tm address_to broken)
{
        bipolar year;
        bipolar month;
        bipolar carried;
        bipolar days;
        bipolar seconds;

        if (is_null(broken))
                return (time_t)-1;

        year = (bipolar)broken->tm_year + 1900;
        month = (bipolar)broken->tm_mon;
        carried = clock_floor_divide(month, 12);
        year += carried;
        month -= carried * 12;

        days = clock_days_from_civil(year, month + 1, 1) +
               (bipolar)broken->tm_mday - 1;

        seconds = days * CLOCK_SECONDS_PER_DAY +
                  (bipolar)broken->tm_hour * 3600 +
                  (bipolar)broken->tm_min * 60 + (bipolar)broken->tm_sec;

        if (!clock_break_down(seconds, broken))
                return (time_t)-1;

        return (time_t)seconds;
}

time_t mktime(tm address_to broken) __attribute__((alias("timegm")));

tm address_to gmtime_r(const time_t address_to stamp, tm address_to into)
{
        if (is_null(stamp) || is_null(into))
                return null;

        if (!clock_break_down((bipolar)(address_to stamp), into))
                return null;

        return into;
}

/*
        The static one structure that gmtime and localtime share, which is
        what glibc does too and what makes both of them unusable from two
        threads at once. The _r forms above and below exist for that reason
        and are the ones anything long-lived should call.
*/
static tm clock_broken_shared;

tm address_to gmtime(const time_t address_to stamp)
{
        return gmtime_r(stamp, address_of clock_broken_shared);
}

//      Without a timezone database, the local reentrant spelling is UTC too.
tm address_to localtime_r(const time_t address_to stamp, tm address_to into)
        __attribute__((alias("gmtime_r")));

tm address_to localtime(const time_t address_to stamp)
        __attribute__((alias("gmtime")));

/*
        Two shapes of decimal field, which between them are every number this
        file prints.

        clock_number_precision is printf's "%.Nd": the sign first, then the
        magnitude zero-filled to at least N digits, so -5 at two digits is
        "-05". clock_number_field is printf's "%Nd": the number as it is, then
        spaces on the left until it is N wide, so -5 at three wide is " -5".
        Neither ever truncates. Both write no terminator and return the count,
        which is what positive_into_padded underneath them does.
*/
static positive clock_number_precision(p8 address_to into, bipolar value,
                                       positive least)
{
        positive length = 0;
        positive magnitude;

        if (value < 0)
        {
                into[0] = '-';
                length = 1;
                magnitude = (positive)(-(value + 1)) + 1;
        }
        else
                magnitude = (positive)value;

        return length +
               positive_into_padded(into + length, magnitude, least, '0');
}

static positive clock_number_field(p8 address_to into, bipolar value,
                                   positive width)
{
        p8 body[32];
        positive length = bipolar_into(body, value);
        positive at = width > length ? width - length : 0;

        memory_fill(into, ' ', at);
        memory_copy_apart(into + at, body, length);

        return at + length;
}

/*
        asctime, which is the oldest thing in <time.h> and the only one with a
        buffer size written into its contract: twenty six bytes, of which
        twenty five are the line

            Thu Jan  1 00:00:00 1970\n

        and the twenty sixth is the terminator. Nothing about that is
        adjustable, and a caller cannot say the buffer is bigger, so the only
        thing to do with a broken-down time that does not fit in twenty five
        bytes is refuse it. A year of five digits does that, and so does a day
        of the month in the hundreds; a year before the common era does not,
        because "Fri Dec 31 23:59:59 -1" is twenty two bytes and glibc prints
        it, so this prints it too.

        The line is assembled somewhere else first and only copied over once
        its length is known, which is what makes the refusal safe: measuring
        it in the caller's twenty six bytes would mean having already written
        past them.
*/
p8 address_to asctime_r(const tm address_to broken, p8 address_to into)
{
        p8 line[128];
        positive at = 0;

        if (is_null(broken) || is_null(into))
                return null;

        /*
                The two fields that index a table are the two that are checked,
                because a wrong one there is a read past the end of the table
                rather than a wrong answer. Everything else is printed as it
                stands and is caught, if it is absurd, by the length test at
                the bottom -- which is exactly the order glibc does it in.
        */
        if (broken->tm_wday < 0 || broken->tm_wday > 6 || broken->tm_mon < 0 ||
            broken->tm_mon > 11)
                return null;

        memory_copy_apart(line + at,
                          (address_any)clock_weekday_short[broken->tm_wday], 3);
        at += 3;
        line[at++] = ' ';
        memory_copy_apart(line + at,
                          (address_any)clock_month_short[broken->tm_mon], 3);
        at += 3;
        at += clock_number_field(line + at, broken->tm_mday, 3);
        line[at++] = ' ';
        at += clock_number_precision(line + at, broken->tm_hour, 2);
        line[at++] = ':';
        at += clock_number_precision(line + at, broken->tm_min, 2);
        line[at++] = ':';
        at += clock_number_precision(line + at, broken->tm_sec, 2);
        line[at++] = ' ';
        at += clock_number_precision(line + at,
                                     (bipolar)broken->tm_year + 1900, 1);
        line[at++] = '\n';

        /*
                Twenty five bytes and a terminator is the whole of the buffer
                this routine is allowed, and a year of five digits or a day of
                the month in the hundreds is how a caller runs out of it.
                Answering a null pointer is what glibc does there, and it is
                the only answer available: the contract names the size.
        */
        if (at > 25)
                return null;

        memory_copy_apart(into, line, at);
        into[at] = end;

        return into;
}

static p8 clock_asctime_shared[32];

p8 address_to asctime(const tm address_to broken)
{
        return asctime_r(broken, clock_asctime_shared);
}

p8 address_to ctime_r(const time_t address_to stamp, p8 address_to into)
{
        tm broken;

        if (is_null(localtime_r(stamp, address_of broken)))
                return null;

        return asctime_r(address_of broken, into);
}

p8 address_to ctime(const time_t address_to stamp)
{
        return ctime_r(stamp, clock_asctime_shared);
}

/*
        strftime.

        The awkward half is not the specifiers, it is the return value, and it
        is the half every hand-written strftime gets wrong. The contract:

          - the answer is the number of bytes written, not counting the
            terminator, which is written;
          - if the whole result plus its terminator will not fit in max, the
            answer is zero and the contents of the buffer are unspecified;
          - max of zero writes nothing at all, not even a terminator, and the
            pointer is allowed to be null.

        Which means the size check cannot be "did I run out while copying",
        because a result that exactly fills max has run out -- there is no room
        for the terminator -- and must still not write a byte past max. So the
        cursor below never advances past max, every append refuses rather than
        truncates, and the terminator is written once at the end only after
        used + 1 has been checked against max. A test that only feeds it a
        generous buffer reaches none of this, so the suite sweeps max from zero
        to past the answer for a format containing every specifier.

        The compound specifiers -- %c %x %X %D %F %R %T %r -- are their own
        format string handed back to the same loop, which is what glibc does
        and is why they cannot drift from the pieces they are made of. None of
        them contains a compound specifier, so the recursion is one deep.

        The C locale is the only locale. There is no LC_TIME here to read, so
        the four name tables above are the answer, and %E and %O -- which ask
        a locale for an alternative era and for alternative digits -- have
        nothing to select and print what the unmodified specifier prints.

        A directive is

            %  flags  width  modifier  specifier

        in that order and in no other. Everything but the last is optional,
        and the order is not advice: %E5Y is not %5EY. It is a directive whose
        specifier is '5', which nobody knows, so "%E5" comes back out as text
        and the Y behind it is an ordinary letter. glibc does exactly that,
        and so does this.

        The flags, which may repeat and may come in any order:

            -   do not pad the number at all
            _   pad the number with spaces
            0   pad the number with zeros
            ^   upper case the letters of the answer
            #   the other case from the one the answer usually has

        The width is decimal, is a minimum, and never truncates. It is a GNU
        extension that neither C nor POSIX has and that every strftime worth
        calling implements, so a format written for one of those is a format
        this has to read.

        What follows is glibc's algorithm rather than an independent one, down
        to two behaviours surprising enough to name. A differential test
        against glibc is the only reference this family has, and matching it
        on the ordinary cases while inventing an answer for the odd ones would
        make that test worth nothing:

          - a width applies to each piece a directive emits, separately, and
            the number path spends the width it consumed. That is why "%5z" is
            "    +00000" rather than "+0000": the sign is one piece widened to
            five, and the four digits of the offset are a second piece widened
            to five again.
          - a directive nobody knows comes back out as the bytes that were
            consumed -- percent, flags, width, modifier and all -- and it comes
            back out through the same path a weekday name takes, so it is
            widened and case folded on the way. "%5Q" is "  %5Q" and "%^q" is
            "%^Q".

        Which specifiers take which modifier is glibc's table and there is no
        principle in it to derive it from: %Ec is a directive and %Ed is not,
        %OS is one and %ES is not. It is transcribed below rather than
        reasoned about. A modifier the specifier does not take is not an
        error, it is a directive nobody knows, and it comes back out as text.
*/

/*
        The writer, and the decoration the directive being read asked for.

        into may be null only when max is zero, which is the one call the
        standard allows to measure nothing at all. width is -1 when the format
        named none, which is not the same as a named zero: the number path
        tells them apart, and a named zero still spends itself. pad holds the
        flag byte, or zero when no flag was given, because for %e %k and %l
        "no flag" and "the 0 flag" mean opposite things.

        to_upper and to_lower are what ^ resolved to and what # resolved to.
        They cannot both be set: # decides its direction inside the specifier
        that reads it -- upper for the four name tables, lower for %p %P and
        %Z -- and whichever it picks it clears the other. That is why # is
        carried as change_case until then rather than as a fold of its own.
*/
typedef struct clock_format_state
{
        p8 address_to into;
        positive max;
        positive used;
        bool failed;
        bipolar width;
        p8 pad;
        bool to_upper;
        bool to_lower;
        bool change_case;
        bool extensions;
} clock_format_state;

/*
        The floor under all three appends: these bytes, as they are, with no
        width and no fold. It is the only place that decides there is no room,
        so every other writer here is bounded by having gone through it.
*/
static inline INLINE bool clock_format_room(clock_format_state address_to state,
                                            positive length)
{
        if (state->failed)
                return false;

        if (length > state->max || state->used > state->max - length)
        {
                state->failed = true;
                return false;
        }

        state->used += length;
        return true;
}

static fn clock_format_raw(clock_format_state address_to state,
                           address_any bytes, positive length)
{
        positive where = state->used;

        if (clock_format_room(state, length) && length)
                memory_copy_apart(state->into + where, bytes, length);
}

/*
        The same, for one byte repeated, which is what every kind of padding
        in this file is. It is a separate routine and not a loop at the four
        call sites because memory_fill is the assembly: a width of two hundred
        is two hundred bytes of one store, not two hundred stores.
*/
static fn clock_format_run(clock_format_state address_to state, p8 byte,
                           positive count)
{
        positive where = state->used;

        if (clock_format_room(state, count) && count)
                memory_fill(state->into + where, (b8)byte, count);
}

/*
        The width, in front of whatever is about to be written.

        Zeros under the 0 flag and spaces under everything else, the - flag
        included: - turns off the number path's own padding and leaves this
        one running, which is why "%-10d" is eight spaces and then "29" while
        "%10d" is eight zeros and then "29".
*/
static fn clock_format_widen(clock_format_state address_to state,
                             positive length)
{
        if (state->width > 0 && (positive)state->width > length)
                clock_format_run(state, state->pad == '0' ? '0' : ' ',
                                 (positive)state->width - length);
}

/*
        One byte, widened and not folded. The ordinary characters of the
        format take this path with no width set; %n, %t, %% and the sign in
        front of %z take it with whatever width the directive named.
*/
static fn clock_format_byte(clock_format_state address_to state, p8 byte)
{
        clock_format_widen(state, 1);

        /*
                One byte, stored rather than copied.

                clock_format_raw takes its length as a variable, so the
                known-size expansion of memory_copy_apart cannot fold there
                and every literal byte of a format string -- the colons in
                %H:%M:%S, the spaces, every character that is not a directive
                -- paid a call into the general routine to move one byte. The
                bounds test below is exactly raw's, written out because after
                folding the length to one there is nothing else left of it:
                `1 > max || used > max - 1` is `used >= max` for every max,
                including zero. Measured on a pure-literal format, 158 cycles
                to 122.
        */
        if (state->failed)
                return;

        if (state->used >= state->max)
        {
                state->failed = true;
                return;
        }

        state->into[state->used] = byte;
        state->used++;
}

/*
        Bytes, widened and then folded to the case the flags asked for.

        The fold happens in place over the range just written rather than over
        a copy on the way in, which is what lets memory_to_upper_ascii do it
        sixteen or thirty two bytes at a time. Nothing else has that range
        yet, so writing it twice is free and correct. The padding in front is
        deliberately outside the fold: a space is a space in both cases, and
        glibc folds only the body.

        Lower is tested before upper because a specifier that forces lower --
        %P does, always -- has to win over a ^ in the flags, which is the
        order glibc's copy does it in.
*/
static fn clock_format_append(clock_format_state address_to state,
                              address_any bytes, positive length)
{
        positive where;

        clock_format_widen(state, length);

        where = state->used;

        clock_format_raw(state, bytes, length);

        if (state->failed || length == 0)
                return;

        if (state->to_lower)
                memory_to_lower_ascii(state->into + where, length);
        else if (state->to_upper)
                memory_to_upper_ascii(state->into + where, length);
}

/*
        A number: its sign, then its own padding, then the width.

        least is how many digits the specifier has when the format said
        nothing -- two for %d, three for %j, one for the three that name a
        year -- and a wider width replaces it rather than adding to it. The
        padding is the difference between that and the digits there actually
        are, and where it goes is the whole of what the three padding flags
        mean:

            -   nowhere. The width is left unspent for the append below.
            _   spaces, in front of the sign, and they spend that much width.
            0   zeros, behind the sign, and they spend all of it.

        so that -1 at five digits is "-0001" and never "000-1", and the same
        number under _ is "   -1". No flag at all is the third row, which is
        why %d pads with zeros without being asked to.

        positive_into_padded writes the digits and there is no loop here that
        divides by ten; the sign is one byte in front of them, in a buffer
        that starts one byte early so that dropping the sign back off under
        the 0 flag is an index and not a move.
*/
static positive clock_number_text(p8 address_to body, bipolar value,
                                  positive address_to at)
{
        bool negative = value < 0;
        positive magnitude = negative ? (positive)(-(value + 1)) + 1
                                      : (positive)value;
        positive length = positive_into(body + 1, magnitude);

        address_to at = 1;

        if (negative)
        {
                body[0] = '-';
                address_to at = 0;
                length++;
        }

        return length;
}

static fn clock_format_number(clock_format_state address_to state,
                              bipolar value, positive least)
{
        p8 body[40];
        bool negative = value < 0;
        positive at;
        positive length = clock_number_text(body, value, address_of at);

        if (state->width > (bipolar)least)
                least = (positive)state->width;

        if (state->pad != '-' && least > length)
        {
                positive padding = least - length;

                if (state->pad == '_')
                {
                        clock_format_run(state, ' ', padding);

                        state->width = state->width > (bipolar)padding
                                               ? state->width - (bipolar)padding
                                               : 0;
                }
                else
                {
                        if (negative)
                        {
                                clock_format_run(state, '-', 1);
                                at++;
                                length--;
                        }

                        clock_format_run(state, '0', padding);
                        state->width = 0;
                }
        }

        clock_format_append(state, body + at, length);
}

/*
        %e, %k and %l, which pad with spaces rather than zeros unless the
        format said otherwise, and that is the whole of what makes them
        different from %d, %H and %I. "Otherwise" is the 0 flag or the - flag;
        ^ and # do not enter into it.
*/
static fn clock_format_number_spaced(clock_format_state address_to state,
                                     bipolar value, positive least)
{
        if (state->pad != '0' && state->pad != '-')
                state->pad = '_';

        clock_format_number(state, value, least);
}

// Guard and append either calendar-name table with the same out-of-range rule.
static fn clock_format_name(clock_format_state address_to state, b32 index,
                            bool full, b32 count,
                            const char address_to short_names[],
                            const char address_to long_names[],
                            const p8 address_to long_lengths)
{
        if (index < 0 || index >= count)
                return clock_format_append(state, (address_any)clock_unknown_name,
                                           1);

        clock_format_append(state,
                            (address_any)(full ? long_names[index]
                                              : short_names[index]),
                            full ? long_lengths[index] : 3);
}

/*
        The ISO 8601 week-based year and week number, which are a different
        calendar sharing the same days: a week belongs to whichever year its
        Thursday is in, so the first days of January can be week 52 or 53 of
        the year before, and the last days of December can be week 1 of the
        year after.

        Written through the day count rather than through a table of cases,
        because with clock_days_from_civil already here the definition is
        directly executable: find the Thursday of this week, ask which year it
        is in, and count weeks from the first of January of that year.
*/
static fn clock_iso_week(const tm address_to broken, bipolar address_to year,
                         bipolar address_to week)
{
        bipolar days = clock_days_from_civil((bipolar)broken->tm_year + 1900,
                                             (bipolar)broken->tm_mon + 1,
                                             (bipolar)broken->tm_mday);
        bipolar weekday = clock_weekday_from_days(days);
        bipolar thursday = days - (weekday == 0 ? 7 : weekday) + 4;
        bipolar thursday_year;
        bipolar thursday_month;
        bipolar thursday_day;

        clock_civil_from_days(thursday, address_of thursday_year,
                              address_of thursday_month,
                              address_of thursday_day);

        address_to year = thursday_year;
        address_to week =
                (thursday - clock_days_from_civil(thursday_year, 1, 1)) / 7 + 1;
}

/* The composite grammar is shared by strftime and strptime. */
static const char address_to clock_composite_format(p8 which)
{
        switch (which)
        {
        case 'c': return "%a %b %e %H:%M:%S %Y";
        case 'D': case 'x': return "%m/%d/%y";
        case 'F': return "%Y-%m-%d";
        case 'r': return "%I:%M:%S %p";
        case 'R': return "%H:%M";
        case 'T': case 'X': return "%H:%M:%S";
        default: return null;
        }
}

static fn clock_format_core(clock_format_state address_to state,
                            const char address_to format,
                            const tm address_to broken);

/*
        A compound specifier is its own format string, and the decoration
        belongs to the compound and not to each of its pieces: "%^c" upper
        cases the finished line once, and "%40c" pads the finished line once,
        rather than doing either to the weekday and then again to the month
        and again to every number between them. So the pieces are assembled
        undecorated somewhere else first and handed over as one run of bytes,
        which is what glibc does and is the only reading of "%40c" that is not
        absurd.

        Sixty four bytes is the longest any of the seven can be, with a year
        of ten digits in it. The buffer is four times that, and running out of
        it fails the whole call rather than truncating: a truncated compound
        would be a wrong answer wearing the shape of a right one.
*/
static fn clock_format_nested(clock_format_state address_to state,
                              const char address_to format,
                              const tm address_to broken)
{
        p8 body[256];
        clock_format_state inner;

        if (state->failed)
                return;

        inner.into = body;
        inner.max = sizeof(body);
        inner.used = 0;
        inner.failed = false;
        inner.width = -1;
        inner.pad = 0;
        inner.to_upper = false;
        inner.to_lower = false;
        inner.change_case = false;
        inner.extensions = state->extensions;

        clock_format_core(address_of inner, format, broken);

        if (inner.failed)
        {
                state->failed = true;
                return;
        }

        clock_format_append(state, body, inner.used);
}

/*
        The specifiers that refuse a modifier, which is glibc's table with the
        sense turned round: the lists of refusals are shorter than the lists
        of acceptances, and a specifier this file has never heard of refuses
        both by falling off the end of the same test.

        There is nothing underneath either list. E asks for an era and O asks
        for a locale's own digits, and which specifiers a locale was allowed
        to have an opinion about was settled one case at a time over thirty
        years. %Ou is a directive and %Oa is not; %OS is and %ES is not.
*/
static const char address_to clock_refuses_era = "aAbBdDeFgGhHIjklmMSUVwW";
static const char address_to clock_refuses_digits = "aAcDFxXY";

static bool clock_format_takes_modifier(p8 modifier, p8 which)
{
        if (modifier == 'E')
                return is_null(string_first_of(
                        (string_address)clock_refuses_era, which));

        if (modifier == 'O')
                return is_null(string_first_of(
                        (string_address)clock_refuses_digits, which));

        return true;
}

static fn clock_format_core(clock_format_state address_to state,
                            const char address_to format,
                            const tm address_to broken)
{
        const char address_to cursor = format;

        while (address_to cursor != end)
        {
                const char address_to opened = cursor;
                p8 which;
                p8 modifier = 0;

                /*
                        The decoration is per directive and nothing carries
                        over, so it is cleared here and not after use: an
                        ordinary character of the format is a directive with
                        no decoration at all and must not inherit the width of
                        the one before it.
                */
                state->width = -1;
                state->pad = 0;
                state->to_upper = false;
                state->to_lower = false;
                state->change_case = false;

                if (address_to cursor != '%')
                {
                        clock_format_byte(state, (p8)(address_to cursor));
                        cursor++;
                        continue;
                }

                cursor++;

                /*
                        The flags, repeatable and in any order. The last of
                        -, _ and 0 wins, because the three of them are one
                        question answered three ways; ^ and # are each their
                        own question and stay set once they are.
                */
                while (true)
                {
                        p8 flag = (p8)(address_to cursor);

                        if (flag == '-' || flag == '_' || flag == '0')
                                state->pad = flag;
                        else if (flag == '^')
                                state->to_upper = true;
                        else if (flag == '#')
                                state->change_case = true;
                        else
                                break;

                        cursor++;
                }

                /*
                        The width. Saturating and not wrapping: a width no
                        buffer could satisfy has to end as an answer of zero
                        for want of room, and never as a negative width that
                        would pad backwards through the caller's memory. The
                        ceiling is the same signed thirty two bit one glibc
                        stops at, so the two agree on where absurd begins.
                */
                if (byte_is_digit((p8)(address_to cursor)))
                {
                        state->width = 0;

                        while (byte_is_digit((p8)(address_to cursor)))
                        {
                                bipolar digit = (p8)(address_to cursor) - '0';

                                if (state->width > 214748364 ||
                                    (state->width == 214748364 && digit > 7))
                                        state->width = 2147483647;
                                else
                                        state->width =
                                                state->width * 10 + digit;

                                cursor++;
                        }
                }

                if (address_to cursor == 'E' || address_to cursor == 'O')
                {
                        modifier = (p8)(address_to cursor);
                        cursor++;
                }

                which = (p8)(address_to cursor);

                if (which != end)
                        cursor++;

                /*
                        # decides which way to fold inside the specifier that
                        reads it, and glibc reads it in a different order for
                        %b and %h than for the other three name specifiers:
                        those two fold before they notice that the modifier in
                        front of them is one they do not take, and %a, %A and
                        %B notice first and never fold. Which is why "%#Eb" is
                        "%#EB" and "%#Ea" is "%#Ea". There is no reason for the
                        difference and nobody meant it; it is transcribed here
                        so that the differential test against glibc needs no
                        exception carved out of it.
                */
                if (state->change_case && (which == 'b' || which == 'h'))
                {
                        state->to_upper = true;
                        state->to_lower = false;
                }

                if (which == end ||
                    (modifier && !clock_format_takes_modifier(modifier, which)))
                {
                        clock_format_append(state, (address_any)opened,
                                            (positive)(cursor - opened));
                        continue;
                }

                const char address_to composite = clock_composite_format(which);
                if (composite)
                {
                        clock_format_nested(state, composite, broken);
                        continue;
                }

                switch (which)
                {
                case 'a':
                case 'A':
                case 'b':
                case 'B':
                case 'h':
                {
                        bool weekday = which == 'a' || which == 'A';
                        if (state->change_case)
                        {
                                state->to_upper = true;
                                state->to_lower = false;
                        }
                        clock_format_name(
                            state, weekday ? broken->tm_wday : broken->tm_mon,
                            which == 'A' || which == 'B', weekday ? 7 : 12,
                            weekday ? clock_weekday_short : clock_month_short,
                            weekday ? clock_weekday_long : clock_month_long,
                            weekday ? clock_weekday_long_length : clock_month_long_length);
                        break;
                }

                case 'C':
                        clock_format_number(
                                state,
                                clock_floor_divide((bipolar)broken->tm_year +
                                                           1900,
                                                   100),
                                1);
                        break;

                case 'd':
                        clock_format_number(state, broken->tm_mday, 2);
                        break;

                case 'e':
                        clock_format_number_spaced(state, broken->tm_mday, 2);
                        break;

                case 'g':
                case 'G':
                case 'V':
                {
                        bipolar year, week;
                        clock_iso_week(broken, address_of year, address_of week);
                        if (which == 'g')
                                year -= clock_floor_divide(year, 100) * 100;
                        clock_format_number(state, which == 'V' ? week : year,
                                            which == 'G' ? 1 : 2);
                        break;
                }

                case 'H':
                        clock_format_number(state, broken->tm_hour, 2);
                        break;

                case 'I':
                {
                        bipolar hour = broken->tm_hour % 12;

                        clock_format_number(state, hour == 0 ? 12 : hour, 2);
                        break;
                }

                case 'j':
                        clock_format_number(state, (bipolar)broken->tm_yday + 1,
                                            3);
                        break;

                /*
                        %k and %l are %H and %I written with a leading space
                        instead of a leading zero, and they are the two
                        specifiers the first draft of this file did not have.
                        They are not in C and not in POSIX; they are in glibc,
                        in BSD, and in every date(1) anybody has typed.
                */
                case 'k':
                        clock_format_number_spaced(state, broken->tm_hour, 2);
                        break;

                case 'l':
                {
                        bipolar hour = broken->tm_hour % 12;

                        clock_format_number_spaced(state,
                                                   hour == 0 ? 12 : hour, 2);
                        break;
                }

                case 'm':
                        clock_format_number(state, (bipolar)broken->tm_mon + 1,
                                            2);
                        break;

                case 'M':
                        clock_format_number(state, broken->tm_min, 2);
                        break;

                case 'N':
                        if (state->extensions)
                                clock_format_append(state,
                                                    (address_any)"000000000", 9);
                        else
                                clock_format_append(state, (address_any)opened,
                                                    (positive)(cursor - opened));
                        break;

                case 'n':
                        clock_format_byte(state, '\n');
                        break;

                case 'p':
                case 'P':
                        // %P forces lowercase even when ^ also requests uppercase.
                        if (state->change_case || which == 'P')
                        {
                                state->to_upper = false;
                                state->to_lower = true;
                        }
                        clock_format_append(state,
                            (address_any)clock_half_day[broken->tm_hour >= 12], 2);
                        break;

                case 'q':
                        if (state->extensions)
                                clock_format_number(
                                    state, (bipolar)broken->tm_mon / 3 + 1, 1);
                        else
                                clock_format_append(state, (address_any)opened,
                                                    (positive)(cursor - opened));
                        break;

                /*
                        %s is the one number here that does not go through
                        the padding above. glibc copies its digits the way it
                        copies a weekday name, so a width in front of it pads
                        with spaces unless the 0 flag asked otherwise, and the
                        zeros of "%020s" land in front of the minus sign
                        rather than behind it. Every other number in this
                        switch does the opposite. It is not a distinction
                        anybody designed; it is where the GNU extension was
                        bolted on, and it is visible enough to match.
                */
                case 's':
                {
                        tm copy = address_to broken;
                        p8 body[40];
                        positive at;
                        positive length = clock_number_text(
                                body, (bipolar)timegm(address_of copy),
                                address_of at);

                        clock_format_append(state, body + at, length);
                        break;
                }

                case 'S':
                        clock_format_number(state, broken->tm_sec, 2);
                        break;

                case 't':
                        clock_format_byte(state, '\t');
                        break;

                case 'u':
                        clock_format_number(state,
                                            broken->tm_wday == 0
                                                    ? 7
                                                    : broken->tm_wday,
                                            1);
                        break;

                case 'U':
                        clock_format_number(state,
                                            ((bipolar)broken->tm_yday + 7 -
                                             (bipolar)broken->tm_wday) /
                                                    7,
                                            2);
                        break;

                case 'w':
                        clock_format_number(state, broken->tm_wday, 1);
                        break;

                case 'W':
                        clock_format_number(
                                state,
                                ((bipolar)broken->tm_yday + 7 -
                                 (broken->tm_wday == 0
                                          ? 6
                                          : (bipolar)broken->tm_wday - 1)) /
                                        7,
                                2);
                        break;

                case 'y':
                {
                        bipolar year = (bipolar)broken->tm_year + 1900;

                        clock_format_number(
                                state,
                                year - clock_floor_divide(year, 100) * 100,
                                2);
                        break;
                }

                case 'Y':
                        clock_format_number(
                                state, (bipolar)broken->tm_year + 1900, 1);
                        break;

                /*
                        One number and not two: the offset is printed as
                        hours times a hundred plus minutes, four digits wide,
                        which is the same bytes as two two-digit fields right
                        up until a width is named and then is not. "%6z" is
                        "     +001800" under glibc because the six applies to
                        the sign and then to the whole four-digit number, and
                        that only comes out if the four digits are one field.
                */
                case 'z':
                {
                        bipolar offset = broken->tm_gmtoff;
                        bipolar magnitude = offset < 0 ? -offset : offset;

                        clock_format_byte(state, offset < 0 ? '-' : '+');
                        clock_format_number(state,
                                            magnitude / 3600 * 100 +
                                                    magnitude / 60 % 60,
                                            4);
                        break;
                }

                case 'Z':
                        if (state->change_case)
                        {
                                state->to_upper = false;
                                state->to_lower = true;
                        }

                        if (is_null(broken->tm_zone))
                                clock_format_append(
                                        state, (address_any)clock_zone_name, 3);
                        else
                                clock_format_append(state, (address_any)broken->tm_zone,
                                    string_length((string_address)broken->tm_zone));
                        break;

                case '%':
                        clock_format_byte(state, '%');
                        break;

                default:
                        clock_format_append(state, (address_any)opened,
                                            (positive)(cursor - opened));
                        break;
                }
        }
}

static positive clock_format(p8 address_to into, positive max,
                             const char address_to format,
                             const tm address_to broken, bool extensions)
{
        clock_format_state state;

        if (is_null(format) || is_null(broken))
                return 0;

        /* A literal format is already the finished answer.  This common
           logging shape needs one terminated scan and one hardware-floor
           copy, not the directive engine once per byte. */
        if (address_to format != '%')
        {
                const char address_to directive =
                        (const char address_to)string_first_of_or_end(
                                (string_address)format, '%');

                if (address_to directive == end)
                {
                        positive length = (positive)(directive - format);

                        if (length >= max)
                                return 0;

                        if (length)
                                memory_copy_apart(into, (address_any)format,
                                                  length);

                        into[length] = end;
                        return length;
                }
        }

        state.into = into;
        state.max = max;
        state.used = 0;
        state.failed = false;
        state.extensions = extensions;

        clock_format_core(address_of state, format, broken);

        if (state.failed || state.used + 1 > max)
                return 0;

        into[state.used] = end;

        return state.used;
}

positive strftime(p8 address_to into, positive max,
                  const char address_to format, const tm address_to broken)
{
        return clock_format(into, max, format, broken, false);
}

/* date(1) shares the full formatter and only opts into the two GNU date
   directives that libc's strftime must return verbatim. */
#define clock_format_extended(into, max, format, broken)                    \
        clock_format((into), (max), (format), (broken), true)

/*
        strptime, which reads a date back out of the text strftime wrote and
        is not, whatever the symmetry of the names suggests, its inverse.

        It is not an inverse for three reasons, all of them in the standard
        and all of them surprising the first time:

          - it does not clear the structure. Fields the format never mentions
            keep whatever the caller left in them, which is what lets two
            calls with two formats fill one struct tm between them, and which
            means a caller who does not clear it first gets stack rubbish in
            the fields nobody parsed. That is the contract, not a defect.
          - it does not have to consume the whole input. It answers a pointer
            to the first byte it did not use, and a caller who cares that the
            input ended checks that byte itself. It answers null, and writes
            nothing more, when the input stopped matching the format.
          - it derives what it can and no more. Give it a year, a month and a
            day and it works out the weekday and the day of the year; give it
            a week number and it throws the week number away, because %U and
            %W do not pin a date down and glibc does not pretend they do.

        The scanning rules, which are as much of the specification as the
        specifier list is:

          - a run of whitespace in the format matches any run of whitespace in
            the input, the empty run included;
          - any other ordinary byte in the format must be that byte in the
            input;
          - a number skips leading whitespace whether the format asked for
            any or not, then takes up to as many digits as the field allows,
            and stops early rather than overflowing the field's range --
            which is why "%m%d" reads "0102" as January the second and not as
            month one thousand and two;
          - a name is matched without regard to case, and the longest name
            that matches wins, so that "Sunday" is never read as "Sun" with
            "day" left over.

        E and O are accepted in front of every specifier and mean the plain
        specifier, because in the C locale there is no era to name and no
        alternative digit to read. That is wider than glibc, which keeps a
        list of the specifiers each modifier may precede and refuses the rest
        -- and which, on the ones it does take, is wrong twice: %Ey reads its
        number, then falls into the plain %y arm without winding the input
        back and reads a second one on top of it, and the second %O in a
        format returns null where the first one fell back. Both are in the
        list of disagreements at the top of CHECK_clock in test/checks.c, with what glibc
        answers and what this answers instead.
*/
typedef struct clock_scan_state
{
        const char address_to at;
        const char address_to last;
        tm address_to broken;
        bipolar century;
        bool have_mday;
        bool have_wday;
        bool have_yday;
        bool have_mon;
        bool have_hour12;
        bool is_afternoon;
        bool want_century;
        bool want_day;
} clock_scan_state;

/*
        A decimal field: whitespace, then digits, then a range test.

        The whitespace is skipped whether or not the format asked for it,
        which is not symmetrical with strftime and is what every caller
        expects: "%H:%M" against " 5:06" reads five o'clock, and a format
        would have to say " %H" to get that if the skip were not here.

        The early stop is the subtle part and it is glibc's. Reading stops
        when one more digit would carry the value past the top of the field's
        range even though the field is allowed more digits -- so "%d" against
        "312" takes "31" and leaves the "2", rather than taking "312" and
        failing. A parser that took the digits first and range-checked after
        would refuse a string that glibc accepts, and "%m%d%y" against a run
        of six digits is exactly the case that breaks.
*/
static bool clock_scan_number(clock_scan_state address_to state, bipolar least,
                              bipolar most, positive digits,
                              bipolar address_to into)
{
        bipolar value = 0;

        state->at += string_span_of_set(
                (string_address)state->at, " \t\n\r\v\f");

        if (!byte_is_digit((p8)(address_to state->at)))
                return false;

        do
        {
                value = value * 10 + ((p8)(address_to state->at) - '0');
                state->at++;
        } while (--digits > 0 && value * 10 <= most &&
                 byte_is_digit((p8)(address_to state->at)));

        if (value < least || value > most)
                return false;

        address_to into = value;

        return true;
}

/*
        One of a table of names, without regard to case, longest first.

        strptime records the input end once.  The candidates longer than what
        remains are never compared, which keeps memory_compare_ascii_case --
        counted, not terminated -- from reading past the caller's string
        without rescanning that string for every name directive. Answers
        which one, or -1.
*/
static b32 clock_scan_name(clock_scan_state address_to state,
                           const char address_to address_to table,
                           const p8 address_to lengths, positive fixed_length,
                           positive count)
{
        positive left = (positive)(state->last - state->at);
        positive best_length = 0;
        b32 best = -1;
        positive which;

        for (which = 0; which < count; which++)
        {
                positive length = fixed_length ? fixed_length : lengths[which];

                if (length <= left && length > best_length &&
                    memory_compare_ascii_case((address_any)state->at,
                                              (address_any)table[which],
                                              length) == 0)
                {
                        best = (b32)which;
                        best_length = length;
                }
        }

        if (best < 0)
                return -1;

        state->at += best_length;

        return best;
}

/* Bounds and digit ceilings for numeric strptime fields. The shared reader
   retains its early range stop; assignments and dependent-date flags follow. */
static const struct { p16 least, most; p8 digits; } clock_scan_ranges[] = {
    ['C'] = {0, 99, 2},   ['d'] = {1, 31, 2},   ['e'] = {1, 31, 2},
    ['H'] = {0, 23, 2},   ['k'] = {0, 23, 2},   ['I'] = {1, 12, 2},
    ['l'] = {1, 12, 2},   ['j'] = {1, 366, 3},  ['m'] = {1, 12, 2},
    ['M'] = {0, 59, 2},   ['S'] = {0, 61, 2},   ['u'] = {1, 7, 1},
    ['w'] = {0, 6, 1},    ['g'] = {0, 99, 2},   ['U'] = {0, 53, 2},
    ['V'] = {0, 53, 2},   ['W'] = {0, 53, 2},   ['y'] = {0, 99, 2},
    ['Y'] = {0, 9999, 4},
};

static bool clock_scan_core(clock_scan_state address_to state,
                            const char address_to format);

// The compound specifiers, read back through the same loop that wrote them,
// against the same format strings strftime expands them to.
static bool clock_scan_core(clock_scan_state address_to state,
                            const char address_to format)
{
        const char address_to cursor = format;
        tm address_to broken = state->broken;

        while (address_to cursor != end)
        {
                p8 which;
                bipolar value = 0;

                if (byte_is_space((p8)(address_to cursor)))
                {
                        cursor += string_span_of_set(
                                (string_address)cursor, " \t\n\r\v\f");
                        state->at += string_span_of_set(
                                (string_address)state->at, " \t\n\r\v\f");
                        continue;
                }

                if (address_to cursor != '%')
                {
                        positive run = string_span_without_set(
                                (string_address)cursor, "% \t\n\r\v\f");

                        if (run > (positive)(state->last - state->at) ||
                            memory_compare((address_any)cursor,
                                           (address_any)state->at, run) != 0)
                                return false;

                        state->at += run;
                        cursor += run;
                        continue;
                }

                cursor++;

                // Accepted and meaningless, both of them, in the only locale
                // there is.
                if (address_to cursor == 'E' || address_to cursor == 'O')
                        cursor++;

                which = (p8)(address_to cursor);

                if (which == end)
                        return false;

                cursor++;

                const char address_to composite = clock_composite_format(which);
                if (composite)
                {
                        if (!clock_scan_core(state, composite))
                                return false;
                        continue;
                }

                if (which < array_count(clock_scan_ranges) &&
                    clock_scan_ranges[which].digits &&
                    !clock_scan_number(state, clock_scan_ranges[which].least,
                                       clock_scan_ranges[which].most,
                                       clock_scan_ranges[which].digits,
                                       address_of value))
                        return false;

                switch (which)
                {
                case 'a':
                case 'A':
                {
                        b32 found = clock_scan_name(
                                state, clock_weekday_long,
                                clock_weekday_long_length, 0, 7);

                        if (found < 0)
                                found = clock_scan_name(
                                        state, clock_weekday_short, null, 3, 7);

                        if (found < 0)
                                return false;

                        broken->tm_wday = found;
                        state->have_wday = true;
                        break;
                }

                case 'b':
                case 'B':
                case 'h':
                {
                        b32 found = clock_scan_name(
                                state, clock_month_long,
                                clock_month_long_length, 0, 12);

                        if (found < 0)
                                found = clock_scan_name(
                                        state, clock_month_short, null, 3, 12);

                        if (found < 0)
                                return false;

                        broken->tm_mon = found;
                        state->have_mon = true;
                        state->want_day = true;
                        break;
                }

                case 'C':
                        state->century = value;
                        state->want_day = true;
                        break;

                case 'd':
                case 'e':
                        broken->tm_mday = (b32)value;
                        state->have_mday = true;
                        state->want_day = true;
                        break;

                case 'H':
                case 'k':
                        broken->tm_hour = (b32)value;
                        state->have_hour12 = false;
                        break;

                case 'I':
                case 'l':
                        broken->tm_hour = (b32)(value % 12);
                        state->have_hour12 = true;
                        break;

                /*
                        The day of the year is stored and nothing is derived
                        from it on its own, which is glibc's choice and is the
                        right one: a day of the year without a year is not a
                        date, so "%j" by itself fills tm_yday and leaves the
                        month, the day and the weekday exactly as the caller
                        left them. Put a year beside it and the year turns the
                        derivation on.
                */
                case 'j':
                        broken->tm_yday = (b32)(value - 1);
                        state->have_yday = true;
                        break;

                case 'm':
                        broken->tm_mon = (b32)(value - 1);
                        state->have_mon = true;
                        state->want_day = true;
                        break;

                case 'M':
                        broken->tm_min = (b32)value;
                        break;

                case 'n':
                case 't':
                        state->at += string_span_of_set(
                                (string_address)state->at, " \t\n\r\v\f");
                        break;

                case 'p':
                case 'P':
                {
                        b32 found = clock_scan_name(state, clock_half_day, null,
                                                   2, 2);

                        if (found < 0)
                                return false;

                        state->is_afternoon = found == 1;
                        break;
                }

                /*
                        %s is the one specifier that does not fill a field, it
                        fills the whole structure: the seconds are read and
                        then broken down, and everything the format said
                        before it is overwritten. Read digit by digit rather
                        than through the field reader above, because there is
                        no range to stop at and no width to stop at either.
                */
                case 's':
                {
                        positive seconds;
                        string_address at = (string_address)state->at;
                        if (!string_digits_checked(address_of at, 10,
                                                   address_of seconds) ||
                            seconds > (positive)bipolar_max)
                                return false;
                        state->at = (const char address_to)at;
                        time_t moment = (time_t)seconds;

                        if (is_null(localtime_r(address_of moment, broken)))
                                return false;

                        break;
                }

                /*
                        Sixty one and not fifty nine: a minute with two leap
                        seconds in it was legal in the C standard long after
                        it stopped being possible in the world, and a parser
                        that refuses "23:59:60" refuses text that exists.
                */
                case 'S':
                        broken->tm_sec = (b32)value;
                        break;

                case 'u':
                        broken->tm_wday = (b32)(value % 7);
                        state->have_wday = true;
                        break;

                case 'w':
                        broken->tm_wday = (b32)value;
                        state->have_wday = true;
                        break;

                /*
                        Read and thrown away, all five of them. A week number
                        does not name a day, and the week-based year does not
                        name the ordinary one; taken with the rest of a
                        format they would be redundant and taken alone they
                        would be ambiguous, so glibc consumes the digits to
                        keep the format in step with the input and stores
                        nothing. This does the same rather than inventing an
                        answer glibc does not have.
                */
                case 'g':
                        break;

                /*
                        %G takes every digit it can see and not four of them,
                        which is glibc's answer to a year that has no width:
                        a week-based year is not stored, so there is no field
                        for a fifth digit to overflow and no reason to stop.
                        It means "%G%j" cannot work -- the %G swallows the day
                        of the year too -- and that is true in glibc as well.
                */
                case 'G':
                        if (!byte_is_digit((p8)(address_to state->at)))
                                return false;

                        state->at += string_span((string_address)state->at,
                                                 string_set_digits);
                        break;

                case 'U':
                case 'V':
                case 'W':
                        break;

                /*
                        Two digits of year, and the window the paper called
                        "Year 2000: The Millennium Rollover" chose: sixty nine
                        and up is the nineteen hundreds, sixty eight and down
                        is the two thousands. It is arbitrary and it is what
                        every other strptime does.
                */
                case 'y':
                        broken->tm_year = (b32)(value >= 69 ? value
                                                            : value + 100);
                        state->want_century = true;
                        state->want_day = true;
                        break;

                case 'Y':
                        broken->tm_year = (b32)(value - 1900);
                        state->want_century = false;
                        state->want_day = true;
                        break;

                /*
                        A zone offset in the four spellings anybody writes:
                        a Z on its own, two digits of hours, four digits of
                        hours and minutes, and the same four with a colon in
                        the middle. The colon is only allowed where it belongs
                        and only when a digit follows it.
                */
                case 'z':
                {
                        bool behind;
                        positive taken = 0;

                        state->at += string_span_of_set(
                                (string_address)state->at, " \t\n\r\v\f");

                        if (address_to state->at == 'Z')
                        {
                                state->at++;
                                broken->tm_gmtoff = 0;
                                break;
                        }

                        if (address_to state->at != '+' &&
                            address_to state->at != '-')
                                return false;

                        behind = address_to state->at == '-';
                        state->at++;

                        while (taken < 4 &&
                               byte_is_digit((p8)(address_to state->at)))
                        {
                                value = value * 10 +
                                        ((p8)(address_to state->at) - '0');
                                state->at++;
                                taken++;

                                if (address_to state->at == ':' &&
                                    taken == 2 &&
                                    byte_is_digit((p8)(state->at[1])))
                                        state->at++;
                        }

                        if (taken == 2)
                                value *= 100;
                        else if (taken != 4)
                                return false;
                        else if (value % 100 >= 60)
                                return false;

                        /*
                                The minutes are checked and the hours are not,
                                which is glibc's asymmetry: "+9959" is an
                                offset of ninety nine hours and it is taken,
                                "+0060" is sixty minutes and it is refused.
                                Nothing on Earth is more than fourteen hours
                                from Greenwich, but a parser is not the place
                                to have opinions about that.
                        */
                        broken->tm_gmtoff =
                                value / 100 * 3600 + value % 100 * 60;

                        if (behind)
                                broken->tm_gmtoff = -broken->tm_gmtoff;

                        break;
                }

                /*
                        %Z takes a word and does nothing with it.

                        There is no table of zone abbreviations to match
                        against, and there could not be a right one: three
                        letters name two different zones often enough that
                        any table would be wrong somewhere. So the word is
                        stepped over so that the format stays in step with the
                        input, and the caller who cares which zone it was
                        reads it out of the input itself. "Word" is literally
                        that -- whitespace, then everything up to the next
                        whitespace -- so "%Z %Y" against "2000-02-29" fails,
                        because the zone ate the date.
                */
                case 'Z':
                        state->at += string_span_of_set(
                                (string_address)state->at, " \t\n\r\v\f");

                        state->at += string_span_without_set(
                                (string_address)state->at, " \t\n\r\v\f");
                        break;

                case '%':
                        if (address_to state->at != '%')
                                return false;

                        state->at++;
                        break;

                default:
                        return false;
                }
        }

        return true;
}

/*
        What the fields imply, worked out once at the end rather than as each
        one arrives, because the order they arrive in is the format's business
        and not this routine's: "%d %m %Y" has to reach the same answer as
        "%Y %m %d", so nothing is derived until there is nothing left to read.

        Three derivations, in the order they depend on each other. The century
        adjusts the year. A day of the year, with no month or day of the month
        beside it, becomes both. And a year, a month and a day become the
        weekday and, if it was not given, the day of the year.
*/
static fn clock_scan_settle(clock_scan_state address_to state)
{
        tm address_to broken = state->broken;
        bipolar year;

        if (state->have_hour12 && state->is_afternoon)
                broken->tm_hour += 12;

        if (state->century >= 0)
        {
                if (state->want_century)
                        broken->tm_year = (b32)(broken->tm_year % 100 +
                                                (state->century - 19) * 100);
                else
                        broken->tm_year = (b32)((state->century - 19) * 100);
        }

        if (!state->want_day)
                return;

        year = (bipolar)broken->tm_year + 1900;

        if (!state->have_wday)
        {
                if (!(state->have_mon && state->have_mday) && state->have_yday)
                {
                        bipolar found_year;
                        bipolar found_month;
                        bipolar found_day;

                        clock_civil_from_days(
                                clock_days_from_civil(year, 1, 1) +
                                        (bipolar)broken->tm_yday,
                                address_of found_year, address_of found_month,
                                address_of found_day);

                        if (!state->have_mon)
                                broken->tm_mon = (b32)(found_month - 1);

                        if (!state->have_mday)
                                broken->tm_mday = (b32)found_day;

                        state->have_mon = true;
                        state->have_mday = true;
                }

                if (state->have_mon ||
                    (broken->tm_mon >= 0 && broken->tm_mon <= 11))
                        broken->tm_wday = (b32)clock_weekday_from_days(
                                clock_days_from_civil(
                                        year, (bipolar)broken->tm_mon + 1,
                                        (bipolar)broken->tm_mday));
        }

        if (!state->have_yday &&
            (state->have_mon || (broken->tm_mon >= 0 && broken->tm_mon <= 11)))
                broken->tm_yday = (b32)(clock_days_from_civil(
                                                year,
                                                (bipolar)broken->tm_mon + 1,
                                                (bipolar)broken->tm_mday) -
                                        clock_days_from_civil(year, 1, 1));
}

p8 address_to strptime(const char address_to input, const char address_to format,
                       tm address_to broken)
{
        clock_scan_state state;

        if (is_null(input) || is_null(format) || is_null(broken))
                return null;

        state.at = input;
        state.last = (const char address_to)string_first_of_or_end(
                (string_address)input, end);
        state.broken = broken;
        state.century = -1;
        state.have_mday = false;
        state.have_wday = false;
        state.have_yday = false;
        state.have_mon = false;
        state.have_hour12 = false;
        state.is_afternoon = false;
        state.want_century = false;
        state.want_day = false;

        if (!clock_scan_core(address_of state, format))
                return null;

        clock_scan_settle(address_of state);

        return (p8 address_to)state.at;
}

/*
        The three globals tzset is supposed to set, set to what they actually
        are here. tzset itself is a call that does nothing, which is honest:
        there is no zone to load, and a program that calls it is asking for
        the timezone to be re-read rather than for anything to change.
*/
static const char address_to clock_zone_names[2] = {"UTC", "UTC"};

const char address_to address_to tzname = clock_zone_names;
b64 timezone = 0;
b32 daylight = 0;

fn tzset(void)
{
}

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_CLOCK
#endif // STANDARD_SKIP_CLOCK

#ifndef STANDARD_SKIP_MATH
/* ---- math.c ---- */

/*
        Experimental C standard library

        The rest of <math.h>: the transcendentals, and the exact ones

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_MATH
#define STANDARD_MODERN_C_STANDARD_MATH

/*
        This is ordinary C, and it has to be.

        src/platform/standard.inc already holds the half of <math.h> that is
        an instruction: sqrt, fabs, trunc, floor, ceil, round, fmin, fmax,
        fma and copysign are one opcode on at least two of the three machines
        and the assembly there is the floor. Nothing below is like that.
        exp is a range reduction, a polynomial and an exponent field write;
        pow is a logarithm carried in two doubles so that multiplying it by
        an exponent of a thousand still leaves fifty three good bits. Those
        are algorithms, and an algorithm written three times in three
        assemblers is an algorithm with three sets of bugs. So it lives here,
        in the one place the library keeps C -- included from
        src/compiler_memory.c, which is deliberately outside library.c's
        graph, in the way src/net/netlink.c holds the netlink wire layer.

        It depends on library.c alone, through standard.inc: square_root,
        absolute, decimal_floor, decimal_truncated, decimal_rounded,
        decimal_with_sign and decimal_multiply_add. Every one of those is a
        single instruction on the machines that have it, so reaching for them
        rather than writing the arithmetic again is both shorter and faster.

        WHAT THE NAMES ARE, AND THE ONE THAT COULD NOT BE HAD

        The library already exports `log`, and it is not a logarithm: it is
        the buffered writer, `fn log(address_any data, positive length)`, and
        `string_format(log, ...)` is written in a hundred places in this tree.
        Defining C's log here -- as a function, as a macro, as anything --
        would turn every one of those call sites into a type error or, worse,
        into a call to a logarithm with a pointer in it. So the natural
        logarithm ships as `logarithm`, with `ln` as its short alias, and
        there is no `log`. A program that wants the C spelling has to make
        that choice for itself, knowing what it costs.

        Everything else has its prose name and a one-line wrapper carrying
        the standard name, at the bottom of the file. The wrappers are
        `static` like everything else here, so an unused one costs nothing
        and a program that takes `&pow` still gets a real function pointer,
        which a macro alias could not give it.

        WHY THE CONTRACTION PRAGMA IS NOT OPTIONAL

        gcc contracts `a*b + c` into a fused multiply-add by default on arm64
        and riscv64, and cannot on baseline x86_64, which has no FMA3. That
        is three different answers from one source, and for most of this file
        it would only mean harmless last-bit disagreement. In the compensated
        arithmetic below it is not harmless: math_two_product computes the
        exact rounding error of a product as fma(a, b, -p), and if the
        compiler is also allowed to fuse the plain product p = a*b that the
        error is measured against, the two roundings that were supposed to
        differ become the same one and the error comes back zero. A pow built
        on that is quietly wrong rather than obviously wrong. So the file
        turns contraction off for its own extent and puts it back, and every
        fused operation it actually wants goes through math_multiply_add,
        the inline adapter to the same decimal_multiply_add floor.

        WHAT IS NOT HERE

        No errno and no fenv. A domain error returns a NaN and raises
        nothing; an overflow returns an infinity and raises nothing. This
        library has no errno to set and no exception flags to read, and
        pretending otherwise would cost every call a store nobody reads.

        HOW THE NUMBERS BELOW WERE TAKEN

        Every routine here is measured against glibc's LONG DOUBLE version of
        the same function, rounded back to a double. On x86_64 that is an
        eighty bit format with a sixty four bit significand, so the reference
        carries eleven more bits than the answer being checked and is a fair
        judge of it; glibc's own double routines are quoted separately,
        because for two of these functions they are the ones that are wrong.
        Arguments are drawn log-spaced across the whole of each domain with
        random signs and random mantissas, eight million of them per routine
        unless the line says otherwise, plus dense sweeps of every place a
        branch changes and every special value C99 names.

        In summary, worst case, against the long double reference:

              exact, bit for bit, no error at all
                    fmod  remainder  ldexp  scalbn  frexp  modf
                    isnan  isinf  isfinite  isnormal  signbit  fpclassify

              within 1 ulp
                    exp  exp2  expm1  logarithm  log2  log10  pow
                    cbrt  hypot  sin  cos  asin  acos  atan

              within 2 ulp
                    tan  atan2  sinh  tanh   (cosh is 1)

        Two of those beat glibc's double routines rather than matching them.
        glibc's cbrt is out by as much as four ulp where this one is inside
        one, and its log10 by two; every other disagreement between the two
        libraries is a single last bit, and this one is on the correct side
        of it as often as not.

        The same test built freestanding runs on x86_64, arm64 under
        qemu-aarch64 and riscv64 under qemu-riscv64, and the three produce
        byte identical output on ten thousand results with four exceptions,
        all of them the sign bit of a NaN: 0/0 gives a negative quiet NaN on
        x86_64 and a positive one on the other two. That is the hardware's
        default NaN and glibc reports it the same way.
*/

/*
        Everything here takes or returns a decimal, and a decimal in a
        signature is refused by the arm64 kernel build whether or not
        anything calls it -- the same reason standard.inc guards its own
        floating point half. Kernel code may not touch the floating point
        registers without asking first, so this is right anyway.

        The second half of the guard is about width. Every polynomial,
        every constant and every bit mask below is double precision, chosen
        for a fifty three bit significand and an eleven bit exponent. On a
        profile where `decimal` is f32 -- which is what library.c gives a
        thirty two bit target -- none of it would be either correct or
        useful, so the family is simply absent there rather than silently
        wrong. All three architectures this project builds are sixty four
        bit, so the guard is documentation rather than a fork.
*/
#if !defined(KERNEL_MODE) && decimal_bits == 64

#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")

/*
        A double seen as its bits, and the bits seen as a double.

        Every classification below and every exponent write is a question
        about the bit pattern rather than about the number, and a union is
        how C asks it. gcc defines reading a union member other than the one
        last written, and it compiles to the one register move the hardware
        needs -- movq on x86_64, fmov on arm64, fmv.x.d on riscv64 -- with no
        store to memory in between.
*/
typedef union
{
        decimal value;
        p64 bits;
        b64 signed_bits;
} math_shape;

#define MATH_SIGN_MASK 0x8000000000000000ULL
#define MATH_MAGNITUDE_MASK 0x7fffffffffffffffULL
#define MATH_INFINITY_BITS 0x7ff0000000000000ULL
#define MATH_EXPONENT_BIAS 1023
#define MATH_MANTISSA_BITS 52
#define MATH_MANTISSA_MASK 0x000fffffffffffffULL
#define MATH_IMPLIED_BIT 0x0010000000000000ULL

/*
        The classes, which are the classification macros the C library calls
        isnan, isinf, isfinite, isnormal, signbit and fpclassify.

        The question that had to be answered before writing these was whether
        they belong beside square_root in standard.inc as assembly. They do
        not, and the reason is that the C library does not define them as
        functions in the first place: they are macros, and a macro is what
        makes them free. isnan of a value already in a register is one move
        to a general register, one and, and one compare -- three instructions
        that fold into whatever the caller was doing. A call to an assembly
        routine that did the same three instructions would pay a call and a
        return on top of them, which is more than doubling the cost of the
        thing being called, and it would do it at every one of the dozens of
        guard sites inside this file. Written as C the compiler inlines them
        and often removes them entirely, because most of the tests below are
        against values it can already see the shape of.

        So they are C, and they are exact: no rounding happens anywhere in
        them. The six float ones were checked against glibc's macros on all
        four billion two hundred and ninety four million float bit patterns,
        exhaustively, and the six double ones on forty million random
        patterns. Zero disagreements in either.

        fpclassify's five answers are numbered the way glibc numbers them --
        NaN 0, infinite 1, zero 2, subnormal 3, normal 4 -- not because the
        standard says so, it deliberately does not, but because a program
        that moves between the two libraries should not find its switch
        statement quietly relabelled.
*/
#define MATH_CLASS_NAN 0
#define MATH_CLASS_INFINITE 1
#define MATH_CLASS_ZERO 2
#define MATH_CLASS_SUBNORMAL 3
#define MATH_CLASS_NORMAL 4

/* The same integer classification in each floating width. The x87 reader
   below normalizes its explicit integer bit into the binary128 layout. */
#define MATH_CLASSIFIER(name, type, word, normal, infinity, sign, read)       \
        static word name##_magnitude_bits(type value)                        \
        { return (read) & (((word)1 << (sign)) - 1); }                        \
        static bool name##_is_nan(type value)                               \
        { return name##_magnitude_bits(value) > (infinity); }                \
        static bool name##_is_infinite(type value)                          \
        { return name##_magnitude_bits(value) == (infinity); }               \
        static bool name##_is_finite(type value)                            \
        { return name##_magnitude_bits(value) < (infinity); }                \
        static bool name##_is_normal(type value)                            \
        { word magnitude = name##_magnitude_bits(value);                     \
          return magnitude >= (normal) && magnitude < (infinity); }          \
        static bool name##_sign_bit(type value)                             \
        { return ((read) >> (sign)) != 0; }                                  \
        static b32 name##_class(type value)                                 \
        { word magnitude = name##_magnitude_bits(value);                     \
          if (!magnitude) return MATH_CLASS_ZERO;                            \
          if (magnitude < (normal)) return MATH_CLASS_SUBNORMAL;              \
          if (magnitude < (infinity)) return MATH_CLASS_NORMAL;               \
          return magnitude == (infinity) ? MATH_CLASS_INFINITE               \
                                           : MATH_CLASS_NAN; }

MATH_CLASSIFIER(decimal, decimal, p64, MATH_IMPLIED_BIT, MATH_INFINITY_BITS,
                63, memory_cast(p64, value))
MATH_CLASSIFIER(narrow, f32, p32, 0x00800000U, 0x7f800000U,
                31, memory_cast(p32, value))

#if __LDBL_MANT_DIG__ == 64
static p128 math_extended_bits(f128 value)
{
        p128 bits = memory_cast(p128, value);
        p64 fraction = (p64)bits & 0x7fffffffffffffffULL;
        p32 exponent = (p32)(bits >> 64) & 0x7fff;
        if ((p64)bits >> 63)
                exponent += !exponent; // x87 pseudo-denormals are normal.
        else if (exponent)
        {
                exponent = 0x7fff; // An absent explicit bit makes an unnormal NaN.
                fraction = 1;
        }
        return ((bits >> 79) & 1) << 127 | (p128)exponent << 112 |
               (p128)fraction << 49;
}
#else
#define math_extended_bits(value) memory_cast(p128, value)
#endif

#if __LDBL_MANT_DIG__ > 53
MATH_CLASSIFIER(extended, f128, p128, (p128)1 << 112, (p128)0x7fff << 112,
                127, math_extended_bits(value))
#else
MATH_CLASSIFIER(extended, f128, p64, MATH_IMPLIED_BIT, MATH_INFINITY_BITS,
                63, memory_cast(p64, value))
#endif
#undef MATH_CLASSIFIER

/*
        The magnitude, with the sign bit cleared where it stands.

        absolute() in library.c is fabs, and it stays the right routine for a
        magnitude a caller asked for. Inside this file it is not, and the
        reason is the one the classification block above already gives for
        isnan: the work is a single AND against a constant, and reaching it
        through a call pays a call and a return around that one instruction.
        It really is a call -- in the linked binary absolute is a move, a bit
        clear, a move back and a ret, and all twenty six places below went
        through it.

        Written here the mask is an expression the compiler can move, and the
        clearest case is sine on arm64: decimal_is_finite immediately above
        has already masked the same word to compare it against infinity, and
        with the magnitude as C gcc keeps that one AND, feeds both band tests
        from it, and answers a small argument without building a stack frame
        at all. Through the call it built a frame and called absolute twice.

        This is not only for the guards. Half the uses below hand the
        magnitude to arithmetic rather than to a comparison -- hypotenuse's
        two sides, power's base, cube_root's, the three hyperbolics -- and
        they are the same one instruction and win the same way.

        Bit for bit the same answer as absolute on every input the format
        has, the NaNs included, which is a measurement and not an argument:
        thirty million results over random bit patterns and every special
        value C99 names are identical before and after on x86_64, arm64 and
        riscv64.
*/
static decimal math_magnitude(decimal value)
{
        math_shape shape;

        shape.value = value;
        shape.bits &= MATH_MAGNITUDE_MASK;
        return shape.value;
}

/* Classify in the argument's floating width: narrowing long double loses
   both its finite range and its subnormal boundary. Each selected arm still
   evaluates the value once; the size tests disappear during compilation. */
#define MATH_CLASSIFY(value, operation)                                      \
        (sizeof(value) == sizeof(f32) ? narrow_##operation((f32)(value)) :   \
         sizeof(value) > sizeof(decimal) ? extended_##operation((f128)(value))\
                                         : decimal_##operation((decimal)(value)))
#define isnan(value) MATH_CLASSIFY(value, is_nan)
#define isinf(value) MATH_CLASSIFY(value, is_infinite)
#define isfinite(value) MATH_CLASSIFY(value, is_finite)
#define isnormal(value) MATH_CLASSIFY(value, is_normal)
#define signbit(value) MATH_CLASSIFY(value, sign_bit)
#define fpclassify(value) MATH_CLASSIFY(value, class)

#define FP_NAN MATH_CLASS_NAN
#define FP_INFINITE MATH_CLASS_INFINITE
#define FP_ZERO MATH_CLASS_ZERO
#define FP_SUBNORMAL MATH_CLASS_SUBNORMAL
#define FP_NORMAL MATH_CLASS_NORMAL

/*
        The constants, and why several of them are written twice.

        A double holds fifty three bits and several of the reductions below
        need more than that from a constant they are about to multiply by a
        number as large as a thousand. Where that happens the constant is
        carried as a pair: a leading double, and a second double holding what
        the first one could not, so that the pair together is good to about a
        hundred and six bits. LN2_HEAD is not the nearest double to ln 2 --
        it is ln 2 rounded to thirty two significant bits, chosen so that
        multiplying it by any integer up to two million is exact and leaves
        no rounding to account for. LN2_TAIL then carries the rest.

        Where a constant is used as a plain multiplier and the pair is only
        there for precision, the head IS the nearest double and the tail is
        the difference. LOG2E and LOG2E_TAIL are that shape, as are LOG10E,
        LOG10_2 and PI_OVER_TWO.

        Every number below was produced by rounding a hundred digit decimal
        expansion, and each is written with the seventeen significant digits
        that round trip exactly through a double.
*/
#define MATH_LN2 0.6931471805599453             // the nearest double to ln 2
#define MATH_LN2_TAIL 2.3190468138462996e-17    // ln 2 minus that
#define MATH_LN2_HEAD 0.6931471806019545        // ln 2 to 32 significant bits
#define MATH_LN2_HEAD_TAIL -4.2009150726810846e-11
#define MATH_LOG2E 1.4426950408889634
#define MATH_LOG2E_TAIL 2.0355273740931033e-17
#define MATH_LOG10E 0.4342944819032518
#define MATH_LOG10E_TAIL 1.098319650216765e-17
#define MATH_LOG10_2 0.3010299956639812
#define MATH_LOG10_2_TAIL -2.8037281277851704e-18
#define MATH_PI 3.141592653589793
#define MATH_PI_TAIL 1.2246467991473532e-16
#define MATH_PI_OVER_TWO 1.5707963267948966
#define MATH_PI_OVER_TWO_TAIL 6.123233995736766e-17
#define MATH_PI_OVER_FOUR 0.7853981633974483
#define MATH_THREE_PI_OVER_FOUR 2.356194490192345
#define MATH_TWO_TO_1023 8.98846567431158e+307
#define MATH_TWO_TO_MINUS_969 2.004168360008973e-292
#define MATH_TWO_TO_MINUS_54 5.551115123125783e-17
#define MATH_TWO_TO_MINUS_28 3.725290298461914e-09
#define MATH_TWO_TO_MINUS_27 7.450580596923828e-09
#define MATH_HUGE 1.7976931348623157e+308

//      The mantissa field of the square root of two, which is where a
//      logarithm splits its argument so that the reduced value lands in
//      [sqrt(1/2), sqrt(2)) and the series that follows sees an argument no
//      larger than about 0.1716 in magnitude.
#define MATH_SQRT2_MANTISSA 0x0006a09e667f3bcdULL

/*
        Two sums and one product that keep what rounding threw away.

        The whole of pow, and the good half of log2 and log10, rests on being
        able to carry a number in two doubles. Addition and multiplication
        both have the property that the error of a single rounded operation
        is itself exactly representable, and these three routines hand it
        back.

        math_two_sum is Knuth's, and it is exact for any two arguments in any
        order: it costs six additions where the version that assumes the
        first argument is the larger costs three. Both appear below, and the
        cheap one is used only where the ordering is known from the
        arithmetic rather than believed.

        math_two_product is the one that could not be written in plain C.
        The error of a rounded product needs the full product, and the only
        way to see the bits a double multiply discarded is a fused
        multiply-add, which computes a*b to full width and subtracts before
        rounding once. That is decimal_multiply_add, which is one instruction
        on arm64 and riscv64 and a software body on a baseline x86_64 without
        FMA3 -- correct either way, which is what matters here. Dekker's
        splitting trick would avoid the dependency, but it is silently
        destroyed by the very contraction this file disables, and one build
        flag going missing should not turn an exact routine into an
        approximate one.
*/
static decimal math_two_sum(decimal first, decimal second, decimal address_to error)
{
        decimal sum = first + second;
        decimal first_part = sum - second;
        decimal second_part = sum - first_part;
        address_to error = (first - first_part) + (second - second_part);
        return sum;
}

//      Only correct when |first| >= |second|, which every caller of it below
//      knows from the shape of what it is adding rather than from a test.
static decimal math_fast_two_sum(decimal first, decimal second, decimal address_to error)
{
        decimal sum = first + second;
        address_to error = second - (sum - first);
        return sum;
}

// Keep compensated products in the caller's registers on an FMA3 machine.
// Calling the public ABI for each residual otherwise spills all live XMM
// temporaries around a single instruction. The exact software body remains
// the fallback; the feature byte includes both ISA and OS state support.
static inline INLINE decimal math_multiply_add(decimal first, decimal second,
                                               decimal addend)
{
#if X64
        if (cpu_has_fma)
        {
                __asm__("vfmadd213sd %2, %1, %0"
                        : "+x"(first) : "x"(second), "x"(addend));
                return first;
        }
#endif
        return decimal_multiply_add(first, second, addend);
}

static decimal math_two_product(decimal first, decimal second, decimal address_to error)
{
        decimal product = first * second;
        address_to error = math_multiply_add(first, second, -product);
        return product;
}

/*
        A quotient with its own rounding repaired.

        A single divide is already correctly rounded, so a quotient of two
        exact numbers needs nothing from this. The arctangent's reduction is
        not that: the answer it produces is added to a table entry it will
        largely cancel against, so the division's half ulp arrives in the
        result multiplied by however much of the table entry survives, and it
        is worth a fused multiply-add to be rid of. One Newton step over the
        exact residual takes the division's own contribution back out.
*/
static decimal math_quotient(decimal numerator, decimal denominator)
{
        decimal quotient = numerator / denominator;
        return quotient +
               math_multiply_add(-quotient, denominator, numerator) / denominator;
}

/*
        Scaling by a power of two, which is ldexp and scalbn and the floor
        under every routine below that finishes by writing an exponent.

        Writing the exponent field directly is the obvious implementation and
        it is wrong twice: it overflows silently when the exponent leaves the
        eleven bit field, and it cannot produce a subnormal at all, because a
        subnormal is not an exponent write but a shift that loses bits and
        has to round once while losing them. Multiplying by a power of two
        instead gets both right, because the hardware multiply already knows
        how to overflow to infinity and how to round into the subnormal
        range, and it rounds exactly once.

        The three stage ladder is what keeps the multiplier itself
        representable. A single 2^n is only a double for n in [-1074, 1023],
        so an exponent outside that is walked in at most two steps of 1023 up
        or 969 down before the last multiply, and clamped after the second
        step because anything past there is going to infinity or to zero no
        matter what the argument was. 969 rather than 1022 going down because
        the second multiply has to be able to land in the subnormal range
        without having already flushed to zero on the way.

        Exact for every argument and every exponent: bit for bit equal to
        glibc's ldexp on every exponent from -2200 to 2200 crossed with four
        hundred random bit patterns, and on every one of those exponents
        crossed with both zeros, both units, both bounds of the format, the
        smallest subnormal and the largest normal. Not one disagreement.
*/
static decimal decimal_scaled(decimal value, b32 exponent)
{
        math_shape multiplier;
        decimal walked = value;

        if (exponent > 1023)
        {
                walked = walked * MATH_TWO_TO_1023;
                exponent = exponent - 1023;
                if (exponent > 1023)
                {
                        walked = walked * MATH_TWO_TO_1023;
                        exponent = exponent - 1023;
                        if (exponent > 1023)
                                exponent = 1023;
                }
        }
        else if (exponent < -1022)
        {
                walked = walked * MATH_TWO_TO_MINUS_969;
                exponent = exponent + 969;
                if (exponent < -1022)
                {
                        walked = walked * MATH_TWO_TO_MINUS_969;
                        exponent = exponent + 969;
                        if (exponent < -1022)
                                exponent = -1022;
                }
        }

        multiplier.bits = (p64)(MATH_EXPONENT_BIAS + exponent) << MATH_MANTISSA_BITS;
        return walked * multiplier.value;
}

/*
        Splitting a number into a significand and an exponent, which is
        frexp, and splitting it into a whole part and a fraction, which is
        modf.

        Neither rounds. frexp hands back a value in [1/2, 1) and an exponent
        that reconstructs the argument exactly, which for a subnormal means
        scaling it up into the normal range first and paying the shift back
        out of the exponent -- 2^54 rather than 2^52 so that the smallest
        subnormal, which has a single bit set fifty two places down, still
        arrives normalised. Zero, infinity and NaN come back unchanged with
        an exponent of zero, which is what C requires and what a bare
        exponent field read would get wrong for all three.

        Both were checked against glibc on twenty million random bit
        patterns, value and exponent and both halves: not one disagreement.

        modf's whole part is a truncation toward zero, so decimal_truncated
        is the whole of it, and the fraction is the exact difference -- exact
        because subtracting the truncation of a number from the number is a
        Sterbenz subtraction whenever the result is not zero. The signed
        zeros are the part worth writing down: modf of -0.0 is -0.0 with a
        whole part of -0.0, and modf of -3.0 is -0.0 with a whole part of
        -3.0, so the sign has to be transplanted rather than left to fall out
        of the arithmetic.
*/
static decimal decimal_split_exponent(decimal value, b32 address_to exponent)
{
        math_shape shape;
        b32 field;

        shape.value = value;
        field = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff);

        if (field == 0x7ff || (shape.bits & MATH_MAGNITUDE_MASK) == 0)
        {
                address_to exponent = 0;
                return value;
        }

        if (field == 0)
        {
                shape.value = value * 18014398509481984.0; // two to the fifty four
                field = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff);
                address_to exponent = field - (MATH_EXPONENT_BIAS - 1) - 54;
        }
        else
        {
                address_to exponent = field - (MATH_EXPONENT_BIAS - 1);
        }

        shape.bits = (shape.bits & ~(0x7ffULL << MATH_MANTISSA_BITS)) |
                     ((p64)(MATH_EXPONENT_BIAS - 1) << MATH_MANTISSA_BITS);
        return shape.value;
}

static decimal decimal_split_whole(decimal value, decimal address_to whole)
{
        decimal integral;
        math_shape shape;

        if (!decimal_is_finite(value))
        {
                address_to whole = value;
                //      An infinity keeps its sign and leaves a zero of that
                //      sign behind; a NaN leaves a NaN in both halves.
                return decimal_is_nan(value) ? value : decimal_with_sign(0.0, value);
        }

        integral = decimal_truncated(value);
        address_to whole = integral;

        shape.value = value - integral;
        //      A whole argument leaves a zero, and that zero has to wear the
        //      argument's sign rather than the sign the subtraction gave it.
        if (shape.value == 0.0)
                return decimal_with_sign(0.0, value);
        return shape.value;
}

/*
        The two that have exact answers, done exactly.

        fmod and remainder are not approximations of anything. x - n*y with n
        an integer is a value the format can hold, always, and a library that
        answers it to within an ulp has got it wrong rather than got it
        nearly right. The tempting implementation -- multiply y by the
        truncated quotient and subtract -- destroys that: the quotient
        rounds, the product rounds, and the subtraction of two nearly equal
        large numbers hands back noise. For x = 1e300 and y = 3.0 it does not
        get a single bit right.

        So this is the long division everyone eventually writes: put both
        significands in integer registers with the implied bit restored,
        subtract when you can, shift, and count down the exponent difference.
        Every step is integer arithmetic on values below 2^54, nothing
        rounds, and the answer that comes out is the answer. The cost is one
        iteration per bit of exponent difference, which for the worst pair a
        double can hold is a little over two thousand -- and there is no
        cheaper way to be right, which is why glibc's is the same loop.

        The quotient's low bits are carried out alongside, because remainder
        needs the parity of the quotient to break a tie and there is no way
        to recover it afterwards from the remainder alone.

        Verified exact against glibc's fmod and remainder over twenty
        million pairs of random bit patterns -- which is most of the special
        values for free -- and four million more built to have large exponent
        differences, so that the loop below runs its full two thousand
        iterations. Identical bit patterns everywhere, both zeros and every
        special value included, and not one disagreement.
*/
static decimal math_modulo_quotient(decimal left, decimal right, p64 address_to quotient)
{
        math_shape numerator, denominator;
        p64 top, bottom, step;
        b32 top_exponent, bottom_exponent;
        p64 sign;
        p64 counted = 0;

        numerator.value = left;
        denominator.value = right;
        sign = numerator.bits & MATH_SIGN_MASK;

        top_exponent = (b32)((numerator.bits >> MATH_MANTISSA_BITS) & 0x7ff);
        bottom_exponent = (b32)((denominator.bits >> MATH_MANTISSA_BITS) & 0x7ff);

        //      A zero or NaN divisor, an infinite or NaN dividend: every one
        //      of these is a NaN, and producing it as zero over zero rather
        //      than as a constant keeps the quiet bit and the payload
        //      whichever operand carried one.
        if ((denominator.bits << 1) == 0 || top_exponent == 0x7ff ||
            decimal_is_nan(right))
        {
                address_to quotient = 0;
                return (left * right) / (left * right);
        }

        //      Nothing to divide: the dividend is already the remainder, and
        //      an exactly equal magnitude leaves a zero wearing its sign.
        if ((numerator.bits << 1) <= (denominator.bits << 1))
        {
                address_to quotient = ((numerator.bits << 1) == (denominator.bits << 1)) ? 1 : 0;
                if ((numerator.bits << 1) == (denominator.bits << 1))
                        return 0.0 * left;
                return left;
        }

        //      Restore the implied bit, or normalise a subnormal by hand and
        //      pay for it out of the exponent, so that both significands are
        //      integers with the same interpretation.
        top = numerator.bits;
        if (top_exponent == 0)
        {
                for (step = top << 12; (step >> 63) == 0; top_exponent--, step <<= 1)
                        ;
                top <<= -top_exponent + 1;
        }
        else
        {
                top &= MATH_MANTISSA_MASK;
                top |= MATH_IMPLIED_BIT;
        }

        bottom = denominator.bits;
        if (bottom_exponent == 0)
        {
                for (step = bottom << 12; (step >> 63) == 0; bottom_exponent--, step <<= 1)
                        ;
                bottom <<= -bottom_exponent + 1;
        }
        else
        {
                bottom &= MATH_MANTISSA_MASK;
                bottom |= MATH_IMPLIED_BIT;
        }

        //      One quotient bit per exponent of difference. The subtraction
        //      is tried unsigned and kept only when it did not borrow, which
        //      is the sign bit of the difference read as a flag rather than
        //      a comparison and a branch on its result.
        for (; top_exponent > bottom_exponent; top_exponent--)
        {
                step = top - bottom;
                counted <<= 1;
                if ((step >> 63) == 0)
                {
                        top = step;
                        counted |= 1;
                }
                top <<= 1;
        }
        step = top - bottom;
        counted <<= 1;
        if ((step >> 63) == 0)
        {
                top = step;
                counted |= 1;
        }
        address_to quotient = counted;

        if (top == 0)
                return 0.0 * left;

        //      Renormalise: shift the significand back up until the implied
        //      bit is where the format wants it, and spend the shifts out of
        //      the exponent. An exponent that walks off the bottom means the
        //      answer is subnormal, and then the significand shifts down
        //      instead and the exponent field stays zero.
        for (; (top >> MATH_MANTISSA_BITS) == 0; top <<= 1, top_exponent--)
                ;

        if (top_exponent > 0)
        {
                top -= MATH_IMPLIED_BIT;
                top |= (p64)top_exponent << MATH_MANTISSA_BITS;
        }
        else
        {
                top >>= -top_exponent + 1;
        }

        numerator.bits = top | sign;
        return numerator.value;
}

static decimal decimal_modulo(decimal left, decimal right)
{
        p64 ignored;
        return math_modulo_quotient(left, right, address_of ignored);
}

/*
        The IEEE remainder, which is fmod with the quotient rounded to
        nearest instead of toward zero.

        The remainder is taken first and then pulled down by one divisor if
        it is past the halfway point, and the comparison that decides is
        written as `left_over > divisor - left_over` rather than
        `2*left_over > divisor` or `left_over > divisor/2` because both of
        those can go wrong at the ends of the range: doubling overflows for a
        divisor near the top of the format, and halving underflows to zero
        for the smallest subnormal, which is exactly the pair where the
        answer matters. The difference `divisor - left_over` is exact
        whenever the comparison is close, by Sterbenz, and when it is not
        close the gap is enormous and no rounding can flip it.

        A remainder exactly at the halfway point goes to whichever side
        leaves an even quotient, which is what makes remainder(x, y)
        symmetric and is why the quotient's low bit had to be carried out of
        the loop.

        The sign at the end is the dividend's, and it is transplanted rather
        than computed, so that a zero remainder from a negative dividend is a
        negative zero.
*/
static decimal decimal_remainder(decimal left, decimal right)
{
        decimal magnitude_left = math_magnitude(left);
        decimal magnitude_right = math_magnitude(right);
        decimal left_over;
        decimal complement;
        p64 quotient;

        if (decimal_is_nan(left) || decimal_is_nan(right) ||
            decimal_is_infinite(left) || right == 0.0)
                return (left * right) / (left * right);

        if (decimal_is_infinite(right))
                return left;

        left_over = math_modulo_quotient(magnitude_left, magnitude_right, address_of quotient);
        complement = magnitude_right - left_over;

        if (left_over > complement || (left_over == complement && (quotient & 1)))
                left_over = left_over - magnitude_right;

        return decimal_sign_bit(left) ? -left_over : left_over;
}

/*
        e to the r minus one, for a reduced r, which is the engine under
        exp, exp2, expm1, pow, sinh, cosh and tanh.

        Everything that exponentiates arrives here with r already brought
        into [-ln2/2, ln2/2], a little over a third either way, and asks for
        e^r. It is written as e^r - 1 rather than e^r because half its
        callers want the difference from one and would have to take it back
        by subtracting, which for a small r throws away every bit that
        mattered.

        The polynomial is the Taylor series and nothing cleverer, carried to
        the fifteenth term. That is a deliberate choice over a minimax fit of
        half the degree. A minimax polynomial has to come from somewhere --
        a Remez exchange run in arbitrary precision -- and the coefficients
        that come out of it cannot be checked by reading them. The Taylor
        coefficients are 1/n!, they are exact rationals, anyone can verify
        every one of them with a calculator, and the truncation error is a
        single term that can be bounded on paper: the first term dropped is
        r^16/16!, which at the worst r in range is 2 parts in 10^21, four
        orders below the last bit of the answer. The price is nine more
        multiply-adds than a minimax fit would need, which is real and is
        paid in latency rather than in accuracy, and is the right way round
        for a first version of a library that has none.

        The evaluation is Horner in r on the coefficients from 1/15! down to
        1/2!, and the result is folded back as r + r*r*polynomial so that the
        leading term, which carries almost all of the value, never passes
        through a rounding at all.
*/
#define MATH_EXP_C2 0.5
#define MATH_EXP_C3 0.16666666666666666
#define MATH_EXP_C4 0.041666666666666664
#define MATH_EXP_C5 0.008333333333333333
#define MATH_EXP_C6 0.001388888888888889
#define MATH_EXP_C7 0.0001984126984126984
#define MATH_EXP_C8 2.48015873015873e-05
#define MATH_EXP_C9 2.7557319223985893e-06
#define MATH_EXP_C10 2.755731922398589e-07
#define MATH_EXP_C11 2.505210838544172e-08
#define MATH_EXP_C12 2.08767569878681e-09
#define MATH_EXP_C13 1.6059043836821613e-10
#define MATH_EXP_C14 1.1470745597729725e-11
#define MATH_EXP_C15 7.647163731819816e-13

static decimal math_exponential_minus_one_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;
        decimal walked;

        walked = MATH_EXP_C15;
        walked = MATH_EXP_C14 + reduced * walked;
        walked = MATH_EXP_C13 + reduced * walked;
        walked = MATH_EXP_C12 + reduced * walked;
        walked = MATH_EXP_C11 + reduced * walked;
        walked = MATH_EXP_C10 + reduced * walked;
        walked = MATH_EXP_C9 + reduced * walked;
        walked = MATH_EXP_C8 + reduced * walked;
        walked = MATH_EXP_C7 + reduced * walked;
        walked = MATH_EXP_C6 + reduced * walked;
        walked = MATH_EXP_C5 + reduced * walked;
        walked = MATH_EXP_C4 + reduced * walked;
        walked = MATH_EXP_C3 + reduced * walked;
        walked = MATH_EXP_C2 + reduced * walked;

        return reduced + squared * walked;
}

//      The nearest whole number, as an integer. decimal_rounded is one
//      instruction on arm64 and riscv64 and six on x86_64, and it rounds
//      halfway cases away from zero, which is what makes the reduced
//      remainder land inside half a step either way rather than a whole
//      step on one side.
#define math_nearest_whole(value) ((b32)decimal_rounded(value))

/*
        e to the x.

        Two lines of reduction and one of reassembly. The integer k nearest
        to x/ln2 is taken out first, leaving r = x - k*ln2 no larger than
        ln2/2, and e^x is then e^r scaled by 2^k -- a scaling that is exact
        for every result the format can hold and that rounds exactly once
        when the result is subnormal, because decimal_scaled multiplies
        rather than writing the exponent field.

        The reduction subtracts k*ln2 in two pieces, and the split of ln2 is
        the whole reason it works. MATH_LN2_HEAD is ln 2 rounded to thirty
        two significant bits, so k*MATH_LN2_HEAD is an exact product for any
        k a double exponent can produce -- k never exceeds 1075 and eleven
        bits of k against thirty two of the constant is forty three, well
        inside the fifty three available. The first subtraction is therefore
        exact, and the second removes what the head could not carry. What is
        left unaccounted for after both is about 2^-90 times k, which against
        an r of a third is thirty five bits below the last one that shows.

        The guards at the top are not decoration. Without the overflow test,
        x/ln2 for a very large x rounds to an integer past what a b32 holds
        and the conversion is undefined; without the small-argument test,
        e^x for x below 2^-54 would go all the way round the reduction to
        return exactly 1 and lose the x that should still have been there.

        Measured: worst 1 ulp, 99.86 percent bit identical, over the whole
        finite range including the subnormal tail below -708. The ulp is
        spent in the polynomial and in the single rounding of 1 + (e^r - 1),
        and there is nowhere in the domain that is worse.
*/
static decimal exponential(decimal value)
{
        b32 scale;
        decimal reduced;
        decimal difference;

        if (decimal_is_nan(value))
                return value;

        if (value > 709.782712893384)
                return MATH_HUGE * MATH_HUGE;

        if (value < -745.1332191019412)
                return 0.0;

        //      Below this the series is 1 + x to every bit the format has,
        //      and going round the reduction would lose the x entirely.
        if (math_magnitude(value) < MATH_TWO_TO_MINUS_54)
                return 1.0 + value;

        scale = math_nearest_whole(value * MATH_LOG2E);
        difference = value - (decimal)scale * MATH_LN2_HEAD;
        reduced = difference - (decimal)scale * MATH_LN2_HEAD_TAIL;

        return decimal_scaled(1.0 + math_exponential_minus_one_reduced(reduced), scale);
}

/*
        Two to the x, which is not e to the x times a constant.

        Writing exp2 as exp(x * ln2) would round x*ln2 once before the
        reduction ever started, and that rounding is an absolute error of up
        to 2^-53 times a thousand in the exponent -- eleven bits of the
        answer, gone before any work was done. Doing it in the right order
        instead costs nothing: the integer part of x comes off exactly,
        because subtracting a nearby integer from a double is exact, and only
        the fraction r, which is at most a half, is multiplied by ln2. That
        product is then taken in two pieces with math_two_product so that the
        part the multiply discarded is still available, and it is folded back
        in as a first order correction to the exponential -- e^(a+d) is
        e^a*(1+d) to well past the last bit when d is 2^-53 of a half.

        Measured: worst 1 ulp, 90.8 percent bit identical, over [-1075,
        1024] including the subnormal tail; every integer argument is
        exact.
*/
static decimal exponential_two(decimal value)
{
        b32 scale;
        decimal fraction;
        decimal product;
        decimal product_error;
        decimal grown;

        if (decimal_is_nan(value))
                return value;

        if (value >= 1024.0)
                return MATH_HUGE * MATH_HUGE;

        if (value < -1075.0)
                return 0.0;

        if (math_magnitude(value) < MATH_TWO_TO_MINUS_54)
                return 1.0 + value * MATH_LN2;

        scale = math_nearest_whole(value);
        fraction = value - (decimal)scale;

        product = math_two_product(fraction, MATH_LN2, address_of product_error);
        product_error = product_error + fraction * MATH_LN2_TAIL;

        grown = math_exponential_minus_one_reduced(product);
        grown = grown + product_error * (1.0 + grown);

        return decimal_scaled(1.0 + grown, scale);
}

/*
        e to the x, minus one, kept accurate where the difference is the
        whole answer.

        For a small x, e^x is one plus something tiny, and computing it and
        then subtracting one throws every significant bit away: at x = 1e-10
        the difference has one correct digit. The series for e^x - 1 does not
        have that problem because it never forms the one.

        Inside the reduction band there is nothing to do but call the
        polynomial. Outside it the reduction has to happen, and the answer is
        reassembled as 2^k*(1 + E) - 1 written so that the two large terms
        that could cancel are the exact ones: 2^k - 1 is exact for every k a
        double exponent allows below fifty three, and 2^k*E is the small
        piece added to it. Above fifty three the one has no effect on the
        answer at all and the subtraction is dropped.

        This is not one of the functions the task asked for; it is here
        because sinh, cosh and tanh cannot be accurate near zero without it,
        and having written it there is no reason to hide it.

        Measured: worst 1 ulp, 99.94 percent bit identical. Below 2^-30 the
        answer is bit identical everywhere.
*/
static decimal exponential_minus_one(decimal value)
{
        b32 scale;
        decimal reduced;
        decimal difference;
        decimal grown;
        decimal scaled_one;

        if (decimal_is_nan(value))
                return value;

        if (value > 709.782712893384)
                return MATH_HUGE * MATH_HUGE;

        if (value < -37.0)
                return -1.0 + exponential(value);

        //      Below this the square term is past the last bit, and the
        //      series would turn a negative zero into a positive one.
        if (math_magnitude(value) < MATH_TWO_TO_MINUS_54)
                return value;

        if (math_magnitude(value) < 0.6)
                return math_exponential_minus_one_reduced(value);

        scale = math_nearest_whole(value * MATH_LOG2E);
        difference = value - (decimal)scale * MATH_LN2_HEAD;
        reduced = difference - (decimal)scale * MATH_LN2_HEAD_TAIL;
        grown = math_exponential_minus_one_reduced(reduced);

        if (scale > 53)
                return decimal_scaled(1.0 + grown, scale);

        scaled_one = decimal_scaled(1.0, scale);
        return (scaled_one - 1.0) + decimal_scaled(grown, scale);
}

/*
        The logarithm, in the two pieces every one of its users needs.

        There is one reduction and one series here, and logarithm, log2,
        log10 and pow all take their answer from it. The argument is split as
        x = 2^k * m with m brought into [sqrt(1/2), sqrt(2)) rather than the
        [1, 2) the exponent field hands over, because the series that follows
        is in f = m - 1 and a band centred on one keeps |f| at 0.41 instead
        of letting it reach 1, where nothing converges.

        The series itself is the inverse hyperbolic tangent, not the
        logarithm's own Taylor expansion. With s = f/(2+f),

              log(1+f) = 2*atanh(s) = 2s + 2s^3/3 + 2s^5/5 + ...

        and |s| never exceeds 0.1716, so s^2 never exceeds 0.0295 and the
        terms fall by a factor of thirty four each time. Eleven of them put
        the truncation four orders below the last bit. The alternating series
        in f would have needed forty at the same accuracy, and would have
        alternated, which is worse than slow.

        The algebra that turns 2s + s*R back into something whose leading
        term is f rather than 2s is worth spelling out, because it is what
        makes the result accurate rather than merely convergent. From
        s(2+f) = f comes 2s = f - f*s, so with hfsq = f*f/2 and 1 - s = 2s/f,

              f - hfsq + s*(hfsq + R) = f - hfsq*(1-s) + s*R
                                      = f - f*s + s*R
                                      = 2s + s*R

        The right hand side is the series; the left hand side is what is
        actually computed. They are equal, but the left hand side leads with
        f, which is exact -- m - 1 is a Sterbenz subtraction for every m in
        the band -- and confines s, which carries a rounding from a division,
        to a term of size f^2. A one ulp error in s therefore arrives in the
        answer scaled down by a factor of f, and stops mattering.

        For pow the result has to be better than a double can hold, so the
        whole of it is carried in two doubles. s gets a low half from the
        exact residual of its own division, s^2 gets one from
        math_two_product, and the leading term of R -- two thirds of s^2,
        which is nine tenths of R -- gets one as well. Only the tail of the
        series beyond that leading term is left in single precision, and it
        is small enough that its last bit is a hundred and ten places below
        the answer's first. The pair that comes out is good to about 2^-100
        relative, which is what lets pow multiply it by an exponent of a
        thousand and still have fifty three bits left.
*/
#define MATH_LOG_P0 0.6666666666666666
#define MATH_LOG_P0_TAIL 3.700743415417188e-17
#define MATH_LOG_P1 0.4
#define MATH_LOG_P1_TAIL -2.2204460492503132e-17
#define MATH_LOG_P2 0.2857142857142857
#define MATH_LOG_P3 0.2222222222222222
#define MATH_LOG_P4 0.18181818181818182
#define MATH_LOG_P5 0.15384615384615385
#define MATH_LOG_P6 0.13333333333333333
#define MATH_LOG_P7 0.11764705882352941
#define MATH_LOG_P8 0.10526315789473684
#define MATH_LOG_P9 0.09523809523809523
#define MATH_LOG_P10 0.08695652173913043

//      Splits the argument into 2^k times a significand in the band, and
//      answers the natural logarithm of that significand as a head and a
//      tail. The argument has already been checked for zero, for a negative
//      sign and for the two non-finite classes by every caller.
static decimal math_log_pieces(decimal value, b32 address_to power,
                               decimal address_to tail)
{
        math_shape shape;
        b32 exponent;
        p64 mantissa;
        decimal significand;
        decimal offset;
        decimal sum, sum_error;
        decimal ratio, ratio_low, residual;
        decimal squared, squared_error, squared_low;
        decimal fourth, fourth_error;
        decimal lead, lead_error;
        decimal second, second_error;
        decimal series;
        decimal correction_high, correction_low;
        decimal product, product_error;
        decimal doubled, doubled_low;
        decimal head, head_error;

        shape.value = value;

        //      A subnormal has no implied bit to read an exponent from, so
        //      it is lifted into the normal range and the lift is paid back
        //      out of k.
        exponent = 0;
        if ((shape.bits >> MATH_MANTISSA_BITS) == 0)
        {
                shape.value = value * 18014398509481984.0; // two to the fifty four
                exponent = -54;
        }

        exponent += (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;
        mantissa = shape.bits & MATH_MANTISSA_MASK;

        //      Centre the band on one: a significand at or above the square
        //      root of two becomes half of itself, and the halving is paid
        //      into k.
        if (mantissa >= MATH_SQRT2_MANTISSA)
        {
                exponent += 1;
                shape.bits = mantissa | ((p64)(MATH_EXPONENT_BIAS - 1) << MATH_MANTISSA_BITS);
        }
        else
        {
                shape.bits = mantissa | ((p64)MATH_EXPONENT_BIAS << MATH_MANTISSA_BITS);
        }

        significand = shape.value;
        address_to power = exponent;

        //      Exact for every significand in the band, by Sterbenz.
        offset = significand - 1.0;

        //      s = f/(2+f), with the divisor carried in two pieces and the
        //      division's own residual recovered by a fused multiply-add, so
        //      that s has a low half as exact as the format allows.
        sum = math_fast_two_sum(2.0, offset, address_of sum_error);
        ratio = offset / sum;
        residual = math_multiply_add(-ratio, sum, offset) - ratio * sum_error;
        ratio_low = residual / sum;

        squared = math_two_product(ratio, ratio, address_of squared_error);
        squared_low = squared_error + 2.0 * ratio * ratio_low;

        fourth = math_two_product(squared, squared, address_of fourth_error);
        fourth_error = fourth_error + 2.0 * squared * squared_low;

        //      The first two terms of the series carry all but a part in ten
        //      thousand of the correction, and pow needs the correction to
        //      seventy bits, so both of them are taken exactly and only the
        //      third term onward is left in a single double.
        //
        //      Both of them need their COEFFICIENT in two pieces as well,
        //      which is the trap this walked into once. Two thirds is not a
        //      double. Multiplying the square exactly by the nearest double
        //      to two thirds is still a relative error of 2^-54 in the
        //      largest term of the correction, and that alone held the pair
        //      to sixty one bits and cost pow four units in the last place
        //      at the far end of its range -- an error introduced by a
        //      constant, in a routine where every operation around it was
        //      exact.
        lead = math_two_product(squared, MATH_LOG_P0, address_of lead_error);
        lead_error = lead_error + MATH_LOG_P0 * squared_low +
                     MATH_LOG_P0_TAIL * squared;

        second = math_two_product(fourth, MATH_LOG_P1, address_of second_error);
        second_error = second_error + MATH_LOG_P1 * fourth_error +
                       MATH_LOG_P1_TAIL * fourth;

        series = MATH_LOG_P10;
        series = MATH_LOG_P9 + squared * series;
        series = MATH_LOG_P8 + squared * series;
        series = MATH_LOG_P7 + squared * series;
        series = MATH_LOG_P6 + squared * series;
        series = MATH_LOG_P5 + squared * series;
        series = MATH_LOG_P4 + squared * series;
        series = MATH_LOG_P3 + squared * series;
        series = MATH_LOG_P2 + squared * series;

        //      Renormalising here rather than at the end is the whole of
        //      why this pair is worth carrying. Each piece below is four
        //      orders under the one before it but eighteen orders ABOVE that
        //      one's last bit, so leaving them stacked in a low half would
        //      make the pair a hundred and six bits wide on paper and sixty
        //      in fact -- and pow, which multiplies this by a thousand,
        //      would give back a result wrong in its last five bits. So
        //      every sum is folded back into a proper head and tail before
        //      the next one uses it.
        correction_high = math_fast_two_sum(lead, second, address_of correction_low);
        correction_high = math_fast_two_sum(correction_high,
                                            correction_low + lead_error +
                                                    second_error +
                                                    (fourth * squared) * series,
                                            address_of correction_low);

        product = math_two_product(ratio, correction_high, address_of product_error);
        product_error = product_error + ratio * correction_low +
                        ratio_low * correction_high;

        doubled = ratio + ratio;
        doubled_low = ratio_low + ratio_low;

        //      The two terms have the same sign, always, so the cheap sum is
        //      the right one and the head is the larger of the pair.
        head = math_fast_two_sum(doubled, product, address_of head_error);
        head = math_fast_two_sum(head, head_error + doubled_low + product_error,
                                 address_of head_error);
        address_to tail = head_error;
        return head;
}

static inline INLINE bool math_log_special(decimal value, decimal address_to answer)
{
        if (decimal_is_nan(value))
                *answer = value;
        else if (value < 0.0)
                *answer = (value - value) / (value - value);
        else if (value == 0.0)
                *answer = -MATH_HUGE / (value * value);
        else if (decimal_is_infinite(value))
                *answer = value;
        else
                return false;
        return true;
}

/*
        The natural logarithm, which cannot be called log.

        `log` in this library is the buffered writer that every
        string_format call in the tree passes as its first argument, and it
        was there first. So this is `logarithm`, aliased `ln`, and a program
        that wants the C spelling has to decide for itself what to break.

        The reassembly is k*ln2 plus the significand's logarithm, with ln2
        again split so that k*head is exact, and with the two large terms
        summed through math_two_sum so that the bits of the significand's
        logarithm that fall off the bottom of a large k*ln2 are recovered
        rather than lost. There is no cancellation to fear anywhere in the
        domain: an argument whose logarithm is near zero is an argument near
        one, and an argument near one lands in the band with k equal to zero,
        so the large term is not there to cancel against.

        Measured: worst 1 ulp, and bit identical on every one of eight
        million arguments spread from 1e-307 to 1e307 -- not a single
        disagreement with the reference anywhere in that sweep. The 1 ulp is
        from a dense sweep of the decade either side of one, where it happens
        once in a few hundred thousand.
*/
static decimal logarithm(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal large, large_error;
        decimal special;

        if (math_log_special(value, address_of special))
                return special;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        large = math_two_sum((decimal)exponent * MATH_LN2_HEAD, head,
                             address_of large_error);
        return large + (large_error + tail +
                        (decimal)exponent * MATH_LN2_HEAD_TAIL);
}

/*
        The base two logarithm, where the exponent is the answer's whole part
        and comes out exact.

        log2 is the one of the three where the reduction and the base agree:
        k is not scaled by anything, it is added, so every power of two in
        the domain answers its own exponent to the bit. What has to be
        careful is the other term -- the significand's natural logarithm
        multiplied by log2(e) -- and that multiplication is taken through
        math_two_product so the bits it discarded can be added back along
        with the tail of log2(e) itself, which a single double cannot hold.

        Measured: worst 1 ulp, 99.97 percent bit identical, over the whole
        finite positive range; every exact power of two answers exactly.
*/
static decimal logarithm_two(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal scaled, scaled_error;
        decimal total, total_error;
        decimal special;

        if (math_log_special(value, address_of special))
                return special;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        scaled = math_two_product(head, MATH_LOG2E, address_of scaled_error);
        scaled_error = scaled_error + head * MATH_LOG2E_TAIL + tail * MATH_LOG2E;

        total = math_two_sum((decimal)exponent, scaled, address_of total_error);
        return total + (total_error + scaled_error);
}

/*
        The base ten logarithm, where neither term is exact and both need a
        tail.

        k*log10(2) is not an integer and not exact, so unlike log2 it has to
        be taken as a two piece product of its own before the significand's
        contribution is added to it. That is the only difference from log2,
        and it is the reason log10 of a power of ten is not guaranteed exact
        here -- nor is it in glibc, for the same reason: log10(1000) is not a
        value the format holds.

        Measured: worst 1 ulp, 99.97 percent bit identical, over the whole
        finite positive range. glibc's own double log10 is out by two ulp on
        the same sweep, so a program moving between the two will see this one
        disagree with it and be the one that is right.
*/
static decimal logarithm_ten(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal from_exponent, from_exponent_error;
        decimal scaled, scaled_error;
        decimal total, total_error;
        decimal special;

        if (math_log_special(value, address_of special))
                return special;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        from_exponent = math_two_product((decimal)exponent, MATH_LOG10_2,
                                         address_of from_exponent_error);
        from_exponent_error = from_exponent_error +
                              (decimal)exponent * MATH_LOG10_2_TAIL;

        scaled = math_two_product(head, MATH_LOG10E, address_of scaled_error);
        scaled_error = scaled_error + head * MATH_LOG10E_TAIL + tail * MATH_LOG10E;

        total = math_two_sum(from_exponent, scaled, address_of total_error);
        return total + (total_error + from_exponent_error + scaled_error);
}

//      log2 of a positive finite argument, carried in two doubles, which is
//      what pow needs and no public entry does.
static decimal math_log_two_pieces(decimal value, decimal address_to tail)
{
        b32 exponent;
        decimal head, head_tail;
        decimal scaled, scaled_error;
        decimal total, total_error;

        head = math_log_pieces(value, address_of exponent, address_of head_tail);

        scaled = math_two_product(head, MATH_LOG2E, address_of scaled_error);
        scaled = math_fast_two_sum(scaled,
                                   scaled_error + head * MATH_LOG2E_TAIL +
                                           head_tail * MATH_LOG2E,
                                   address_of scaled_error);

        total = math_two_sum((decimal)exponent, scaled, address_of total_error);
        address_to tail = total_error + scaled_error;
        return total;
}

/*
        Is the exponent a whole number, and is it odd?

        pow needs this three times over -- to decide whether a negative base
        is legal at all, to decide the sign of the answer when it is, and to
        decide what an infinite or zero base does -- and the answer has to be
        right for exponents far past where every double is already an even
        integer. Above 2^53 the last bit of a double is worth two, so every
        value there is even, and asking about its parity by halving it would
        be a rounding rather than a question. The exponent field says it
        instead: a value whose exponent puts its lowest set bit at or above
        the ones place is whole, and it is odd exactly when the ones place is
        the lowest bit it has.
*/
#define MATH_NOT_WHOLE 0
#define MATH_ODD_WHOLE 1
#define MATH_EVEN_WHOLE 2

static b32 math_whole_kind(decimal value)
{
        math_shape shape;
        b32 exponent;

        shape.value = value;
        exponent = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;

        if (exponent < 0)
        {
                //      Smaller than one in magnitude: whole only if zero.
                return (shape.bits & MATH_MAGNITUDE_MASK) == 0 ? MATH_EVEN_WHOLE
                                                               : MATH_NOT_WHOLE;
        }
        if (exponent > MATH_MANTISSA_BITS)
        {
                //      Every bit the format still has is worth two or more,
                //      including the infinities and the NaNs, which fall
                //      here and are never asked about after the special
                //      cases above have run.
                return MATH_EVEN_WHOLE;
        }
        if ((shape.bits << (12 + exponent)) != 0)
                return MATH_NOT_WHOLE;
        return ((shape.bits >> (MATH_MANTISSA_BITS - exponent)) & 1) ? MATH_ODD_WHOLE
                                                                    : MATH_EVEN_WHOLE;
}

/*
        The general power, which is the hardest routine in the file and the
        one whose error is not a constant.

        x^y is 2^(y * log2 x) and the whole difficulty is in the exponent.
        Its magnitude can reach a thousand before the result leaves the
        format, and an absolute error of eps in it becomes a relative error
        of ln2 * eps in the answer. Turn that around: to have the answer
        right to a last bit, y * log2 x must be right to about 2^-63 --
        eleven bits more than a double holds, and the reason log2 is computed
        here to a hundred bits and multiplied out in two pieces rather than
        one. Anything less and pow is not slightly wrong at the edges, it is
        wrong by hundreds of units in the last place.

        That is also why the error is quoted as a function of the exponent
        rather than as one number. The relative error of the result is
        0.693 * |y * log2 x| * (the relative error of log2 x), so a call
        whose exponent is small is far more accurate than a call whose result
        is near the overflow boundary, and quoting only the worst would
        misrepresent every ordinary use.

        MEASURED, on forty million random pairs plus every special case C99
        lists, bucketed by that exponent:

              |y*log2 x| < 1           worst 1 ulp, 97.8 percent identical
              |y*log2 x| < 32          worst 1 ulp, 90.0 percent identical
              |y*log2 x| < 256         worst 1 ulp, 90.0 percent identical
              |y*log2 x| up to 1075    worst 1 ulp, 90.1 percent identical

        It stays inside one ulp all the way out, which is what the hundred
        bit logarithm bought. An earlier version of that logarithm carried a
        coefficient of two thirds as a single double -- one rounded constant
        in an otherwise exact chain -- and the last bucket was four ulp.

        The special cases are the other half of the routine and there are
        twenty of them. They are in the order C99 Annex F puts them, and the
        order matters: pow(1, NaN) is 1 and pow(NaN, 0) is 1, so both of
        those have to be answered before anything looks at whether an
        argument is a NaN.
*/
static decimal power(decimal base, decimal exponent)
{
        b32 kind;
        decimal magnitude;
        decimal sign = 1.0;
        decimal log_head, log_tail;
        decimal product, product_error, product_tail, total;
        b32 scale;
        decimal reduced, reduced_error;
        decimal grown;

        //      A zero exponent is one for every base there is, a NaN
        //      included, and a base of exactly one is one for every
        //      exponent, a NaN included. Both before the NaN tests.
        if (exponent == 0.0)
                return 1.0;
        if (base == 1.0)
                return 1.0;

        if (decimal_is_nan(base) || decimal_is_nan(exponent))
                return base + exponent;

        kind = math_whole_kind(exponent);

        if (decimal_is_infinite(exponent))
        {
                //      A base of minus one is the one magnitude that neither
                //      grows nor shrinks, so it answers one either way.
                if (base == -1.0)
                        return 1.0;
                magnitude = math_magnitude(base);
                if (magnitude > 1.0)
                        return exponent > 0.0 ? MATH_HUGE * MATH_HUGE : 0.0;
                return exponent > 0.0 ? 0.0 : MATH_HUGE * MATH_HUGE;
        }

        if (decimal_is_infinite(base))
        {
                //      A negative infinity raised to an odd whole exponent
                //      keeps its sign; every other case loses it.
                decimal answer = exponent > 0.0 ? MATH_HUGE * MATH_HUGE : 0.0;
                if (base < 0.0 && kind == MATH_ODD_WHOLE)
                        return -answer;
                return answer;
        }

        if (base == 0.0)
        {
                if (exponent < 0.0)
                {
                        decimal answer = 1.0 / (base * base);
                        return (kind == MATH_ODD_WHOLE && decimal_sign_bit(base))
                                       ? -answer
                                       : answer;
                }
                return (kind == MATH_ODD_WHOLE) ? base : 0.0;
        }

        if (base < 0.0)
        {
                //      A negative base has no real power unless the exponent
                //      is a whole number, and then the sign is the exponent's
                //      parity.
                if (kind == MATH_NOT_WHOLE)
                        return (base - base) / (base - base);
                if (kind == MATH_ODD_WHOLE)
                        sign = -1.0;
        }

        magnitude = math_magnitude(base);

        log_head = math_log_two_pieces(magnitude, address_of log_tail);

        //      One cheap product before the careful one. The careful one is
        //      a fused multiply-add against its own result, and if that
        //      result has already overflowed to infinity the fused operation
        //      hands back a NaN and every test after it is false -- which is
        //      how an exponent of DBL_MAX turned into a NaN rather than into
        //      the infinity it obviously is. The loose bounds here are far
        //      enough outside the real ones that no argument can be decided
        //      wrongly by this product's own rounding; the real bounds are
        //      applied below, to the accurate sum.
        total = exponent * log_head;
        if (total > 1100.0)
                return sign * (MATH_HUGE * MATH_HUGE);
        if (total < -1200.0)
                return sign * 0.0;

        product = math_two_product(exponent, log_head, address_of product_error);
        product_tail = product_error + exponent * log_tail;
        total = product + product_tail;

        //      The result leaves the format before the reduction can be
        //      trusted to stay inside a b32, so both ends are caught here.
        if (total > 1024.0)
                return sign * (MATH_HUGE * MATH_HUGE);
        if (total < -1080.0)
                return sign * 0.0;

        scale = math_nearest_whole(total);
        //      Exact: the difference of a double below 1080 and a nearby
        //      whole number needs at most forty three bits.
        reduced = (product - (decimal)scale) + product_tail;

        reduced = math_two_product(reduced, MATH_LN2, address_of reduced_error);
        reduced_error = reduced_error + (total - (decimal)scale) * MATH_LN2_TAIL;

        grown = math_exponential_minus_one_reduced(reduced);
        grown = grown + reduced_error * (1.0 + grown);

        return sign * decimal_scaled(1.0 + grown, scale);
}

/*
        The cube root, which is the one root that is defined for a negative
        argument and therefore cannot be had from a logarithm.

        The sign comes off first and goes back on last, which is what makes
        cbrt(-8) equal -2 rather than a NaN, and the magnitude is brought
        into [1/2, 4) by taking the exponent out in multiples of three. The
        remaining one or two thirds of an exponent stay in the argument
        rather than being multiplied back in as a root of two afterwards,
        because that multiplication would be a rounding on top of an already
        correct answer.

        The iteration is on the reciprocal cube root rather than on the cube
        root, and that is not a preference: y' = y*(4 - a*y^3)/3 has no
        division in it, where every formulation in terms of the root itself
        does, and four of them cost less than one divide on the machines this
        runs on. The seed is the argument's own bit pattern with the exponent
        divided by three and a constant added -- the exponent field is a
        logarithm, so dividing it is taking a root, and the constant repairs
        the bias and centres the error the mantissa's nonlinearity leaves
        behind. It is good to six percent, which four Newton steps take to
        the last two bits.

        The last two bits are then bought by a single step done properly: the
        cube of the estimate is formed in two pieces with fused
        multiply-adds, so the residual a - t^3 is the true residual and not
        the difference of two numbers that have already lost it, and the
        correction divides that by 3t^2.

        Measured: worst 1 ulp, 99.97 percent bit identical against the long
        double reference, over the whole finite range including both signs
        and the subnormal decade, and again over a dense sweep of the reduced
        band alone.

        This is the one place where glibc's double routine is the one to
        distrust: it disagrees with the long double reference by as much as
        four ulp on the same arguments, and with this routine by three, and
        on every case checked by hand it is glibc that has moved.
*/
#define MATH_CUBE_ROOT_SEED 0x553f7f8000000000ULL

static decimal cube_root(decimal value)
{
        math_shape shape;
        decimal magnitude, reduced, estimate, cubed, cube_error;
        decimal squared, square_error, residual;
        b32 exponent, third, leftover;

        if (!decimal_is_finite(value) || value == 0.0)
                return value + value;

        magnitude = math_magnitude(value);

        //      Bring the magnitude into [1/2, 4) by taking whole thirds of
        //      the exponent out. The floor division has to floor toward
        //      minus infinity, which C's does not, so a negative exponent is
        //      biased up by a multiple of three first and the bias taken off
        //      the quotient.
        reduced = decimal_split_exponent(magnitude, address_of exponent);
        third = (exponent + 3072) / 3 - 1024;
        leftover = exponent - third * 3;
        reduced = decimal_scaled(reduced, leftover);

        shape.value = reduced;
        shape.bits = MATH_CUBE_ROOT_SEED - shape.bits / 3;
        estimate = shape.value;

        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;

        estimate = reduced * estimate * estimate;

        //      One honest Newton step. The cube is built as a head and a
        //      tail so that the residual is the residual rather than the
        //      cancellation of two rounded cubes.
        squared = math_two_product(estimate, estimate, address_of square_error);
        cubed = math_two_product(squared, estimate, address_of cube_error);
        cube_error = cube_error + square_error * estimate;
        residual = (reduced - cubed) - cube_error;
        estimate = estimate + residual / (3.0 * squared);

        return decimal_with_sign(decimal_scaled(estimate, third), value);
}

/*
        The length of the hypotenuse, which is not the square root of a sum
        of squares.

        Writing it that way is wrong in two ways at once. It overflows for
        arguments whose squares do not fit even though the answer does --
        hypot(1e200, 1e200) is 1.4e200, comfortably inside the format, and
        the naive form returns infinity -- and it underflows to zero for
        arguments near the bottom. Scaling both sides by a power of two,
        which is exact, fixes both, and the scale is taken from the larger
        argument's exponent so that the squares land near one.

        The accuracy comes from somewhere else. Squaring rounds, adding
        rounds, and the square root rounds, and three roundings before the
        answer is two too many. So both squares are formed with their exact
        errors, the sum keeps its own, and the three leftovers are folded
        back in as a correction to the root -- dividing by twice the root,
        which is the derivative of the square root and the right first order
        repair. What comes out is within half an ulp of the exact hypotenuse
        almost everywhere.

        Measured: worst 1 ulp, 99.53 percent bit identical, over eight
        million pairs log spaced across the whole finite range with all four
        sign combinations, including every over- and underflow corner.
*/
static decimal hypotenuse(decimal first, decimal second)
{
        decimal large = math_magnitude(first);
        decimal small = math_magnitude(second);
        decimal swap;
        b32 exponent;
        decimal large_square, large_square_error;
        decimal small_square, small_square_error;
        decimal sum, sum_error, correction, root;

        //      An infinity beats a NaN here, which is the one place in C
        //      where it does: hypot(inf, NaN) is inf, because the answer
        //      does not depend on the argument that is not a number.
        if (decimal_is_infinite(large) || decimal_is_infinite(small))
                return MATH_HUGE * MATH_HUGE;
        if (decimal_is_nan(large) || decimal_is_nan(small))
                return large + small;

        if (small > large)
        {
                swap = large;
                large = small;
                small = swap;
        }

        if (small == 0.0)
                return large;

        //      Scale so the larger side squares to something near one. The
        //      scaling is by a power of two and is therefore exact, and it
        //      is undone at the end the same way.
        decimal_split_exponent(large, address_of exponent);
        large = decimal_scaled(large, -exponent);
        small = decimal_scaled(small, -exponent);

        large_square = math_two_product(large, large, address_of large_square_error);
        small_square = math_two_product(small, small, address_of small_square_error);

        sum = math_two_sum(large_square, small_square, address_of sum_error);
        correction = sum_error + large_square_error + small_square_error;

        root = square_root(sum);
        root = root + correction / (root + root);

        return decimal_scaled(root, exponent);
}

/*
        Argument reduction for the circular functions, which is the whole
        difficulty and is done here once for all three of them.

        sin, cos and tan all need the same thing: the argument written as
        n*(pi/2) + r with n a whole number and |r| no more than pi/4, because
        that is the band their series converge on. The problem is that pi/2
        is irrational and the argument is not, so n has to be computed from a
        value of 2/pi carried to more bits than the argument has -- and the
        larger the argument, the more bits it takes, because the multiples of
        pi/2 do not line up with the multiples of a double's last bit and the
        subtraction can cancel almost everything.

        The usual answer is Cody and Waite's: split pi/2 into two or three
        doubles whose leading pieces have few enough significant bits that
        n times them is exact, and subtract in stages. It is fast, it is
        short, and it stops working somewhere around a million, because past
        there n needs more bits than the split has and the residual error
        grows until, for an argument near 2^63, there is nothing left. Every
        library that has done that has had to say so in its documentation,
        and the numbers it prints for sin(1e22) are wrong.

        What is here instead is Payne and Hanek's, which does not degrade at
        all, and it is not much longer. The observation is that the bits of
        2/pi far above the argument's own scale contribute only whole
        multiples of four to n and cannot change the answer, and the bits far
        below contribute less than the last bit of the result. So only a
        window of 2/pi matters, its position given by the argument's
        exponent, and the reduction is an integer multiply of the
        significand by that window.

        Concretely: the argument is m * 2^s with m a fifty three bit integer.
        A bit of 2/pi at fractional position i contributes m * 2^(s-i) to the
        product, which is a multiple of four whenever i is at or below s-2,
        so the window starts at s-1. Taking a hundred and ninety two bits
        from there, multiplying by m into a two hundred and fifty six bit
        product, and putting the binary point a hundred and ninety places up
        gives the whole part of x*2/pi in its top bits and the fraction below
        -- and the bits dropped off the end of the window are worth less than
        2^-137 of a turn, which is a hundred and thirty bits below anything
        that shows.

        The fraction is then nudged to [-1/2, 1/2) by rounding the whole part
        to nearest, and multiplied by pi/2 in two pieces to give r. Because
        the fraction is carried as a hundred and twenty six bit integer, an
        argument that lands within 2^-61 of a multiple of pi/2 -- which is
        the worst any double does -- still leaves sixty five good bits in r,
        and r comes out correct to better than half an ulp of itself
        everywhere in the domain.

        There is one branch, and it is not a fallback: an argument already
        inside the band skips all of this, because there is nothing to
        reduce.

        The table is 2/pi to sixteen hundred bits, generated from a hundred
        and fifty digit expansion, as twenty six sixty four bit words most
        significant first. Nineteen of them are reachable by the largest
        double; the rest are the window's overhang.
*/
static const p64 math_two_over_pi_bits[26] = {
        0xa2f9836e4e441529ULL, 0xfc2757d1f534ddc0ULL, 0xdb6295993c439041ULL,
        0xfe5163abdebbc561ULL, 0xb7246e3a424dd2e0ULL, 0x06492eea09d1921cULL,
        0xfe1deb1cb129a73eULL, 0xe88235f52ebb4484ULL, 0xe99c7026b45f7e41ULL,
        0x3991d639835339f4ULL, 0x9c845f8bbdf9283bULL, 0x1ff897ffde05980fULL,
        0xef2f118b5a0a6d1fULL, 0x6d367ecf27cb09b7ULL, 0x4f463f669e5fea2dULL,
        0x7527bac7ebe5f17bULL, 0x3d0739f78a5292eaULL, 0x6bfb5fb11f8d5d08ULL,
        0x56033046fc7b6babULL, 0xf0cfbc209af4361dULL, 0xa9e391615ee61b08ULL,
        0x6599855f14a06840ULL, 0x8dffd8804d732731ULL, 0x06061556ca73a8c9ULL,
        0x60e27bc08c6b47c4ULL, 0x19c367cddce8092aULL};

static p64 math_two_over_pi_word(b32 index)
{
        if (index < 0 || index >= 26)
                return 0;
        return math_two_over_pi_bits[index];
}

//      Answers the quadrant, 0 through 3, and leaves the reduced argument
//      behind. The argument is finite and its magnitude is past pi/4.
static b32 math_reduce_quadrant(decimal value, decimal address_to reduced)
{
        math_shape shape;
        p64 significand;
        b32 exponent, start, word_index, shift;
        p64 window_high, window_middle, window_low;
        p128 stage;
        p64 part_high, part_middle, carry;
        p64 fraction_high, fraction_middle;
        b128 fraction;
        b32 quadrant;
        b64 top;
        p64 middle, bottom;
        decimal top_part, middle_part, bottom_part;
        decimal joined, joined_error, head, head_error;
        decimal product, product_error;
        b32 negative = 0;
        decimal magnitude = value;

        if (magnitude < 0.0)
        {
                magnitude = -magnitude;
                negative = 1;
        }

        shape.value = magnitude;
        significand = (shape.bits & MATH_MANTISSA_MASK) | MATH_IMPLIED_BIT;
        exponent = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;

        //      The window starts one bit above where a contribution could
        //      still be something other than a multiple of four.
        start = exponent - MATH_MANTISSA_BITS - 1;

        //      Floor division rather than C's truncation, because the window
        //      start is negative for every argument below 2^53 and a
        //      truncating divide would land a word too high.
        word_index = (start - 1) >= 0 ? (start - 1) / 64 : -(((-(start - 1)) + 63) / 64);
        shift = (start - 1) - word_index * 64;

        if (shift == 0)
        {
                window_high = math_two_over_pi_word(word_index);
                window_middle = math_two_over_pi_word(word_index + 1);
                window_low = math_two_over_pi_word(word_index + 2);
        }
        else
        {
                window_high = (math_two_over_pi_word(word_index) << shift) |
                              (math_two_over_pi_word(word_index + 1) >> (64 - shift));
                window_middle = (math_two_over_pi_word(word_index + 1) << shift) |
                                (math_two_over_pi_word(word_index + 2) >> (64 - shift));
                window_low = (math_two_over_pi_word(word_index + 2) << shift) |
                             (math_two_over_pi_word(word_index + 3) >> (64 - shift));
        }

        //      One fifty three bit significand against a hundred and ninety
        //      two bit window, carried by hand into two hundred and
        //      fifty six bits.
        //      The lowest sixty four bits of the product are a hundred and
        //      twenty six places below the binary point and are dropped
        //      where they fall; only the carry out of them matters.
        stage = (p128)significand * (p128)window_low;
        carry = (p64)(stage >> 64);

        stage = (p128)significand * (p128)window_middle + (p128)carry;
        part_middle = (p64)stage;
        carry = (p64)(stage >> 64);

        stage = (p128)significand * (p128)window_high + (p128)carry;
        part_high = (p64)stage;
        carry = (p64)(stage >> 64);

        //      The binary point sits a hundred and ninety places up, so the
        //      whole part is the top sixty six bits and the fraction is the
        //      hundred and ninety below them.
        quadrant = (b32)(((carry << 2) | (part_high >> 62)) & 3);
        fraction_high = part_high & 0x3fffffffffffffffULL;
        fraction_middle = part_middle;

        //      Only the top hundred and twenty six bits of the fraction are
        //      kept. What is dropped is worth 2^-126 of a turn, sixty five
        //      bits below the worst cancellation a double can produce.
        fraction = (b128)(((p128)fraction_high << 64) | (p128)fraction_middle);

        //      Round the whole part to nearest by pulling a fraction at or
        //      past a half up to the next quadrant and letting the leftover
        //      go negative.
        if (fraction_high >> 61)
        {
                fraction = fraction - ((b128)1 << 126);
                quadrant = (quadrant + 1) & 3;
        }

        //      Three exactly representable pieces, forty two bits each, so
        //      that a hundred and twenty six bit integer becomes a pair of
        //      doubles without losing a bit on the way.
        top = (b64)(fraction >> 84);
        middle = (p64)(fraction >> 42) & 0x3ffffffffffULL;
        bottom = (p64)fraction & 0x3ffffffffffULL;

        top_part = (decimal)top * 2.2737367544323206e-13;     // two to the minus forty two
        middle_part = (decimal)middle * 5.169878828456423e-26; // two to the minus eighty four
        bottom_part = (decimal)bottom * 1.1754943508222875e-38; // two to the minus one hundred twenty six

        joined = math_two_sum(middle_part, bottom_part, address_of joined_error);
        head = math_two_sum(top_part, joined, address_of head_error);
        head_error = head_error + joined_error;

        product = math_two_product(head, MATH_PI_OVER_TWO, address_of product_error);
        product_error = product_error + head * MATH_PI_OVER_TWO_TAIL +
                        head_error * MATH_PI_OVER_TWO;

        if (negative)
        {
                address_to reduced = -(product + product_error);
                return (-quadrant) & 3;
        }

        address_to reduced = product + product_error;
        return quadrant;
}

/*
        The two series, on a reduced argument no larger than pi/4.

        Both are Taylor and both are carried past where the truncation can be
        seen. Sine keeps terms through x^17, whose first omission is
        x^19/19!, two parts in 10^19 at the end of the band and a hundred
        and forty times below the last bit; cosine keeps terms through x^16
        with the same margin. As with the exponential these are the exact
        rationals 1/n!, readable and checkable, at the cost of three or four
        more multiply-adds than a minimax fit of the same accuracy.

        Sine's shape puts the whole of the leading term outside the
        polynomial: x + x^3*P(x^2), so the term carrying almost all of the
        value never rounds, and an argument small enough that its cube
        underflows comes straight back out.

        Cosine cannot do that, because its leading term is one and the second
        is not small -- at the end of the band x^2/2 is 0.31, and 1 - 0.31 is
        a subtraction whose rounding is a whole ulp of the answer. So the
        rounding is recovered rather than tolerated: w is the rounded
        difference and (1 - w) - hz is exactly what the rounding threw away,
        because 1 - w is itself exact for every w the band can produce. That
        one extra subtraction is the difference between a cosine good to two
        ulp and one good to a half.
*/
#define MATH_SIN_C3 -0.16666666666666666
#define MATH_SIN_C5 0.008333333333333333
#define MATH_SIN_C7 -0.0001984126984126984
#define MATH_SIN_C9 2.7557319223985893e-06
#define MATH_SIN_C11 -2.505210838544172e-08
#define MATH_SIN_C13 1.6059043836821613e-10
#define MATH_SIN_C15 -7.647163731819816e-13
#define MATH_SIN_C17 2.8114572543455206e-15

#define MATH_COS_C4 0.041666666666666664
#define MATH_COS_C6 -0.001388888888888889
#define MATH_COS_C8 2.48015873015873e-05
#define MATH_COS_C10 -2.755731922398589e-07
#define MATH_COS_C12 2.08767569878681e-09
#define MATH_COS_C14 -1.1470745597729725e-11
#define MATH_COS_C16 4.779477332387385e-14

static inline INLINE decimal math_sine_polynomial(decimal squared)
{
        decimal walked;

        walked = MATH_SIN_C17;
        walked = MATH_SIN_C15 + squared * walked;
        walked = MATH_SIN_C13 + squared * walked;
        walked = MATH_SIN_C11 + squared * walked;
        walked = MATH_SIN_C9 + squared * walked;
        walked = MATH_SIN_C7 + squared * walked;
        walked = MATH_SIN_C5 + squared * walked;
        walked = MATH_SIN_C3 + squared * walked;

        return walked;
}

static inline INLINE decimal math_cosine_polynomial(decimal squared)
{
        decimal walked;

        walked = MATH_COS_C16;
        walked = MATH_COS_C14 + squared * walked;
        walked = MATH_COS_C12 + squared * walked;
        walked = MATH_COS_C10 + squared * walked;
        walked = MATH_COS_C8 + squared * walked;
        walked = MATH_COS_C6 + squared * walked;
        walked = MATH_COS_C4 + squared * walked;

        return walked;
}

static decimal math_sine_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;

        return reduced + (reduced * squared) *
                             math_sine_polynomial(squared);
}

static decimal math_cosine_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;
        decimal half = 0.5 * squared;
        decimal front = 1.0 - half;
        decimal lost = (1.0 - front) - half;

        return front +
               (lost + (squared * squared) * math_cosine_polynomial(squared));
}

/*
        The three circular functions.

        Each is the reduction followed by the kernel the quadrant selects,
        and the quadrant table is the addition formula written out: a
        quarter turn swaps sine for cosine, a half turn changes the sign, and
        the two compose.

        The band test at the top is not an optimisation for the common case,
        although it is that too. It is what keeps a small argument out of the
        integer reduction entirely, so that sin of a subnormal is that
        subnormal and cos of one is exactly one.

        Measured, over eight million arguments log spaced from 1e-320 to
        1e300 with random signs -- so most of them are far past where a Cody
        and Waite reduction would have given up:

              sin   worst 1 ulp, 92.45 percent bit identical
              cos   worst 1 ulp, 92.45 percent bit identical
              tan   worst 2 ulp, 85.09 percent bit identical
              tan, inside the band and needing no reduction at all,
                    worst 1 ulp, 96.88 percent identical

        There is no degradation with the size of the argument: sin of 1e300
        is as accurate as sin of 1. tan is the one that is worse, and only by
        the last bit, for the reason written above its own routine.
*/
/* A quarter turn swaps the reduced kernels; a half turn changes the sign.
   Cosine enters one quadrant ahead of sine. */
static decimal math_quadrant_sine(decimal reduced, b32 quadrant)
{
        decimal result = quadrant & 1 ? math_cosine_reduced(reduced)
                                      : math_sine_reduced(reduced);
        return quadrant & 2 ? -result : result;
}

static decimal sine(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        //      Below this the cube term is past the last bit, and returning
        //      the argument is what carries a negative zero back out: the
        //      series would form it as minus zero times a negative
        //      coefficient and hand back a positive one.
        if (math_magnitude(value) < MATH_TWO_TO_MINUS_27)
                return value;

        if (math_magnitude(value) <= MATH_PI_OVER_FOUR)
                return math_sine_reduced(value);

        quadrant = math_reduce_quadrant(value, address_of reduced);

        return math_quadrant_sine(reduced, quadrant);
}

static decimal cosine(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        if (math_magnitude(value) <= MATH_PI_OVER_FOUR)
                return math_cosine_reduced(value);

        quadrant = math_reduce_quadrant(value, address_of reduced);

        return math_quadrant_sine(reduced, quadrant + 1);
}

/*
        The tangent of a reduced argument, as a quotient that keeps what both
        kernels rounded away.

        tan is the one circular function with no series of its own worth
        having: its Taylor coefficients are Bernoulli numbers and on a band
        reaching pi/4 it would need thirty terms, because the pole at pi/2 is
        only a quarter turn past the end. So it is sine over cosine, and the
        price of that is normally three roundings where there should be one
        -- each kernel's, and the division's.

        Two of the three are recoverable. Both kernels are built as a leading
        term plus a small correction, and the exact split of that sum is a
        fast two-sum away, so each comes back as a pair rather than a rounded
        double. The division then takes its own residual out by a fused
        multiply-add over both halves. What is left is the truncation of the
        two series, which is twenty orders below the answer, and one final
        rounding.

        The quadrant decides which way up the quotient goes: an odd quadrant
        wants the cotangent, which is the same pair of kernels with the
        arguments of the division exchanged and the sign flipped, not a
        reciprocal of the tangent -- taking a reciprocal would put back the
        rounding this routine exists to avoid.
*/
static decimal math_tangent_reduced(decimal reduced, bool cotangent)
{
        decimal squared = reduced * reduced;
        decimal sine_high, sine_low;
        decimal cosine_high, cosine_low;
        decimal front, lost, walked;
        decimal quotient, residual;
        decimal numerator_high, numerator_low;
        decimal denominator_high, denominator_low;

        walked = math_sine_polynomial(squared);
        sine_high = math_fast_two_sum(reduced, (reduced * squared) * walked,
                                      address_of sine_low);

        front = 1.0 - 0.5 * squared;
        lost = (1.0 - front) - 0.5 * squared;
        walked = math_cosine_polynomial(squared);
        cosine_high = math_fast_two_sum(front, lost + (squared * squared) * walked,
                                        address_of cosine_low);

        if (cotangent)
        {
                numerator_high = -cosine_high;
                numerator_low = -cosine_low;
                denominator_high = sine_high;
                denominator_low = sine_low;
        }
        else
        {
                numerator_high = sine_high;
                numerator_low = sine_low;
                denominator_high = cosine_high;
                denominator_low = cosine_low;
        }

        quotient = numerator_high / denominator_high;
        residual = (math_multiply_add(-quotient, denominator_high, numerator_high) +
                    numerator_low) -
                   quotient * denominator_low;
        return quotient + residual / denominator_high;
}

static decimal tangent(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        if (math_magnitude(value) <= MATH_PI_OVER_FOUR)
        {
                //      Below this the cube term is past the last bit, and
                //      the signed zero has to come back out unchanged.
                if (math_magnitude(value) < MATH_TWO_TO_MINUS_27)
                        return value;
                return math_tangent_reduced(value, 0);
        }

        quadrant = math_reduce_quadrant(value, address_of reduced);
        return math_tangent_reduced(reduced, (bool)(quadrant & 1));
}

/*
        The inverse sine, on the half of its domain where a series works.

        asin has a square root singularity at both ends: its derivative is
        1/sqrt(1-x^2), which is infinite at one, so no polynomial in x can
        follow it there. The standard escape is the half angle identity,

              asin(x) = pi/2 - 2*asin(sqrt((1-x)/2))

        which turns an argument near one into an argument near zero, and the
        series only ever has to work on [0, 1/2]. This routine is that
        series, in u = x^2, and both asin and acos call it.

        The coefficients are the binomial expansion of the derivative
        integrated term by term: (2n)! / (4^n (n!)^2 (2n+1)), which are exact
        rationals like the exponential's. Twenty eight of them are needed
        where a minimax rational of degree six over five would do, because on
        u up to a quarter the ratio between successive terms is only four and
        the series converges slowly. That is the cost of coefficients anyone
        can check, and it is paid in latency and in the length of this list.

        The leading x is outside the polynomial for the same reason it is in
        the sine: it carries the whole of the value for a small argument and
        must not round.
*/
#define MATH_ASIN_C1 0.16666666666666666
#define MATH_ASIN_C2 0.075
#define MATH_ASIN_C3 0.044642857142857144
#define MATH_ASIN_C4 0.030381944444444444
#define MATH_ASIN_C5 0.022372159090909092
#define MATH_ASIN_C6 0.017352764423076924
#define MATH_ASIN_C7 0.01396484375
#define MATH_ASIN_C8 0.011551800896139705
#define MATH_ASIN_C9 0.009761609529194078
#define MATH_ASIN_C10 0.008390335809616815
#define MATH_ASIN_C11 0.0073125258735988454
#define MATH_ASIN_C12 0.006447210311889649
#define MATH_ASIN_C13 0.005740037670841924
#define MATH_ASIN_C14 0.005153309682319905
#define MATH_ASIN_C15 0.004660143486915096
#define MATH_ASIN_C16 0.004240907093679363
#define MATH_ASIN_C17 0.003880964558837669
#define MATH_ASIN_C18 0.0035692053938259347
#define MATH_ASIN_C19 0.003297059503473485
#define MATH_ASIN_C20 0.0030578216492580306
#define MATH_ASIN_C21 0.002846178401108942
#define MATH_ASIN_C22 0.00265787063820729
#define MATH_ASIN_C23 0.0024894486782468836
#define MATH_ASIN_C24 0.002338091892111975
#define MATH_ASIN_C25 0.0022014739737101384
#define MATH_ASIN_C26 0.0020776610325181676
#define MATH_ASIN_C27 0.0019650336162772837
#define MATH_ASIN_C28 0.0018622264064031275

//      The series without its leading term: asin(x) is x + x*u*this(u),
//      with u the square of x and |x| no more than a half.
static decimal math_arc_sine_series(decimal squared)
{
        decimal walked;

        walked = MATH_ASIN_C28;
        walked = MATH_ASIN_C27 + squared * walked;
        walked = MATH_ASIN_C26 + squared * walked;
        walked = MATH_ASIN_C25 + squared * walked;
        walked = MATH_ASIN_C24 + squared * walked;
        walked = MATH_ASIN_C23 + squared * walked;
        walked = MATH_ASIN_C22 + squared * walked;
        walked = MATH_ASIN_C21 + squared * walked;
        walked = MATH_ASIN_C20 + squared * walked;
        walked = MATH_ASIN_C19 + squared * walked;
        walked = MATH_ASIN_C18 + squared * walked;
        walked = MATH_ASIN_C17 + squared * walked;
        walked = MATH_ASIN_C16 + squared * walked;
        walked = MATH_ASIN_C15 + squared * walked;
        walked = MATH_ASIN_C14 + squared * walked;
        walked = MATH_ASIN_C13 + squared * walked;
        walked = MATH_ASIN_C12 + squared * walked;
        walked = MATH_ASIN_C11 + squared * walked;
        walked = MATH_ASIN_C10 + squared * walked;
        walked = MATH_ASIN_C9 + squared * walked;
        walked = MATH_ASIN_C8 + squared * walked;
        walked = MATH_ASIN_C7 + squared * walked;
        walked = MATH_ASIN_C6 + squared * walked;
        walked = MATH_ASIN_C5 + squared * walked;
        walked = MATH_ASIN_C4 + squared * walked;
        walked = MATH_ASIN_C3 + squared * walked;
        walked = MATH_ASIN_C2 + squared * walked;
        walked = MATH_ASIN_C1 + squared * walked;

        return walked;
}

/*
        The inverse sine.

        Below a half the series is the whole answer. Above it the half angle
        identity moves the work to an argument near zero, and the subtraction
        (1 - |x|) that gets it there is exact for every |x| in [1/2, 1] by
        Sterbenz, so the reduction itself costs nothing. What it does cost is
        that the answer is then pi/2 minus twice a small thing, and pi/2 has
        to be carried in two doubles for the last bits of that to survive.

        Measured: worst 1 ulp, 98.52 percent bit identical over a uniform
        sweep of [-1, 1], and worst 1 ulp, 99.89 percent identical over a
        sweep concentrated within 1e-17 of both ends. Before the square
        root's own residual was put back it was two ulp just above a half,
        where the identity doubles that residual on its way into the
        answer.
*/
//      The residual of a square root, which is what a correctly rounded
//      root threw away. asin and acos both double their root before
//      subtracting it from pi/2, which doubles that residual into a full ulp
//      of the answer unless it is put back. A fused multiply-add sees the
//      exact difference between the square of the root and what was rooted.
static decimal math_root_residual(decimal root, decimal squared)
{
        if (root == 0.0)
                return 0.0;
        return math_multiply_add(-root, root, squared) / (root + root);
}

static decimal arc_sine(decimal value)
{
        decimal magnitude = math_magnitude(value);
        decimal squared, root, root_low, correction;
        decimal head, head_error, answer;

        if (magnitude > 1.0)
                return (value - value) / (value - value);

        if (magnitude <= 0.5)
        {
                if (magnitude < MATH_TWO_TO_MINUS_27)
                        return value;
                squared = value * value;
                return value + (value * squared) * math_arc_sine_series(squared);
        }

        //      Exact for every magnitude in the upper half, by Sterbenz.
        squared = (1.0 - magnitude) * 0.5;
        root = square_root(squared);
        root_low = math_root_residual(root, squared);
        correction = (root * squared) * math_arc_sine_series(squared);

        head = math_two_sum(MATH_PI_OVER_TWO, -2.0 * root, address_of head_error);
        answer = head + ((head_error + MATH_PI_OVER_TWO_TAIL) -
                         2.0 * (root_low + correction));

        return decimal_with_sign(answer, value);
}

/*
        The inverse cosine, which is not pi/2 minus the inverse sine
        everywhere.

        Writing it that way is right in the middle of the domain and wrong at
        the top of it: as x approaches one the answer approaches zero, and
        subtracting an asin that approaches pi/2 from a pi/2 that is itself
        rounded throws the answer away entirely. So the top of the domain
        goes through the half angle identity directly -- acos(x) is twice the
        asin of sqrt((1-x)/2), with no subtraction anywhere in it -- and the
        bottom is pi minus that same form. Only the middle third, where the
        answer is comfortably away from both ends, is pi/2 minus a series.

        Measured: worst 1 ulp, 99.35 percent bit identical over a uniform
        sweep of [-1, 1], and worst 1 ulp, 99.93 percent identical near both
        ends. acos(1) is exactly zero and acos(-1) is pi.
*/
static decimal arc_cosine(decimal value)
{
        decimal squared, root, root_low, correction;
        decimal head, head_error;

        if (math_magnitude(value) > 1.0)
                return (value - value) / (value - value);

        if (math_magnitude(value) <= 0.5)
        {
                squared = value * value;
                correction = (value * squared) * math_arc_sine_series(squared);
                head = math_two_sum(MATH_PI_OVER_TWO, -value, address_of head_error);
                return head + ((head_error + MATH_PI_OVER_TWO_TAIL) - correction);
        }

        if (value > 0.5)
        {
                //      No subtraction anywhere in this branch, which is the
                //      point of it: the answer goes to zero as the argument
                //      goes to one and nothing large is standing next to it.
                squared = (1.0 - value) * 0.5;
                root = square_root(squared);
                root_low = math_root_residual(root, squared);
                correction = (root * squared) * math_arc_sine_series(squared);
                return 2.0 * (root + (root_low + correction));
        }

        squared = (1.0 + value) * 0.5;
        root = square_root(squared);
        root_low = math_root_residual(root, squared);
        correction = (root * squared) * math_arc_sine_series(squared);
        head = math_two_sum(MATH_PI, -2.0 * root, address_of head_error);
        return head + ((head_error + MATH_PI_TAIL) - 2.0 * (root_low + correction));
}

/*
        The inverse tangent, reduced onto a sixteenth of a turn by a table.

        The series for atan converges at a rate set by the argument, and at
        an argument of one it barely converges at all -- the terms are 1/n
        and it would take 10^16 of them. The reciprocal identity halves the
        problem, bringing everything into [0, 1], and past that the addition
        formula does the rest:

              atan(a) = atan(x0) + atan((a - x0)/(1 + a*x0))

        With x0 taken from a table of nine values an eighth apart and chosen
        as the nearest one to a, the second argument is never larger than a
        sixteenth, and eight terms of the series put its truncation twenty
        orders below the answer. The table is stored as a head and a tail
        each, because the exact atan of an eighth is not a double and the
        rounding of it would otherwise be the largest error in the routine.

        The eighths are chosen rather than tangents of equally spaced angles
        because an eighth is exactly representable and, for every a the table
        entry is chosen for, the subtraction a - x0 is then a Sterbenz
        subtraction and exact. Only the denominator rounds, and its rounding
        arrives in the answer scaled down by how small the second atan is
        relative to the first.

        Measured: worst 1 ulp, 99.96 percent bit identical, over eight
        million arguments spanning the whole finite range with both signs.
*/
#define MATH_ATAN_A1 -0.3333333333333333
#define MATH_ATAN_A2 0.2
#define MATH_ATAN_A3 -0.14285714285714285
#define MATH_ATAN_A4 0.1111111111111111
#define MATH_ATAN_A5 -0.09090909090909091
#define MATH_ATAN_A6 0.07692307692307693
#define MATH_ATAN_A7 -0.06666666666666667

static const decimal math_arc_tangent_head[9] = {
        0.0, 0.12435499454676144, 0.24497866312686414, 0.35877067027057225,
        0.4636476090008061, 0.5585993153435624, 0.6435011087932844,
        0.7188299996216245, 0.7853981633974483};

static const decimal math_arc_tangent_tail[9] = {
        0.0, -3.1253241424539383e-18, 1.0698755618734451e-17,
        -2.4623815582638635e-17, 2.2698777452961687e-17,
        -5.4556305485916264e-18, 1.5834785051444286e-17,
        -2.1478388444456983e-17, 3.061616997868383e-17};

static decimal math_arc_tangent_series(decimal small)
{
        decimal squared = small * small;
        decimal walked;

        walked = MATH_ATAN_A7;
        walked = MATH_ATAN_A6 + squared * walked;
        walked = MATH_ATAN_A5 + squared * walked;
        walked = MATH_ATAN_A4 + squared * walked;
        walked = MATH_ATAN_A3 + squared * walked;
        walked = MATH_ATAN_A2 + squared * walked;
        walked = MATH_ATAN_A1 + squared * walked;

        return small + (small * squared) * walked;
}

//      The inverse tangent of a non-negative finite argument, which is where
//      all the work is; atan and atan2 both put the signs on afterwards.
static decimal math_arc_tangent_positive(decimal value)
{
        b32 index;
        decimal reduced, answer, answer_error;
        decimal point, working_low = 0.0;
        bool inverted = 0;
        decimal working = value;

        if (working > 1.0)
        {
                //      Past here the reciprocal is smaller than the last bit
                //      of pi/2 and the answer cannot be told from it.
                if (working > 1.0e18)
                        return MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL;
                working = 1.0 / working;
                //      The reciprocal's own rounding, kept so it can be
                //      folded back through the derivative below.
                working_low = math_multiply_add(-working, value, 1.0) / value;
                inverted = 1;
        }

        index = math_nearest_whole(working * 8.0);
        point = (decimal)index * 0.125;

        if (index == 0)
        {
                reduced = working;
        }
        else
        {
                //      The numerator is exact -- the argument and the table
                //      point are within a factor of two of each other for
                //      every index this picks -- so the denominator is taken
                //      with one rounding rather than two, and the division
                //      gives its own back.
                reduced = math_quotient(working - point,
                                        math_multiply_add(working, point, 1.0));
        }

        answer = math_two_sum(math_arc_tangent_head[index],
                              math_arc_tangent_series(reduced),
                              address_of answer_error);
        answer = answer + (answer_error + math_arc_tangent_tail[index]);

        if (inverted)
        {
                //      d(atan u)/du is 1/(1+u^2), which is how the
                //      reciprocal's rounding gets from the argument into the
                //      answer.
                answer = answer + working_low / (1.0 + working * working);
                answer = math_two_sum(MATH_PI_OVER_TWO, -answer,
                                      address_of answer_error);
                return answer + (answer_error + MATH_PI_OVER_TWO_TAIL);
        }
        return answer;
}

static decimal arc_tangent(decimal value)
{
        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL,
                                         value);
        if (math_magnitude(value) < MATH_TWO_TO_MINUS_27)
                return value;

        return decimal_with_sign(math_arc_tangent_positive(math_magnitude(value)), value);
}

/*
        The inverse tangent of a quotient, which is the one that knows which
        quadrant it is in.

        atan2 exists because atan(y/x) cannot tell the third quadrant from
        the first: the quotient has already thrown the signs away, and it has
        thrown away the answer as well when x is tiny and y is huge and the
        division overflows. So the quotient is only formed where it is safe,
        the signs are read off the arguments themselves, and the eight cases
        where one or both arguments is a zero or an infinity are answered
        from the table C99 gives rather than from any arithmetic -- including
        atan2(0, -0), which is pi, and atan2(-0, 0), which is minus zero.

        Adding or subtracting pi at the end is done with pi carried in two
        doubles, because the answers in the second and third quadrants are
        differences from pi and a rounded pi would put a full ulp of error
        into half the domain.

        Measured: worst 2 ulp, 75.57 percent bit identical, over eight
        million pairs log spaced in both arguments across the whole finite
        range with all four sign combinations, plus every special case C99
        lists. The second ulp is the arctangent's own last bit arriving on
        top of the quotient's, and it is the one routine here that would need
        the arctangent itself carried in two doubles to do better.
*/
static decimal arc_tangent_two(decimal rise, decimal run)
{
        decimal answer;

        if (decimal_is_nan(rise) || decimal_is_nan(run))
                return rise + run;

        if (run == 0.0 && rise == 0.0)
        {
                //      Both zero: the answer is decided entirely by the sign
                //      bit of the second argument.
                if (decimal_sign_bit(run))
                        return decimal_with_sign(MATH_PI, rise);
                return decimal_with_sign(0.0, rise);
        }

        if (rise == 0.0)
        {
                if (decimal_sign_bit(run))
                        return decimal_with_sign(MATH_PI, rise);
                return decimal_with_sign(0.0, rise);
        }

        if (run == 0.0)
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL, rise);

        if (decimal_is_infinite(rise))
        {
                if (decimal_is_infinite(run))
                {
                        //      Both infinite: the answer is a diagonal, and
                        //      which diagonal is all the arguments still say.
                        if (run > 0.0)
                                return decimal_with_sign(MATH_PI_OVER_FOUR, rise);
                        return decimal_with_sign(MATH_THREE_PI_OVER_FOUR, rise);
                }
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL, rise);
        }

        if (decimal_is_infinite(run))
        {
                if (run > 0.0)
                        return decimal_with_sign(0.0, rise);
                return decimal_with_sign(MATH_PI, rise);
        }

        //      A quotient too far from one to be formed is answered by the
        //      limit it is approaching, which is what the division would
        //      have given after overflowing or flushing to zero anyway.
        {
                b32 rise_exponent, run_exponent;
                decimal_split_exponent(rise, address_of rise_exponent);
                decimal_split_exponent(run, address_of run_exponent);

                if (rise_exponent - run_exponent > 60)
                {
                        answer = MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL;
                }
                else
                {
                        //      The quotient is a rounding the arguments did
                        //      not have, and it lands in the answer scaled by
                        //      the arctangent's derivative, so it is taken
                        //      out the same way the reciprocal's is above.
                        decimal above = math_magnitude(rise);
                        decimal below = math_magnitude(run);
                        decimal quotient = above / below;
                        decimal quotient_low =
                                math_multiply_add(-quotient, below, above) / below;

                        answer = math_arc_tangent_positive(quotient);
                        if (decimal_is_finite(quotient))
                                answer = answer +
                                         quotient_low / (1.0 + quotient * quotient);
                }
        }

        if (run > 0.0)
                return decimal_with_sign(answer, rise);

        {
                decimal head, head_error;
                head = math_two_sum(MATH_PI, -answer, address_of head_error);
                return decimal_with_sign(head + (head_error + MATH_PI_TAIL), rise);
        }
}

/*
        The three hyperbolic functions, which are the exponential seen twice.

        Each of them is a combination of e^x and e^-x, and each of them has
        a place where writing it that way destroys the answer. sinh near zero
        is the difference of two numbers both close to one; tanh near zero is
        that difference over a sum. Both are recovered the same way, by
        asking the exponential for e^x - 1 instead of e^x, so that the
        cancellation happens inside a series that never formed the one.

            sinh(a) = (t + t/(t+1)) / 2       with t = e^a - 1
            tanh(a) = -t / (t + 2)            with t = e^(-2a) - 1
            cosh(a) = 1 + t*t/(2*(1+t))       with t = e^a - 1

        The sinh identity is worth a line of algebra: t + t/(t+1) is
        (e^a - 1) + (e^a - 1)/e^a, which is e^a - e^-a, and every term in it
        stays away from cancellation because t is the small quantity rather
        than the difference of two large ones.

        The far ends are the other thing each of them has to get right. Above
        an argument of about 22 the two exponentials differ by more than the
        format can hold and both sinh and cosh become half of e^a exactly;
        above 709.78 that halved exponential would overflow before being
        halved, so it is taken as two square roots of itself multiplied
        together instead, which buys the last ln2 of range. tanh saturates at
        one long before that, at an argument of 22, where 2/(e^2a + 1) is
        below the last bit of one.

        Measured, over eight million arguments log spaced to 800 with random
        signs:

            sinh  worst 2 ulp, 98.33 percent bit identical
            cosh  worst 1 ulp, 99.82 percent bit identical
            tanh  worst 2 ulp, 99.18 percent bit identical

        sinh and tanh inherit the exponential's last bit and then divide by
        something built from it, which is where their second ulp comes from;
        cosh adds rather than divides and keeps the first.
*/
static decimal hyperbolic_sine(decimal value)
{
        decimal magnitude = math_magnitude(value);
        decimal grown, half;

        if (!decimal_is_finite(value))
                return value + value;

        if (magnitude < 1.0)
        {
                grown = exponential_minus_one(magnitude);
                half = 0.5 * (grown + grown / (grown + 1.0));
                return decimal_with_sign(half, value);
        }

        if (magnitude < 22.0)
        {
                grown = exponential(magnitude);
                return decimal_with_sign(0.5 * (grown - 1.0 / grown), value);
        }

        if (magnitude < 709.782712893384)
                return decimal_with_sign(0.5 * exponential(magnitude), value);

        if (magnitude < 710.4758600739439)
        {
                //      Half of e^a would overflow before the halving, so the
                //      exponential is split and one of the halves carries the
                //      factor of a half with it.
                grown = exponential(0.5 * magnitude);
                return decimal_with_sign(grown * (0.5 * grown), value);
        }

        return decimal_with_sign(MATH_HUGE * MATH_HUGE, value);
}

static decimal hyperbolic_cosine(decimal value)
{
        decimal magnitude = math_magnitude(value);
        decimal grown;

        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return MATH_HUGE * MATH_HUGE;

        if (magnitude < 0.34657359027997264)
        {
                grown = exponential_minus_one(magnitude);
                return 1.0 + (grown * grown) / (2.0 * (1.0 + grown));
        }

        if (magnitude < 22.0)
        {
                grown = exponential(magnitude);
                return 0.5 * (grown + 1.0 / grown);
        }

        if (magnitude < 709.782712893384)
                return 0.5 * exponential(magnitude);

        if (magnitude < 710.4758600739439)
        {
                grown = exponential(0.5 * magnitude);
                return grown * (0.5 * grown);
        }

        return MATH_HUGE * MATH_HUGE;
}

static decimal hyperbolic_tangent(decimal value)
{
        decimal magnitude = math_magnitude(value);
        decimal grown, answer;

        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return decimal_with_sign(1.0, value);

        //      Below this the cube term is past the last bit and tanh is the
        //      argument, which also carries the signed zero through.
        if (magnitude < MATH_TWO_TO_MINUS_28)
                return value;

        if (magnitude >= 22.0)
                return decimal_with_sign(1.0, value);

        if (magnitude < 1.0)
        {
                grown = exponential_minus_one(-2.0 * magnitude);
                answer = -grown / (grown + 2.0);
        }
        else
        {
                grown = exponential_minus_one(2.0 * magnitude);
                answer = 1.0 - 2.0 / (grown + 2.0);
        }

        return decimal_with_sign(answer, value);
}

/*
        The standard names.

        Each standard spelling is a static alias rather than a macro or a
        forwarding body: a program can take its address, a local variable
        called `exp` remains a variable, and both names reach the same code.
        An unused alias leaves no code behind and cannot collide at link time
        with a program that also links a real libc.

        There is no `log`. The library's `log` is the writer, and the natural
        logarithm is `logarithm`, or `ln` for short.

        Also absent, because standard.inc already has them under prose names:
        sqrt is square_root, fabs is absolute, trunc floor ceil round and
        nearbyint are the decimal_ roundings, copysign is decimal_with_sign,
        fmin and fmax are decimal_smaller and decimal_larger, fdim is
        decimal_difference and fma is decimal_multiply_add.
*/
static decimal ln(decimal) __attribute__((alias("logarithm")));
static decimal exp(decimal) __attribute__((alias("exponential")));
static decimal exp2(decimal) __attribute__((alias("exponential_two")));
static decimal expm1(decimal) __attribute__((alias("exponential_minus_one")));
static decimal log2(decimal) __attribute__((alias("logarithm_two")));
static decimal log10(decimal) __attribute__((alias("logarithm_ten")));
static decimal pow(decimal, decimal) __attribute__((alias("power")));
static decimal cbrt(decimal) __attribute__((alias("cube_root")));
static decimal hypot(decimal, decimal) __attribute__((alias("hypotenuse")));
static decimal sin(decimal) __attribute__((alias("sine")));
static decimal cos(decimal) __attribute__((alias("cosine")));
static decimal tan(decimal) __attribute__((alias("tangent")));
static decimal asin(decimal) __attribute__((alias("arc_sine")));
static decimal acos(decimal) __attribute__((alias("arc_cosine")));
static decimal atan(decimal) __attribute__((alias("arc_tangent")));
static decimal atan2(decimal, decimal) __attribute__((alias("arc_tangent_two")));
static decimal sinh(decimal) __attribute__((alias("hyperbolic_sine")));
static decimal cosh(decimal) __attribute__((alias("hyperbolic_cosine")));
static decimal tanh(decimal) __attribute__((alias("hyperbolic_tangent")));
static decimal fmod(decimal, decimal) __attribute__((alias("decimal_modulo")));
static decimal remainder(decimal, decimal)
    __attribute__((alias("decimal_remainder")));
static decimal ldexp(decimal, b32) __attribute__((alias("decimal_scaled")));
static decimal scalbn(decimal, b32) __attribute__((alias("decimal_scaled")));
static decimal frexp(decimal, b32 address_to)
    __attribute__((alias("decimal_split_exponent")));
static decimal modf(decimal, decimal address_to)
    __attribute__((alias("decimal_split_whole")));

#pragma GCC pop_options

#endif // !KERNEL_MODE && decimal_bits == 64

#endif // STANDARD_MODERN_C_STANDARD_MATH
#endif // STANDARD_SKIP_MATH

#ifndef STANDARD_SKIP_SIGNAL
/* ---- signal.c ---- */
/*
        Experimental C standard library

        <signal.h>: dispositions, masks, and the half of setjmp that was missing

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SIGNAL
#define STANDARD_MODERN_C_STANDARD_SIGNAL

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason src/standard/text.c gives at the same point: core.c
        includes the umbrella, library.c sets KERNEL_MODE from __MODULE__, and
        a module that pulled a second struct sigaction in beside the one
        <linux/signal.h> already has would not compile.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        What this file needs under it, said out loud so it can be included
        from a test before the umbrella grows a line for it.

        errno and the three return-shape wrappers belong to the error family
        and every routine here reports failure through them. The include is
        guarded on that file's own guard, so the umbrella including error.c
        first and a test including this file directly both end up with exactly
        one copy.

        setjmp.inc is next door and holds jump_mark, jump_to_mark and the
        thirty two slot jump_state that sigsetjmp extends. It guards itself
        too, and src/standard/text.c may already have read it.
*/
#ifndef STANDARD_MODERN_C_STANDARD_ERROR
#include "error.c"
#endif

#include "platform/setjmp.inc"

/*
        The numbers, which are the same on all three machines and are checked
        rather than remembered.

        Linux gives x86_64, arm64 and riscv64 the identical signal numbering:
        the asm-generic list, plus SIGSTKFLT at sixteen, which x86 contributed
        and the other two inherited. That was confirmed by building a program
        against each toolchain's own <signal.h> and printing all thirty one --
        every number below came back the same on all three.

        Each one is wrapped in its own ifndef and not in one block ifndef.
        library.c already defines SIGTRAP, SIGKILL, SIGSTOP and SIGCHLD for
        its own use and src/standard/stdlib.c defines SIGABRT for abort, so a
        single guard around the whole list would silently drop the other
        twenty six the first time any one of them was already present. Four
        families colliding on names that were each correct alone is the
        failure this arrangement exists to not have.

        SIGIOT, SIGPOLL and SIGCLD are the older spellings of SIGABRT, SIGIO
        and SIGCHLD and cost nothing to carry; code being ported in uses them
        and a missing one is a compile error a long way from its cause.
*/
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGIOT
#define SIGIOT 6
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGSTKFLT
#define SIGSTKFLT 16
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCLD
#define SIGCLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif
#ifndef SIGURG
#define SIGURG 23
#endif
#ifndef SIGXCPU
#define SIGXCPU 24
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 25
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 26
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGIO
#define SIGIO 29
#endif
#ifndef SIGPOLL
#define SIGPOLL 29
#endif
#ifndef SIGPWR
#define SIGPWR 30
#endif
#ifndef SIGSYS
#define SIGSYS 31
#endif
#ifndef SIGUNUSED
#define SIGUNUSED 31
#endif

/*
        The real-time range, which is a range and not a list.

        Linux reserves thirty two through sixty four for queued signals and
        gives the first few to whoever asks first; glibc takes thirty two and
        thirty three for its own thread cancellation and identity changes and
        hands out thirty four upwards. This library has no threads and takes
        none of them, so SIGRTMIN here is the kernel's thirty two rather than
        glibc's thirty four. That is a deliberate difference from glibc and it
        is the only one in this file's numbering; it is written down here
        because a program ported between the two and using SIGRTMIN+1 as a
        cookie would otherwise find the two libraries disagreeing about which
        signal it meant.
*/
#ifndef SIGRTMIN
#define SIGRTMIN 32
#endif
#ifndef SIGRTMAX
#define SIGRTMAX 64
#endif

//      Sixty four signals, numbered from one, so the highest is also the
//      count. NSIG is one past it, which is the spelling every header uses
//      for the bound of a loop over dispositions.
#define SIGNAL_HIGHEST 64

#ifndef NSIG
#define NSIG 65
#endif
#ifndef _NSIG
#define _NSIG 65
#endif

/*
        The flags, which are the kernel's own bit values and identical on the
        three machines -- again checked against each toolchain's
        <asm/signal.h> rather than recalled.

        SA_RESTORER is in this list and is not for callers. It is the bit that
        says a restorer address is present in the structure, this file sets it
        on x86_64 and only on x86_64, and it is cleared out of anything handed
        back to a caller so that a program reading a disposition it did not
        install does not see a flag it never set. riscv64 does not define it
        at all, which is why it is guarded and why nothing outside the one
        line that sets it may mention it.
*/
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x00000001
#endif
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x00000002
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x00000004
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0x40000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef SA_NOMASK
#define SA_NOMASK SA_NODEFER
#endif
#ifndef SA_ONESHOT
#define SA_ONESHOT SA_RESETHAND
#endif

//      Only x86_64 has the field this names. Spelled with the family key on
//      the internal side so that nothing here depends on whether a sibling
//      header already defined the standard name.
#define SIGNAL_KERNEL_RESTORER 0x04000000

//      What sigprocmask's first argument may be.
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif

/*
        A disposition is a function pointer, and the three that are not
        functions.

        Zero and one are addresses the kernel reads as "the default action"
        and "throw it away", and minus one is never a disposition -- it is
        what signal returns when it could not install one. All three are
        written as a pointer type so that a caller can compare against them
        without a cast and so that the comparison is against the same thing
        the field holds.
*/
typedef fn(address_to signal_handler)(b32 number);
typedef fn(address_to signal_restorer)(void);

#ifndef SIG_DFL
#define SIG_DFL ((signal_handler)0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((signal_handler)1)
#endif
#ifndef SIG_ERR
#define SIG_ERR ((signal_handler) - 1)
#endif

/*
        The restorer and the sigsetjmp stub, which are instructions and live
        next door for the reason that file sets out at length.

        It is read here, before anything that uses it, because
        signal_action_change fills in the restorer address on x86_64 and
        cannot do that with a name it has not seen. Everything the .inc needs
        is already in scope: jump_state and DEAD_END from setjmp.inc above,
        the syscall table and MOONWATER_NUMBER from library.c.
*/
#include "platform/signal.inc"

/*
        A set of signals, in the shape a program that was written against
        glibc already believes it has.

        This is the load-bearing decision in the file and it deserves the
        whole paragraph. There are two candidate layouts. The kernel's
        sigset_t is eight bytes, one bit per signal, and every rt_ system call
        below is told so by a trailing size argument that is always eight.
        glibc's sigset_t is a hundred and twenty eight bytes -- sixteen words,
        a thousand and twenty four bits -- of which it uses the first sixty
        four and leaves the rest alone. The kernel size is what the calls
        want; the glibc size is what a caller's header says a sigset_t is.

        This file uses the glibc size. The reason is the one thing this
        library is short of: the names it exports are real symbols with no C
        declaration, and the whole point of attaching the standard spellings
        is that an object compiled against real headers can be linked against
        this. Such an object declares sigset_t as a hundred and twenty eight
        bytes, allocates that many on its stack, and hands the address to
        sigemptyset. If sigemptyset here believed a set was eight bytes it
        would still work; if sigaction here believed a struct sigaction was
        the kernel's thirty two bytes it would read the caller's mask out of
        the wrong place and the whole file would be a quiet no-op for the one
        argument it exists to carry. Matching the caller costs a copy of eight
        bytes at each call and buys the only compatibility that matters.

        So: the public shapes are glibc's, the private ones are the kernel's,
        and the translation between them is one field assignment in each
        direction, written out below where it can be read.

        Confirmed on all three toolchains rather than assumed: sizeof
        sigset_t is 128, sizeof struct sigaction is 152, and its fields are at
        0, 8, 136 and 144 on x86_64, arm64 and riscv64 alike.
*/
#define SIGNAL_SET_WORDS 16
#define SIGNAL_KERNEL_SET_BYTES 8

typedef struct signal_set
{
        positive words[SIGNAL_SET_WORDS];
} signal_set;

/*
        Whether a number can name a signal at all.

        One through sixty four. Zero is not a signal -- kill takes it as "test
        whether the process exists" and sigaddset must refuse it -- and
        anything above sixty four has no bit. glibc answers EINVAL for both
        ends and so does this; the boundary was checked against glibc on all
        three machines, where sigaddset(64) succeeds and sigaddset(65) does
        not.
*/
static bool signal_number_bad(b32 number)
{
        return number < 1 || number > SIGNAL_HIGHEST;
}

/*
        The five set operations, which are a bit each.

        There is no library routine to reach for here and this is one of the
        places where a loop would be the wrong thing rather than the slow
        thing: every signal that exists lives in the first word, so the whole
        of each operation is a shift and one bitwise instruction. memory_zero
        does clear the other fifteen words, and it is used rather than a
        written-out loop because the size is a literal at the call site and
        the umbrella folds it to straight line stores.

        Clearing all sixteen words rather than only the first is what makes a
        set from here comparable, byte for byte, with a set a program built
        for itself. Recent glibc touches only the first word and leaves the
        rest as whatever was on the stack, which is defensible and is also why
        two sets holding the same signals can differ in a hundred and twenty
        bytes of it. Zeroing costs nothing measurable and removes a whole
        class of confusing comparison.
*/
b32 signal_set_empty(signal_set address_to set)
{
        memory_zero(set, sizeof(signal_set));

        return 0;
}

/*
        Every signal, and this library takes none of them for itself.

        glibc's sigfillset leaves thirty two and thirty three out, because
        NPTL uses those two to cancel threads and to change credentials across
        them, and a program that blocked them would break its own runtime.
        There are no threads here and nothing reserved, so a full set is full.
        The difference is visible if the two libraries are compared byte for
        byte and it is deliberate: copying a reservation this library does not
        have would be a lie in the one direction that is hard to find later.
*/
b32 signal_set_fill(signal_set address_to set)
{
        memory_zero(set, sizeof(signal_set));

        set->words[0] = (positive) - 1;

        return 0;
}

#define SIGNAL_SET_CHANGE(name, operation, mask)                             \
b32 name(signal_set address_to set, b32 number)                             \
{                                                                           \
        if (signal_number_bad(number))                                      \
        {                                                                   \
                errno = EINVAL;                                             \
                return -1;                                                  \
        }                                                                   \
        set->words[0] operation (mask);                                      \
        return 0;                                                           \
}
SIGNAL_SET_CHANGE(signal_set_add, |=, (positive)1 << (number - 1))
SIGNAL_SET_CHANGE(signal_set_remove, &=, ~((positive)1 << (number - 1)))
#undef SIGNAL_SET_CHANGE

b32 signal_set_has(const signal_set address_to set, b32 number)
{
        if (signal_number_bad(number))
        {
                errno = EINVAL;
                return -1;
        }

        return (b32)((set->words[0] >> (number - 1)) & 1);
}

/*
        The structure the kernel actually reads, which is not the one above
        and is not the same on all three machines.

        Linux builds struct sigaction out of two conditionals. The handler and
        the flags come first everywhere. A restorer field follows on
        architectures that define __ARCH_HAS_SA_RESTORER, and the mask comes
        last. x86_64 and arm64 define it, so their mask is at twenty four and
        the structure is thirty two bytes. riscv64 does not, so its mask is at
        sixteen and the structure is twenty four.

        That is exactly the trap this file was written to avoid walking into.
        A single four-word layout compiles on all three and works on all
        three as long as the third word is zero -- which is why the shell next
        door gets away with one, since it only ever installs SIG_IGN, SIG_DFL
        or a handler with an empty mask. The moment sa_mask carries anything,
        a four-word layout writes it into the restorer slot on riscv64 and the
        kernel reads zero: handlers still run, they still return, and the
        blocking the caller asked for silently does not happen. A test that
        only checks that a handler runs and returns passes with this bug in
        it, on the one machine of the three that has it.

        Verified rather than believed: preprocessing <asm/signal.h> with each
        of the three toolchains shows SA_RESTORER and __ARCH_HAS_SA_RESTORER
        defined for x86_64 and aarch64 and absent for riscv64. And the kernel
        does not object to being told otherwise -- a program that hands
        riscv64 the four word layout with the restorer bit set gets a zero
        back from rt_sigaction, its handler runs, and its mask is whatever
        eight bytes were in the restorer slot. That was measured, not
        imagined, and it is why the shape below is decided at compile time and
        checked at compile time rather than trusted.

        The condition the kernel actually uses is __ARCH_HAS_SA_RESTORER, not
        "x86_64 or arm64", and those two happen to be the same thing on every
        machine this library builds for. A fourth architecture arriving must
        be checked against that name in its own <asm/signal.h> rather than
        added to the list below by resemblance.
*/
#if X64 || ARM64
#define SIGNAL_KERNEL_ACTION_BYTES 32
#else
#define SIGNAL_KERNEL_ACTION_BYTES 24
#endif

typedef struct signal_kernel_action
{
        signal_handler handler;
        positive flags;

#if X64 || ARM64
        signal_restorer restorer;
#endif

        positive mask;
} signal_kernel_action;

/*
        Three sizes this file's correctness rests on, made into compile
        errors.

        A negative array bound is the portable way to say "stop" at
        preprocessing time, and these three are worth saying it about because
        every one of them was established by a separate probe program rather
        than by anything in this file. If a compiler ever packs or pads these
        differently, the failure without the check is not a build error: it is
        a mask read out of the wrong offset, on one architecture, at run time,
        with the handler still running and returning correctly.

        128 and 152 are glibc's sigset_t and struct sigaction on all three
        toolchains. 32 and 24 are the kernel's struct sigaction with and
        without the restorer slot.
*/
typedef char signal_set_is_the_size_a_caller_believes
        [(sizeof(signal_set) == 128) ? 1 : -1];

typedef char signal_kernel_action_is_the_size_the_kernel_reads
        [(sizeof(signal_kernel_action) == SIGNAL_KERNEL_ACTION_BYTES) ? 1 : -1];

/*
        The full disposition, in the caller's shape.

        Field for field this is glibc's struct sigaction: the handler union
        first, then the mask, then the flags, then the restorer. The order is
        not the kernel's and is not chosen here -- it is whatever a program's
        own <signal.h> says, and agreeing with that is the entire reason this
        type exists separately from the one above.

        There is one union in C's version, between a handler taking a number
        and a handler taking a number, a siginfo and a context. Both are
        pointers to code and the kernel is told which by SA_SIGINFO, so this
        holds the first spelling and a caller installing a three-argument
        handler casts. A union of two function pointer types would be the same
        eight bytes with two names, and the cast is the honest way to say that
        the flag and the pointer have to agree.
*/
typedef struct signal_action
{
        signal_handler handler;
        signal_set mask;
        b32 flags;
        signal_restorer restorer;
} signal_action;

typedef char signal_action_is_the_size_a_caller_believes
        [(sizeof(signal_action) == 152) ? 1 : -1];

/*
        sigaction, and the translation in both directions.

        The flags are widened through p32 and not through b32, because
        SA_RESETHAND is 0x80000000 and a signed widening of it would set every
        one of the top thirty two bits in the word the kernel reads. That is
        the kind of thing that works on a test that never uses the flag.

        On the way out the restorer is filled in on x86_64 only and the flag
        that announces it is set at the same moment, so the two can never
        disagree. On the way back that same flag is cleared from what the
        caller is told, along with the restorer field, because neither was
        the caller's: a program that installs a handler, reads the disposition
        back, and compares it against what it asked for should find them
        equal.

        A caller passing null for wanted is asking only to read, and one
        passing null for previous is asking only to write; both are passed
        straight through as null so the kernel does the deciding, and neither
        structure is copied when it is not going to be looked at.
*/
b32 signal_action_change(b32 number, const signal_action address_to wanted,
                         signal_action address_to previous)
{
        signal_kernel_action asked;
        signal_kernel_action had;
        b32 outcome;

        memory_zero(address_of asked, sizeof asked);
        memory_zero(address_of had, sizeof had);

        if (wanted)
        {
                asked.handler = wanted->handler;
                asked.flags = (positive)(p32)wanted->flags;
                asked.mask = wanted->mask.words[0];

#if X64
                asked.flags |= (positive)SIGNAL_KERNEL_RESTORER;
                asked.restorer = signal_return_trampoline;
#endif
        }

        outcome = error_result(system_call_4(
                syscall(rt_sigaction), (positive)number,
                wanted ? (positive)address_of asked : 0,
                previous ? (positive)address_of had : 0,
                SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        if (previous)
        {
                memory_zero(previous, sizeof(signal_action));

                previous->handler = had.handler;
                previous->flags =
                        (b32)(p32)(had.flags &
                                   ~(positive)SIGNAL_KERNEL_RESTORER);
                previous->mask.words[0] = had.mask;
        }

        return 0;
}

/*
        signal, which is sigaction with an opinion.

        There have been two of these historically. System V's resets the
        disposition to the default before the handler runs and does not
        restart an interrupted system call, so a handler that wants to keep
        catching has to reinstall itself and every read has to be retried by
        hand. BSD's does neither, and glibc's signal is BSD's. This is BSD's,
        for the same reason: a program ported in was written against whatever
        its libc did, and its libc was glibc.

        So SA_RESTART and an empty mask -- empty because the kernel blocks the
        signal being delivered for the duration of its own handler unless
        SA_NODEFER says otherwise, and sa_mask is for blocking the OTHER
        signals a handler must not be interrupted by.

        The previous handler is read back in the same call and returned, which
        is the one thing signal does that a caller usually keeps.
*/
signal_handler signal_handle(b32 number, signal_handler handler)
{
        signal_action wanted;
        signal_action had;

        memory_zero(address_of wanted, sizeof wanted);
        memory_zero(address_of had, sizeof had);

        wanted.handler = handler;
        wanted.flags = SA_RESTART;

        if (signal_action_change(number, address_of wanted, address_of had) < 0)
                return SIG_ERR;

        return had.handler;
}

/*
        raise, sent to this thread and not to this process.

        kill would deliver to the process, which means any thread that has the
        signal unblocked may take it -- and for a signal a program raises at
        itself, another thread taking it is never what was meant. tgkill names
        the thread. src/standard/stdlib.c's abort says the same thing about
        SIGABRT and reaches for the same call, and there is only one process
        and one thread in a spark binary today, so the difference is currently
        invisible and will stop being invisible the day there is a second
        thread.

        Neither getpid nor gettid can fail, so both are unwrapped, exactly as
        the error family's getpid is.
*/
b32 signal_raise(b32 number)
{
        b32 process = (b32)system_call(syscall(getpid));
        b32 thread = (b32)system_call(syscall(gettid));

        return error_result(system_call_3(syscall(tgkill), (positive)process,
                                         (positive)thread, (positive)number));
}

/*
        The mask, which is the one place the eight byte kernel set and the
        hundred and twenty eight byte caller set meet.

        Reading is into a single word and writing is out of one. The rest of
        the caller's set is not consulted on the way in -- there are no
        signals there to consult -- and is zeroed on the way out, so what
        comes back is a set that compares equal to one built by hand holding
        the same signals.

        how is not checked here. The kernel checks it and answers EINVAL, and
        checking it twice would mean this file having an opinion about which
        values are legal that could drift from the kernel's. Confirmed against
        glibc: an out of range how gives -1 and EINVAL from both.
*/
b32 signal_mask_change(b32 how, const signal_set address_to wanted,
                       signal_set address_to previous)
{
        positive asked = wanted ? wanted->words[0] : 0;
        positive had = 0;
        b32 outcome;

        outcome = error_result(system_call_4(
                syscall(rt_sigprocmask), (positive)how,
                wanted ? (positive)address_of asked : 0,
                previous ? (positive)address_of had : 0,
                SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        if (previous)
        {
                memory_zero(previous, sizeof(signal_set));
                previous->words[0] = had;
        }

        return 0;
}

/*
        sigsuspend, which is the only routine here whose success is a failure.

        It installs a mask, waits, runs whatever handler arrives, restores the
        old mask and returns. It always returns -1 with EINTR, because the
        only way out is a signal, and POSIX says so rather than inventing a
        success value for it. A caller that checks the return against -1 and
        reports an error is a caller with a bug, and every libc has the same
        shape here.

        A null mask means the empty mask -- suspend with nothing blocked --
        and that is the one place in this file where null is a value rather
        than an absence. Everywhere else a null structure means "not
        supplied", because there is another argument doing the work; here the
        mask IS the work and there is nothing for an absence to mean. glibc
        dereferences the null and the program stops, which is defensible and
        is not more useful. Written down because it is the only pointer in
        this file whose contract is not the one the others have.
*/
b32 signal_mask_suspend(const signal_set address_to mask)
{
        positive asked = mask ? mask->words[0] : 0;

        return error_result(system_call_2(syscall(rt_sigsuspend),
                                         (positive)address_of asked,
                                         SIGNAL_KERNEL_SET_BYTES));
}

//      What has arrived and is being held back because it is blocked.
b32 signal_mask_pending(signal_set address_to into)
{
        positive had = 0;
        b32 outcome;

        outcome = error_result(system_call_2(syscall(rt_sigpending),
                                            (positive)address_of had,
                                            SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        memory_zero(into, sizeof(signal_set));
        into->words[0] = had;

        return 0;
}

/*
        A timer, because two of the three machines have no alarm.

        alarm is an old x86 system call, number thirty seven, and the
        asm-generic table that arm64 and riscv64 both use does not carry it at
        all -- there is no syscall_linux_arm64_alarm to reach for and a
        version written against the x86 number would not compile on the other
        two, which is the good outcome. setitimer exists on all three and
        ITIMER_REAL is the same clock alarm arms, so alarm is setitimer with
        the repeat interval left at zero, which is what glibc does on exactly
        the same machines and for exactly the same reason.

        The four fields are two timevals, repeat first and first-fire second,
        and they are spelled out rather than reusing the clock family's
        timeval so that this file can be included before that one. Nothing is
        gained by sharing a structure of two longs across an ordering
        dependency.

        The value handed back is what was left of a previous alarm, in whole
        seconds, rounded UP when there were microseconds on the end. Rounding
        down would let alarm(1) followed immediately by alarm(0) report zero
        seconds remaining, which reads as "there was no alarm" when there
        certainly was one. glibc rounds up here and this rounds up with it.
*/
#define SIGNAL_TIMER_REAL 0

typedef struct signal_interval
{
        bipolar repeat_seconds;
        bipolar repeat_microseconds;
        bipolar first_seconds;
        bipolar first_microseconds;
} signal_interval;

p32 signal_alarm(p32 seconds)
{
        signal_interval wanted;
        signal_interval had;

        memory_zero(address_of wanted, sizeof wanted);
        memory_zero(address_of had, sizeof had);

        wanted.first_seconds = (bipolar)seconds;

        if (error_result(system_call_3(syscall(setitimer), SIGNAL_TIMER_REAL,
                                      (positive)address_of wanted,
                                      (positive)address_of had)) < 0)
                return 0;

        return (p32)had.first_seconds + (had.first_microseconds ? 1 : 0);
}

/*
        pause, which is also not a system call on two of the three.

        Number thirty four on x86_64 and absent from the asm-generic table.
        The stand-in is the one glibc uses on those machines: read the mask
        that is in force and suspend with it, which blocks exactly what was
        already blocked and waits.

        It is used on all three rather than only on the two that need it. The
        alternative -- the real call on x86_64 and this everywhere else --
        would mean the one routine in this file whose behaviour was written
        twice, and the difference between the two is nothing a program can
        observe: a signal that arrives in the window between reading the mask
        and suspending runs its handler and then this waits forever, and a
        signal that arrives just before the real pause does the same.
*/
b32 signal_wait(void)
{
        positive mask = 0;

        system_signal_mask(SIG_BLOCK, 0, address_of mask,
                           SIGNAL_KERNEL_SET_BYTES);

        return error_result(system_call_2(syscall(rt_sigsuspend),
                                         (positive)address_of mask,
                                         SIGNAL_KERNEL_SET_BYTES));
}

/*
        The half of sigsetjmp that is not a jump.

        Called from the stub with the state and the flag still in the
        registers they arrived in. Slot 26 records whether a mask was wanted,
        because siglongjmp has to know without being told again, and slot 27
        holds it.

        Both slots are written even when no mask was asked for. A jump_state
        is an automatic array in most callers and holds whatever was on the
        stack; leaving slot 26 alone would mean a sigsetjmp with a zero second
        argument being read later as one that saved a mask, and restoring
        eight bytes of stack rubbish as the process's signal mask is a failure
        with no plausible symptom.

        The mask is read with SIG_BLOCK and a null set, which is how every
        libc asks "what is blocked right now" -- blocking nothing changes
        nothing and the old value comes back through the third argument.
*/
KEEP fn signal_jump_save(jump_state state, b32 save_mask)
{
        positive mask = 0;

        state[SIGNAL_JUMP_SAVED] = save_mask ? 1 : 0;
        state[SIGNAL_JUMP_MASK] = 0;

        if (!save_mask)
                return;

        system_signal_mask(SIG_BLOCK, 0, address_of mask,
                           SIGNAL_KERNEL_SET_BYTES);

        state[SIGNAL_JUMP_MASK] = mask;
}

/*
        siglongjmp, which unlike its partner is ordinary C.

        The asymmetry is worth stating because it looks like an oversight.
        sigsetjmp had to be assembly because jump_mark records the stack of
        whoever called it, and a C wrapper would have interposed a frame that
        is dead by the time the jump comes back. jump_to_mark records nothing
        and returns to nobody -- it moves the stack pointer to the mark and
        continues there -- so a C wrapper's frame is simply abandoned, which
        is what would have happened to it anyway.

        The mask goes back before the jump and not after, because after there
        is no after: jump_to_mark does not return here. Restoring first also
        means the code at the mark starts with the mask the mark was taken
        under, which is the entire promise sigsetjmp's second argument makes.

        A state saved with a zero second argument leaves the mask exactly as
        it is, which is _setjmp's behaviour and is what a program that did not
        ask for the syscall is paying nothing for.
*/
fn signal_jump_to_mark(jump_state state, b32 value)
{
        if (state[SIGNAL_JUMP_SAVED])
        {
                positive mask = state[SIGNAL_JUMP_MASK];

                system_signal_mask(SIG_SETMASK, address_of mask, 0,
                                   SIGNAL_KERNEL_SET_BYTES);
        }

        jump_to_mark(state, value);
}

/*
        The names a C program knows these by, as second labels on the same
        addresses, exactly as library.c attaches strlen to string_length.

        kill is not in this list and must not be added to it. The error family
        already defines kill, as a static C function with the POSIX name
        itself, so there is no prose routine here to attach and a .set naming
        it would be a second definition of a symbol the assembler has already
        seen in this translation unit.

        __sigsetjmp is beside sigsetjmp because glibc's header makes sigsetjmp
        a macro over __sigsetjmp, so an object compiled against real headers
        has a relocation against the underscored spelling and nothing against
        the plain one. Both are the same address here.
*/
__asm__(
    ASM_ALIAS(signal,      signal_handle)
    ASM_ALIAS(raise,       signal_raise)
    ASM_ALIAS(sigaction,   signal_action_change)
    ASM_ALIAS(sigemptyset, signal_set_empty)
    ASM_ALIAS(sigfillset,  signal_set_fill)
    ASM_ALIAS(sigaddset,   signal_set_add)
    ASM_ALIAS(sigdelset,   signal_set_remove)
    ASM_ALIAS(sigismember, signal_set_has)
    ASM_ALIAS(sigprocmask, signal_mask_change)
    ASM_ALIAS(sigsuspend,  signal_mask_suspend)
    ASM_ALIAS(sigpending,  signal_mask_pending)
    ASM_ALIAS(alarm,       signal_alarm)
    ASM_ALIAS(pause,       signal_wait)
    ASM_ALIAS(sigsetjmp,   signal_jump_mark)
    ASM_ALIAS(__sigsetjmp, signal_jump_mark)
    ASM_ALIAS(siglongjmp,  signal_jump_to_mark)
);

/*
        And, unusually for this tree, the declarations to go with them.

        Everywhere else in this library a standard name is a symbol with no
        prototype: nm shows memcpy and strlen and sqrt in the object, and
        &memcpy still does not compile, because a .set tells the linker a name
        and tells the compiler nothing. That gap is the single largest thing
        between this library and an ordinary C program, and it is not this
        family's to close in general.

        It is closable for one family at a time at the cost of four lines,
        which is what follows. A declaration with no definition is not an
        error; the .set above supplies the definition at link time. So code in
        this tree can write sigaction(SIGINT, &wanted, 0) and have it compile
        and link, which is the first place in this library where a POSIX name
        is usable as a name rather than only as a symbol.

        The types are this file's, not <signal.h>'s, and that is the honest
        thing rather than a limitation: a translation unit that included the
        real header would take its declarations from there and never see
        these. The one turned into a name a caller must not shadow is
        sigaction, which is a function here and a structure in the real
        header; nothing in this tree declares struct sigaction, and a file
        that needs to should not include this one.

        STANDARD_SIGNAL_NO_DECLARATIONS turns the block off in one line if a
        later family arrives with its own spelling of any of these.
*/
#ifndef STANDARD_SIGNAL_NO_DECLARATIONS

extern signal_handler signal(b32 number, signal_handler handler);
extern b32 raise(b32 number);
extern b32 sigaction(b32 number, const signal_action address_to wanted,
                     signal_action address_to previous);
extern b32 sigemptyset(signal_set address_to set);
extern b32 sigfillset(signal_set address_to set);
extern b32 sigaddset(signal_set address_to set, b32 number);
extern b32 sigdelset(signal_set address_to set, b32 number);
extern b32 sigismember(const signal_set address_to set, b32 number);
extern b32 sigprocmask(b32 how, const signal_set address_to wanted,
                       signal_set address_to previous);
extern b32 sigsuspend(const signal_set address_to mask);
extern b32 sigpending(signal_set address_to into);
extern p32 alarm(p32 seconds);
extern b32 pause(void);
extern b32 sigsetjmp(jump_state state, b32 save_mask) __attribute__((returns_twice));
extern fn siglongjmp(jump_state state, b32 value) DEAD_END;

#endif // STANDARD_SIGNAL_NO_DECLARATIONS

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SIGNAL
#endif // STANDARD_SKIP_SIGNAL

/*
        stream.c and everything after it. Its last act is to #undef stdin,
        stdout and stderr and redefine them from the descriptor numbers into
        the pointers a C program means, so anything above this line that
        wanted the numbers still has them and anything below gets streams.
*/

#ifndef STANDARD_SKIP_STREAM
/* ---- stream.c ---- */
/*
        Experimental C standard library

        <stdio.h>'s FILE: a descriptor, a buffer, a position and a few flags

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_STREAM
#define STANDARD_MODERN_C_STANDARD_STREAM

/*
        This is ordinary C on purpose, for the reason netlink.c gives.

        library.c and everything it includes holds declarations and assembly
        and nothing else, and that is checked. A buffered stream is policy all
        the way down -- when to refill, when to flush, which direction the
        buffer is currently facing, whether a terminal means line buffering --
        and none of it is something one machine would do differently from
        another. Written here it is written once and the same bytes run on all
        three targets.

        It depends on library.c alone: the raw traps, memory_copy,
        memory_first_of, system_write_all and system_read_retry. The one thing
        it borrows from a sibling is the allocator, and that borrowing is
        confined to the three macros at the top of the file so there is one
        place to change if the allocator lands under different names.

        Everything below the guard needs a platform: syscall numbers, a
        descriptor table, a kernel that answers ioctl. library.c puts all of
        that inside "#ifndef KERNEL_MODE" and then "#ifndef
        STANDARD_NO_PLATFORM", and the two are not the same condition -- the
        standard test lane sets the second to prove the pure library still
        compiles with nothing underneath it, while a kernel module sets
        neither and still has no system_call_3 to reach for, because inside a
        kernel there is nobody to trap to. Both guards are repeated here. A
        build that is either of those things simply has no streams, which is
        right: a kernel module has the kernel's own file layer and a buffered
        FILE on top of it would be a second one.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        The allocator this family assumes exists.

        fopen has to put the stream object somewhere, getline has to hand back
        a buffer the caller is allowed to free, and neither can come from
        memory()/memory_free() because those want the size back at release
        time and free() does not get one. So this family assumes the sibling
        allocator family's malloc, realloc and free, with the signatures the
        standard gives them.

        Three macros rather than three calls spread through the file: if the
        allocator arrives under other names, or arrives as macros of its own,
        this is the only block that has to change. The prototypes are here so
        the file compiles on its own; they are the standard ones, so a second
        identical declaration from the allocator's own header is legal C and
        costs nothing.

        The standard streams deliberately do not use any of it. Their buffers
        are static arrays below, so a program that only writes to stdout never
        touches the allocator at all, and a broken or absent allocator cannot
        take the diagnostic path down with it.
*/
address_any malloc(positive size);
address_any calloc(positive count, positive size);
fn free(address_any block);

#define stream_allocate(size) malloc(size)
#define stream_allocate_zeroed(size) calloc(1, size)
#define stream_release(block) free(block)

/*
        The three names library.c gave to descriptor numbers.

        library.c defines stdin, stdout and stderr as 0, 1 and 2, which is
        what a program wants when it is about to call write(2) by hand and
        exactly not what it wants when it is about to call fprintf. This file
        takes the names for the streams, because that is what every C program
        in the world means by them, and keeps the numbers under names that say
        what they are. Nothing in the tree used the macros as descriptors when
        this was written -- the shell and the text tools carry their own
        constants -- so no caller changes meaning; a later one that wants the
        number now has somewhere to ask for it.
*/
#undef stdin
#undef stdout
#undef stderr

#define standard_input_descriptor 0
#define standard_output_descriptor 1
#define standard_error_descriptor 2

/*
        setvbuf's three modes, and the sizes.

        The numbers are glibc's, because a program that writes _IONBF has
        almost certainly been compiled against glibc's headers at some point
        and a different numbering would silently turn "unbuffered" into
        "fully buffered". BUFSIZ is 4096 rather than glibc's 8192: a page is
        the unit every read and write here ends up costing, and MAX_INPUT
        beside it in any.inc is the same number.

        A dynamically attached default is eight bytes smaller. malloc puts
        its tag in the same allocation, so asking it for a 4096-byte payload
        needs the next 5120-byte shelf; 4088 plus that tag fits one 4096-byte
        shelf exactly. Static stdin and stdout buffers have no allocation
        header and keep the full BUFSIZ. Large fread/fwrite calls bypass the
        buffer, so the only throughput difference is one extra refill or
        flush per roughly two megabytes of buffered traffic.
*/
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define EOF (-1)
#define BUFSIZ 4096
#define STREAM_DYNAMIC_BUFFER (BUFSIZ - sizeof(positive))

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/*
        The open(2) bits, spelled out rather than reused.

        library.c has FILE_READ, FILE_WRITE and the rest, but they are
        combinations -- FILE_WRITE is O_WRONLY|O_CREAT|O_TRUNC in one name --
        and fopen needs the individual bits so that "a" can ask for append
        without truncate and "r+" can ask for read-write without create. These
        are the asm-generic values, identical on all three targets.
*/
#define stream_open_read_only 00
#define stream_open_write_only 01
#define stream_open_read_write 02
#define stream_open_create 0100
#define stream_open_exclusive 0200
#define stream_open_truncate 01000
#define stream_open_append 02000

// rw-rw-rw- before the process umask, which is what fopen("w") creates with.
#define stream_create_permissions 0666

/*
        TCGETS, which is how isatty asks.

        There is no isatty syscall. The question "is this descriptor a
        terminal" is answered by asking the descriptor for its terminal
        attributes and seeing whether the kernel refuses: a pipe, a regular
        file and a socket all answer ENOTTY. The request number is 0x5401 on
        x86_64 and on everything that took asm-generic's ioctls, which is
        arm64 and riscv64 both.

        struct termios is sixty bytes on Linux. The scratch here is one
        hundred and twenty eight, aligned to eight by being an array of p64,
        because the kernel writes the whole struct and a scratch that is too
        small smashes whatever is next on the stack -- and the caller of
        isatty is usually deciding how to buffer, which is to say it is called
        once at the top of a program where a smashed frame is hardest to find.
*/
#define stream_terminal_attributes_request 0x5401

/*
        How deep a pushback goes.

        The standard promises one byte and says a second may fail. One is
        genuinely all a conforming program may rely on, and one is also
        exactly what a hand-rolled tokeniser reaches for at the worst possible
        moment -- the first byte of the stream, before any read, where a
        design that pushes back into the read buffer has nowhere to put it.
        This is a separate small array for that reason: it works at offset
        zero, it works on an unbuffered stream, and it works on a pipe. Eight
        rather than one because the array costs eight bytes either way once
        the struct is aligned, and a recursive-descent parser that wants three
        is not doing anything unreasonable.
*/
#define stream_pushback_bytes 8

/*
        The flags, which are the whole state machine.

        READABLE and WRITABLE come from the mode text and never change after
        that. AT_END and FAILED are the two indicators feof and ferror report
        and clearerr resets. BUFFER_OURS and STRUCT_OURS say what fclose is
        allowed to hand back to the allocator -- a buffer the caller supplied
        through setvbuf is not ours, and neither is the struct behind stdout.

        MODE_KNOWN is the one that is not obvious. Whether a stream is line
        buffered is decided by asking the descriptor whether it is a terminal,
        and that costs an ioctl, and setvbuf is allowed to overrule the answer
        before any input or output happens. So the question is not asked at
        open time; it is asked the first time the stream is actually used, and
        MODE_KNOWN records that it has been asked. setvbuf sets it without
        asking, which is what makes setvbuf's promise -- "before any other
        operation" -- cost nothing to keep.
*/
#define STREAM_READABLE 0x0001
#define STREAM_WRITABLE 0x0002
#define STREAM_APPEND 0x0004
#define STREAM_AT_END 0x0008
#define STREAM_FAILED 0x0010
#define STREAM_BUFFER_OURS 0x0020
#define STREAM_STRUCT_OURS 0x0040
#define STREAM_LINE_BUFFERED 0x0080
#define STREAM_UNBUFFERED 0x0100
#define STREAM_MODE_KNOWN 0x0200
#define STREAM_REGISTERED 0x0400

/*
        What a FILE is here.

        A descriptor, one buffer, and the two cursors that say what is in it.
        The buffer faces one direction at a time: either read_head..read_tail
        holds bytes the kernel has handed over that the caller has not asked
        for yet, or 0..write_used holds bytes the caller has handed over that
        the kernel has not been told about yet. Never both, which is what
        stream_face_reading and stream_face_writing enforce, and which is why
        an update stream has to flush or seek between a write and a read --
        the standard requires that of the caller and this is the reason.

        There is deliberately no mirror of the kernel's file offset. Keeping
        one is the obvious design and it is wrong twice over: fileno is a
        public entry, so any caller may lseek the descriptor out from under
        the mirror with nothing here able to notice, and a mirror is a second
        source of truth that only ever agrees with the first by luck. ftell
        asks the kernel instead, every time, and corrects the answer by what
        the buffer is holding.

        single is the one-byte buffer an unbuffered stream reads through, so
        that every path in the file can assume buffer and buffer_size are
        valid and nothing has to special-case a null buffer. It is inside the
        struct rather than allocated because an unbuffered stream is usually
        stderr, and stderr must work when the allocator does not.
*/
typedef struct stream stream;

struct stream
{
        b32 descriptor;
        p32 flags;

        p8 address_to buffer;
        positive buffer_size;

        positive read_head;
        positive read_tail;
        positive write_used;

        //      Which process put the bytes in write_used there. Stamped when
        //      the buffer goes from empty to holding something, and read by
        //      the flush at exit, which will not write out a buffer another
        //      process filled. See the block above stdlib_buffers_are_ours.
        positive owner;

        positive pushback_used;
        p8 pushback[stream_pushback_bytes];

        p8 single[1];

        stream address_to next;
};

typedef stream FILE;

/*
        The standard streams.

        Static objects rather than pointers into allocated memory, and macros
        rather than exported pointer variables. A pointer variable would need
        its initialiser resolved before main, and there is no code before main
        here -- _start calls it directly -- so it would have to be an address
        written into .data by the linker. That works on a static non-PIE link
        and it is one more thing that has to keep working; an address-of on a
        static object is the same value with nothing to arrange.

        Only scalars are initialised, so these three occupy sixteen bytes of
        .data between them and everything else about them is zero from .bss.
        The buffers are attached on first use by stream_ready, which is also
        where the terminal question gets asked.

        stderr is unbuffered from the start, as the standard requires: a
        diagnostic that is still sitting in a buffer when the program dies is
        a diagnostic that was never written.
*/
static stream stream_standard_input = {
        .descriptor = standard_input_descriptor,
        .flags = STREAM_READABLE,
};

static stream stream_standard_output = {
        .descriptor = standard_output_descriptor,
        .flags = STREAM_WRITABLE,
};

static stream stream_standard_error = {
        .descriptor = standard_error_descriptor,
        .flags = STREAM_WRITABLE | STREAM_UNBUFFERED | STREAM_MODE_KNOWN,
};

#define stdin (address_of stream_standard_input)
#define stdout (address_of stream_standard_output)
#define stderr (address_of stream_standard_error)

static p8 stream_standard_input_bytes[BUFSIZ];
static p8 stream_standard_output_bytes[BUFSIZ];

/*
        Every stream fflush(null) has to reach.

        The three above are always reachable by name. Everything fopen and
        fdopen hand out is on this list, singly linked through the struct's
        own next field, newest first, because the order fflush visits them in
        is not observable and prepending is the only insertion that cannot
        fail.
*/
static stream address_to stream_open_list = null;

bool stream_is_terminal(b32 descriptor);
stream address_to stream_open(string_address path, string_address mode);
stream address_to stream_adopt(b32 descriptor, string_address mode);
stream address_to stream_reopen(string_address path, string_address mode,
                                stream address_to handle);
b32 stream_close(stream address_to handle);
sized stream_read(address_any into, sized size, sized count,
                  stream address_to handle);
sized stream_write(address_any from, sized size, sized count,
                   stream address_to handle);
b32 stream_seek(stream address_to handle, bipolar offset, b32 whence);
bipolar stream_tell(stream address_to handle);
fn stream_rewind(stream address_to handle);
b32 stream_flush(stream address_to handle);
PURE b32 stream_at_end(stream address_to handle);
PURE b32 stream_failed(stream address_to handle);
fn stream_clear_state(stream address_to handle);
b32 stream_set_buffering(stream address_to handle, string_address buffer,
                         b32 mode, sized size);
fn stream_set_buffer(stream address_to handle, string_address buffer);
PURE b32 stream_descriptor(stream address_to handle);
b32 stream_get_byte(stream address_to handle);
b32 stream_get_byte_standard(void);
b32 stream_unget_byte(b32 byte, stream address_to handle);
string_address stream_get_line(string_address into, b32 limit,
                               stream address_to handle);
bipolar stream_get_delimited(address_any line, sized address_to capacity,
                             b32 delimiter, stream address_to handle);
bipolar stream_get_line_allocated(address_any line, sized address_to capacity,
                                  stream address_to handle);
b32 stream_put_byte(b32 byte, stream address_to handle);
b32 stream_put_string(string_address text, stream address_to handle);
positive stream_put_bytes(stream address_to handle, address_any data,
                          positive length);

/* Keep syscall policies named at each call; only writes need both the
   accepted prefix and the terminal error from the shared assembly loop. */
#define stream_trap_read(descriptor, into, length)                           \
        error_result(system_read_retry((positive)(descriptor), (into), (length)))
#define stream_trap_seek(descriptor, offset, whence)                         \
        error_result(system_seek((descriptor), (offset), (whence)))
#define stream_trap_close(descriptor) error_result(system_close(descriptor))
#define stream_trap_open(path, flags, permissions)                           \
        error_result(system_open_at_mode(AT_FDCWD, (path), (flags), (permissions)))

static positive stream_trap_write(b32 descriptor, address_any from, positive length)
{
        system_write_result result =
                system_write_all_checked((positive)descriptor, from, length);
        if (result.bytes != length)
                errno = result.error < 0 ? (b32)-result.error : EIO;
        return result.bytes;
}

/*
        Is this descriptor a terminal.

        A failed ioctl is the answer, not an error: every non-terminal refuses
        it, and refusing is how they say what they are. The result is
        deliberately not remembered anywhere -- it is asked once per stream,
        by stream_ready, and a stream is opened far less often than it is
        written to.
*/
bool stream_is_terminal(b32 descriptor)
{
        p64 attributes[16];

        return system_control(descriptor, stream_terminal_attributes_request,
                              attributes) == 0;
}

/*
        Attach a buffer and settle the buffering policy, once.

        Called at the top of every operation that touches the buffer. After
        the first call it is a load, a test and a return, which is why it is
        allowed to be everywhere.

        The policy is the one the standard describes: a stream that refers to
        a terminal is line buffered, everything else is fully buffered. The
        distinction matters in one direction only -- an interactive program
        that writes a prompt without a newline and then reads has to have the
        prompt on the screen already -- and it costs one ioctl per stream to
        get right.

        A buffer that cannot be allocated is not a failure. The stream becomes
        unbuffered and keeps working, one syscall per byte, which is slow and
        correct; refusing to open the stream at all would be neither.
*/
static fn stream_ready(stream address_to handle)
{
        if (!(handle->flags & STREAM_MODE_KNOWN))
        {
                if (stream_is_terminal(handle->descriptor))
                        handle->flags |= STREAM_LINE_BUFFERED;

                handle->flags |= STREAM_MODE_KNOWN;
        }

        if (handle->buffer != null)
                return;

        if (handle->flags & STREAM_UNBUFFERED)
        {
                handle->buffer = handle->single;
                handle->buffer_size = 1;
                return;
        }

        if (handle == address_of stream_standard_input)
        {
                handle->buffer = stream_standard_input_bytes;
                handle->buffer_size = BUFSIZ;
                return;
        }

        if (handle == address_of stream_standard_output)
        {
                handle->buffer = stream_standard_output_bytes;
                handle->buffer_size = BUFSIZ;
                return;
        }

        handle->buffer = (p8 address_to)stream_allocate(STREAM_DYNAMIC_BUFFER);

        if (handle->buffer == null)
        {
                handle->flags |= STREAM_UNBUFFERED;
                handle->buffer = handle->single;
                handle->buffer_size = 1;
                return;
        }

        handle->flags |= STREAM_BUFFER_OURS;
        handle->buffer_size = STREAM_DYNAMIC_BUFFER;
}

/*
        Push what is staged at the kernel, and forget it either way.

        Forgetting it on failure is deliberate and is what buffered_flush in
        any.inc does for the same reason: a caller that reports the failure
        and then flushes again -- or simply exits through a path that flushes
        -- must not write the same prefix a second time. The bytes are lost,
        the error indicator is set, and ferror is how anyone finds out.
*/
static b32 stream_flush_output(stream address_to handle)
{
        positive staged = handle->write_used;
        positive written;

        if (staged == 0)
                return 0;

        handle->write_used = 0;
        written = stream_trap_write(handle->descriptor, handle->buffer, staged);

        if (written != staged)
        {
                handle->flags |= STREAM_FAILED;
                return EOF;
        }

        return 0;
}

/*
        Throw away buffered input, and put the file offset back where the
        caller thinks it is.

        Everything read ahead of the caller is, from the caller's point of
        view, still in front of it: the kernel offset is past bytes nobody has
        asked for. Anything that abandons the buffer therefore has to seek
        back by exactly that much, or the next read -- or the next lseek by
        anyone holding fileno -- starts in the wrong place.

        Pushback counts toward that distance. ungetc moves the caller's
        position backwards by one, so a byte in the pushback array is a byte
        the caller is entitled to see again and the kernel has already passed.

        A pipe or a terminal cannot seek and does not need to: there is no
        position to be wrong about. The seek is attempted and its failure
        ignored, which is one syscall on a stream that is being abandoned
        anyway.
*/
static fn stream_drop_input(stream address_to handle, bool restore_position)
{
        positive unread = (handle->read_tail - handle->read_head) +
                          handle->pushback_used;

        if (restore_position && unread != 0)
                system_seek(handle->descriptor, -(bipolar)unread, SEEK_CUR);

        handle->read_head = 0;
        handle->read_tail = 0;
        handle->pushback_used = 0;
}

// Turn the buffer around to face the kernel. Anything read ahead is given
// back before the first byte is staged, or the write lands past it.
static fn stream_face_writing(stream address_to handle)
{
        if (handle->read_head != handle->read_tail || handle->pushback_used != 0)
                stream_drop_input(handle, true);
}

// And the other way. Staged output has to reach the file before a read can
// see the file, or the read returns what the file said before the write.
static fn stream_face_reading(stream address_to handle)
{
        if (handle->write_used != 0)
                stream_flush_output(handle);
}

/*
        Fill the buffer, and set end-of-file only on a genuine end.

        This is the routine the whole family is judged on. A read of a
        four-kilobyte buffer against a three-byte file returns three, and
        three is not an end of file -- it is three bytes, and the end has not
        been reached until a later read returns zero. Setting the indicator on
        a short read is the classic hand-written-stdio bug: every byte still
        comes out correct, feof answers true one call early, and a loop
        written as "while (!feof)" silently drops the last line of every file
        whose size is not a multiple of the buffer.

        Zero is the end. Negative is an error -- system_read_retry has already
        absorbed EINTR, so anything still negative is real. Neither is a
        partial answer, and the distinction between them is the only thing
        this routine decides.
*/
static bool stream_refill(stream address_to handle)
{
        bipolar got;

        handle->read_head = 0;
        handle->read_tail = 0;

        got = stream_trap_read(handle->descriptor, handle->buffer,
                               handle->buffer_size);

        if (got > 0)
        {
                handle->read_tail = (positive)got;
                return true;
        }

        if (got == 0)
                handle->flags |= STREAM_AT_END;
        else
                handle->flags |= STREAM_FAILED;

        return false;
}

/*
        Read the mode text fopen was given.

        The first letter decides everything; a '+' anywhere after it adds the
        other direction. 'b' is accepted and ignored because Linux has no text
        mode to distinguish it from, 'x' is C11's exclusive create, and 'e' is
        glibc's close-on-exec, accepted here because programs that spawn write
        it and a stream that leaks into a child is a real bug rather than a
        style one.

        Returns false for a mode that does not start with r, w or a, which is
        the one case fopen has to refuse before it touches the file system.
*/
static bool stream_read_mode(string_address mode, b32 address_to open_flags,
                             p32 address_to stream_flags)
{
        positive index;
        bool update = false;
        bool exclusive = false;
        bool close_on_exec = false;

        if (mode == null || mode[0] == end)
        {
                errno = EINVAL;
                return false;
        }

        for (index = 1; mode[index] != end; index++)
        {
                if (mode[index] == '+')
                        update = true;
                else if (mode[index] == 'x')
                        exclusive = true;
                else if (mode[index] == 'e')
                        close_on_exec = true;
        }

        p8 kind = mode[0];
        if (kind != 'r' && kind != 'w' && kind != 'a')
        {
                errno = EINVAL;
                return false;
        }
        address_to open_flags = update ? stream_open_read_write
                                : kind == 'r' ? stream_open_read_only
                                              : stream_open_write_only;
        address_to stream_flags = update ? STREAM_READABLE | STREAM_WRITABLE
                                  : kind == 'r' ? STREAM_READABLE : STREAM_WRITABLE;
        if (kind != 'r')
                address_to open_flags |= stream_open_create |
                    (kind == 'a' ? stream_open_append : stream_open_truncate);
        if (kind == 'a')
                address_to stream_flags |= STREAM_APPEND;

        if (exclusive)
                address_to open_flags |= stream_open_exclusive;

        if (close_on_exec)
                address_to open_flags |= O_CLOEXEC;

        return true;
}

static fn stream_register(stream address_to handle)
{
        handle->next = stream_open_list;
        stream_open_list = handle;
        handle->flags |= STREAM_REGISTERED;
}

static fn stream_forget(stream address_to handle)
{
        stream address_to address_to link = address_of stream_open_list;

        while (address_to link != null)
        {
                if (address_to link == handle)
                {
                        address_to link = handle->next;
                        break;
                }

                link = address_of(address_to link)->next;
        }

        handle->next = null;
        handle->flags &= ~STREAM_REGISTERED;
}

/*
        A write-only append stream starts at the end; a+ keeps its read position.

        Where an O_APPEND write actually lands is the kernel's decision at
        write time, so nothing here can move it; what this settles is what
        ftell says before the first write. Leaving the offset at zero makes a
        freshly opened append stream claim it is at the start of a file it can
        only add to, which is both wrong and what a program checking "am I
        appending to something" reads as an empty file. This is what glibc
        does at the same point and for the same reason.

        The seek is allowed to fail. An append stream on a pipe is a real
        thing and has no end to seek to.
*/
static fn stream_land_at_end(stream address_to handle)
{
        if ((handle->flags & (STREAM_APPEND | STREAM_READABLE)) == STREAM_APPEND)
                stream_trap_seek(handle->descriptor, 0, SEEK_END);
}

static stream address_to stream_attach(b32 descriptor, p32 flags)
{
        stream address_to handle = (stream address_to)stream_allocate_zeroed(sizeof(stream));
        if (handle)
        {
                handle->descriptor = descriptor;
                handle->flags = flags | STREAM_STRUCT_OURS;
                stream_land_at_end(handle);
                stream_register(handle);
        }
        else
                errno = ENOMEM;
        return handle;
}

static inline INLINE fn stream_reset_buffer(stream address_to handle)
{
        handle->buffer = null;
        handle->buffer_size = 0;
        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
}

/*
        fopen.

        The struct comes from the allocator and nothing else: a static pool
        would put a fixed ceiling on how many files a program may hold open,
        and the number the kernel is willing to give is the only ceiling worth
        having. A descriptor that was opened and then could not be wrapped is
        closed again before returning, or fopen leaks a descriptor on every
        allocation failure and the program dies of EMFILE somewhere unrelated.
*/
stream address_to stream_open(string_address path, string_address mode)
{
        b32 open_flags = 0;
        p32 stream_flags = 0;
        bipolar descriptor;
        stream address_to handle;

        if (path == null || !stream_read_mode(mode, address_of open_flags,
                                              address_of stream_flags))
                return null;

        descriptor = stream_trap_open(path, open_flags, stream_create_permissions);

        if (descriptor < 0)
                return null;

        handle = stream_attach((b32)descriptor, stream_flags);
        if (handle == null)
        {
                stream_trap_close((b32)descriptor);
                errno = ENOMEM;
        }
        return handle;
}

/* Adopt without creating or truncating; access must agree with the open
   descriptor, and append must remain true even after a later seek. */
stream address_to stream_adopt(b32 descriptor, string_address mode)
{
        b32 open_flags = 0;
        p32 stream_flags = 0;

        if (!stream_read_mode(mode, address_of open_flags, address_of stream_flags))
                return null;
        bipolar flags = error_result(system_call_3(syscall(fcntl),
                                                  (positive)descriptor, 3, 0));
        if (flags < 0)
                return null;
        if (((stream_flags & STREAM_READABLE) && (flags & 3) == 1) ||
            ((stream_flags & STREAM_WRITABLE) && (flags & 3) == 0))
        {
                errno = EINVAL;
                return null;
        }
        if ((stream_flags & STREAM_APPEND) && !(flags & stream_open_append) &&
            error_result(system_call_3(syscall(fcntl), (positive)descriptor, 4,
                                        (positive)flags | stream_open_append)) < 0)
                return null;
        return stream_attach(descriptor, stream_flags);
}

/*
        freopen: a new file behind an existing stream object.

        The point of it is that the object's identity survives, which is what
        lets a program point stdout at a file without every other piece of
        code that captured stdout noticing. So the struct is reused in place
        and the buffer with it, and only the descriptor underneath changes.

        The null-path form, which asks to change the mode of the file already
        open, is refused here rather than half-implemented. Doing it properly
        means fcntl(F_SETFL) and a way to report that the kernel would not
        allow the change; doing it improperly means setting the READABLE bit
        on a write-only descriptor and getting EBADF from somewhere the caller
        cannot connect to freopen.
*/
stream address_to stream_reopen(string_address path, string_address mode,
                                stream address_to handle)
{
        b32 open_flags = 0;
        p32 stream_flags = 0;
        bipolar descriptor;

        if (handle == null || path == null ||
            !stream_read_mode(mode, address_of open_flags,
                              address_of stream_flags))
                return null;

        stream_face_reading(handle);
        stream_trap_close(handle->descriptor);

        descriptor = stream_trap_open(path, open_flags, stream_create_permissions);

        if (descriptor < 0)
        {
                handle->descriptor = -1;
                handle->flags |= STREAM_FAILED;
                return null;
        }

        handle->descriptor = (b32)descriptor;
        handle->flags = stream_flags |
                        (handle->flags & (STREAM_BUFFER_OURS | STREAM_STRUCT_OURS |
                                          STREAM_REGISTERED));
        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
        handle->pushback_used = 0;
        stream_land_at_end(handle);
        return handle;
}

/*
        fclose.

        Flush first, and report the flush's failure even though the descriptor
        is closed anyway: the last buffer is the one most likely to be lost to
        a full disk, and a program that ignores fclose's result there loses
        the tail of its output with no other sign.

        The struct is released only if it came from the allocator, so
        fclose(stdout) closes descriptor one and leaves the object valid but
        pointing at nothing, exactly as it does everywhere else.
*/
b32 stream_close(stream address_to handle)
{
        b32 result = 0;

        if (handle == null)
                return EOF;

        if (stream_flush_output(handle) != 0)
                result = EOF;

        if (stream_trap_close(handle->descriptor) < 0)
                result = EOF;

        if (handle->flags & STREAM_REGISTERED)
                stream_forget(handle);

        if (handle->flags & STREAM_BUFFER_OURS)
                stream_release(handle->buffer);

        stream_reset_buffer(handle);
        handle->pushback_used = 0;
        handle->descriptor = -1;
        handle->flags &= ~(STREAM_BUFFER_OURS | STREAM_READABLE | STREAM_WRITABLE);

        if (handle->flags & STREAM_STRUCT_OURS)
                stream_release(handle);

        return result;
}

/*
        fflush, including the null form.

        fflush(null) flushes every output stream the program has open, which
        is what a program calls before fork, before exec, and before whatever
        it is about to do that might not come back. The three standard streams
        are visited by name because they are static objects and are never on
        the list; everything fopen produced is on the list.

        On an input stream this does what POSIX says rather than what C says:
        C leaves it undefined, POSIX makes it discard the buffered input and
        put the file offset back, and that is both useful and exactly what
        stream_drop_input already is.
*/
b32 stream_flush(stream address_to handle)
{
        b32 result = 0;
        stream address_to walk;

        if (handle != null)
        {
                if (handle->write_used != 0)
                        return stream_flush_output(handle);

                if ((handle->flags & STREAM_READABLE) &&
                    (handle->read_head != handle->read_tail ||
                     handle->pushback_used != 0))
                        stream_drop_input(handle, true);

                return 0;
        }

        if (stream_flush_output(address_of stream_standard_output) != 0)
                result = EOF;

        if (stream_flush_output(address_of stream_standard_error) != 0)
                result = EOF;

        for (walk = stream_open_list; walk != null; walk = walk->next)
                if (stream_flush_output(walk) != 0)
                        result = EOF;

        return result;
}

/*
        What exit calls, and what exit must not have to know.

        stdlib.c owns leaving and cannot name a stream: it holds a null
        function pointer instead and calls it once, after the atexit handlers.
        This is the function that pointer is aimed at, and the umbrella's
        startup shim is where the aiming happens, because there is no code
        before main here to do it earlier and nothing can reach exit before
        main has been entered.

        It is not stream_flush(null), and the difference is the whole point. A
        buffer holding bytes some other process put there is left alone: that
        process is still holding the same bytes and will write them itself, so
        writing them here would print them twice. A buffer this process filled
        is written out exactly as an unforked program would expect. One getpid
        answers it for every stream at once.

        Flushing rather than closing is deliberate. C says exit closes every
        open stream, and closing flushes, but the only part of a close that is
        observable from outside a process about to stop existing is the bytes
        reaching the descriptor: the kernel closes the descriptors itself, and
        the arena a handle sits in goes away with the address space. Walking
        the open list while stream_close unlinks entries from it would be a
        second correctness problem bought for no visible difference.
*/
static fn stream_flush_at_exit_one(stream address_to handle,
                                   positive address_to here)
{
        if (handle->write_used == 0)
                return;

        //      A silent process reaches no identity syscall at exit. Delay
        //      the one shared answer until the first stream that actually
        //      holds bytes, then reuse it for every remaining stream.
        if (address_to here == 0)
                address_to here = stdlib_process_identity();

        if (handle->owner != address_to here)
                return;

        stream_flush_output(handle);
}

static fn stream_flush_at_exit(void)
{
        positive here = 0;
        stream address_to walk;

        stream_flush_at_exit_one(address_of stream_standard_output,
                                 address_of here);
        stream_flush_at_exit_one(address_of stream_standard_error,
                                 address_of here);

        for (walk = stream_open_list; walk != null; walk = walk->next)
                stream_flush_at_exit_one(walk, address_of here);
}

/*
        Hand bytes to the stream, which is fwrite with the item arithmetic
        taken off and the primitive everything else in the family writes
        through.

        Three policies meet here. An unbuffered stream writes straight through
        with no copy. A fully buffered stream fills the buffer and flushes
        when it is full. A line buffered stream additionally flushes whenever
        the run it just copied contained a newline -- the last one in the run,
        found once with memory_last_of rather than by scanning per byte, so a
        line buffered stream handed a large block still costs one pass.

        A chunk at least as large as the buffer does not go through the
        buffer at all. There is nothing for the buffer to do with it: every
        byte would be copied in and handed straight back out, one syscall per
        buffer's worth, and a caller that hands over a megabyte would pay two
        hundred and fifty six writes and a megabyte of copying to say what one
        write says. The ordering question the buffer raises is answered by
        emptying it first -- whatever is staged reaches the kernel before the
        run does, which is the only ordering there is -- and after that the
        run is the whole of what is outstanding, so line buffering has nothing
        left to decide either. Measured on x86_64, a gigabyte written as
        megabyte blocks to /dev/null: 25 milliseconds through the buffer,
        1 millisecond direct, against glibc's 1 millisecond.

        The count returned is not the count copied. A byte sitting in the
        buffer has been accepted and is counted; a byte that was in the buffer
        when a flush failed has been lost and is not, which is what staged
        keeps track of and subtracts. Counting copies instead reports nine
        thousand bytes written to a device that refused all of them, because
        the first four thousand did reach the buffer before the buffer reached
        the kernel. glibc answers zero there and so does this.
*/
positive stream_put_bytes(stream address_to handle, address_any data,
                          positive length)
{
        p8 address_to bytes = (p8 address_to)data;
        positive done = 0;
        positive staged = 0;

        if (handle == null || !(handle->flags & STREAM_WRITABLE))
        {
                if (handle != null)
                        handle->flags |= STREAM_FAILED;

                return 0;
        }

        if (length == 0)
                return 0;

        stream_ready(handle);
        stream_face_writing(handle);

        if (handle->flags & STREAM_UNBUFFERED)
        {
                done = stream_trap_write(handle->descriptor, bytes, length);

                if (done != length)
                        handle->flags |= STREAM_FAILED;

                return done;
        }

        //      One getpid per buffer that goes from empty to not, which for a
        //      program writing steadily is once per four kilobytes, and the
        //      only thing that lets a child of a process that flushed before
        //      it forked have its own output written out at exit.
        if (handle->write_used == 0)
                handle->owner = stdlib_process_identity();

        //      A run at least a whole buffer wide has nothing the buffer can
        //      do for it: every byte would be copied in and handed straight
        //      back out, one syscall per buffer's worth. Empty what is staged
        //      so the order is kept, then hand the run to the kernel entire.
        //      Tested once here rather than at the top of the loop, because
        //      after it the whole request is answered and the loop would
        //      never come round again.
        if (length >= handle->buffer_size)
        {
                positive written;

                if (stream_flush_output(handle) != 0)
                        return 0;

                written = stream_trap_write(handle->descriptor, bytes, length);

                if (written != length)
                        handle->flags |= STREAM_FAILED;

                return written;
        }

        /*
                The usual buffered write is already resident and fits in the
                space left. It can complete in one copy and, for a line
                buffer, one newline decision; entering the multi-chunk loop
                only to leave after its first iteration adds control with no
                semantics.
        */
        if (length <= handle->buffer_size - handle->write_used)
        {
                memory_copy(handle->buffer + handle->write_used, bytes,
                            length);
                handle->write_used += length;

                if (handle->write_used != handle->buffer_size &&
                    (!(handle->flags & STREAM_LINE_BUFFERED) ||
                     memory_last_of(bytes, '\n', length) == null))
                        return length;

                if (stream_flush_output(handle) != 0)
                        return 0;

                return length;
        }

        while (done < length)
        {
                positive space = handle->buffer_size - handle->write_used;
                positive take = length - done;
                bool flush_now;

                if (take > space)
                        take = space;

                memory_copy(handle->buffer + handle->write_used, bytes + done,
                            take);
                handle->write_used += take;
                done += take;
                staged += take;

                flush_now = handle->write_used == handle->buffer_size ||
                            ((handle->flags & STREAM_LINE_BUFFERED) &&
                             memory_last_of(bytes + done - take, '\n', take) !=
                                     null);

                if (!flush_now)
                        continue;

                if (stream_flush_output(handle) != 0)
                        return done - staged;

                staged = 0;
        }

        return done;
}

/*
        fwrite.

        Items, not bytes. The return is how many whole items reached the
        stream, so a partial item at the end of a short write is not counted,
        and a size or a count of zero is zero items with no syscall made --
        the standard says so explicitly and a program that writes
        fwrite(buffer, 1, 0, f) to mean "flush nothing" relies on it.

        The multiplication is checked. size times count is the one place in
        the family where a caller's arithmetic can wrap a 64 bit register, and
        a wrapped length is a write of the wrong size out of a buffer that was
        never that large.
*/
sized stream_write(address_any from, sized size, sized count,
                   stream address_to handle)
{
        positive total;
        positive done;

        if (handle == null || size == 0 || count == 0)
                return 0;

        if ((positive)count > positive_max / (positive)size)
        {
                handle->flags |= STREAM_FAILED;
                return 0;
        }

        total = (positive)size * (positive)count;
        done = stream_put_bytes(handle, from, total);
        return (sized)(done / (positive)size);
}

/*
        fread.

        Four sources in order: the pushback array, the buffer, a direct read
        into the caller's memory for anything at least a buffer wide, and a
        refill for the tail. The direct read is what keeps a large fread from
        costing a copy per buffer, and it is safe here in a way the
        corresponding shortcut in the write path is not -- there is no
        ordering to maintain, because the buffer is empty by the time the
        direct path is reached.

        The loop continues until the request is filled or the stream ends. A
        single read(2) returning short is not the end of a fread: a pipe hands
        over what it has, and stopping there would report an item count the
        caller reads as end-of-file when the writer is merely slow.
*/
sized stream_read(address_any into, sized size, sized count,
                  stream address_to handle)
{
        p8 address_to bytes = (p8 address_to)into;
        positive total;
        positive done = 0;

        if (handle == null || size == 0 || count == 0)
                return 0;

        if (!(handle->flags & STREAM_READABLE))
        {
                handle->flags |= STREAM_FAILED;
                return 0;
        }

        if ((positive)count > positive_max / (positive)size)
        {
                handle->flags |= STREAM_FAILED;
                return 0;
        }

        total = (positive)size * (positive)count;

        stream_ready(handle);
        stream_face_reading(handle);

        while (done < total)
        {
                positive available;
                positive take;

                if (handle->pushback_used != 0)
                {
                        handle->pushback_used--;
                        bytes[done++] = handle->pushback[handle->pushback_used];
                        continue;
                }

                available = handle->read_tail - handle->read_head;

                if (available != 0)
                {
                        take = total - done;

                        if (take > available)
                                take = available;

                        memory_copy(bytes + done,
                                    handle->buffer + handle->read_head, take);
                        handle->read_head += take;
                        done += take;
                        continue;
                }

                if (handle->flags & STREAM_AT_END)
                        break;

                if (total - done >= handle->buffer_size)
                {
                        bipolar got = stream_trap_read(handle->descriptor,
                                                       bytes + done, total - done);

                        if (got > 0)
                        {
                                done += (positive)got;
                                continue;
                        }

                        if (got == 0)
                                handle->flags |= STREAM_AT_END;
                        else
                                handle->flags |= STREAM_FAILED;

                        break;
                }

                if (!stream_refill(handle))
                        break;
        }

        return (sized)(done / (positive)size);
}

/*
        fgetc, and getc, which is the same routine.

        Once the end has been seen, nothing is read again until the indicator
        is cleared -- by clearerr, by a seek, or by ungetc. That is what the
        standard requires, and it is also what stops a loop over a terminal
        that has seen its end-of-file from trapping into the kernel forever.
*/
b32 stream_get_byte(stream address_to handle)
{
        if (handle == null || !(handle->flags & STREAM_READABLE))
        {
                if (handle != null)
                        handle->flags |= STREAM_FAILED;

                return EOF;
        }

        if (handle->pushback_used != 0)
        {
                handle->pushback_used--;
                return (b32)handle->pushback[handle->pushback_used];
        }

        /*
                Unread input proves both that the buffer is ready and that
                there is no staged output: the first write after a read calls
                stream_face_writing and drops the unread run before it stages
                anything. This is the per-byte path, so settle it before the
                two state helpers below.
        */
        if (handle->read_head != handle->read_tail)
                return (b32)handle->buffer[handle->read_head++];

        stream_ready(handle);
        stream_face_reading(handle);

        if (handle->flags & STREAM_AT_END)
                return EOF;

        if (!stream_refill(handle))
                return EOF;

        return (b32)handle->buffer[handle->read_head++];
}

b32 stream_get_byte_standard(void)
{
        return stream_get_byte(address_of stream_standard_input);
}

/*
        ungetc.

        A byte, not a character: the value is taken modulo 256 exactly as the
        standard says, so ungetc(-1) is EOF and fails while ungetc(0x1FF) is
        an 0xFF that succeeds.

        Clearing the end-of-file indicator is required and is easy to forget.
        A tokeniser that reads to the end, pushes the last byte back and then
        asks for it again is doing something entirely reasonable, and without
        this line it gets EOF from a stream that is holding the byte in its
        hand.
*/
b32 stream_unget_byte(b32 byte, stream address_to handle)
{
        if (handle == null || byte == EOF ||
            !(handle->flags & STREAM_READABLE))
                return EOF;

        if (handle->pushback_used >= stream_pushback_bytes)
                return EOF;

        stream_ready(handle);

        handle->pushback[handle->pushback_used] = (p8)byte;
        handle->pushback_used++;
        handle->flags &= ~STREAM_AT_END;
        return (b32)(p8)byte;
}

b32 stream_put_byte(b32 byte, stream address_to handle)
{
        p8 value = (p8)byte;

        if (stream_put_bytes(handle, address_of value, 1) != 1)
                return EOF;

        return (b32)value;
}

// No newline, unlike puts. That difference is the single most common thing a
// C programmer gets wrong about these two and it is not this file's to fix.
b32 stream_put_string(string_address text, stream address_to handle)
{
        positive length;

        if (text == null)
                return EOF;

        length = string_length(text);

        if (length == 0)
                return 0;

        if (stream_put_bytes(handle, text, length) != length)
                return EOF;

        return 1;
}

/*
        The shared line reader, which fgets and getdelim are both spellings of.

        It works out of the buffer rather than through stream_get_byte,
        because a line is found with one memory_first_of over a run the
        library already has in cache instead of a call and a bounds test per
        byte. The pushback array is drained first and one byte at a time,
        which is the only place the slow path survives and is where it belongs
        -- there are never more than a handful of bytes in it.

        take_limit is how many bytes the caller can accept in this pass, and
        the caller is the one that grows a buffer between passes. Returns the
        number copied and reports through found_delimiter whether the run
        ended because the delimiter was in it.
*/
static positive stream_take_line(stream address_to handle, p8 address_to into,
                                 positive take_limit, b32 delimiter,
                                 bool address_to found_delimiter,
                                 bool address_to ended)
{
        positive available;
        positive take;
        p8 address_to found;

        address_to found_delimiter = false;
        address_to ended = false;

        if (take_limit == 0)
                return 0;

        if (handle->pushback_used != 0)
        {
                handle->pushback_used--;
                into[0] = handle->pushback[handle->pushback_used];

                if ((b32)into[0] == delimiter)
                        address_to found_delimiter = true;

                return 1;
        }

        if (handle->read_head == handle->read_tail)
        {
                if (handle->flags & STREAM_AT_END)
                {
                        address_to ended = true;
                        return 0;
                }

                if (!stream_refill(handle))
                {
                        address_to ended = true;
                        return 0;
                }
        }

        available = handle->read_tail - handle->read_head;
        if (available > take_limit)
                available = take_limit;
        found = (p8 address_to)memory_first_of(handle->buffer + handle->read_head,
                                               (b8)delimiter, available);

        if (found != null)
        {
                take = (positive)(found - (handle->buffer + handle->read_head)) + 1;
                address_to found_delimiter = true;
        }
        else
                take = available;

        memory_copy(into, handle->buffer + handle->read_head, take);
        handle->read_head += take;
        return take;
}

/*
        fgets.

        The terminator is always written when anything is returned, and null
        is returned only when nothing at all was read -- which is the
        difference between "the file ended" and "the file ended right after a
        line without a newline". A limit of one leaves an empty string and
        reads nothing, and a limit of zero or less reads nothing and returns
        null, because there is not even room for the terminator.
*/
string_address stream_get_line(string_address into, b32 limit,
                               stream address_to handle)
{
        positive room;
        positive filled = 0;
        bool found = false;
        bool ended = false;

        if (into == null || handle == null || limit <= 0 ||
            !(handle->flags & STREAM_READABLE))
                return null;

        if (limit == 1)
        {
                into[0] = end;
                return into;
        }

        stream_ready(handle);
        stream_face_reading(handle);

        room = (positive)limit - 1;

        while (filled < room && !found && !ended)
                filled += stream_take_line(handle, into + filled, room - filled,
                                           '\n', address_of found,
                                           address_of ended);

        if (filled == 0)
                return null;

        into[filled] = end;
        return into;
}

/*
        getdelim, and getline which is getdelim with a newline.

        The buffer belongs to the caller across calls: the same pointer and
        the same capacity are handed back in on the next call and grown only
        when a line does not fit, which is what makes a loop over a million
        lines cost a handful of allocations rather than a million. A null
        pointer with a zero capacity is the documented way to say "allocate
        it for me", and it is the way almost every caller uses it.

        The growth is doubling with a floor, through memory_growth, which is
        the library's own policy for exactly this and means the sequence of
        sizes here matches the sequence everywhere else in the tree.

        Returns the length not counting the terminator, and -1 at the end of
        the file -- including when the end arrives with nothing read, which is
        how the caller's loop stops.
*/
bipolar stream_get_delimited(address_any line, sized address_to capacity,
                             b32 delimiter, stream address_to handle)
{
        p8 address_to address_to held = (p8 address_to address_to)line;
        positive filled = 0;
        bool found = false;
        bool ended = false;

        if (line == null || capacity == null || handle == null ||
            !(handle->flags & STREAM_READABLE))
                return -1;

        stream_ready(handle);
        stream_face_reading(handle);

        if (address_to held == null)
                address_to capacity = 0;

        if (!memory_resize_reserve(held, capacity, 120, 120))
                return -1;

        while (!found && !ended)
        {
                positive room;
                positive taken;

                if (filled + 1 >= (positive)address_to capacity)
                {
                        if (!memory_resize_reserve(held, capacity,
                                                   filled + 2, 120))
                                return -1;
                }

                room = (positive)address_to capacity - 1 - filled;
                taken = stream_take_line(handle, address_to held + filled, room,
                                         delimiter, address_of found,
                                         address_of ended);
                filled += taken;
        }

        if (filled == 0)
                return -1;

        (address_to held)[filled] = end;
        return (bipolar)filled;
}

bipolar stream_get_line_allocated(address_any line, sized address_to capacity,
                                  stream address_to handle)
{
        return stream_get_delimited(line, capacity, '\n', handle);
}

/*
        fseek.

        Staged output goes to the file before the position moves, or it lands
        wherever the seek left the offset. Relative offsets are corrected by
        unread and pushed-back bytes. Discard those bytes only after the seek
        succeeds; a refused seek must not consume input.

        The end-of-file indicator is cleared. It says "a read hit the end",
        and after a seek no read has hit anything. The error indicator is not
        cleared: it says something went wrong, and that stays true.
*/
b32 stream_seek(stream address_to handle, bipolar offset, b32 whence)
{
        bipolar landed;

        if (handle == null)
                return -1;

        stream_ready(handle);

        if (stream_flush_output(handle) != 0)
                return -1;

        if (whence == SEEK_CUR &&
            __builtin_sub_overflow(offset,
                (bipolar)((handle->read_tail - handle->read_head) +
                           handle->pushback_used), address_of offset))
        {
                errno = EINVAL;
                return -1;
        }

        landed = stream_trap_seek(handle->descriptor, offset, whence);

        if (landed < 0)
                return -1;

        stream_drop_input(handle, false);
        handle->flags &= ~STREAM_AT_END;
        return 0;
}

/*
        ftell.

        One lseek, corrected by what the buffer holds. Staged output is ahead
        of the kernel's offset, so it is added; buffered input the caller has
        not asked for yet is behind it, so it is subtracted, and so is
        anything in the pushback array.

        Asking the kernel every time rather than tracking an offset is the
        whole point: fileno hands the descriptor to anyone, and a tracked
        offset is wrong from the moment they use it, silently and with no way
        to find out.

        The floor at zero is for one case: a byte pushed back at the start of
        the file puts the caller's position one before the first byte, and
        there is no such place. The standard says the answer there is
        unspecified, glibc answers zero, and answering minus one would be
        indistinguishable from the failure return.
*/
bipolar stream_tell(stream address_to handle)
{
        bipolar position;

        if (handle == null)
                return -1;

        position = stream_trap_seek(handle->descriptor, 0, SEEK_CUR);

        if (position < 0)
                return -1;

        if (__builtin_add_overflow(position, (bipolar)handle->write_used,
                                   address_of position))
        {
                errno = EOVERFLOW;
                return -1;
        }
        position -= (bipolar)(handle->read_tail - handle->read_head);
        position -= (bipolar)handle->pushback_used;

        if (position < 0)
                position = 0;

        return position;
}

// rewind is fseek to the start with both indicators cleared and no way to
// report a failure, which is exactly why fseek exists and rewind is a
// convenience rather than a primitive.
fn stream_rewind(stream address_to handle)
{
        if (handle == null)
                return;

        stream_seek(handle, 0, SEEK_SET);
        handle->flags &= ~(STREAM_AT_END | STREAM_FAILED);
}

/*
        feof.

        True only after a read has asked for bytes and been told there are
        none. Not when the position happens to be at the end of the file: a
        stream sitting at the last byte has not seen the end yet, and a loop
        written as "while (!feof(f)) { fgetc(f); use it; }" processes one
        phantom byte on every file in the world when this is wrong. The whole
        reason stream_refill separates zero from short is to make this
        answerable.
*/
PURE b32 stream_at_end(stream address_to handle)
{
        return handle && (handle->flags & STREAM_AT_END) != 0;
}

PURE b32 stream_failed(stream address_to handle)
{
        return handle && (handle->flags & STREAM_FAILED) != 0;
}

fn stream_clear_state(stream address_to handle)
{
        if (handle == null)
                return;

        handle->flags &= ~(STREAM_AT_END | STREAM_FAILED);
}

PURE b32 stream_descriptor(stream address_to handle)
{
        if (handle == null)
                return -1;

        return handle->descriptor;
}

/*
        setvbuf.

        Only legal before anything else happens to the stream, and this does
        not check: the check would mean remembering whether any operation has
        occurred, and a program that calls setvbuf late has a bug this cannot
        fix and would only rename. What it does do is release a buffer it
        allocated earlier, so calling setvbuf twice does not leak.

        A caller-supplied buffer is never freed and never grown. A null buffer
        with a size means "allocate one that big", which is the form almost
        everyone writes.

        The mode is recorded as known, which is what stops stream_ready from
        asking the terminal question afterwards and overruling the answer.
*/
b32 stream_set_buffering(stream address_to handle, string_address buffer,
                         b32 mode, sized size)
{
        if (handle == null)
                return -1;

        if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
                return -1;

        if (handle->flags & STREAM_BUFFER_OURS)
                stream_release(handle->buffer);

        stream_reset_buffer(handle);
        handle->flags &= ~(STREAM_BUFFER_OURS | STREAM_LINE_BUFFERED |
                           STREAM_UNBUFFERED);
        handle->flags |= STREAM_MODE_KNOWN;

        if (mode == _IONBF)
        {
                handle->flags |= STREAM_UNBUFFERED;
                handle->buffer = handle->single;
                handle->buffer_size = 1;
                return 0;
        }

        if (mode == _IOLBF)
                handle->flags |= STREAM_LINE_BUFFERED;

        if (size == 0)
                size = BUFSIZ;

        if (buffer != null)
        {
                handle->buffer = (p8 address_to)buffer;
                handle->buffer_size = (positive)size;
                return 0;
        }

        handle->buffer = (p8 address_to)stream_allocate((positive)size);

        if (handle->buffer == null)
        {
                handle->flags |= STREAM_UNBUFFERED;
                handle->buffer = handle->single;
                handle->buffer_size = 1;
                return -1;
        }

        handle->flags |= STREAM_BUFFER_OURS;
        handle->buffer_size = (positive)size;
        return 0;
}

fn stream_set_buffer(stream address_to handle, string_address buffer)
{
        stream_set_buffering(handle, buffer, buffer != null ? _IOFBF : _IONBF,
                             BUFSIZ);
}

/*
        The names <stdio.h> knows these by.

        Aliases rather than second bodies, which is the same choice library.c
        makes where it gives its assembly routines their libc names: a .set
        there, an alias attribute here, and in both cases one address with two
        labels on it. Nothing is wrapped and nothing jumps.

        The argument order of every prose name above was chosen to match the
        standard function it ends up being -- fputs takes the string first and
        so does stream_put_string -- because an alias is a second name for one
        address and nothing checks that the two prototypes agree. A prose name
        with its arguments in a nicer order would compile, link, and read the
        stream pointer as a string.
*/
stream address_to fopen(string_address path, string_address mode)
        __attribute__((alias("stream_open"), used));
stream address_to fdopen(b32 descriptor, string_address mode)
        __attribute__((alias("stream_adopt"), used));
stream address_to freopen(string_address path, string_address mode,
                          stream address_to handle)
        __attribute__((alias("stream_reopen"), used));
b32 fclose(stream address_to handle)
        __attribute__((alias("stream_close"), used));
sized fread(address_any into, sized size, sized count, stream address_to handle)
        __attribute__((alias("stream_read"), used));
sized fwrite(address_any from, sized size, sized count, stream address_to handle)
        __attribute__((alias("stream_write"), used));
b32 fseek(stream address_to handle, bipolar offset, b32 whence)
        __attribute__((alias("stream_seek"), used));
b32 fseeko(stream address_to handle, bipolar offset, b32 whence)
        __attribute__((alias("stream_seek"), used));
bipolar ftell(stream address_to handle)
        __attribute__((alias("stream_tell"), used));
bipolar ftello(stream address_to handle)
        __attribute__((alias("stream_tell"), used));
fn rewind(stream address_to handle)
        __attribute__((alias("stream_rewind"), used));
b32 fflush(stream address_to handle)
        __attribute__((alias("stream_flush"), used));
PURE b32 feof(stream address_to handle)
        __attribute__((alias("stream_at_end"), used));
PURE b32 ferror(stream address_to handle)
        __attribute__((alias("stream_failed"), used));
fn clearerr(stream address_to handle)
        __attribute__((alias("stream_clear_state"), used));
b32 setvbuf(stream address_to handle, string_address buffer, b32 mode, sized size)
        __attribute__((alias("stream_set_buffering"), used));
fn setbuf(stream address_to handle, string_address buffer)
        __attribute__((alias("stream_set_buffer"), used));
PURE b32 fileno(stream address_to handle)
        __attribute__((alias("stream_descriptor"), used));
b32 fgetc(stream address_to handle)
        __attribute__((alias("stream_get_byte"), used));
b32 getc(stream address_to handle)
        __attribute__((alias("stream_get_byte"), used));
b32 getchar(void)
        __attribute__((alias("stream_get_byte_standard"), used));
b32 ungetc(b32 byte, stream address_to handle)
        __attribute__((alias("stream_unget_byte"), used));
string_address fgets(string_address into, b32 limit, stream address_to handle)
        __attribute__((alias("stream_get_line"), used));
bipolar getline(address_any line, sized address_to capacity,
                stream address_to handle)
        __attribute__((alias("stream_get_line_allocated"), used));
bipolar getdelim(address_any line, sized address_to capacity, b32 delimiter,
                 stream address_to handle)
        __attribute__((alias("stream_get_delimited"), used));
b32 fputc(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte"), used));
b32 putc(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte"), used));
b32 fputs(string_address text, stream address_to handle)
        __attribute__((alias("stream_put_string"), used));
//      isatty belongs to the error family, which owns the POSIX wrappers and
//      leaves the kernel's own ENOTTY or EBADF behind for a caller that looks.
//      stream_is_terminal stays: it is what chooses line buffering below.

#endif // KERNEL_MODE, STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_STREAM
#endif // STANDARD_SKIP_STREAM

#ifndef STANDARD_SKIP_FORMAT
/* ---- format.c ---- */
/*
        Experimental C standard library

        printf: a format string, its arguments, and the bytes they mean

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_FORMAT
#define STANDARD_MODERN_C_FORMAT

/*
        This is ordinary C on purpose, for the reason netlink.c gives.

        library.c holds declarations and assembly and nothing else, and that is
        checked. printf is not a floor. It is a small language -- five flags, a
        width, a precision, eight length modifiers, twenty conversions -- and
        the part of it a machine could do differently from another machine is
        already downstairs: positive_into_base turns an integer into digits,
        positive_into does the decimal case through the pair table, memory_copy
        moves the run of literal text between two specifiers. What is left up
        here is the grammar, the argument fetch, and the decision about which
        byte goes where, and that is the same reasoning on every architecture.

        It is built beside string_format rather than on top of it. They are
        different languages that happen to share a percent sign: string_format
        reads %p as an unsigned integer where printf reads it as a pointer, it
        has no width, no precision and no flags, and it hands each piece to a
        writer the moment it has one. printf cannot do that. A right-aligned
        field pads before it emits, so the length has to be known before the
        first byte leaves, and snprintf has to keep counting after the buffer
        has stopped accepting. What the two do share is the number engine
        underneath, and both call the same assembly for it.

        Nothing here reaches for positive_to_base_field, which is the library's
        complete integer field and looks like exactly the right thing to call.
        Three of C's rules are not its rules. A zero value at a precision of
        zero prints nothing in C and prints one digit there. The # flag on
        octal is a precision bump in C and a two-byte prefix there. An explicit
        precision turns the 0 flag off in C and leaves it on there unless the
        caller has already cleared it. Pre-correcting for three of those at
        every call site is more code than assembling the field, so the field is
        assembled here and only the digits are borrowed.
*/

/*
        WHY THE WHOLE FILE IS BEHIND KERNEL_MODE

        src/core.c includes compiler_memory.c, so everything here is compiled
        into the kernel build as well as into every program. The kernel build
        refuses a decimal in a signature on arm64 whether or not anything calls
        it, which is the same reason decimal_to_string and fast_sin are guarded
        one floor down, and a kernel has its own logging and no business
        carrying a float formatter. So the file is one guarded block.
*/

#ifndef KERNEL_MODE

/*
        WHAT THIS FILE TAKES FROM THE STREAM FAMILY AND THE ERROR FAMILY

        Both were written beside this one and both are deferred to by name.
        Nothing here redefines anything either of them owns.

        From the stream family, guarded on STANDARD_MODERN_C_STANDARD_STREAM:

                FILE / stream           the handle, and stdout and stderr
                stream_put_bytes        bytes to a handle, count accepted
                fputc, fputs            already there as aliases

        From the error family, guarded on STANDARD_MODERN_C_STANDARD_ERROR:

                errno                   an lvalue through __errno_location
                strerror                the sentence for a number
                perror                  the whole line, written without a
                                        stream, which is the right way round

        Each guard has a fallback below it so this file builds and runs alone,
        which is how it was tested: a handle is then the descriptor itself and
        the write is one trap. When a family is present its version wins and
        the fallback is not compiled, so the merge is include order and nothing
        else -- stream, then error, then this.

        THE HANDLE GOES THROUGH A TYPEDEF ON PURPOSE. library.c defines stdin,
        stdout and stderr as the integers 0, 1 and 2 at library.c:12577, and
        src/sh/shell.c:899 uses stdout as a descriptor in a dup3. Naming FILE
        directly in these signatures would tie them to whichever of the two
        meanings happened to be current. format_stream is whichever one is
        real, so fprintf(stdout, ...) compiles and means the right thing in
        both worlds, and the entries below never have to be edited again.

        (The stream family undefines those three and redefines them as
        pointers. That is its business and its risk -- programs/shell.c
        includes compiler_memory.c before src/sh/shell.c, so the shell sees
        whatever compiler_memory.c last said -- but whoever merges the two
        should build programs/shell.c and run it before believing it worked.
        This file was built and run against the shell for exactly that reason
        and does not move any of the three.)
*/

#ifndef STANDARD_MODERN_C_STANDARD_STREAM

typedef b32 format_stream;

#define format_output ((format_stream)stdout)
#define format_error ((format_stream)stderr)
#define FORMAT_OWNS_BYTE_ENTRIES 1

/*
        The placeholder writes straight through. It has no buffer, so it also
        has no flush and no ordering question against the library's own log
        buffer, and a caller that prints one line per check pays one write call
        per line. The stream family replaces this with something that buffers,
        which is the only reason the placeholder is allowed to be this slow.
*/
static positive format_stream_write(format_stream stream, address_any data,
                                    positive length)
{
        bipolar written;

        if (length == 0)
                return 0;

        written = (bipolar)system_write_all((positive)stream, data, length);

        return written < 0 ? 0 : (positive)written;
}

#else

typedef FILE address_to format_stream;

#define format_output stdout
#define format_error stderr

//      stream_put_bytes already has this shape, so this is a second name.
static positive format_stream_write(format_stream, address_any, positive)
        __attribute__((alias("stream_put_bytes")));

#endif // STANDARD_MODERN_C_STANDARD_STREAM

#ifndef STANDARD_MODERN_C_STANDARD_ERROR

static bipolar errno = 0;

/* Standalone formatting retains its historical subset and sign normalization.
   The complete libc interface below the error guard accepts signed errno. */
static string_address strerror(b32 code)
{
        static const p8 supported[] = {
                0, 1, 2, 3, 4, 5, 9, 11, 12, 13, 14, 17, 18, 20, 21, 22,
                24, 25, 28, 32, 39,
        };
        positive magnitude = code < 0 ? -(bipolar)code : code;
        return magnitude <= 39 &&
               memory_first_of((address_any)supported, magnitude, sizeof(supported))
                   ? system_error_message(magnitude)
                   : (string_address)"Unknown error";
}

#define FORMAT_OWNS_PERROR 1

#endif // STANDARD_MODERN_C_STANDARD_ERROR

/*
        Where the bytes go.

        One structure covers every destination the family has. A buffer with a
        capacity is sprintf and snprintf. A buffer with a capacity of zero is
        the snprintf call that must not touch the pointer at all and still has
        to answer with a length. A stream is printf and fprintf. A downstream
        writer is the entry this tree's own code wants, so that a caller
        already holding a writer can format into it without inventing a FILE.

        counted is the answer, and it is deliberately not used. That difference
        is the single most commonly broken rule in a hand-written printf:
        snprintf returns the length it WOULD have written, so the count keeps
        rising long after the buffer has stopped accepting, and a caller sizing
        a second allocation off the return value gets the size it needs rather
        than the size it already had.
*/
typedef struct
{
        p8 address_to buffer;
        positive capacity;
        positive used;
        positive counted;
        writer downstream;
        format_stream stream;
        bool streaming;
        bool failed;
} format_sink;

#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

/* printf's answer is an int even where a native word is wider.  Counting in
   the native word keeps the common emit path branch-free; the one bounded
   padding shortcut below checks its addition, and this final seam rejects an
   answer C cannot represent. */
static inline INLINE bipolar format_answer(format_sink address_to sink)
{
        if (sink->failed)
                return -1;

        if (sink->counted > (positive)b32_max)
        {
                errno = EOVERFLOW;
                return -1;
        }

        return (bipolar)sink->counted;
}

static fn format_emit(format_sink address_to sink, address_any data,
                      positive length)
{
        positive room;

        if (length == 0)
                return;

        if (sink->streaming && sink->failed)
                return;

        sink->counted += length;

        if (sink->streaming)
        {
                if (format_stream_write(sink->stream, data, length) != length)
                        sink->failed = true;
                return;
        }

        if (sink->downstream)
        {
                sink->downstream(data, length);
                return;
        }

        if (is_null(sink->buffer) || sink->used >= sink->capacity)
                return;

        room = sink->capacity - sink->used;

        if (length < room)
                room = length;

        //      Nine of these a call and six of them one byte: the format
        //      string's own colons, a sign, a one place pad. Measured with a
        //      histogram on "%08d:%-12s:%6.3f", which is 28 bytes of output
        //      and asks for nine copies -- six of one byte, then a two, a
        //      four and an eight.
        //
        //      A byte through the general routine is a call, an overlap test
        //      it cannot need, a tail jump into the copy and a width ladder,
        //      to move one byte. Writing sizes up to eight here instead took
        //      the call from 3,781 instructions to 3,533 and 757 cycles to
        //      682, about a tenth, and dropped the copy routine from 23% of
        //      the call's instructions to under 10%.
        //
        //      Overlapping windows so no size needs its own branch. The sink
        //      buffer is the caller's output and never overlaps a source, so
        //      this is a copy and not a move.
        {
                p8 address_to to = (p8 address_to)sink->buffer + sink->used;
                p8 address_to from = (p8 address_to)data;

                if (room == 1)
                {
                        to[0] = from[0];
                }
                else if (room >= 8)
                {
                        if (room == 8)
                                __builtin_memcpy(to, from, 8);
                        else
                                memory_copy_apart(to, from, room);
                }
                else if (room >= 4)
                {
                        p32 head = memory_load_unaligned(p32, from);
                        p32 tail = memory_load_unaligned(p32, from + room - 4);
                        __builtin_memcpy(to, address_of head, 4);
                        __builtin_memcpy(to + room - 4, address_of tail, 4);
                }
                else if (room >= 2)
                {
                        p16 head = memory_load_unaligned(p16, from);
                        p16 tail = memory_load_unaligned(p16, from + room - 2);
                        __builtin_memcpy(to, address_of head, 2);
                        __builtin_memcpy(to + room - 2, address_of tail, 2);
                }
        }
        sink->used += room;
}

/*
        Padding is a run of one repeated byte and there is never a buffer of it
        lying around, so it comes out of a small stack block a chunk at a time.
        Sixty-four bytes means the widths that actually occur -- a column in a
        table, an eight place zero fill -- are one pass, while a pathological
        %2000d is thirty-two rather than two thousand.
*/
static fn format_fill(format_sink address_to sink, p8 byte, positive count)
{
        p8 block[64];
        positive part;

        if (count == 0 || (sink->streaming && sink->failed))
                return;

        /* A bounded buffer stops accepting bytes at capacity, but snprintf
           must still count the whole field.  Filling sixty-four byte blocks
           after that point made a count-only "%100000000d" walk 1,562,500
           iterations.  Copy the resident prefix once and account for the
           requested run once; sprintf keeps the ordinary full write because
           its advertised capacity is the complete addressable buffer. */
        if (count > sizeof(block) && !sink->streaming && !sink->downstream)
        {
                positive room = !is_null(sink->buffer) &&
                                        sink->used < sink->capacity
                                    ? sink->capacity - sink->used
                                    : 0;

                /* Keep the measured short-field path below when every byte
                   fits.  This arm is for truncation: it avoids work only for
                   the suffix the destination cannot retain. */
                if (count > room)
                {
                        if (room)
                        {
                                memory_fill(sink->buffer + sink->used, byte,
                                            room);
                                sink->used += room;
                        }

                        if (__builtin_add_overflow(sink->counted, count,
                                                   address_of sink->counted))
                        {
                                sink->failed = true;
                                errno = EOVERFLOW;
                        }
                        return;
                }
        }

        //      The whole block, not the part this call needs: sizeof is a
        //      literal, so the umbrella's specializer folds it to straight
        //      line stores, while a variable part is a call into the general
        //      routine and padding is nearly always a handful of bytes.
        memory_fill(block, byte, sizeof(block));

        while (count && !sink->failed)
        {
                part = count < sizeof(block) ? count : sizeof(block);
                format_emit(sink, block, part);
                count -= part;
        }
}

//      The five flags, packed so a whole specifier fits in registers.

#define FORMAT_FLAG_LEFT CONVERSION_FLAG_LEFT
#define FORMAT_FLAG_PLUS CONVERSION_FLAG_PLUS
#define FORMAT_FLAG_SPACE CONVERSION_FLAG_SPACE
#define FORMAT_FLAG_ALTERNATE CONVERSION_FLAG_ALTERNATE
#define FORMAT_FLAG_ZERO CONVERSION_FLAG_ZERO

//      The length modifiers, one code each rather than the one or two bytes
//      they are spelled with.

#define FORMAT_LENGTH_INT CONVERSION_LENGTH_INT
#define FORMAT_LENGTH_CHAR CONVERSION_LENGTH_CHAR
#define FORMAT_LENGTH_SHORT CONVERSION_LENGTH_SHORT
#define FORMAT_LENGTH_LONG CONVERSION_LENGTH_LONG
#define FORMAT_LENGTH_LONG_LONG CONVERSION_LENGTH_LONG_LONG
#define FORMAT_LENGTH_SIZE CONVERSION_LENGTH_SIZE
#define FORMAT_LENGTH_DIFFERENCE CONVERSION_LENGTH_DIFFERENCE
#define FORMAT_LENGTH_WIDEST CONVERSION_LENGTH_WIDEST
/*
        L is parsed and then ignored, which is not the same as supported.

        A %Lf reads a double out of the place a double would have been, and on
        x86_64 a long double was not put there -- it arrives on the stack with
        sixteen byte alignment while a double arrives in a vector register --
        so the number printed is whatever was in that register instead. The
        code is here so that the conversion after it is still found and the
        rest of the format still comes out; nobody should write %Lf against
        this until somebody implements it.

        Implementing it means three different types: eighty bits on x86_64 and
        a hundred and twenty eight on arm64 and riscv64, and converting between
        them calls into libgcc, which a -nostdlib link does not have. The
        exact-decimal engine below would take a wider mantissa and a wider
        exponent without complaint; it is the argument fetch and the libgcc
        dependency that are the work.
*/
#define FORMAT_LENGTH_WIDE_DECIMAL CONVERSION_LENGTH_WIDE_DECIMAL

typedef struct
{
        positive flags;
        positive width;
        bipolar precision;
        positive length;
        p8 conversion;
} format_spec;

/* Integer, decimal and hexadecimal numbers have one field prefix: outside spaces,
   sign, radix prefix, then inside zeros.  Forced inline keeps the prefix-less
   decimal path free of even a test or empty emit. */
static inline INLINE positive format_number_begin(
    format_sink address_to sink, format_spec address_to spec, positive body,
    p8 sign, address_any prefix, positive prefix_length)
{
        positive spaces = spec->width > body ? spec->width - body : 0;

        if (!(spec->flags & FORMAT_FLAG_LEFT) &&
            !(spec->flags & FORMAT_FLAG_ZERO))
                format_fill(sink, ' ', spaces);
        if (sign)
                format_emit(sink, address_of sign, 1);
        if (prefix_length)
                format_emit(sink, prefix, prefix_length);
        if (!(spec->flags & FORMAT_FLAG_LEFT) &&
            (spec->flags & FORMAT_FLAG_ZERO))
        {
                format_fill(sink, '0', spaces);
                spaces = 0;
        }

        return spaces;
}

/*
        The integer field, assembled in the order C says it appears.

        Everything that is not a digit is decided here: the sign byte, the 0x
        or 0b or 0 that the # flag asks for, the zeros an explicit precision
        demands, and the spaces the width demands on whichever side the - flag
        did not claim. Only the digits come out of the library.

        Three rules are worth stating because each is a corner every
        implementation gets wrong exactly once.

        A precision turns the 0 flag off. "%08.3d" of 42 is "     042" and not
        "00000042": the zeros a precision asks for are part of the number, the
        zeros the flag asks for are padding, and C says the padding loses.

        A zero value at a precision of zero is no characters at all. "%.0d" of
        0 is the empty string. A sign or a prefix still appears, so "%+.0d" of
        0 is "+", which is why the emptiness here is a digit length of zero
        rather than an early return.

        The # flag on octal is not a prefix. C says it raises the precision far
        enough to force a leading zero and no further, which is why "%#.5o" of
        8 is "00010" in five characters and not six: the zero the flag wants is
        already there, so the flag adds nothing.
*/
static fn format_integer(format_sink address_to sink, positive value,
                         positive base, bool negative,
                         format_spec address_to spec)
{
        p8 digits[72];
        p8 prefix[2];
        p8 sign = 0;
        positive prefix_length = 0;
        positive length;
        positive zeros = 0;
        positive forced = 0;
        positive body;
        positive spaces = 0;
        bool upper = spec->conversion == 'X' || spec->conversion == 'B';
        bool has_sign = spec->conversion == 'd' || spec->conversion == 'i';

        length = positive_into_base(digits, value, base, upper);

        //      A zero at an explicit precision of zero occupies no columns.
        if (value == 0 && spec->precision == 0)
                length = 0;

        //      The + and space flags belong to the signed conversions alone.
        //      An unsigned value has no sign to make explicit, so glibc drops
        //      both flags there and so does this.
        if (negative)
                sign = '-';
        else if (!has_sign)
                sign = 0;
        else if (spec->flags & FORMAT_FLAG_PLUS)
                sign = '+';
        else if (spec->flags & FORMAT_FLAG_SPACE)
                sign = ' ';

        if (spec->conversion == 'p')
        {
                prefix[0] = '0';
                prefix[1] = 'x';
                prefix_length = 2;
        }

        if (spec->precision >= 0 && (positive)spec->precision > length)
                zeros = (positive)spec->precision - length;

        if ((spec->flags & FORMAT_FLAG_ALTERNATE) && spec->conversion != 'p')
        {
                if (base == 16 && value != 0)
                {
                        prefix[0] = '0';
                        prefix[1] = upper ? 'X' : 'x';
                        prefix_length = 2;
                }
                else if (base == 2 && value != 0)
                {
                        prefix[0] = '0';
                        prefix[1] = upper ? 'B' : 'b';
                        prefix_length = 2;
                }
                else if (base == 8 && zeros == 0 && !(value == 0 && length == 1))
                {
                        //      One zero, and only where the body does not
                        //      already begin with one. It is counted apart
                        //      from the precision zeros on purpose: a
                        //      precision turns the 0 flag off and this does
                        //      not, so "%#05o" of 8 is "00010" and not
                        //      "  010".
                        forced = 1;
                }
        }

        body = (sign != 0) + prefix_length + forced + zeros + length;

        format_spec field = *spec;
        if (spec->precision >= 0)
                field.flags &= ~FORMAT_FLAG_ZERO;
        spaces = format_number_begin(sink, &field, body, sign, prefix, prefix_length);

        format_fill(sink, '0', zeros + forced);
        format_emit(sink, digits, length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        A counted run of bytes inside a width, which is %s and %c both.

        The name carries the word field only because CHECK_verify in test/checks.c already
        has a static array called format_text, and one translation unit cannot
        hold both.

        writer_field downstairs does exactly this and is not called, for the
        same reason positive_to_base_field is not: it takes a writer, and a
        writer has no way to tell snprintf how many bytes it refused. The three
        lines saved are not worth a second path through the sink.
*/
static fn format_text_field(format_sink address_to sink, address_any data,
                            positive length, format_spec address_to spec)
{
        positive spaces = spec->width > length ? spec->width - length : 0;

        if (!(spec->flags & FORMAT_FLAG_LEFT))
                format_fill(sink, ' ', spaces);

        format_emit(sink, data, length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        EVERY DOUBLE IS A TERMINATING DECIMAL, AND THAT IS THE WHOLE TRICK

        A finite double is m times 2^E with m below 2^53. When E is not
        negative that is an integer and it has an exact decimal spelling. When
        E is negative, write k for -E and split m at the binary point:

                m * 2^-k  =  (m >> k)  +  (m & (2^k - 1)) / 2^k

        and multiply the fraction on the right, top and bottom, by 5^k. It
        becomes (m_low * 5^k) / 10^k -- which is to say the exact decimal
        digits after the point are the digits of m_low * 5^k written out in
        exactly k places with leading zeros. No approximation enters anywhere.
        One tenth really does print as 0.1000000000000000055511151231257827
        021181583404541015625 when a caller asks for fifty-five places, and a
        real glibc agrees digit for digit, because there is exactly one right
        answer and both of us are computing it rather than estimating it.

        The cost is a big integer, but a small and one-directional one. The
        widest value that ever appears is 5^1074 times a fifty-three bit
        mantissa, which is 767 decimal digits. Holding it in limbs of 10^9
        rather than 2^64 means every operation needed is a multiply by
        something below 10^9, whose product is below 10^18 and fits a register
        without help, and the digits fall straight out of the limbs at the end.
        Nothing here divides by anything but a compile time constant and
        nothing calls libgcc, which matters because a -nostdlib link does not
        have libgcc to call.

        Rounding then happens on decimal digits, where a tie is a real tie
        rather than an artifact of the estimate, and it is round half to even
        because that is what a glibc in its default rounding mode does: "%.0f"
        of 0.5 is "0", of 1.5 is "2", and of 2.5 is "2".
*/

#define FORMAT_LIMB_BASE 1000000000
#define FORMAT_LIMB_DIGITS 9
#define FORMAT_LIMBS 90
#define FORMAT_DIGITS 1120

typedef struct
{
        positive limb[FORMAT_LIMBS];
        positive count;
} format_bignum;

static fn format_bignum_set(format_bignum address_to number, positive value)
{
        number->count = 0;

        while (value)
        {
                number->limb[number->count++] = value % FORMAT_LIMB_BASE;
                value /= FORMAT_LIMB_BASE;
        }
}

//      Multiply by anything below the limb base. The product of two such
//      values is below 10^18 and the carry never reaches the base, so the
//      whole operation stays inside a sixty-four bit register.
static fn format_bignum_scale(format_bignum address_to number, positive factor)
{
        positive carry = 0;
        positive index;

        for (index = 0; index < number->count; index++)
        {
                positive product = number->limb[index] * factor + carry;

                number->limb[index] = product % FORMAT_LIMB_BASE;
                carry = product / FORMAT_LIMB_BASE;
        }

        while (carry && number->count < FORMAT_LIMBS)
        {
                number->limb[number->count++] = carry % FORMAT_LIMB_BASE;
                carry /= FORMAT_LIMB_BASE;
        }
}

static fn format_bignum_add(format_bignum address_to into,
                            format_bignum address_to from)
{
        positive carry = 0;
        positive index;
        positive reach = into->count > from->count ? into->count : from->count;

        for (index = 0; (index < reach || carry) && index < FORMAT_LIMBS; index++)
        {
                positive total = carry;

                if (index < into->count)
                        total += into->limb[index];

                if (index < from->count)
                        total += from->limb[index];

                into->limb[index] = total % FORMAT_LIMB_BASE;
                carry = total / FORMAT_LIMB_BASE;

                if (index >= into->count)
                        into->count = index + 1;
        }
}

/*
        Multiply by a mantissa, which is too wide for one scale.

        Splitting at bit twenty-six leaves two halves each below 2^27, both of
        them under the limb base, and the shift that puts the high half back is
        itself a multiply by 2^26 which is also under the base. Three scales
        and one add, and no operation anywhere widens past sixty-four bits.
*/
static fn format_bignum_multiply(format_bignum address_to number,
                                 positive factor)
{
        format_bignum low;

        if (factor < FORMAT_LIMB_BASE)
        {
                format_bignum_scale(number, factor);
                return;
        }

        low.count = number->count;
        memory_copy(low.limb, number->limb, number->count * sizeof(positive));

        format_bignum_scale(number, factor >> 26);
        format_bignum_scale(number, (positive)1 << 26);
        format_bignum_scale(address_of low, factor & (((positive)1 << 26) - 1));
        format_bignum_add(number, address_of low);
}

/*
        The digits of the big integer, most significant first.

        The top limb loses its leading zeros and every limb below it keeps all
        nine of its own, which is what makes the concatenation the number
        rather than a list of limbs. A zero big integer writes nothing, and
        both callers want that: as an integer part it is replaced by a single
        "0", and as a fraction it is entirely the leading zero run the caller
        pads with anyway.
*/
static positive format_bignum_digits(format_bignum address_to number,
                                     p8 address_to into)
{
        positive length;
        positive index;

        if (number->count == 0)
                return 0;

        length = positive_into(into, number->limb[number->count - 1]);

        //      A limb below the base is a nine digit zero padded field, and
        //      positive_into_padded is that field: its three architectures
        //      each carry a fast path gated on exactly this shape -- pad
        //      zero, width nine, value below ten to the ninth -- which walks
        //      the digit pair table with no division in it at all.
        for (index = number->count - 1; index > 0; index--)
                length += positive_into_padded(into + length,
                                               number->limb[index - 1],
                                               FORMAT_LIMB_DIGITS, '0');

        return length;
}

/*
        A finite double, taken apart into the digits it exactly is.

        The result is a run of decimal digits with no leading and no trailing
        zero, and an exponent that says where the point sits. The value is

                0.d[0] d[1] ... d[count-1]  times ten to the exponent

        which is the one shape that serves %f, %e and %g without any of them
        needing to know how the other two work. A zero value is the empty digit
        run with an exponent of one, chosen so that %e writes e+00 rather than
        e-01 and %f writes a bare 0. A digit read past either end of the run
        reads as zero, which is true of the value and is what lets every emit
        loop below run without a bound.
*/
typedef struct
{
        p8 digit[FORMAT_DIGITS];
        positive count;
        bipolar exponent;
} format_number;

static fn format_expand(decimal value, format_number address_to number)
{
        union
        {
                decimal value;
                p64 bits;
        } view;
        p64 bits;
        positive mantissa;
        bipolar raw;
        bipolar power;
        p8 whole[400];
        positive whole_length = 0;
        positive first;

        view.value = value;
        bits = view.bits;

        number->count = 0;
        number->exponent = 1;

        mantissa = (positive)(bits & (((p64)1 << 52) - 1));
        raw = (bipolar)((bits >> 52) & 0x7ff);

        if (raw == 0)
        {
                power = -1074;
        }
        else
        {
                mantissa |= (positive)1 << 52;
                power = raw - 1075;
        }

        if (mantissa == 0)
                return;

        if (power >= 0)
        {
                format_bignum big;

                format_bignum_set(address_of big, mantissa);

                while (power >= 29)
                {
                        format_bignum_scale(address_of big, (positive)1 << 29);
                        power -= 29;
                }

                format_bignum_scale(address_of big, (positive)1 << power);

                whole_length = format_bignum_digits(address_of big, whole);

                memory_copy(number->digit, whole, whole_length);

                number->count = whole_length;
                number->exponent = (bipolar)whole_length;
        }
        else
        {
                static const positive powers_of_five[12] = {
                    1,      5,       25,      125,     625,      3125,
                    15625,  78125,   390625,  1953125, 9765625,  48828125};
                positive shift = (positive)(-power);
                positive above = shift >= 64 ? 0 : mantissa >> shift;
                positive below =
                    shift >= 64 ? mantissa
                                : mantissa & ((((positive)1) << shift) - 1);
                positive left = shift;
                format_bignum big;
                p8 fraction[FORMAT_DIGITS];
                positive fraction_length;
                positive pad;
                positive room;
                positive take;
                positive count = 0;

                if (above)
                        whole_length = positive_into(whole, above);

                format_bignum_set(address_of big, 1);

                //      Five to the shift, twelve powers at a time, because
                //      5^12 is the largest power of five below the limb base.
                while (left >= 12)
                {
                        format_bignum_scale(address_of big, 244140625);
                        left -= 12;
                }

                if (left)
                        format_bignum_scale(address_of big, powers_of_five[left]);

                format_bignum_multiply(address_of big, below);

                fraction_length = format_bignum_digits(address_of big, fraction);

                //      The fraction is exactly shift places wide. Whatever the
                //      big integer is short by is its leading zero run.
                pad = shift > fraction_length ? shift - fraction_length : 0;

                //      Three runs, each clipped to what is left of the digit
                //      array exactly as the byte loops that were here clipped
                //      it: the integer part, the fraction's leading zeros,
                //      and the fraction's own digits. The pad alone reaches a
                //      thousand places on a subnormal, which is a thousand
                //      loop iterations where memory_fill is one call.
                room = FORMAT_DIGITS - count;
                take = whole_length < room ? whole_length : room;
                memory_copy(number->digit + count, whole, take);
                count += take;

                room = FORMAT_DIGITS - count;
                take = pad < room ? pad : room;
                memory_fill(number->digit + count, '0', take);
                count += take;

                room = FORMAT_DIGITS - count;
                take = fraction_length < room ? fraction_length : room;
                memory_copy(number->digit + count, fraction, take);
                count += take;

                number->count = count;
                number->exponent = (bipolar)whole_length;
        }

        //      Leading zeros are not digits of the value. They are a statement
        //      about where the point is, so they move into the exponent.
        //      The run of leading zeros is a span of one byte value, which
        //      is what memory_span_byte answers. On a small number it is a
        //      few bytes; on 1e-300 it is three hundred.
        first = memory_span_byte(number->digit, '0', number->count);

        if (first == number->count)
        {
                number->count = 0;
                number->exponent = 1;
                return;
        }

        if (first)
        {
                //      The regions overlap and memory_copy is the overlap
                //      aware one, which is what its contract in library.c
                //      says and is why memory_copy_apart is not named here.
                memory_copy(number->digit, number->digit + first,
                            number->count - first);

                number->count -= first;
                number->exponent -= (bipolar)first;
        }

        //      Trailing zeros are real digits but nothing ever asks for them:
        //      a read past the end already answers zero. Dropping them makes
        //      every later loop shorter and changes no answer.
        while (number->count && number->digit[number->count - 1] == '0')
                number->count--;
}

/*
        Keep the leading `keep` digits and round the rest away, half to even.

        A carry that runs off the front is the 999 becoming 1000 case. The
        digits become a single one and the exponent goes up, and that is the
        only place in the whole conversion where the exponent moves after
        expansion. It is also why %g picks between its two shapes after
        rounding rather than before: 9.99e-5 at three significant digits is
        still 9.99e-5, but at two it is 1.0e-4, and the two print differently.
*/
static fn format_round(format_number address_to number, bipolar keep)
{
        positive index;
        bool up;

        if (keep < 0)
        {
                number->count = 0;
                number->exponent = 1;
                return;
        }

        if ((positive)keep >= number->count)
                return;

        index = (positive)keep;

        if (number->digit[index] > '5')
        {
                up = true;
        }
        else if (number->digit[index] < '5')
        {
                up = false;
        }
        else
        {
                // A span call makes this leaf spill its rounding state even
                // when the first byte rejects. The shared scanner plus inline
                // first-byte guard was rechecked on the 9950X and lost the
                // mixed decimal/hex workload; retain the local sticky policy.
                positive after = index + 1;

                up = false;

                while (after < number->count)
                {
                        if (number->digit[after] != '0')
                        {
                                up = true;
                                break;
                        }

                        after++;
                }

                //      An exact tie goes to the even neighbour, and the digit
                //      before the first kept one, when there is none, is a
                //      zero and zero is even.
                if (!up)
                        up = index > 0 &&
                             ((number->digit[index - 1] - '0') & 1) != 0;
        }

        number->count = index;

        if (!up)
        {
                while (number->count && number->digit[number->count - 1] == '0')
                        number->count--;

                if (number->count == 0)
                        number->exponent = 1;

                return;
        }

        while (number->count)
        {
                if (number->digit[number->count - 1] != '9')
                {
                        number->digit[number->count - 1]++;
                        return;
                }

                number->count--;
        }

        number->digit[0] = '1';
        number->count = 1;
        number->exponent++;
}

static inline INLINE p8 format_number_sign(p64 bits,
                                            format_spec address_to spec)
{
        return bits >> 63 ? '-' : spec->flags & FORMAT_FLAG_PLUS ? '+' :
               spec->flags & FORMAT_FLAG_SPACE ? ' ' : 0;
}

/*
        The three decimal float shapes, and the field around them.

        The body's length is computed before a byte is written, because a right
        aligned field pads first and no streaming emitter can know how long a
        float body is halfway through it. Everything after that is the pieces
        in the order C lists them.

        The 0 flag goes after the sign and before the digits, which is why the
        two padding decisions are separated rather than written once: a space
        pad precedes the sign and a zero pad follows it.
*/
static fn format_decimal_field(format_sink address_to sink, decimal value,
                               format_spec address_to spec)
{
        format_number number;
        union
        {
                decimal value;
                p64 bits;
        } view;
        p8 sign = 0;
        p8 style = spec->conversion;
        bipolar precision = spec->precision;
        positive integer_length;
        positive fraction_length;
        positive exponent_length = 0;
        p8 exponent_digits[8];
        positive body;
        positive spaces = 0;
        positive run;
        bool point;
        bool upper = style == 'E' || style == 'F' || style == 'G' || style == 'A';

        view.value = value;

        sign = format_number_sign(view.bits, spec);

        //      Infinities and not-a-numbers are words, and a word is never
        //      zero padded however loudly the flag asks.
        if (((view.bits >> 52) & 0x7ff) == 0x7ff)
        {
                string_address word;
                positive length;

                if (view.bits & (((p64)1 << 52) - 1))
                        word = (string_address)(upper ? "NAN" : "nan");
                else
                        word = (string_address)(upper ? "INF" : "inf");

                length = 3 + (sign != 0);
                spaces = spec->width > length ? spec->width - length : 0;

                if (!(spec->flags & FORMAT_FLAG_LEFT))
                        format_fill(sink, ' ', spaces);

                if (sign)
                        format_emit(sink, address_of sign, 1);

                format_emit(sink, (address_any)word, 3);

                if (spec->flags & FORMAT_FLAG_LEFT)
                        format_fill(sink, ' ', spaces);

                return;
        }

        if (precision < 0)
                precision = 6;

        format_expand(value, address_of number);

        if (style == 'F')
                style = 'f';

        if (style == 'g' || style == 'G')
        {
                bipolar significant = precision == 0 ? 1 : precision;
                bipolar shown;

                format_round(address_of number, significant);

                shown = number.exponent - 1;

                if (shown < -4 || shown >= significant)
                {
                        style = (style == 'G') ? 'E' : 'e';
                        precision = significant - 1;
                }
                else
                {
                        style = 'f';
                        precision = significant - 1 - shown;
                }

                //      A %g that was not asked to keep its trailing zeros
                //      keeps only the places a digit actually reaches.
                if (!(spec->flags & FORMAT_FLAG_ALTERNATE))
                {
                        bipolar reach;

                        if (style == 'f')
                                reach = (bipolar)number.count - number.exponent;
                        else
                                reach = (bipolar)number.count - 1;

                        if (reach < 0)
                                reach = 0;

                        if (reach < precision)
                                precision = reach;
                }
        }
        else if (style == 'e' || style == 'E')
        {
                format_round(address_of number, precision + 1);
        }
        else
        {
                format_round(address_of number, number.exponent + precision);
        }

        point = precision > 0 || (spec->flags & FORMAT_FLAG_ALTERNATE);

        if (style == 'e' || style == 'E')
        {
                bipolar shown = number.count ? number.exponent - 1 : 0;
                positive magnitude = (positive)(shown < 0 ? -shown : shown);

                integer_length = 1;
                fraction_length = (positive)precision;

                exponent_digits[0] = (p8)style;
                exponent_digits[1] = shown < 0 ? '-' : '+';

                //      Two exponent digits at least, which is the one place
                //      printf pads without being asked.
                if (magnitude < 10)
                {
                        exponent_digits[2] = '0';
                        exponent_digits[3] = (p8)('0' + magnitude);
                        exponent_length = 4;
                }
                else
                {
                        exponent_length =
                            2 + positive_into(exponent_digits + 2, magnitude);
                }
        }
        else
        {
                integer_length =
                    number.exponent > 0 ? (positive)number.exponent : 1;
                fraction_length = (positive)precision;
        }

        body = (sign != 0) + integer_length + (point ? 1 : 0) + fraction_length +
               exponent_length;

        spaces = format_number_begin(sink, spec, body, sign, null, 0);

        if (style == 'e' || style == 'E')
        {
                p8 lead = number.count ? number.digit[0] : '0';

                format_emit(sink, address_of lead, 1);

                if (point)
                        format_emit(sink, (address_any) ".", 1);

                //      Digits one upward, which are contiguous in the run for
                //      as far as the run reaches and are zeros after that.
                run = number.count > 1 ? number.count - 1 : 0;

                if (run > fraction_length)
                        run = fraction_length;

                format_emit(sink, number.digit + 1, run);
                format_fill(sink, '0', fraction_length - run);

                format_emit(sink, exponent_digits, exponent_length);
        }
        else
        {
                if (number.exponent > 0)
                {
                        positive whole = (positive)number.exponent;

                        run = number.count < whole ? number.count : whole;

                        format_emit(sink, number.digit, run);
                        format_fill(sink, '0', whole - run);
                }
                else
                {
                        format_emit(sink, (address_any) "0", 1);
                }

                if (point)
                        format_emit(sink, (address_any) ".", 1);

                //      The fraction reads digits from number.exponent upward
                //      for fraction_length places. Anything below zero and
                //      anything at or past the end of the run is a zero, so
                //      the whole field is at most three pieces: a leading
                //      zero run, the part of the digit run that falls inside
                //      the field, and a trailing zero run.
                {
                        bipolar reach = number.exponent + (bipolar)fraction_length;
                        bipolar begin = number.exponent < 0 ? 0 : number.exponent;
                        bipolar stop = reach > (bipolar)number.count
                                               ? (bipolar)number.count
                                               : reach;
                        bipolar cut = begin > reach ? reach : begin;
                        positive lead_zeros = (positive)(cut - number.exponent);

                        run = stop > begin ? (positive)(stop - begin) : 0;

                        format_fill(sink, '0', lead_zeros);
                        format_emit(sink, number.digit + begin, run);
                        format_fill(sink, '0',
                                    fraction_length - lead_zeros - run);
                }
        }

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        The hexadecimal float, which is the one shape that needs no arithmetic.

        A normal double is written with its implicit leading one before the
        point and its fifty-two stored bits after it, and the exponent is the
        unbiased power of two. A subnormal keeps the stored leading zero and
        prints the same -1022 every subnormal has, which is why nothing here
        renormalises: matching means printing what is stored.

        With no precision the fraction is as short as it can be without losing
        a bit, so trailing zero nibbles go. With a precision the nibbles round
        half to even at that place, and a carry can reach the leading digit and
        turn its 1 into a 2. glibc does the same and does not renormalise
        either.

        Nibbles are held as values rather than as characters so the carry is
        arithmetic; the alphabet is applied once, on the way out.
*/
static fn format_hex_field(format_sink address_to sink, decimal value,
                           format_spec address_to spec)
{
        union
        {
                decimal value;
                p64 bits;
        } view;
        const p8 address_to alphabet;
        p8 sign = 0;
        p8 nibble[16];
        p8 text[16];
        p8 exponent_digits[8];
        positive nibble_count = 13;
        positive exponent_length;
        positive index;
        positive extra = 0;
        positive body;
        positive spaces = 0;
        positive mantissa;
        bipolar raw;
        bipolar power;
        p8 lead;
        bool point;
        bool upper = spec->conversion == 'A';

        alphabet = (const p8 address_to)(upper ? "0123456789ABCDEF"
                                               : "0123456789abcdef");
        view.value = value;

        if (((view.bits >> 52) & 0x7ff) == 0x7ff)
        {
                format_spec word = address_to spec;

                word.conversion = upper ? 'E' : 'e';
                format_decimal_field(sink, value, address_of word);
                return;
        }

        sign = format_number_sign(view.bits, spec);

        mantissa = (positive)(view.bits & (((p64)1 << 52) - 1));
        raw = (bipolar)((view.bits >> 52) & 0x7ff);

        if (raw == 0)
        {
                lead = 0;
                power = mantissa ? -1022 : 0;
        }
        else
        {
                lead = 1;
                power = raw - 1023;
        }

        for (index = 0; index < 13; index++)
                nibble[index] = (p8)((mantissa >> (48 - 4 * index)) & 15);

        if (spec->precision < 0)
        {
                while (nibble_count && nibble[nibble_count - 1] == 0)
                        nibble_count--;
        }
        else if ((positive)spec->precision < nibble_count)
        {
                positive keep = (positive)spec->precision;
                bool up;

                if (nibble[keep] > 8)
                {
                        up = true;
                }
                else if (nibble[keep] < 8)
                {
                        up = false;
                }
                else
                {
                        positive after = keep + 1;

                        up = false;

                        while (after < nibble_count)
                        {
                                if (nibble[after])
                                {
                                        up = true;
                                        break;
                                }

                                after++;
                        }

                        if (!up)
                                up = ((keep ? nibble[keep - 1] : lead) & 1) != 0;
                }

                nibble_count = keep;

                while (up && nibble_count)
                {
                        if (nibble[nibble_count - 1] == 15)
                        {
                                nibble[nibble_count - 1] = 0;
                                nibble_count--;
                                continue;
                        }

                        nibble[nibble_count - 1]++;
                        up = false;
                }

                //      The carry ran out of nibbles, so it lands on the digit
                //      before the point. Nothing renormalises afterwards.
                if (up)
                        lead++;

                nibble_count = keep;
        }
        else
        {
                extra = (positive)spec->precision - nibble_count;
        }

        for (index = 0; index < nibble_count; index++)
                text[index] = alphabet[nibble[index]];

        point = nibble_count + extra > 0 || (spec->flags & FORMAT_FLAG_ALTERNATE);

        exponent_digits[0] = upper ? 'P' : 'p';
        exponent_digits[1] = power < 0 ? '-' : '+';
        exponent_length =
            2 + positive_into(exponent_digits + 2,
                              (positive)(power < 0 ? -power : power));

        body = (sign != 0) + 2 + 1 + (point ? 1 : 0) + nibble_count + extra +
               exponent_length;

        spaces = format_number_begin(
            sink, spec, body, sign,
            (address_any)(upper ? "0X" : "0x"), 2);

        {
                p8 first = alphabet[lead & 15];

                format_emit(sink, address_of first, 1);
        }

        if (point)
                format_emit(sink, (address_any) ".", 1);

        format_emit(sink, text, nibble_count);
        format_fill(sink, '0', extra);
        format_emit(sink, exponent_digits, exponent_length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        The engine.

        One pass over the format string. A run of plain text is found whole and
        emitted in one call, because that is where the bytes are: a message
        with one specifier in forty characters should cross the sink twice and
        not forty times.

        A specifier nobody knows is written out complete, percent and all,
        which makes a stray percent in a message survive rather than silently
        eat the character after it. glibc agrees on "%y" and disagrees on
        "%zz", where it drops the length modifier and prints "%z"; both are
        undefined behaviour and printing the bytes back is the more useful of
        the two. A percent that is the last byte of the format writes nothing,
        which is string_format's rule one floor down, where glibc instead
        abandons the call and returns minus one. Those two and one glibc bug in
        %#g are the only places in 45,225 compared pairs where this and a real
        glibc do not produce the same bytes.
*/
static fn format_run(format_sink address_to sink, string_address format,
                     var_args list)
{
        string_address at = format;

        if (is_null(format))
        {
                format_emit(sink, (address_any) "(null)", 6);
                return;
        }

        while (!sink->failed && string_get(at))
        {
                string_address start = at;
                format_spec spec;
                positive base = 10;

                //      MEASURED, AND LEFT AS A BYTE LOOP ON PURPOSE.
                //
                //      This is exactly strchrnul and string_first_of_or_end
                //      is exactly that routine, so the house rule says call
                //      it. It was called, and it lost. The vectorised scan
                //      costs a flat forty-eight cycles a call whatever the
                //      run length; this loop costs about 1.3 cycles a byte
                //      on top of a smaller fixed cost, so the crossover is
                //      near ten bytes -- and 65% of the literal runs in this
                //      tree's own format strings are shorter than that, mean
                //      length 8.9. Whole formats measured both ways on
                //      x86_64: "%d\n" 11.8% slower with the call, "connecting
                //      to %s on port %d\n" 4.5% slower, a sixty-two byte
                //      three-conversion line 3.5% slower, and a two-line
                //      usage message 26% faster. A format string is short
                //      text between conversions, which is the shape the byte
                //      loop is better at.
                while (string_get(at) && string_get(at) != '%')
                        at++;

                if (at != start)
                        format_emit(sink, (address_any)start,
                                    (positive)(at - start));

                if (!string_get(at))
                        break;

                start = at;
                at++;

                conversion_spec parsed = conversion_spec_take_max(&at, positive_max);
                spec.flags = parsed.flags;
                spec.width = 0;
                spec.precision = -1;
                for (p8 field = 0; field < parsed.fields; field++)
                {
                        bipolar value;
                        if (parsed.stars & (1u << field))
                                value = var_list_get(list, b32);
                        else
                        {
                                if ((parsed.overflow & (1u << field)) ||
                                    parsed.field[field] > (positive)b32_max)
                                {
                                        sink->failed = true;
                                        errno = EOVERFLOW;
                                        return;
                                }
                                value = (bipolar)parsed.field[field];
                        }
                        if (field)
                                spec.precision = value < 0 ? -1 : value;
                        else
                        {
                                spec.flags |= value < 0 ? FORMAT_FLAG_LEFT : 0;
                                spec.width = (positive)absolute_wide(value);
                        }
                }

                spec.length = conversion_length_take(address_of at);

                spec.conversion = string_get(at);

                switch (spec.conversion)
                {
                case 'd':
                case 'i':
                {
                        bipolar signed_value;
                        bool negative;
                        positive value;

                        at++;

                        switch (spec.length)
                        {
                        case FORMAT_LENGTH_CHAR:
                                signed_value = (b8)var_list_get(list, b32);
                                break;
                        case FORMAT_LENGTH_SHORT:
                                signed_value = (b16)var_list_get(list, b32);
                                break;
                        case FORMAT_LENGTH_LONG:
                        case FORMAT_LENGTH_LONG_LONG:
                        case FORMAT_LENGTH_SIZE:
                        case FORMAT_LENGTH_DIFFERENCE:
                        case FORMAT_LENGTH_WIDEST:
                                signed_value = var_list_get(list, bipolar);
                                break;
                        default:
                                signed_value = var_list_get(list, b32);
                                break;
                        }

                        negative = signed_value < 0;

                        //      Negating the most negative value is undefined
                        //      as a signed operation and exact as an unsigned
                        //      one, which is why the cast happens first.
                        value = negative ? (positive)0 - (positive)signed_value
                                         : (positive)signed_value;

                        format_integer(sink, value, 10, negative,
                                       address_of spec);
                        continue;
                }
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                case 'b':
                case 'B':
                {
                        positive value;

                        at++;

                        switch (spec.length)
                        {
                        case FORMAT_LENGTH_CHAR:
                                value = (p8)var_list_get(list, p32);
                                break;
                        case FORMAT_LENGTH_SHORT:
                                value = (p16)var_list_get(list, p32);
                                break;
                        case FORMAT_LENGTH_LONG:
                        case FORMAT_LENGTH_LONG_LONG:
                        case FORMAT_LENGTH_SIZE:
                        case FORMAT_LENGTH_DIFFERENCE:
                        case FORMAT_LENGTH_WIDEST:
                                value = var_list_get(list, positive);
                                break;
                        default:
                                value = var_list_get(list, p32);
                                break;
                        }

                        if (spec.conversion == 'o')
                                base = 8;
                        else if (spec.conversion == 'x' || spec.conversion == 'X')
                                base = 16;
                        else if (spec.conversion == 'b' || spec.conversion == 'B')
                                base = 2;

                        format_integer(sink, value, base, false, address_of spec);
                        continue;
                }
                case 'c':
                {
                        p8 byte = (p8)var_list_get(list, b32);

                        at++;
                        format_text_field(sink, address_of byte, 1, address_of spec);
                        continue;
                }
                case 's':
                {
                        string_address text = var_list_get(list, string_address);
                        positive length;

                        at++;

                        if (is_null(text))
                        {
                                //      A null pointer prints the word, but a
                                //      precision that cannot hold the whole
                                //      word prints nothing at all rather than
                                //      a piece of it. glibc draws the line
                                //      there and a truncated "(nu" would be
                                //      worse than silence anyway.
                                length = 6;
                                text = (string_address) "(null)";

                                if (spec.precision >= 0 &&
                                    (positive)spec.precision < length)
                                        length = 0;
                        }
                        else if (spec.precision >= 0)
                        {
                                length = string_length_max(
                                    text, (positive)spec.precision);
                        }
                        else
                        {
                                length = string_length(text);
                        }

                        format_text_field(sink, (address_any)text, length,
                                          address_of spec);
                        continue;
                }
                case 'p':
                {
                        address_any pointer = var_list_get(list, address_any);

                        at++;
                        spec.precision = -1;
                        spec.flags &= ~FORMAT_FLAG_ALTERNATE;

                        if (is_null(pointer))
                        {
                                spec.conversion = 's';
                                format_text_field(sink,
                                                  (address_any) "(nil)", 5,
                                                  address_of spec);
                                continue;
                        }

                        format_integer(sink, (positive)pointer, 16, false,
                                       address_of spec);
                        continue;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                {
                        decimal given = var_list_get(list, decimal);

                        at++;
                        format_decimal_field(sink, given, address_of spec);
                        continue;
                }
                case 'a':
                case 'A':
                {
                        decimal given = var_list_get(list, decimal);

                        at++;
                        format_hex_field(sink, given, address_of spec);
                        continue;
                }
                case '%':
                        at++;
                        format_emit(sink, (address_any) "%", 1);
                        continue;
                default:
                        //      %n is not here and is not an oversight. It is
                        //      the primitive that turns a format string a
                        //      program did not control into a write anywhere
                        //      in memory, nothing in this tree needs it, and
                        //      it falls through to the line below and prints
                        //      itself back like any other conversion nobody
                        //      knows.
                        //
                        //      A percent that is the last byte of the format
                        //      writes nothing, which is string_format's rule
                        //      one floor down and is what callers in this
                        //      tree already expect. glibc calls it an error
                        //      and returns minus one instead.
                        if (!string_get(at))
                                continue;

                        //      Anything else that is not a conversion is
                        //      text, and the whole run of it including the
                        //      percent comes out unchanged.
                        at++;
                        format_emit(sink, (address_any)start,
                                    (positive)(at - start));
                        continue;
                }
        }
}

/*
        THE STANDARD NAMES

        These carry the standard spellings rather than prose ones. printf is
        not a description of an operation the way memory_copy is; it is a fixed
        interface with a fixed signature that thirty years of C expects to find
        under that exact name, and a prose alias would only be a second name
        for the same thing. The prose names in this family are the ones nobody
        else fixed: format_run is the engine, format_sink is the destination,
        and format_to_writer below is the entry this tree's own code should
        prefer, since a writer is what everything here already passes around.

        Every entry returns the length the format WANTED, which for printf and
        fprintf is also the length written, and for snprintf deliberately is
        not.
*/

static bipolar format_to_writer(writer write, string_address format, ...)
{
        format_sink sink = {0};
        var_args list;

        sink.downstream = write;

        var_list(list, format);
        format_run(address_of sink, format, list);
        var_list_end(list);

        return format_answer(address_of sink);
}

static bipolar vfprintf(format_stream stream, string_address format,
                        var_args list)
{
        format_sink sink = {0};

        sink.stream = stream;
        sink.streaming = true;
        format_run(address_of sink, format, list);

        return format_answer(address_of sink);
}

static bipolar vprintf(string_address format, var_args list)
{
        return vfprintf(format_output, format, list);
}

/*
        The bounded buffer entries.

        Capacity here is the room for bytes, not the room for the buffer: one
        byte of the caller's size is always the terminator. A size of zero
        leaves the buffer alone entirely -- no terminator, no read, and the
        pointer is allowed to be null -- and still returns the length, which is
        the call a caller makes on purpose to find out how much to allocate.
*/
static bipolar vsnprintf(p8 address_to into, positive size, string_address format,
                         var_args list)
{
        format_sink sink = {0};

        sink.buffer = size ? into : null;
        sink.capacity = size ? size - 1 : 0;

        format_run(address_of sink, format, list);

        if (size)
                into[sink.used] = end;

        return format_answer(address_of sink);
}

static bipolar vsprintf(p8 address_to into, string_address format, var_args list)
{
        format_sink sink = {0};

        sink.buffer = into;
        sink.capacity = ~(positive)0 >> 1;

        format_run(address_of sink, format, list);
        into[sink.used] = end;

        return format_answer(address_of sink);
}

var_list_entry(printf, bipolar, (string_address format, ...), format,
               vprintf(format, _variadic_list))
var_list_entry(fprintf, bipolar,
               (format_stream stream, string_address format, ...), format,
               vfprintf(stream, format, _variadic_list))
var_list_entry(snprintf, bipolar,
               (p8 address_to into, positive size, string_address format, ...),
               format, vsnprintf(into, size, format, _variadic_list))
var_list_entry(sprintf, bipolar,
               (p8 address_to into, string_address format, ...), format,
               vsprintf(into, format, _variadic_list))

/*
        The entries that write bytes without reading a format.

        fputc and fputs are here only when the stream family is not, because a
        byte to a stream is that family's operation and it already exports both
        as aliases onto its own put_byte and put_string. puts and putchar are
        always here: nothing else in the tree defines them, and puts is not
        fputs with a newline glued on -- the asymmetry is real and is what C
        says, and it is the single most common surprise in the whole header.
*/
#ifdef FORMAT_OWNS_BYTE_ENTRIES

static b32 fputc(b32 byte, format_stream stream)
{
        p8 value = (p8)byte;

        if (format_stream_write(stream, address_of value, 1) != 1)
                return -1;

        return (b32)value;
}

static b32 fputs(string_address text, format_stream stream)
{
        positive length;

        if (is_null(text))
                return -1;

        length = string_length(text);

        if (length &&
            format_stream_write(stream, (address_any)text, length) != length)
                return -1;

        return 1;
}

#endif // FORMAT_OWNS_BYTE_ENTRIES

static b32 putchar(b32 byte)
{
        return fputc(byte, format_output);
}

static b32 puts(string_address text)
{
        positive length;

        if (is_null(text))
                return -1;

        length = string_length(text);

        if (length &&
            format_stream_write(format_output, (address_any)text, length) !=
                length)
                return -1;

        if (format_stream_write(format_output, (address_any) "\n", 1) != 1)
                return -1;

        return 1;
}

/*
        perror writes the prefix, a colon, a space, the sentence and a newline;
        with an empty or absent prefix it writes the sentence and the newline
        alone. It is one call per piece rather than one formatted line because
        the sentence is the last thing a failing program gets to say, and a
        formatter that itself buffered would be the wrong thing to depend on
        there.

        Only when the error family is absent. That family builds the same line
        into a stack buffer and writes it with one trap, without going through
        a stream at all, which is a better answer for the same reason: a
        diagnostic wants to be independent of everything that might be what
        failed. Its version wins wherever it is present.
*/
#ifdef FORMAT_OWNS_PERROR

static fn perror(string_address prefix)
{
        string_address reason = strerror((b32)errno);

        if (prefix && string_get(prefix))
        {
                format_stream_write(format_error, (address_any)prefix,
                                    string_length(prefix));
                format_stream_write(format_error, (address_any) ": ", 2);
        }

        format_stream_write(format_error, (address_any)reason,
                            string_length(reason));
        format_stream_write(format_error, (address_any) "\n", 1);
}

#endif // FORMAT_OWNS_PERROR

#endif // KERNEL_MODE

#endif // STANDARD_MODERN_C_FORMAT
#endif // STANDARD_SKIP_FORMAT

#ifndef STANDARD_SKIP_SCAN
/* ---- scan.c ---- */
/*
        Experimental C standard library

        scanf: a format string, an input, and the values it takes out of it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SCAN
#define STANDARD_MODERN_C_STANDARD_SCAN

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason text.c gives beside the same two words. core.c includes
        this umbrella, library.c sets KERNEL_MODE from __MODULE__, and a
        kernel has no business carrying a float parser -- arm64 refuses a
        decimal in a signature there whether or not anything calls it, which
        is why math.c and format.c are each one guarded block as well.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        THIS IS PRINTF READ BACKWARDS, AND IT IS NOT SYMMETRIC

        format.c decides which byte goes where. This decides how far to read
        before it knows, which is the harder half: a formatter can measure its
        answer and then emit it, while a parser has to commit to consuming a
        byte before it can tell whether the byte belongs to the thing it is
        reading. Every corner of scanf is that one problem wearing a different
        hat, and C answers it with one sentence in 7.21.6.2 paragraph 9 that
        the whole of this file is an implementation of:

            the input item is the longest sequence of input characters which
            does not exceed any specified field width and which is, or is a
            prefix of, a matching input sequence

        Read that twice. It does not say "read while the bytes are valid". It
        says read while what you have could still GROW into something valid,
        and only then ask whether what you have is itself valid. The two are
        different at exactly the places a hand-written scanf is wrong:

            "0x"    with %x   -- a prefix of "0x1", so both bytes are eaten,
                                 and then it is not a number: matching failure
            "0x1f"  with %2x  -- the width stops it at "0x", same answer
            "1e+"   with %f   -- a prefix of "1e+5", eaten, matching failure
            "infinit" with %f -- a prefix of "infinity", eaten, and "inf" is
                                 NOT the answer even though it was passed
            "nan("  with %f   -- a prefix of "nan(1)", eaten, no fallback
            "- 5"   with %d   -- "-" is a prefix of "-5", eaten, then a space
            "0xz"   with %f   -- "0x" is a prefix, "0xz" is not, so the item
                                 is "0x" and the answer is a matching failure
                                 rather than the zero that strtod would give

        A real glibc answers every one of those the way this file does, and
        every one of them was measured before a line here was written rather
        than reasoned about afterwards. The last of them is the reason this
        file does its own syntax scan for %f instead of handing the rest of
        the input to strtod and believing the pointer it returns: strtod is
        specified to take the longest sequence that IS valid, which for "0xz"
        is "0", and scanf is specified to take the longest sequence that MAY
        BECOME valid, which is "0x". They differ on the input and they differ
        on the answer, so the split here is scan the shape in this file, and
        convert the value with the one float parser the tree has.

        The other thing worth writing down before the code: because the rule
        is "read while it could still grow", nothing here ever backtracks.
        The scan stops at the first byte that cannot extend a prefix and that
        one byte goes back. One byte, never two, which is exactly what the C
        standard promises a stream can take and exactly what stream.c's
        ungetc guarantees -- its array holds eight and this needs one of them.
        An implementation that tried "inf" after "infinity" failed would need
        five, and would be wrong as well as greedy.
*/

/*
        THE SEAM ONTO THE FLOAT PARSER

        There is no float parser here and there should not be. This file
        decides where a number ends -- which is a different question from
        which digits are valid, and the note above says why -- and hands the
        bytes it decided on to the family that turns digits into values.

        That family is numbers.c and it exports the three C spells for the
        three widths: strtof for a float, strtod for a double, strtold for a
        long double, each taking a string and the address of a pointer that
        comes back saying where the parse stopped. Nothing here reads that
        pointer. The scan above already decided where the number ends and the
        buffer handed over holds exactly that and no more, so the answer is
        always its terminator; a parser that ignores a null ending is fine.

        All three are used, one per width, which is not a nicety. Parsing to a
        double and narrowing to a float is wrong twice: it rounds twice, which
        can land a value one place off at the bottom of the float, and it
        throws away a not-a-number's payload, which "%f" against "nan(1)"
        shows immediately. glibc calls strtof there and so does this.

        Where numbers.c is absent the seam still closes, on the one entry that
        cannot be done without: strtod is declared and the other two widths
        are the double narrowed and the double widened. That configuration is
        what the whole family was developed and diffed against, before
        numbers.c landed, and the two rounding divergences above are its and
        not scanf's. Either way the merge is include order and nothing else --
        numbers.c, then stream.c, then format.c, then this.
*/
#if defined(STANDARD_MODERN_C_STANDARD_NUMBERS) && decimal_bits == 64

#define SCAN_DECIMAL_DECLARED 1
//      The three take a const char * and everything here carries a
//      string_address, which is the same pointer wearing the other
//      signedness. The cast is in the macro so the twenty odd call sites
//      below do not each have to say it.
#define SCAN_DECIMAL_NARROW(where, stop) \
        strtof((const char address_to)(where), (char address_to address_to)(stop))
#define SCAN_DECIMAL_WIDE(where, stop) \
        strtold((const char address_to)(where), (char address_to address_to)(stop))

#endif

#ifndef SCAN_DECIMAL
#define SCAN_DECIMAL(where, stop) \
        strtod((const char address_to)(where), (char address_to address_to)(stop))
#endif

#ifndef SCAN_DECIMAL_DECLARED
decimal SCAN_DECIMAL(string_address text, string_address address_to ending);
#endif

/*
        Whether the stream family is under this one.

        fscanf, scanf and their var_args forms need a FILE to read from and a
        byte of pushback to give back, and both are stream.c's. Without it the
        string entries still build and still work, which is how the sscanf
        half was first tested; there is deliberately no descriptor fallback
        the way format.c has one for writing, because a raw descriptor has no
        end-of-file indicator, no error indicator and no pushback, and three
        of scanf's answers are about exactly those.
*/
#ifdef STANDARD_MODERN_C_STANDARD_STREAM
#define SCAN_STREAMS 1
typedef FILE address_to scan_stream;
#else
#define SCAN_STREAMS 0
typedef address_any scan_stream;
#endif

/*
        Out of range, reported the way strtol reports it.

        glibc's scanf converts through strtol and strtoul and lets their
        ERANGE stand, so a %d that overflows sets errno and stores the
        saturated value rather than failing the conversion. That is measured
        behaviour and is matched below. Where the error family is absent there
        is no errno to set and the value is still saturated, which is the only
        part a caller without errno could have observed anyway.
*/
#ifdef STANDARD_MODERN_C_STANDARD_ERROR
#define scan_out_of_range() (errno = ERANGE)
#define scan_errno_save(into) ((into) = errno, errno = 0)
#define scan_errno_keep(from) ((fn)(errno == 0 ? (errno = (from)) : errno))
#define scan_errno_clear() (errno = 0)
#else
#define scan_out_of_range() ((fn)0)
#define scan_errno_save(into) ((into) = 0)
#define scan_errno_keep(from) ((fn)(from))
#define scan_errno_clear() ((fn)0)
#endif

/*
        The six bytes isspace calls white, written once. The umbrella folds a
        literal set into a 256 byte table and a jump into string_span, so
        every use of this below is a table lookup per byte and no set build.
*/
#define scan_white " \t\n\v\f\r"

/*
        How much of one number is kept.

        The scan consumes every byte the input item has, however many that is,
        because the position it leaves behind is part of the answer. What it
        STORES is capped, and the cap is here. An integer needs at most twenty
        significant digits before the answer is "too big" and nothing past
        that can change it, so the cap is unreachable there by construction --
        the leading zeros are not stored at all and a run of more than a
        thousand significant digits has overflowed a hundred times over.

        A float is the one place the cap can be reached and change an answer,
        and it takes a literal with more than a thousand characters in its
        mantissa to do it. Past that the stored text is truncated, the
        conversion is of the truncated text, and the consumption and the
        return value are still exact. It is written down rather than hidden
        because it is a real divergence from a glibc that allocates.
*/
#define scan_stage_bytes 1024

/*
        WHERE THE BYTES COME FROM

        One structure for both halves, because every directive below would
        otherwise be written twice. A string source walks a pointer; a stream
        source calls stream.c. The difference shows up in three places and
        they are all marked: pushback, the whole-run scans, and the end.

        held is the stream's pushback and the string source never uses it. A
        string can put a byte back by walking backwards, which costs nothing
        and has no depth limit, and keeping the string source's pushback empty
        is what lets the run scans below hand a whole run to the library in
        one call instead of a byte at a time. A stream cannot walk backwards,
        so its returned byte is kept here until the scan is over and then
        pushed into stream.c's array -- once, at the end, rather than through
        ungetc in the middle, so that nothing here disturbs the end-of-file
        indicator that feof is about to be asked for.

        consumed is what %n reports. It counts a byte when the byte is handed
        out and uncounts it when the byte goes back, so a lookahead that was
        rejected is not in the number, which is the whole of what %n is for.

        ran_out is not about the answer either; it is about errno, and the
        note beside scan_leave is what it is for. A white space skip whose
        FIRST read is the end of the source is what glibc treats as an input
        failure even on a call that then succeeds, and telling that apart
        from a skip that read a space and then the end is one bit of state
        that nothing else in the file can see.

        ended is this file's own memory of end-of-file and is not the stream's
        indicator. The two are deliberately separate: the stream's is what
        feof answers and must not be cleared by anything here, and this one is
        cleared whenever a byte goes back, because a source holding a byte is
        not at its end.
*/
typedef struct scan_source scan_source;

struct scan_source
{
        string_address text;
        positive place;

        scan_stream handle;

        positive consumed;
        positive held;
        p8 holding[8];

        bipolar entered_errno;

        bool ended;
        bool ran_out;
};

#define scan_is_text(source) ((source)->handle == null)

static fn scan_text_take(scan_source address_to source, p8 address_to into,
                         positive count)
{
        if (into != null && count != 0)
                memory_copy(into, source->text + source->place, count);

        source->place += count;
        source->consumed += count;
}

/*
        One byte out, and one byte back.

        EOF is never put into the holding array and never given to stream.c's
        ungetc, which refuses it: the end of a source is a fact about the
        source and not a byte it is holding. scan_unget of EOF is therefore a
        deliberate no-op, so that every caller can push back whatever stopped
        it without first asking whether anything did.
*/
static b32 scan_get(scan_source address_to source)
{
        b32 byte;

        if (source->held != 0)
        {
                source->held--;
                source->consumed++;

                return (b32)source->holding[source->held];
        }

#if SCAN_STREAMS
        if (source->handle != null)
        {
                byte = stream_get_byte(source->handle);

                if (byte == EOF)
                {
                        source->ended = true;

                        return EOF;
                }

                source->consumed++;

                return byte;
        }
#endif

        if (string_get(source->text + source->place) == end)
        {
                source->ended = true;

                return EOF;
        }

        byte = (b32)source->text[source->place];
        source->place++;
        source->consumed++;

        return byte;
}

static fn scan_unget(scan_source address_to source, b32 byte)
{
        if (byte == EOF)
                return;

        source->consumed--;
        source->ended = false;

#if SCAN_STREAMS
        if (source->handle != null)
        {
                source->holding[source->held] = (p8)byte;
                source->held++;

                return;
        }
#endif

        source->place--;
}

/*
        The end of the call, which is where a stream gets its byte back.

        stream.c's pushback is a stack, so a byte pushed last comes out first,
        and the array here is a stack for the same reason. Handing one to the
        other therefore walks this one from the bottom: the byte at index zero
        was returned first, so it is the one that must end up deepest.

        In practice there is one byte here at most, because the prefix rule at
        the top of this file never needs two. The loop is written for the
        array's size rather than for one because a routine that is correct
        only for the case that happens to occur is a routine that is wrong the
        first time the case changes.
*/
static fn scan_finish(scan_source address_to source)
{
#if SCAN_STREAMS
        positive index;

        if (source->handle == null)
                return;

        for (index = 0; index < source->held; index++)
                stream_unget_byte((b32)source->holding[index], source->handle);
#endif

        source->held = 0;
}

/*
        A run of white space, skipped.

        This is the one scan where the two sources differ in shape rather than
        only in mechanism. A string has the whole run in front of it and hands
        it to string_span, which the umbrella has already folded into a table
        lookup per byte over the six white bytes; a stream has one byte at a
        time and no buffer this file is allowed to reach into, so it asks
        byte_is_space per byte -- the library's classifier, not a comparison
        chain written here.

        Reaching into stream.c's buffer would make this one call as well, and
        it is deliberately not done: format.c writes through stream_put_bytes
        for the same reason. The buffer, its direction and its refill are that
        file's, and a second file that knew where the bytes were would have to
        be right about all three forever.
*/
static fn scan_skip_white(scan_source address_to source)
{
        b32 byte;

        if (scan_is_text(source))
        {
                positive run = string_span_of_set(source->text + source->place,
                                                  (string_address)scan_white);

                if (run == 0 &&
                    string_get(source->text + source->place) == end)
                        source->ran_out = true;

                source->place += run;
                source->consumed += run;

                return;
        }

        byte = scan_get(source);

        if (byte == EOF)
        {
                source->ran_out = true;

                return;
        }

        while (byte != EOF && byte_is_space(byte))
                byte = scan_get(source);

        scan_unget(source, byte);
}

/*
        WHAT ONE DIRECTIVE ASKS FOR

        The star is assignment suppression, the width is a byte count and zero
        means none -- a written "%0d" is a width of zero, which C leaves
        undefined and glibc reads as no width at all, measured and matched.
        The length is the width of the object being written and not the width
        of the value: every one of long, long long, size_t, ptrdiff_t and
        intmax_t is eight bytes on all three machines this builds for, so the
        eight distinct modifiers land on four distinct stores.
*/
#define SCAN_LENGTH_CHAR 1
#define SCAN_LENGTH_SHORT 2
#define SCAN_LENGTH_INT 4
#define SCAN_LENGTH_WIDE 8
#define SCAN_LENGTH_DECIMAL 9
#define SCAN_LENGTH_WIDE_DECIMAL 10

static const p8 scan_conversion_lengths[] = {
    [CONVERSION_LENGTH_INT] = SCAN_LENGTH_INT,
    [CONVERSION_LENGTH_CHAR] = SCAN_LENGTH_CHAR,
    [CONVERSION_LENGTH_SHORT] = SCAN_LENGTH_SHORT,
    [CONVERSION_LENGTH_LONG] = SCAN_LENGTH_DECIMAL,
    [CONVERSION_LENGTH_LONG_LONG] = SCAN_LENGTH_WIDE,
    [CONVERSION_LENGTH_SIZE] = SCAN_LENGTH_WIDE,
    [CONVERSION_LENGTH_DIFFERENCE] = SCAN_LENGTH_WIDE,
    [CONVERSION_LENGTH_WIDEST] = SCAN_LENGTH_WIDE,
    [CONVERSION_LENGTH_WIDE_DECIMAL] = SCAN_LENGTH_WIDE_DECIMAL,
};

/*
        The staging buffer and what is in it.

        Two very different fills share it, which is why the counts are named
        rather than one length. A float stages the raw text it matched, ready
        for strtod, and staged is how much of it fits. An integer stages only
        its SIGNIFICANT digits -- the leading zeros are dropped on the way in,
        because the only two questions asked of that run afterwards are how
        many digits it has and whether they are above the largest value the
        machine holds, and a leading zero answers neither.

        significant is the true count and staged is how much of it was kept.
        They differ only past the cap, and past the cap the answer is already
        "too big" whatever the digits are.
*/
typedef struct scan_stage scan_stage;

struct scan_stage
{
        p8 bytes[scan_stage_bytes + 1];
        positive staged;
        positive significant;
};

static fn scan_stage_clear(scan_stage address_to stage)
{
        stage->staged = 0;
        stage->significant = 0;
}

//      A raw byte, for the float scan, kept while there is room.
static fn scan_stage_byte(scan_stage address_to stage, b32 byte)
{
        if (stage->staged < scan_stage_bytes)
        {
                stage->bytes[stage->staged] = (p8)byte;
                stage->staged++;
        }
}

//      A digit, for the integer scan, with a leading zero counted by neither.
static fn scan_stage_digit(scan_stage address_to stage, b32 byte)
{
        if (stage->significant == 0 && byte == '0')
                return;

        stage->significant++;

        if (stage->staged < scan_stage_bytes)
        {
                stage->bytes[stage->staged] = (p8)byte;
                stage->staged++;
        }
}

/*
        Is this byte a digit in this base?

        Four bases, because %i can read a base out of its own input and one
        of the four it can find is two. %o is eight, %x and %p are sixteen,
        %i is whichever of two, eight, ten and sixteen its prefix said, and
        everything else is ten. The two that a classifier covers are the
        library's branchless ones rather than comparisons written here; octal
        is the decimal classifier with its top two values removed, which is
        the shape the classifier already has, and binary is two values and
        has nothing worth borrowing.
*/
static bool scan_digit_of_base(b32 byte, positive base)
{
        if (base == 16)
                return byte_is_hexadecimal(byte) != 0;

        if (base == 8)
                return byte_is_digit(byte) != 0 && byte <= '7';

        if (base == 2)
                return byte == '0' || byte == '1';

        return byte_is_digit(byte) != 0;
}

/*
        THE MAGNITUDE, AND WHETHER IT FIT

        string_digits_base_max is the library's bounded digit run and it is
        the whole of the arithmetic here: a slice of digits, a bound, a base,
        and the number they spell. It wraps silently, which is the right
        contract for the thirty three parsers it was written for and the wrong
        one for scanf, because glibc converts through strtoul and saturates.

        So overflow is decided before the routine is called, by counting
        rather than by arithmetic. The largest value the machine holds has a
        fixed number of digits in any base -- twenty in ten, sixteen in
        sixteen, twenty two in eight -- and a run of significant digits that
        is shorter than that cannot overflow, a run that is longer always
        does, and a run of exactly that length is decided by comparing the two
        strings. That comparison is a plain memcmp because the digits of every
        base up to thirty six rise through ASCII in the same order as their
        values once the letters are folded down: '0' through '9' at 0x30, then
        'a' through 'z' at 0x61, with nothing out of order in between. The
        fold is memory_to_lower_ascii over the slice, which is assembly, and
        the largest value's own digits come from positive_into_base, which is
        the same assembly the formatter uses.

        A leading zero is not a significant digit and never reaches this,
        because the scan above declined to store it: "0000000000000000000005"
        is twenty two digits and one significant one, and a count that took
        the leading zeros in would call every one of those an overflow. That
        is why the staging buffer keeps two counts rather than a length.

        Nothing here looks at a digit one at a time. The comparison is
        memory_compare, the fold is memory_to_lower_ascii, the limit's digits
        are positive_into_base and the value is string_digits_base_max, and
        each of those four is assembly at three-architecture parity.
*/
static positive scan_magnitude(scan_stage address_to stage, positive base,
                               bool address_to overflowed)
{
        p8 largest[72];
        positive largest_length;

        address_to overflowed = false;

        if (stage->significant == 0)
                return 0;

        if (stage->significant > stage->staged)
        {
                //      More digits than the buffer kept, and the buffer holds
                //      a thousand. Nothing that long is representable.
                address_to overflowed = true;

                return 0;
        }

        largest_length = positive_into_base(largest, ~(positive)0, base, false);

        if (stage->significant > largest_length)
        {
                address_to overflowed = true;

                return 0;
        }

        if (stage->significant == largest_length)
        {
                memory_to_lower_ascii(stage->bytes, stage->staged);

                if (memory_compare(stage->bytes, largest, largest_length) > 0)
                {
                        address_to overflowed = true;

                        return 0;
                }
        }

        return string_digits_base_max(stage->bytes, stage->staged, base, null);
}

/*
        THE INTEGER SCAN

        One routine for %d %i %u %o %x %X and %p's hexadecimal half, because
        they differ only in which base they start from and whether a "0x" is
        allowed to change it. base is the base to read in, or zero for %i,
        which reads the base out of the input the way C source does.

        The order below is the grammar and the grammar is the prefix rule:
        the sign is taken before anything says a digit follows it, the "0x" is
        taken before anything says a hexadecimal digit follows it, and both
        are what makes "-" and "0x" matching failures rather than pushed-back
        bytes. Only ONE byte is ever pushed back, and it is the byte that
        could not extend the prefix.

        digits is how many digits were seen at all and decides whether this
        matched. The stage's significant count is how many of them were not
        leading zeros and decides the value. "0" has one of the first and none
        of the second, which is exactly the distinction the two counts exist
        for.
*/
static bool scan_integer(scan_source address_to source, scan_stage address_to stage,
                         positive width, positive base, bool address_to negative,
                         positive address_to base_used)
{
        positive limit = width != 0 ? width : ~(positive)0;
        positive taken = 0;
        positive digits = 0;
        bool letter_last = false;
        b32 byte;

        scan_stage_clear(stage);
        address_to negative = false;

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == '+' || byte == '-')
                {
                        address_to negative = byte == '-';
                        taken++;
                }
                else
                {
                        scan_unget(source, byte);
                }
        }

        //      The base prefix, which only two of the conversions have. %i
        //      has no base yet and reads one here; %x and %p have sixteen
        //      already and allow the prefix to be written anyway.
        if ((base == 0 || base == 16) && taken < limit)
        {
                byte = scan_get(source);

                if (byte == '0')
                {
                        taken++;
                        digits++;
                        scan_stage_digit(stage, byte);

                        if (taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == 'x' || byte == 'X')
                                {
                                        //      The prefix is taken and the
                                        //      digit it promised has not
                                        //      arrived yet. If none does,
                                        //      this is a matching failure
                                        //      with both bytes eaten.
                                        taken++;
                                        base = 16;
                                        digits = 0;
                                        letter_last = true;
                                        scan_stage_clear(stage);
                                }
                                else if (base == 0 &&
                                         (byte == 'b' || byte == 'B'))
                                {
                                        //      "0b" is C23's binary literal
                                        //      and glibc reads it here, in
                                        //      %i and nowhere else -- an
                                        //      explicit %x never takes it,
                                        //      because sixteen was not asked
                                        //      to go looking for a base.
                                        taken++;
                                        base = 2;
                                        digits = 0;
                                        letter_last = true;
                                        scan_stage_clear(stage);
                                }
                                else
                                {
                                        scan_unget(source, byte);

                                        if (base == 0)
                                                base = 8;
                                }
                        }
                        else if (base == 0)
                        {
                                base = 8;
                        }
                }
                else
                {
                        scan_unget(source, byte);

                        if (base == 0)
                                base = 10;
                }
        }

        while (taken < limit)
        {
                byte = scan_get(source);

                if (byte == EOF)
                        break;

                if (!scan_digit_of_base(byte, base))
                {
                        scan_unget(source, byte);
                        break;
                }

                taken++;
                digits++;
                letter_last = false;
                scan_stage_digit(stage, byte);
        }

        /*
                One byte past the end, looked at and put straight back.

                A run that stopped because it ran out of width has not yet
                touched the byte after it, and glibc has: its digit loop
                reads the next byte before it tests the width, so a "%3d"
                that ends exactly at the end of a file leaves feof set and
                one that ends anywhere else leaves the byte pushed back. The
                position is the same either way and the indicator is not, so
                the look is taken here too rather than leaving feof to
                disagree after every bounded conversion.

                Not when the width ran out on the "x" of a "0x" or the "b"
                of a "0b" itself, which is the one place glibc stops without
                looking. "%2x" against "0x" consumes both bytes, answers a
                matching failure and leaves the end-of-file indicator alone,
                while "%1i" against "0" consumes its one byte, answers zero
                and DOES set it. Both are measured, both are one line apart,
                and there is no reading of the standard that predicts either.
        */
        if (taken == limit && !letter_last)
                scan_unget(source, scan_get(source));

        address_to base_used = base;

        return digits != 0;
}

/*
        THE FLOAT SCAN

        The shape only. The value is strtod's, and the text handed to it is
        exactly the bytes matched here and nothing after them.

        Four alternatives share one sign: a hexadecimal significand with an
        optional binary exponent, a decimal significand with an optional
        decimal exponent, an infinity spelled either "inf" or "infinity", and
        a not-a-number optionally carrying a parenthesised tag. All four are
        scanned as prefixes and each says separately whether what was taken is
        also complete, which is the difference between "inf" -- complete at
        three bytes -- and "infinit", which is a longer prefix of the same
        word and complete at nothing.

        The numeric arm is written as flags rather than as states because the
        two bases differ only in which byte introduces the exponent and which
        classifier says what a digit is. Everything else -- one point at most,
        an exponent only after a digit, a sign only immediately after the
        exponent letter, at least one digit in the exponent once the letter is
        there -- is the same sentence for both, and writing it twice would be
        writing the same four rules twice.
*/
static bool scan_decimal_shape(scan_source address_to source,
                               scan_stage address_to stage, positive width)
{
        positive limit = width != 0 ? width : ~(positive)0;
        positive taken = 0;
        b32 byte;

        scan_stage_clear(stage);

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == '+' || byte == '-')
                {
                        scan_stage_byte(stage, byte);
                        taken++;
                }
                else
                {
                        scan_unget(source, byte);
                }
        }

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == EOF)
                        return false;

                if (byte_to_lower(byte) == 'i' || byte_to_lower(byte) == 'n')
                {
                        string_address word = byte_to_lower(byte) == 'i'
                                                      ? (string_address) "infinity"
                                                      : (string_address) "nan";
                        positive full = byte_to_lower(byte) == 'i' ? 8 : 3;
                        positive matched = 1;
                        bool tagged = false;
                        bool closed = false;

                        scan_stage_byte(stage, byte);
                        taken++;

                        while (matched < full && taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == EOF)
                                        break;

                                if ((b32)(p8)byte_to_lower(byte) !=
                                    (b32)word[matched])
                                {
                                        scan_unget(source, byte);
                                        break;
                                }

                                scan_stage_byte(stage, byte);
                                taken++;
                                matched++;
                        }

                        //      "nan" may carry a tag, and the tag is a prefix
                        //      the same way the word is: an open bracket with
                        //      no close is a longer prefix of something valid
                        //      and therefore not an answer.
                        if (full == 3 && matched == 3 && taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == '(')
                                {
                                        scan_stage_byte(stage, byte);
                                        taken++;
                                        tagged = true;

                                        while (taken < limit)
                                        {
                                                byte = scan_get(source);

                                                if (byte == EOF)
                                                        break;

                                                if (byte == ')')
                                                {
                                                        scan_stage_byte(stage,
                                                                        byte);
                                                        taken++;
                                                        closed = true;
                                                        break;
                                                }

                                                if (!byte_is_alnum(byte) &&
                                                    byte != '_')
                                                {
                                                        scan_unget(source, byte);
                                                        break;
                                                }

                                                scan_stage_byte(stage, byte);
                                                taken++;
                                        }
                                }
                                else
                                {
                                        scan_unget(source, byte);
                                }
                        }

                        if (tagged)
                                return closed;

                        //      Three bytes is "inf" and eight is "infinity",
                        //      and the five lengths between them are prefixes
                        //      of the longer word and answers to nothing.
                        return matched == 3 || matched == full;
                }

                scan_unget(source, byte);
        }

        {
                bool hexadecimal = false;
                bool pointed = false;
                bool exponent = false;
                bool exponent_signed = false;
                positive significand = 0;
                positive exponent_digits = 0;

                if (taken < limit)
                {
                        byte = scan_get(source);

                        if (byte == '0')
                        {
                                scan_stage_byte(stage, byte);
                                taken++;
                                significand++;

                                if (taken < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == 'x' || byte == 'X')
                                        {
                                                scan_stage_byte(stage, byte);
                                                taken++;
                                                hexadecimal = true;
                                                significand = 0;
                                        }
                                        else
                                        {
                                                scan_unget(source, byte);
                                        }
                                }
                        }
                        else
                        {
                                scan_unget(source, byte);
                        }
                }

                while (taken < limit)
                {
                        byte = scan_get(source);

                        if (byte == EOF)
                                break;

                        if (!exponent)
                        {
                                if (scan_digit_of_base(byte,
                                                       hexadecimal ? 16 : 10))
                                {
                                        significand++;
                                }
                                else if (byte == '.' && !pointed)
                                {
                                        pointed = true;
                                }
                                else if (significand != 0 &&
                                         byte_to_lower(byte) ==
                                             (hexadecimal ? 'p' : 'e'))
                                {
                                        exponent = true;
                                }
                                else
                                {
                                        scan_unget(source, byte);
                                        break;
                                }
                        }
                        else if (exponent_digits == 0 && !exponent_signed &&
                                 (byte == '+' || byte == '-'))
                        {
                                exponent_signed = true;
                        }
                        else if (byte_is_digit(byte))
                        {
                                exponent_digits++;
                        }
                        else
                        {
                                scan_unget(source, byte);
                                break;
                        }

                        scan_stage_byte(stage, byte);
                        taken++;
                }

                return significand != 0 &&
                       (!exponent || exponent_digits != 0);
        }
}

/*
        THE SCAN SET

        %[ builds a table of two hundred and fifty six answers on the stack
        and then hands it to string_span, which is the library's table-driven
        run scanner and the same routine the umbrella's folded literal sets
        jump into. The table cannot be folded here because the members are
        read at run time out of the format, but the scan over the input is the
        same assembly either way.

        The four rules that every implementation gets at least one of wrong,
        each measured against glibc rather than reasoned about:

            a ']' immediately after the '[' or after the '^' is a MEMBER and
            not the close, so "%[]]" is the set holding one close bracket

            a '-' that is first, or last before the ']', is a member, so
            "%[a-]" is 'a' and a dash and "%[-a]" is a dash and 'a'

            a '-' between two members is a range, and the range runs from the
            member most recently added, so "%[a-c-e]" is 'a' through 'e'

            a range whose ends are the wrong way round is not an empty range
            and not an error: glibc adds the two ends and the dash as three
            plain members, so "%[z-a]" matches "z-a" and nothing else

        A format that runs out before its ']' is not a scan set at all. glibc
        stops the whole call there and returns what it had already assigned,
        which is zero rather than EOF even on empty input -- a broken format
        is not an input failure -- and that is what the caller does with the
        null this returns.
*/
static string_address scan_build_set(string_address at, b8 address_to set,
                                     bool address_to negated)
{
        b32 previous = -1;
        b8 included;

        address_to negated = false;

        if (string_get(at) == '^')
        {
                address_to negated = true;
                at++;
        }

        included = !address_to negated;
        memory_fill(set, address_to negated, 256);

        if (string_get(at) == ']')
        {
                set[']'] = included;
                previous = ']';
                at++;
        }

        while (string_get(at) != end && string_get(at) != ']')
        {
                b32 member = (b32)string_get(at);

                if (member == '-' && previous >= 0 && at[1] != end &&
                    at[1] != ']')
                {
                        b32 last = (b32)at[1];

                        if (previous <= last)
                                memory_fill(set + previous, included,
                                            (positive)(last - previous + 1));
                        else
                        {
                                set['-'] = included;
                                set[last] = included;
                        }

                        previous = last;
                        at += 2;

                        continue;
                }

                set[member] = included;
                previous = member;
                at++;
        }

        if (string_get(at) != ']')
                return null;

        return at + 1;
}

/*
        THE STORE

        One place where a converted value becomes bytes in the caller's
        object, so that the eight length modifiers are read once rather than
        at every conversion. long, long long, size_t, ptrdiff_t and intmax_t
        are all eight bytes on x86_64, arm64 and riscv64, so there are four
        integer widths here and not eight, and the narrowing is the store
        itself: glibc converts at the full width of the machine word, saturates
        there, and then truncates into whatever the caller pointed at, which
        is why "%hhd" of 300 stores 44 and "%d" of a number past a signed long
        stores minus one.
*/
static fn scan_store_whole(address_any into, positive length, positive value)
{
        switch (length)
        {
        case SCAN_LENGTH_CHAR:
                address_to (p8 address_to)into = (p8)value;
                break;
        case SCAN_LENGTH_SHORT:
                address_to (p16 address_to)into = (p16)value;
                break;
        case SCAN_LENGTH_INT:
                address_to (p32 address_to)into = (p32)value;
                break;
        default:
                address_to (positive address_to)into = value;
                break;
        }
}

/*
        A DOUBLE, WRITTEN INTO A LONG DOUBLE, WITHOUT ASKING THE COMPILER

        %Lf points at an object this tree has no arithmetic for. Writing to it
        with a cast -- "(f128)value" -- compiles on x86_64, where the widening
        is one x87 instruction, and does not link on arm64 or riscv64, where
        long double is IEEE binary128 and the widening is a call into libgcc
        that a -nostdlib link has no copy of. Writing eight bytes of double
        into a sixteen byte object instead is not a precision divergence, it
        is a wrong object, so neither of those is the answer.

        The answer is that both formats are the same three fields in different
        places, and moving a field is a shift. x86_64's long double is the x87
        eighty bit extended: fifteen bits of exponent, then an EXPLICIT
        integer bit that IEEE's other formats leave implied, then sixty three
        bits of fraction, in a sixteen byte object with six bytes of nothing
        on the end. arm64 and riscv64 use binary128: fifteen bits of exponent
        and a hundred and twelve of fraction with the integer bit implied as
        usual. Every double is exactly representable in both, so the move is
        exact and there is no rounding to get wrong.

        A subnormal double is a NORMAL number in either wide format, because
        both have room for exponents a double cannot reach, so its leading
        zeros have to come off the fraction and go onto the exponent. Counting
        them and shifting is the obvious way; multiplying by two to the sixty
        four first is exact, is one instruction, lands the value in the normal
        range in one step whatever it was, and takes the sixty four back off
        the exponent afterwards.

        The value itself is still only as good as a double, because the parse
        was strtod's. That is the divergence worth naming and it is named in
        the file that ships beside this one: "%Lf" here reads a long double's
        worth of object and a double's worth of digits, and printf in this
        tree does not read the L at all.
*/
#ifndef SCAN_DECIMAL_WIDE
static fn scan_store_wide_decimal(address_any into, decimal value)
{
        union {
                decimal value;
                p64 bits;
        } narrow;

        p64 sign;
        p64 exponent;
        p64 fraction;
        p64 wide;
        positive lowered = 0;

        narrow.value = value;

        if (((narrow.bits >> 52) & 0x7FF) == 0 &&
            (narrow.bits & 0x000FFFFFFFFFFFFFull) != 0)
        {
                narrow.value = value * 18446744073709551616.0;
                lowered = 64;
        }

        sign = narrow.bits >> 63;
        exponent = (narrow.bits >> 52) & 0x7FF;
        fraction = narrow.bits & 0x000FFFFFFFFFFFFFull;

        if (exponent == 0x7FF)
                wide = 0x7FFF;
        else if (exponent == 0 && fraction == 0)
                wide = 0;
        else
                wide = exponent + 16383 - 1023 - lowered;

#if __LDBL_MANT_DIG__ == 64
        {
                //      The x87 extended, whose integer bit is written down.
                //      It is set for everything except a true zero, which
                //      leaves both infinities and every not-a-number with it
                //      set, as that format requires.
                p64 piece[2];

                piece[0] = fraction << 11;

                if (!(exponent == 0 && fraction == 0))
                        piece[0] |= 0x8000000000000000ull;

                piece[1] = (sign << 15) | wide;

                //      Ten bytes and not sixteen. The object is sixteen for
                //      alignment and the format is eighty bits, and the
                //      instruction that stores one writes exactly the ten it
                //      needs: a caller that had something in the padding
                //      still has it afterwards, which is measurable against
                //      glibc and is what it leaves behind.
                memory_copy(into, piece, 10);
        }
#elif __LDBL_MANT_DIG__ == 113
        {
                //      binary128, whose integer bit is implied as usual, so
                //      the fraction simply moves sixty places up and splits
                //      across the two halves of the object.
                p64 piece[2];

                piece[0] = (fraction & 0xF) << 60;
                piece[1] = (sign << 63) | (wide << 48) | (fraction >> 4);

                memory_copy(into, piece, 16);
        }
#else
        /*
                Neither of the two formats this tree builds for. A target
                whose long double is its double needs no move at all, and one
                with a third format is a target this has never seen, so both
                take the compiler's own widening -- which is free in the
                first case and a call into libgcc in the second. Naming that
                here rather than assembling bytes for a layout nobody has
                looked at is the honest answer; a link that then asks for
                __extenddftf2 is asking a question that has to be answered
                with a measurement and not with a guess.
        */
        (void)sign;
        (void)exponent;
        (void)fraction;
        (void)wide;
        address_to (f128 address_to)into = (f128)value;
#endif
}
#endif // !SCAN_DECIMAL_WIDE

/*
        The three widths, each asking the parser that owns it.

        Every one of these runs even when the conversion was suppressed,
        because a suppressed conversion still reports a range error through
        errno and glibc's does: "%*f" against 1e400 answers zero assignments
        and ERANGE. So the parse happens and only the store is conditional.
*/
static f32 scan_narrow_decimal(string_address text, address_any into)
{
#ifdef SCAN_DECIMAL_NARROW
        f32 value = SCAN_DECIMAL_NARROW(text, null);
#else
        f32 value = (f32)SCAN_DECIMAL(text, null);
#endif

        if (into != null)
                address_to (f32 address_to)into = value;

        return value;
}

static fn scan_extended_decimal(string_address text, address_any into)
{
#ifdef SCAN_DECIMAL_WIDE

        //      A long double out and a long double in, which is a copy and
        //      not a conversion, so nothing here asks libgcc for anything.
        f128 value = SCAN_DECIMAL_WIDE(text, null);

        if (into != null)
                address_to (f128 address_to)into = value;
#else
        decimal value = SCAN_DECIMAL(text, null);

        if (into != null)
                scan_store_wide_decimal(into, value);
#endif
}

//      Signed magnitude saturates at the positive or negative limit.
static positive scan_signed_value(positive magnitude, bool negative,
                                  bool overflowed)
{
        positive limit = (~(positive)0 >> 1) + negative;

        if (overflowed || magnitude > limit)
        {
                scan_out_of_range();

                return limit;
        }

        return negative ? 0 - magnitude : magnitude;
}

//      And the unsigned one, where strtoul saturates at the top and a minus
//      sign on a value that fit is a wrap and not an error at all: "-1" into
//      %u is every bit set, and glibc does not call that out of range.
static positive scan_unsigned_value(positive magnitude, bool negative,
                                    bool overflowed)
{
        if (overflowed)
        {
                scan_out_of_range();

                return ~(positive)0;
        }

        return negative ? 0 - magnitude : magnitude;
}

/*
        WHICH FAILURE THIS WAS, AND WHAT THE CALL ANSWERS BECAUSE OF IT

        mark is what the source had already handed out when this directive's
        own reading began -- after its white space skip, because the skip is
        not part of the item. If the directive read nothing past that and the
        source is at its end, nothing was there and this is an INPUT failure,
        which answers EOF when nothing has been assigned yet. If it read
        something and that something was wrong, it is a MATCHING failure and
        answers the count, which may be zero.

        The two are one line apart and are the whole of what separates "the
        file ended" from "the file said something else", which is the answer
        every caller of scanf actually wants. "%d" against "" is EOF, "%d"
        against "+" is zero because the sign was read, and "%d" against " "
        is EOF again because the space was skipped rather than read.
*/
static b32 scan_leave(scan_source address_to source, b32 assigned,
                      bool starved)
{
        scan_finish(source);

        /*
                AND WHAT ERRNO SAYS ABOUT IT AFTERWARDS

                glibc puts errno aside on the way in and sets it to zero, so
                that a conversion which overflows can report ERANGE through
                it and one that does not cannot leave a stale number behind.
                On the way out it puts the caller's value back, unless
                something inside actually set one. That much is ordinary.

                The part that is not ordinary, and is measured rather than
                reasoned: a call that ends on an INPUT failure puts nothing
                back and clears what was set. "%ldx" against a number too
                large to hold, with nothing after it, answers one and leaves
                errno at zero -- the range error the conversion really had is
                thrown away by the literal that then ran out of input. The
                same call with a byte after the number keeps the ERANGE. A
                program that reads errno after a scanf is reading a value
                that depends on whether the LAST directive found anything,
                which is worth knowing before depending on it.

                The caller's own value does not survive that either: an entry
                errno of seven and an input failure leaves zero and not seven.
                Both halves were measured against glibc 2.44 rather than read
                out of it, and both are matched here because a program that
                tests errno after a failed scanf is testing this and not the
                sentence in the standard, which says nothing at all.
        */
        if (starved || source->ran_out)
                scan_errno_clear();
        else
                scan_errno_keep(source->entered_errno);

        if (starved && assigned == 0)
                return EOF;

        return assigned;
}

static b32 scan_stop(scan_source address_to source, positive mark, b32 assigned)
{
        return scan_leave(source, assigned,
                          source->ended && source->consumed == mark);
}

/*
        THE ENGINE

        A walk over the format with three kinds of thing in it: white space,
        which matches a run of white space including none; an ordinary byte,
        which must be there exactly; and a directive, which is everything
        else in this file.

        The two failures are not the same failure and the difference is the
        single most-missed thing in scanf. An INPUT failure is the source
        running out before a directive had anything to work with, and it makes
        the whole call answer EOF if nothing has been assigned yet. A MATCHING
        failure is bytes that were there and were wrong, and it answers with
        the count so far however small, including zero. "" against "%d" is EOF
        and "abc" against "%d" is zero, and a program that tells them apart is
        a program that can tell a short file from a bad one.

        A directive that read at least one byte and then ran out is a MATCHING
        failure and not an input one -- "1e+" at the end of a file answers
        zero, not EOF -- because the end of the file is what stopped a prefix
        from growing, and a prefix that stopped is a thing that failed to
        match. %c is the exception and is glibc's: a width it could not fill
        answers EOF even though it stored what it read.
*/
static b32 scan_run(scan_source address_to source, string_address format,
                    var_args list)
{
        scan_stage stage;
        b32 assigned = 0;
        string_address at = format;

        if (is_null(format))
                return scan_leave(source, 0, true);

        while (string_get(at) != end)
        {
                positive width = 0;
                positive length = SCAN_LENGTH_INT;
                positive mark = 0;
                bool suppress = false;
                bool literal = false;
                bool literal_skips = false;
                b32 byte;

                /* A string source exposes the whole ordinary literal run.
                   The common-prefix floor gives both the match and the exact
                   mismatch position, so successful bytes remain consumed and
                   the rejected byte remains untouched just as scan_get plus
                   scan_unget would leave them.  Streams retain the byte path:
                   their refill buffer belongs to stream.c. */
                if (scan_is_text(source) &&
                    string_get(at) != '%' &&
                    !byte_is_space(string_get(at)))
                {
                        string_address input =
                                source->text + source->place;
                        positive run = string_span_without_set(
                                at, (string_address)"% \t\n\r\v\f");
                        positive available = string_length_max(input, run);
                        positive matched = memory_common_prefix(
                                (address_any)at, (address_any)input, available);

                        source->place += matched;
                        source->consumed += matched;
                        at += matched;

                        if (matched != run)
                        {
                                if (matched == available)
                                {
                                        source->ended = true;
                                        return scan_leave(source, assigned,
                                                          true);
                                }

                                return scan_leave(source, assigned, false);
                        }

                        continue;
                }

                if (byte_is_space(string_get(at)))
                {
                        at += string_span_of_set(at, " \t\n\r\v\f");

                        scan_skip_white(source);

                        continue;
                }

                if (string_get(at) != '%')
                {
                        literal = true;
                }
                else
                {
                        at++;

                        if (string_get(at) == '*')
                        {
                                suppress = true;
                                at++;
                        }

                        while (byte_is_digit(string_get(at)))
                        {
                                width = width * 10 +
                                        (positive)(string_get(at) - '0');
                                at++;
                        }

                        length = scan_conversion_lengths[
                            conversion_length_take(address_of at)];

                        //      "%%" is a percent in the input, and it is
                        //      NOT the same thing as writing a percent as an
                        //      ordinary character: glibc skips the white
                        //      space in front of the one and not in front of
                        //      the other, so " %" matches "%%" and does not
                        //      match a bare percent. Measured, and the sort
                        //      of asymmetry only a diff finds.
                        if (string_get(at) == '%')
                        {
                                literal = true;
                                literal_skips = true;
                        }
                }

                if (literal)
                {
                        if (literal_skips)
                                scan_skip_white(source);

                        mark = source->consumed;
                        byte = scan_get(source);

                        if (byte == EOF)
                                return scan_leave(source, assigned, true);

                        if (byte != (b32)string_get(at))
                        {
                                scan_unget(source, byte);

                                return scan_leave(source, assigned, false);
                        }

                        at++;

                        continue;
                }

                switch (string_get(at))
                {
                case 'd':
                case 'i':
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                {
                        b32 conversion = (b32)string_get(at);
                        positive base = conversion == 'i'   ? 0
                                        : conversion == 'o' ? 8
                                        : conversion == 'x' || conversion == 'X'
                                            ? 16
                                            : 10;
                        bool negative;
                        bool overflowed;
                        positive magnitude;
                        positive value;
                        positive base_used;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        if (!scan_integer(source, address_of stage, width, base,
                                          address_of negative,
                                          address_of base_used))
                                return scan_stop(source, mark, assigned);

                        magnitude = scan_magnitude(address_of stage, base_used,
                                                   address_of overflowed);

                        if (conversion == 'd' || conversion == 'i')
                                value = scan_signed_value(magnitude, negative,
                                                          overflowed);
                        else
                                value = scan_unsigned_value(magnitude, negative,
                                                            overflowed);

                        if (!suppress)
                        {
                                scan_store_whole(var_list_get(list, address_any),
                                                 length, value);
                                assigned++;
                        }

                        break;
                }
                case 'p':
                {
                        bool negative;
                        bool overflowed;
                        positive magnitude;
                        positive value;
                        positive base_used;
                        bool matched;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        byte = scan_get(source);

                        //      "(nil)" is five bytes and a width that cannot
                        //      hold five is not offered it at all: glibc
                        //      pushes the bracket back untouched rather than
                        //      starting a word it has no room to finish, so
                        //      "%4p" against "(nil)" leaves the position at
                        //      nothing and answers a matching failure.
                        if (byte == '(' && (width == 0 || width >= 5))
                        {
                                //      glibc prints a null pointer as "(nil)"
                                //      and reads it back, so this does too.
                                //      It is a prefix like every other word
                                //      here: five bytes or nothing.
                                string_address word = (string_address) "(nil)";
                                positive matched_bytes = 1;
                                positive limit = width != 0 ? width
                                                            : ~(positive)0;

                                while (matched_bytes < 5 &&
                                       matched_bytes < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if ((b32)(p8)byte_to_lower(byte) !=
                                            (b32)word[matched_bytes])
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        matched_bytes++;
                                }

                                if (matched_bytes != 5)
                                        return scan_stop(source, mark,
                                                         assigned);

                                if (!suppress)
                                {
                                        address_to (address_any address_to)
                                            var_list_get(list, address_any) =
                                                null;
                                        assigned++;
                                }

                                break;
                        }

                        scan_unget(source, byte);

                        matched = scan_integer(source, address_of stage, width,
                                               16, address_of negative,
                                               address_of base_used);

                        if (!matched)
                                return scan_stop(source, mark, assigned);

                        magnitude = scan_magnitude(address_of stage, base_used,
                                                   address_of overflowed);
                        value = scan_unsigned_value(magnitude, negative,
                                                    overflowed);

                        if (!suppress)
                        {
                                address_to (address_any address_to)
                                    var_list_get(list, address_any) =
                                        (address_any)value;
                                assigned++;
                        }

                        break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'a':
                case 'A':
                {
                        address_any into = null;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        if (!scan_decimal_shape(source, address_of stage, width))
                                return scan_stop(source, mark, assigned);

                        stage.bytes[stage.staged] = end;

                        if (!suppress)
                                into = var_list_get(list, address_any);

                        if (length == SCAN_LENGTH_WIDE_DECIMAL)
                        {
                                scan_extended_decimal(
                                    (string_address)stage.bytes, into);
                        }
                        else if (length == SCAN_LENGTH_DECIMAL ||
                                 length == SCAN_LENGTH_WIDE)
                        {
                                decimal value = SCAN_DECIMAL(
                                    (string_address)stage.bytes, null);

                                if (into != null)
                                        address_to (f64 address_to)into = value;
                        }
                        else
                        {
                                scan_narrow_decimal(
                                    (string_address)stage.bytes, into);
                        }

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 'c':
                {
                        positive want = width != 0 ? width : 1;
                        positive got = 0;
                        p8 address_to into = null;

                        at++;

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        //      A string source hands the whole run over: how
                        //      much is there is one bounded string_length and
                        //      the move is one memory_copy.
                        if (scan_is_text(source))
                        {
                                got = string_length_max(source->text +
                                                            source->place,
                                                        want);

                                scan_text_take(source, into, got);

                                if (got < want)
                                        source->ended = true;
                        }
                        else
                        {
                                while (got < want)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got < want)
                                //      glibc keeps the bytes it did read and
                                //      still calls the short count an input
                                //      failure, which is measured.
                                return scan_leave(source, assigned, true);

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 's':
                {
                        positive limit = width != 0 ? width : ~(positive)0;
                        positive got = 0;
                        p8 address_to into = null;

                        at++;
                        scan_skip_white(source);

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        if (scan_is_text(source))
                        {
                                got = string_span_without_set(
                                    source->text + source->place,
                                    (string_address)scan_white);

                                if (got > limit)
                                        got = limit;

                                scan_text_take(source, into, got);
                        }
                        else
                        {
                                while (got < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (byte_is_space(byte))
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got == 0)
                                return scan_leave(source, assigned, true);

                        if (into != null)
                                into[got] = end;

                        if (!suppress)
                                assigned++;

                        break;
                }
                case '[':
                {
                        b8 set[256];
                        bool negated;
                        positive limit = width != 0 ? width : ~(positive)0;
                        positive got = 0;
                        p8 address_to into = null;
                        string_address after;

                        after = scan_build_set(at + 1, set, address_of negated);

                        if (is_null(after))
                                //      A format with no close bracket in it.
                                //      glibc stops and reports what it had,
                                //      and does not call it an input failure.
                                return scan_leave(source, assigned, false);

                        at = after;

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        if (scan_is_text(source))
                        {
                                //      The terminator ends a string source
                                //      whatever the set says, which is what
                                //      clearing this one entry buys: a
                                //      negated set holds the zero byte, and a
                                //      string has no zero byte in it to hold.
                                set[0] = 0;

                                got = string_span(source->text + source->place,
                                                  set);

                                if (got > limit)
                                        got = limit;

                                scan_text_take(source, into, got);

                                if (got == 0 &&
                                    string_get(source->text + source->place) ==
                                        end)
                                        source->ended = true;
                        }
                        else
                        {
                                while (got < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (!set[(p8)byte])
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got == 0)
                                return scan_leave(source, assigned,
                                                  source->ended);

                        if (into != null)
                                into[got] = end;

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 'n':
                        //      Not a conversion. It reads nothing, it cannot
                        //      fail, it never reaches the end of the input,
                        //      and it is not counted in the answer -- all
                        //      four of which are things a plausible scanf
                        //      gets wrong by treating it like the others.
                        at++;

                        if (!suppress)
                                scan_store_whole(var_list_get(list, address_any),
                                                 length, source->consumed);

                        break;
                default:
                        /*
                                An unknown conversion, or a percent that was
                                the last byte of the format. Either way the
                                call stops and answers with what it had
                                already assigned, and NOT with EOF even when
                                the input was empty: a format nobody can read
                                is not the input running out.

                                An unknown conversion still skips the white
                                space in front of it before giving up, which
                                is glibc's, and is visible: "%y" against three
                                spaces leaves the position at three and the
                                end-of-file indicator set. A percent at the
                                very end of the format skips nothing, because
                                there is no conversion there to have skipped
                                for.
                        */
                        if (string_get(at) != end)
                                scan_skip_white(source);

                        return scan_leave(source, assigned, false);
                }
        }

        return scan_leave(source, assigned, false);
}

/*
        THE STANDARD NAMES

        The same argument format.c makes for printf's: scanf is a fixed
        interface with a fixed signature that thirty years of C expects under
        that exact name, and a prose alias would be a second name for one
        thing. The prose name in this family is scan_run, which is the engine,
        and scan_source, which is where the bytes come from.
*/
static b32 vsscanf(string_address text, string_address format, var_args list)
{
        scan_source source;

        memory_zero(address_of source, sizeof(source));
        scan_errno_save(source.entered_errno);
        source.text = is_null(text) ? (string_address) "" : text;

        return scan_run(address_of source, format, list);
}

var_list_entry(sscanf, b32,
               (string_address text, string_address format, ...), format,
               vsscanf(text, format, _variadic_list))

#if SCAN_STREAMS

static b32 vfscanf(scan_stream handle, string_address format, var_args list)
{
        scan_source source;

        if (is_null(handle))
                return EOF;

        memory_zero(address_of source, sizeof(source));
        scan_errno_save(source.entered_errno);
        source.handle = handle;

        return scan_run(address_of source, format, list);
}

static b32 vscanf(string_address format, var_args list)
{
        return vfscanf(stdin, format, list);
}

var_list_entry(fscanf, b32,
               (scan_stream handle, string_address format, ...), format,
               vfscanf(handle, format, _variadic_list))
var_list_entry(scanf, b32, (string_address format, ...), format,
               vfscanf(stdin, format, _variadic_list))

#endif // SCAN_STREAMS

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SCAN
#endif // STANDARD_SKIP_SCAN

#ifndef STANDARD_SKIP_SPOOL
/* ---- spool.c ---- */
/*
        Experimental C standard library

        spool: the rest of <stdio.h> -- a stream with something behind it the
        stream family does not own. A child process, a temporary name, a
        buffer of bytes, or a file that is about to stop existing.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SPOOL
#define STANDARD_MODERN_C_STANDARD_SPOOL

/*
        What is here and why it is not in stream.c.

        stream.c is a buffer and a descriptor and the state machine between
        them, and every line of it is about that. Nothing in it forks, nothing
        in it invents a filename, and nothing in it knows there is a shell on
        this system. The entries below all do exactly one of those things and
        then hand the result to stream.c, so keeping them apart keeps the file
        that everybody's printf goes through free of process management.

        The order is remove, then the temporary names, then popen and pclose,
        then the small stream entries that had nowhere else to live, then the
        unlocked spellings.

        Two names this family deliberately does not define, because a sibling
        already has them and a second definition would not link: rename is
        error.c line 1517, and setbuf is stream.c line 1728. The shell path
        is stdlib.c's STDLIB_SHELL rather than a second "/bin/sh" written out
        here, so that a system that moves its shell moves it once.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        remove, which is unlink and then rmdir.

        C says remove deletes a file and leaves it implementation-defined
        whether it can delete a directory; POSIX says it must, by being unlink
        for everything that is not a directory and rmdir for one that is.
        There is no syscall that is both, so the question is which one to try
        first, and the answer is unlink -- it is the case that happens, and it
        answers EISDIR for the case that does not, which is a cheap and exact
        signal. Asking stat first would be a second syscall on every call and
        a race besides.

        errno after a failure is whichever call failed last: a missing path
        reports ENOENT from unlink, a non-empty directory reports ENOTEMPTY
        from rmdir. That second one only happens because Linux answers unlink
        on a directory with EISDIR specifically. A kernel that answered EPERM
        instead -- which some do, and which is what the standard permits --
        would never reach rmdir at all and a directory would be unremovable.
        This is the same behaviour glibc has on Linux for the same reason, so
        it is a shared limit rather than a divergence, and widening the test
        to EPERM as well would diverge from the reference on the case where
        EPERM is the true answer.
*/
static b32 spool_remove_path(string_address path)
{
        b32 answer = unlink(path);

        if (answer == 0)
                return 0;

        if (errno == EISDIR)
                return rmdir(path);

        return answer;
}

/*
        Where a temporary file goes, and how many distinct names there are.

        P_tmpdir and TMP_MAX are the two numbers <stdio.h> is required to
        publish. TMP_MAX is 238328, which is 62 cubed and is the count glibc
        publishes, and it is the number of names a six-X template can hold if
        the alphabet is the sixty-two alphanumerics. The alphabet below is
        sixty-four rather than sixty-two, for the reason written above
        spool_name_table, so the real count is larger and TMP_MAX is a floor
        on it rather than an exact figure. A floor is what the standard asks
        for.
*/
#define P_tmpdir "/tmp"
#define TMP_MAX 238328

//      L_tmpnam is the buffer size tmpnam demands of a caller who supplies
//      one: "/tmp/" is five, the prefix "tmp" is three, six X's, and the
//      terminator. Twenty is that with room, and is what glibc publishes.
#define L_tmpnam 20

#define SPOOL_TEMPORARY_PREFIX P_tmpdir "/tmp"
#define SPOOL_TEMPORARY_TEMPLATE SPOOL_TEMPORARY_PREFIX "XXXXXX"

//      The six bytes a template ends in, and the mode a temporary file is
//      created with. 0600 and not 0666: a file whose name was picked to be
//      unguessable should not be readable by everybody the moment it exists.
#define SPOOL_TEMPLATE_MARKS 6
#define SPOOL_TEMPORARY_MODE 0600

/*
        The alphabet, written four times, which is the whole of the random
        name generation.

        A random name is six bytes drawn from an alphabet, and the naive way
        to write it is a loop with letters[byte % 62] in it. Two things are
        wrong with that. It is a hand-rolled byte loop where library.c has
        memory_translate, which is a vectorised table walk that does four
        bytes a round; and the modulus is biased, because 256 is not a
        multiple of 62 and the first eight letters come up one time in
        thirty-two more often than the rest.

        Both go away at once by choosing an alphabet of sixty-four and writing
        it out four times. 64 divides 256 exactly, so table[byte] is
        alphabet[byte & 63] with no bias at all and no arithmetic; and a
        256-byte table is precisely what memory_translate takes. The six
        random bytes go in and six random letters come out, in one call, with
        no loop in this file.

        The two extra characters are underscore and hyphen, both legal in
        every filesystem this runs on and neither of them shell-special in a
        way that matters for a name that is never passed to a shell. A hyphen
        can never lead, because the name always begins with the prefix.
*/
#define SPOOL_ALPHABET                                   \
        "abcdefghijklmnopqrstuvwxyz"                     \
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"                     \
        "0123456789_-"

//      The bound is left for the compiler to count rather than written as
//      256. The four repetitions are exactly 256 bytes and the table is only
//      ever indexed with a byte, so the size was right; what it was not is a
//      string, and saying 256 told gcc to drop the terminator it would
//      otherwise have written. That is a diagnostic under
//      -Wunterminated-string-initialization, which the terminal harness
//      builds with, and the whole tree stopped compiling in that lane for a
//      byte nothing reads. Now it is 257 bytes, one of them a terminator
//      nobody looks at, and every index below is unchanged.
static const p8 spool_name_table[] = SPOOL_ALPHABET SPOOL_ALPHABET
        SPOOL_ALPHABET SPOOL_ALPHABET;

/*
        Six random bytes, from the kernel where it will give them and from the
        clock where it will not.

        getrandom with GRND_NONBLOCK is the right primitive and the flag is
        the important half: without it the call blocks until the entropy pool
        is initialised, which on a machine that has just booted -- which is
        exactly when init and the shell are calling this -- can be seconds.
        src/net/dhcp.c has the same note beside the same syscall and reached
        the same conclusion.

        When it refuses, the fallback is not pretending to be random. It is a
        monotonic clock's nanoseconds, the thread id, and a counter that moves
        on every call, stirred by a multiply-shift so that adjacent
        nanosecond readings do not produce adjacent names. That is enough to
        make a collision unlikely; it is emphatically not enough to make a
        name unguessable, and no caller should treat mkstemp as a security
        boundary on a kernel that has no entropy. What makes mkstemp safe is
        O_EXCL, not the name.
*/
#define SPOOL_RANDOM_NONBLOCK 1
#define SPOOL_CLOCK_MONOTONIC 1

static positive spool_name_counter = 0;

static fn spool_random_bytes(p8 address_to into, positive size)
{
        positive when[2] = {0, 0};
        positive mixed;
        positive index = 0;

        if ((positive)system_call_3(syscall(getrandom), (positive)into, size,
                                    SPOOL_RANDOM_NONBLOCK) == size)
                return;

        system_call_2(syscall(clock_gettime), SPOOL_CLOCK_MONOTONIC,
                      (positive)address_of when);

        spool_name_counter++;

        mixed = when[1] ^ (when[0] << 20) ^
                ((positive)system_call(syscall(gettid)) << 40) ^
                (spool_name_counter * 0x9e3779b97f4a7c15ULL);

        while (index < size)
        {
                mixed ^= mixed >> 33;
                mixed *= 0xff51afd7ed558ccdULL;
                mixed ^= mixed >> 29;

                into[index] = (p8)mixed;
                index++;
        }
}

//      Six random letters into the six bytes a template's X's occupy. One
//      random draw and one memory_translate; see spool_name_table for why
//      there is no loop and no modulus.
static fn spool_name_marks(p8 address_to marks)
{
        spool_random_bytes(marks, SPOOL_TEMPLATE_MARKS);
        memory_translate(marks, SPOOL_TEMPLATE_MARKS,
                         (address_any)spool_name_table);
}

/*
        Where a template's X's are, or -1 with errno set.

        POSIX requires the six X's be the last six characters for mkstemp and
        allows a fixed suffix after them for mkstemps, so the search is
        arithmetic from the end rather than a scan: the caller has told us
        exactly how many bytes follow. string_length and memory_compare are
        library.c's, and memory_compare with a literal six folds to a
        four-byte and a two-byte compare at the call site.
*/
static bipolar spool_template_marks(string_address form, positive suffix)
{
        positive length;

        if (is_null(form))
        {
                errno = EINVAL;
                return -1;
        }

        length = string_length(form);

        if (length < SPOOL_TEMPLATE_MARKS + suffix)
        {
                errno = EINVAL;
                return -1;
        }

        length -= SPOOL_TEMPLATE_MARKS + suffix;

        if (memory_compare((address_any)(form + length), (address_any) "XXXXXX",
                           SPOOL_TEMPLATE_MARKS) != 0)
        {
                errno = EINVAL;
                return -1;
        }

        return (bipolar)length;
}

/*
        The one loop every mkstemp-shaped entry shares.

        Draw six letters, try to create, and on EEXIST draw again. Everything
        else about the four public spellings is arguments: mkdtemp asks for a
        directory instead of a file, mkostemp adds caller flags, mkstemps
        allows a suffix.

        The bound is TMP_MAX because that is the number the standard names,
        but it is not really a bound on work: each attempt is an independent
        draw from an alphabet with 64^6 names in it, so the loop runs once
        unless the directory is astonishingly full. What actually stops a
        broken call quickly is the second test -- any failure that is not
        EEXIST is a real failure and returns at once. Without that, mkstemp in
        an unwritable directory would make two hundred thousand syscalls
        before reporting EACCES.

        O_EXCL is what makes this safe, and it is not optional: it is the
        kernel promising that exactly one of any number of racing creators
        wins. The unguessability of the name is a second line of defence and
        not the first.
*/
static b32 spool_temporary_make(string_address form, positive suffix,
                                b32 flags, bool directory)
{
        bipolar at = spool_template_marks(form, suffix);
        positive attempt = 0;

        if (at < 0)
                return -1;

        while (attempt < TMP_MAX)
        {
                b32 answer;

                spool_name_marks((p8 address_to)form + at);

                if (directory)
                        answer = mkdir(form, 0700);
                else
                        answer = open(form,
                                      O_RDWR | O_CREAT | O_EXCL | flags,
                                      SPOOL_TEMPORARY_MODE);

                if (answer >= 0)
                        return answer;

                if (errno != EEXIST)
                        return -1;

                attempt++;
        }

        errno = EEXIST;
        return -1;
}

//      The four spellings, each one argument away from the loop above.
//      mkdtemp returns the template rather than a descriptor, which is the
//      one of the four that is not a file.
static b32 spool_temporary_open(string_address form)
{
        return spool_temporary_make(form, 0, 0, false);
}

static b32 spool_temporary_open_flagged(string_address form, b32 flags)
{
        return spool_temporary_make(form, 0, flags, false);
}

static b32 spool_temporary_open_suffixed(string_address form, b32 suffix)
{
        if (suffix < 0)
        {
                errno = EINVAL;
                return -1;
        }

        return spool_temporary_make(form, (positive)suffix, 0, false);
}

static string_address spool_temporary_directory(string_address form)
{
        if (spool_temporary_make(form, 0, 0, true) < 0)
                return null;

        return form;
}

/*
        tmpnam, which is the entry in this file that cannot be made good.

        It returns a name that does not exist and then returns, and between
        the returning and the caller's open anybody may create that name. The
        standard knows this; every compiler on earth warns about it; the
        function is still in <stdio.h> and old code still calls it, so it is
        here and it is written to be as close to harmless as it can be: the
        name is drawn from the same unguessable alphabet mkstemp uses, and it
        is checked for non-existence with faccessat rather than assumed.

        A caller who can change is asking for mkstemp, which hands back a
        descriptor to a file it created and never lets a name exist without
        one.

        With a null argument it answers into a static buffer, which is what
        the standard defines and is the other reason the function is bad: two
        calls return the same pointer and the first answer is gone.
*/
static p8 spool_name_storage[L_tmpnam];

static string_address spool_temporary_name(string_address into)
{
        string_address answer = is_null(into)
                                        ? (string_address)spool_name_storage
                                        : into;
        positive attempt = 0;

        while (attempt < TMP_MAX)
        {
                memory_copy((address_any)answer,
                            (address_any)SPOOL_TEMPORARY_TEMPLATE,
                            sizeof(SPOOL_TEMPORARY_TEMPLATE));

                spool_name_marks((p8 address_to)answer +
                                 sizeof(SPOOL_TEMPORARY_TEMPLATE) - 1 -
                                 SPOOL_TEMPLATE_MARKS);

                if (faccessat(AT_FDCWD, answer, F_OK, 0) < 0 &&
                    errno == ENOENT)
                        return answer;

                attempt++;
        }

        return null;
}

/*
        tempnam, tmpnam's other half: a caller-chosen directory and prefix,
        and an answer the caller must free.

        The directory is the first of TMPDIR, the argument, P_tmpdir that is
        writable, which is the order glibc uses. The prefix is at most five
        bytes, also glibc's rule, and the reason is that the whole answer has
        to stay under the historical name limit.

        It has tmpnam's race and adds an allocation to it. It is here because
        old code calls it; new code should not.
*/
#define SPOOL_PREFIX_MAX 5
#define SPOOL_NAME_MAX 255

static string_address spool_temporary_named(string_address directory,
                                            string_address prefix)
{
        string_address chosen = getenv((string_address) "TMPDIR");
        p8 address_to answer;
        positive length;
        positive at;

        if (is_null(chosen) || faccessat(AT_FDCWD, chosen, W_OK, 0) < 0)
                chosen = directory;

        if (is_null(chosen) || faccessat(AT_FDCWD, chosen, W_OK, 0) < 0)
                chosen = (string_address)P_tmpdir;

        length = string_length(chosen);

        if (is_null(prefix))
                prefix = (string_address) "tmp";

        at = string_length(prefix);

        if (at > SPOOL_PREFIX_MAX)
                at = SPOOL_PREFIX_MAX;

        if (length + 1 + at + SPOOL_TEMPLATE_MARKS + 1 > SPOOL_NAME_MAX)
        {
                errno = ENAMETOOLONG;
                return null;
        }

        answer = (p8 address_to)malloc(length + 1 + at +
                                       SPOOL_TEMPLATE_MARKS + 1);

        if (is_null(answer))
        {
                errno = ENOMEM;
                return null;
        }

        memory_copy((address_any)answer, (address_any)chosen, length);
        answer[length] = '/';
        memory_copy((address_any)(answer + length + 1), (address_any)prefix, at);
        memory_fill((address_any)(answer + length + 1 + at), 'X',
                    SPOOL_TEMPLATE_MARKS);
        answer[length + 1 + at + SPOOL_TEMPLATE_MARKS] = end;

        spool_name_marks(answer + length + 1 + at);

        return (string_address)answer;
}

/*
        tmpfile: a stream on a file with no name, deleted when the program
        ends whether or not it ends well.

        Two ways to get one, and the first is strictly better. O_TMPFILE
        creates an inode in a directory without ever linking a name to it, so
        there is no window in which a name exists, nothing to clean up, and
        nothing for another process to open. It arrived in Linux 3.11 and not
        every filesystem implements it -- tmpfs and ext4 do, some do not --
        which is why the answer is tested rather than assumed.

        The fallback is the historical one: mkstemp a name, unlink it
        immediately, and keep the descriptor. The file survives on the open
        descriptor alone and vanishes when it closes. The window between
        create and unlink is small and O_EXCL makes it harmless, but it exists,
        which is why it is second.

        O_TMPFILE is the create bit or-ed with O_DIRECTORY, and O_DIRECTORY is
        the one open flag whose value differs across these three targets --
        library.c line 12598 has 040000 for one and 0200000 for the others,
        with the reason. Naming both here rather than a literal is what keeps
        that difference in one place.
*/
#define SPOOL_O_TMPFILE (O_TMPFILE_CREATE | O_DIRECTORY)

static stream address_to spool_temporary_stream(void)
{
        p8 form[sizeof(SPOOL_TEMPORARY_TEMPLATE)];
        stream address_to answer;
        b32 handle;

        handle = open((string_address)P_tmpdir,
                      O_RDWR | SPOOL_O_TMPFILE, SPOOL_TEMPORARY_MODE);

        if (handle < 0)
        {
                memory_copy((address_any)form,
                            (address_any)SPOOL_TEMPORARY_TEMPLATE,
                            sizeof(SPOOL_TEMPORARY_TEMPLATE));

                handle = spool_temporary_open((string_address)form);

                if (handle < 0)
                        return null;

                unlink((string_address)form);
        }

        answer = stream_adopt(handle, (string_address) "w+");

        if (is_null(answer))
        {
                close(handle);
                return null;
        }

        return answer;
}

/*
        popen and pclose, which are the substance of this file.

        WHAT IS ACTUALLY BUILT

        A pipe, a child, and a FILE on this end of it. For "r" the child's
        standard output is the pipe's write end and the caller reads; for "w"
        the child's standard input is the read end and the caller writes.
        Exactly one end survives in each process, which matters more than it
        looks: a reader that leaves the write end open in its own process
        never sees end of file, because the kernel counts writers and it is
        one of them.

        WHAT IS EXECUTED

        stdlib.c's STDLIB_SHELL, which is "/bin/sh", with -c and the command,
        through execve with this process's environment. That is the same path
        system() takes, deliberately: one spelling of the shell in the tree.
        On this distribution the file at that path is programs/shell.c built
        as sh, which handles -c and is what the test lane runs against.

        WHEN THE SHELL IS NOT THERE

        popen still succeeds. It has to: the fork happened, the pipe exists,
        and by the time execve fails the parent has already returned. The
        child's execve returns, the child leaves with 127 -- the number a
        shell itself uses for a command it could not find, and what POSIX
        requires here -- and the caller sees an immediate end of file on a
        read stream, or EPIPE and SIGPIPE on a write stream. pclose then
        answers a raw status whose exit code is 127. A caller who wants to
        know before spending a fork should call system(null), which asks
        exactly that question and is in stdlib.c.

        THE SIDE TABLE

        struct stream has no field for a child, and adding one would put eight
        bytes into every FILE in the program for a feature almost none of them
        use. So the pairing lives here, in a fixed array. Thirty-two is more
        concurrent pipelines than anything in this tree opens and the array
        costs 512 bytes of .bss; a caller who exhausts it gets EMFILE, which
        is the honest errno for "no more of this resource".

        The table is also what makes the child's cleanup possible. glibc
        closes every other popen'd descriptor in a new child, and so does
        this, for a reason that is a real bug otherwise: a program with two
        pipelines open, where the second child inherits the first child's read
        end, keeps that pipe's writer count above zero forever, and the first
        pipeline never reports end of file. Closing them is one walk of the
        table between the fork and the exec.

        WHAT IS MISSING AGAINST POSIX

        The same thing system() is missing and for the same reason: signal
        handling. POSIX says nothing about popen and SIGINT, but it does say
        pclose must not return until the child is collected, and a caller who
        has installed a SIGCHLD handler that reaps everything will find the
        child already gone and get -1/ECHILD here. That is a real divergence
        from nothing -- glibc has it too -- and it is written down rather than
        worked around.
*/
#define SPOOL_CHILDREN 32

typedef struct
{
        stream address_to handle;
        b32 child;
} spool_pipeline;

static spool_pipeline spool_pipeline_table[SPOOL_CHILDREN];

//      A free slot, or -1. The array is small and walked twice in a
//      pipeline's life, which is cheaper than any structure that would avoid
//      the walk.
#define SPOOL_PIPELINE_SLOT(name, parameters, matches)                       \
static bipolar name parameters                                              \
{                                                                           \
        positive at = 0;                                                    \
        while (at < SPOOL_CHILDREN)                                         \
        {                                                                   \
                if (matches)                                                \
                        return (bipolar)at;                                 \
                at++;                                                       \
        }                                                                   \
        return -1;                                                          \
}
SPOOL_PIPELINE_SLOT(spool_pipeline_free_slot, (void),
                    is_null(spool_pipeline_table[at].handle))
SPOOL_PIPELINE_SLOT(spool_pipeline_slot_of, (stream address_to handle),
                    spool_pipeline_table[at].handle == handle)
#undef SPOOL_PIPELINE_SLOT

//      Run in the child, between the fork and the exec: every pipe belonging
//      to a pipeline this process opened earlier is not this child's business.
//      See the note above about a writer count that never reaches zero.
static fn spool_pipeline_close_others(void)
{
        positive at = 0;

        while (at < SPOOL_CHILDREN)
        {
                if (!is_null(spool_pipeline_table[at].handle))
                        close(stream_descriptor(spool_pipeline_table[at].handle));

                at++;
        }
}

static stream address_to spool_open_process(string_address command,
                                            string_address mode)
{
        string_address words[4];
        b32 pair[2];
        bipolar slot;
        b32 child;
        bool reading;
        b32 pipe_flags = 0;
        positive at;
        stream address_to answer;

        if (is_null(command) || is_null(mode))
        {
                errno = EINVAL;
                return null;
        }

        if (mode[0] == 'r')
                reading = true;
        else if (mode[0] == 'w')
                reading = false;
        else
        {
                errno = EINVAL;
                return null;
        }

        //      The only modifier POSIX 2008 gives popen is 'e', which asks
        //      that the caller's end not survive an exec. Anything else is a
        //      caller who thinks this is fopen.
        at = 1;

        while (mode[at] != end)
        {
                if (mode[at] != 'e')
                {
                        errno = EINVAL;
                        return null;
                }

                pipe_flags = O_CLOEXEC;
                at++;
        }

        slot = spool_pipeline_free_slot();

        if (slot < 0)
        {
                errno = EMFILE;
                return null;
        }

        if (pipe2(pair, pipe_flags) < 0)
                return null;

        /*
                Everything this process has buffered belongs to this process,
                and there are two layers of it here where system() only had
                one. log is library.c's own buffered writer and log_flush
                empties it; the FILE buffers are stream.c's and stream_flush
                with a null argument empties all of them. A child that
                inherits either would write the parent's pending bytes a
                second time. stdlib.c's system() flushes only the first of the
                two, which is a real gap in that function and is noted here
                rather than fixed from a different file.
        */
        log_flush();
        stream_flush(null);

        child = fork();

        if (child < 0)
        {
                close(pair[0]);
                close(pair[1]);
                return null;
        }

        if (child == 0)
        {
                b32 theirs = reading ? pair[1] : pair[0];
                b32 ours = reading ? pair[0] : pair[1];
                b32 wanted = reading ? standard_output_descriptor
                                     : standard_input_descriptor;

                //      The other pipelines go first, before anything is
                //      moved onto descriptor zero or one. Doing it after the
                //      dup2 also works today, but only because no popen'd
                //      stream can be holding descriptor zero or one while
                //      this process still has its own standard streams open
                //      -- an invariant a caller breaks by closing stdout and
                //      then opening two pipelines, at which point the walk
                //      would close the descriptor the dup2 had just
                //      installed. glibc closes them first for this reason and
                //      so does this: then the invariant is not needed at all.
                spool_pipeline_close_others();

                close(ours);

                //      dup2 clears close-on-exec on the new descriptor, which
                //      is what lets the 'e' flag be set on both ends of the
                //      pipe and still leave the child with a usable one.
                if (theirs != wanted)
                {
                        if (dup2(theirs, wanted) < 0)
                                _exit(127);
                        close(theirs);
                }
                else if (pipe_flags &&
                         system_call_3(syscall(fcntl), (positive)theirs, 2, 0) < 0)
                        _exit(127);

                words[0] = (string_address)STDLIB_SHELL;
                words[1] = (string_address) "-c";
                words[2] = command;
                words[3] = null;

                execve(words[0], words, stdlib_environment_list());

                //      execve only returns when it failed, and 127 is what a
                //      shell answers for a command it could not run.
                _exit(127);
        }

        close(reading ? pair[1] : pair[0]);

        answer = stream_adopt(reading ? pair[0] : pair[1],
                              reading ? (string_address) "r"
                                      : (string_address) "w");

        if (is_null(answer))
        {
                positive raw = 0;

                close(reading ? pair[0] : pair[1]);
                system_wait4_retry(child, address_of raw, 0, null);
                errno = ENOMEM;
                return null;
        }

        spool_pipeline_table[slot].handle = answer;
        spool_pipeline_table[slot].child = child;

        return answer;
}

/*
        pclose: close this end, wait, and hand back the raw status.

        The raw status and not the exit code, which is the part callers get
        wrong. POSIX says pclose returns the termination status "as returned
        by waitpid", so a child that exited 3 gives 768 and not 3, and a
        caller wanting the number a shell would print passes it through
        library.c's wait_status_code. glibc does the same and this matches it
        byte for byte in the diff.

        The wait retries EINTR, through library.c's system_wait4_retry rather
        than error.c's deliberately non-retrying waitpid. That is the opposite
        choice from the one error.c made and it is right here for the opposite
        reason: error.c's waitpid is a thin wrapper whose caller asked for a
        wait and is entitled to be interrupted out of it, while pclose has
        already closed the stream and has nothing sensible to return if it
        gives up. glibc loops here too.

        The slot is released before the wait, not after, so that a stream is
        never in the table with a child that has been collected -- a second
        pclose on the same pointer answers -1/ECHILD rather than waiting on a
        pid that may by then belong to somebody else.
*/
static b32 spool_close_process(stream address_to handle)
{
        bipolar slot = spool_pipeline_slot_of(handle);
        positive raw = 0;
        b32 child;

        if (is_null(handle) || slot < 0)
        {
                errno = ECHILD;
                return -1;
        }

        child = spool_pipeline_table[slot].child;
        spool_pipeline_table[slot].handle = null;
        spool_pipeline_table[slot].child = -1;

        stream_close(handle);

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
        {
                errno = ECHILD;
                return -1;
        }

        return (b32)raw;
}

/*
        The three small stream entries that had nowhere else to live.

        setlinebuf is setvbuf's line mode with the library picking the size,
        and setbuffer is the BSD spelling that takes a size where setbuf does
        not. Both are one call, and both are here rather than in stream.c only
        because stream.c had already closed its list of aliases; a future edit
        that moves them there loses nothing.

        __fpurge is the interesting one. It throws away everything a stream is
        holding in either direction without writing any of it, which is what a
        forked child wants before it execs something -- the child inherits the
        parent's buffered bytes and must not emit them -- and what a program
        that has decided its output does not matter any more wants. It is
        deliberately not fflush: fflush writes the bytes out, this drops them.
        Both spellings are given, glibc's void-returning __fpurge and BSD's
        int-returning fpurge, because code in the wild calls both.

        Dropping the state is four assignments and not a call to anything,
        because there is no library routine for "forget", and the fields are
        reachable from here for the reason the file header gives: this is the
        same translation unit as stream.c.
*/
static fn spool_set_line_buffered(stream address_to handle)
{
        stream_set_buffering(handle, null, _IOLBF, STREAM_DYNAMIC_BUFFER);
}

static fn spool_set_buffer_sized(stream address_to handle,
                                 string_address buffer, sized size)
{
        stream_set_buffering(handle, buffer,
                             is_null(buffer) ? _IONBF : _IOFBF, size);
}

static fn spool_purge(stream address_to handle)
{
        if (is_null(handle))
                return;

        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
        handle->pushback_used = 0;
}

/*
        fgetpos and fsetpos, and what fpos_t has to be.

        The standard is careful never to say fpos_t is an integer. It is an
        opaque object that records everything needed to restore a position,
        which on a library with multibyte state would include the conversion
        state as well as the offset. There is no such state here -- this
        library has no mbstate and every stream is bytes -- so the object is
        the offset and nothing else, and it is a struct rather than a typedef
        of bipolar precisely so that a caller cannot do arithmetic on it and
        then be surprised when a future version has two fields.

        The field is prefixed like everything else in this family, which is
        invisible to a conforming caller: the standard gives no name to any
        member of fpos_t, so there is nothing to collide with.
*/
typedef struct
{
        bipolar spool_offset;
} fpos_t;

static b32 spool_get_position(stream address_to handle,
                              fpos_t address_to into)
{
        bipolar where;

        if (is_null(into))
        {
                errno = EINVAL;
                return -1;
        }

        where = stream_tell(handle);

        if (where < 0)
                return -1;

        into->spool_offset = where;

        return 0;
}

static b32 spool_set_position(stream address_to handle,
                              const fpos_t address_to from)
{
        if (is_null(from))
        {
                errno = EINVAL;
                return -1;
        }

        return stream_seek(handle, from->spool_offset, SEEK_SET);
}

/*
        fmemopen, for reading, and why writing is refused rather than
        approximated.

        Every stream in this library bottoms out at a descriptor. There is no
        vtable in struct stream, no per-stream read and write function
        pointers, and adding them would put two words in every FILE and a
        pair of indirect calls in the hot path of every getc -- to serve a
        function most programs never call. So the way to make a stream out of
        a block of memory without touching stream.c is to give the kernel the
        memory and take a descriptor back, and memfd_create does exactly that:
        an anonymous file that lives in tmpfs, with no name in any directory
        and no cleanup.

        For reading that is exactly right. The bytes are copied in, the offset
        is rewound, and everything stream.c does -- refill, seek, ungetc,
        getline -- works with no special case anywhere.

        For writing it would be exactly wrong, and quietly so. fmemopen's
        write mode promises that the caller's own array receives the bytes and
        that a NUL is maintained after them; a memfd receives them instead and
        the caller's array is never touched. A program would run, produce no
        error, and read back whatever was in its buffer before. That is worse
        than not having the function, so a write mode is refused with EINVAL
        and the reason is this paragraph. Doing it properly means a stream
        that can be flushed into a caller's buffer, which is the same hook
        open_memstream needs; see the note where that one is not defined.

        A kernel without memfd_create -- before 3.17 -- answers ENOSYS and
        this returns null with that errno, which is a true statement about the
        machine.
*/
#define SPOOL_MEMFD_CLOEXEC 1

static stream address_to spool_open_memory(address_any bytes, sized size,
                                           string_address mode)
{
        stream address_to answer;
        bipolar handle;
        bipolar written;

        b32 open_flags;
        p32 stream_flags;
        if (is_null(bytes) ||
            !stream_read_mode(mode, address_of open_flags, address_of stream_flags) ||
            stream_flags != STREAM_READABLE)
        {
                errno = EINVAL;
                return null;
        }

        handle = system_call_2(syscall(memfd_create),
                               (positive)(address_any) "fmemopen",
                               SPOOL_MEMFD_CLOEXEC);

        if (handle < 0)
        {
                errno = (b32) - handle;
                return null;
        }

        written = stream_trap_write((b32)handle, bytes, (positive)size);

        if ((positive)written != (positive)size)
        {
                b32 saved = errno;
                close((b32)handle);
                errno = saved;
                return null;
        }

        if (lseek((b32)handle, 0, SEEK_SET) < 0)
        {
                close((b32)handle);
                return null;
        }

        answer = stream_adopt((b32)handle, (string_address) "r");

        if (is_null(answer))
        {
                close((b32)handle);
                errno = ENOMEM;
                return null;
        }

        return answer;
}

/*
        The names <stdio.h> and <stdlib.h> know these by.

        Static aliases put both spellings on the same body. Section collection
        still removes an entry a program never calls, while an address taken
        through either spelling remains the address of the same operation.
*/
static b32 remove(string_address) __attribute__((alias("spool_remove_path")));
static b32 mkstemp(string_address)
        __attribute__((alias("spool_temporary_open")));
static b32 mkostemp(string_address, b32)
        __attribute__((alias("spool_temporary_open_flagged")));
static b32 mkstemps(string_address, b32)
        __attribute__((alias("spool_temporary_open_suffixed")));
static string_address mkdtemp(string_address)
        __attribute__((alias("spool_temporary_directory")));
static string_address tmpnam(string_address)
        __attribute__((alias("spool_temporary_name")));
static string_address tempnam(string_address, string_address)
        __attribute__((alias("spool_temporary_named")));
static stream address_to tmpfile(void)
        __attribute__((alias("spool_temporary_stream")));
static stream address_to popen(string_address, string_address)
        __attribute__((alias("spool_open_process")));
static b32 pclose(stream address_to)
        __attribute__((alias("spool_close_process")));
static fn setlinebuf(stream address_to)
        __attribute__((alias("spool_set_line_buffered")));
static fn setbuffer(stream address_to, string_address, sized)
        __attribute__((alias("spool_set_buffer_sized")));
static fn __fpurge(stream address_to)
        __attribute__((alias("spool_purge")));

static b32 fpurge(stream address_to handle)
{
        if (is_null(handle))
                return -1;

        spool_purge(handle);

        return 0;
}

static b32 fgetpos(stream address_to, fpos_t address_to)
        __attribute__((alias("spool_get_position")));
static b32 fsetpos(stream address_to, const fpos_t address_to)
        __attribute__((alias("spool_set_position")));
static stream address_to fmemopen(address_any, sized, string_address)
        __attribute__((alias("spool_open_memory")));

/*
        The _unlocked family, which in a single-threaded library is the same
        family with a different spelling.

        There is no per-call stream lock, so the matching signatures below
        are aliases of the plain entries. If stream.c gains locking, these
        declarations become the lock-free bodies instead.

        flockfile and funlockfile are consequently no-ops, and ftrylockfile
        succeeds: in a single-threaded library there is no contention.
*/
static fn flockfile(stream address_to handle)
{
        (void)handle;
}

static fn funlockfile(stream address_to handle)
        __attribute__((alias("flockfile")));

static b32 ftrylockfile(stream address_to handle)
{
        (void)handle;

        return 0;
}

static b32 getc_unlocked(stream address_to handle)
        __attribute__((alias("stream_get_byte")));
static b32 fgetc_unlocked(stream address_to handle)
        __attribute__((alias("stream_get_byte")));
static b32 getchar_unlocked(void)
        __attribute__((alias("stream_get_byte_standard")));
static b32 putc_unlocked(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte")));
static b32 fputc_unlocked(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte")));

static b32 putchar_unlocked(b32 byte)
{
        return stream_put_byte(byte, stdout);
}

static b32 fputs_unlocked(string_address text, stream address_to handle)
        __attribute__((alias("stream_put_string")));
static string_address fgets_unlocked(string_address into, b32 limit,
                                     stream address_to handle)
        __attribute__((alias("stream_get_line")));
static sized fread_unlocked(address_any into, sized size, sized count,
                            stream address_to handle)
        __attribute__((alias("stream_read")));
static sized fwrite_unlocked(address_any from, sized size, sized count,
                             stream address_to handle)
        __attribute__((alias("stream_write")));
static b32 fflush_unlocked(stream address_to handle)
        __attribute__((alias("stream_flush")));

static PURE b32 feof_unlocked(stream address_to handle)
        __attribute__((alias("stream_at_end")));
static PURE b32 ferror_unlocked(stream address_to handle)
        __attribute__((alias("stream_failed")));
static fn clearerr_unlocked(stream address_to handle)
        __attribute__((alias("stream_clear_state")));
static PURE b32 fileno_unlocked(stream address_to handle)
        __attribute__((alias("stream_descriptor")));

/*
        open_memstream is deliberately not here.

        It is the one entry on this family's list that cannot be built without
        changing stream.c, and building it wrong is worse than not building
        it. What it promises is a stream whose bytes accumulate in an
        allocation the library grows, with the caller's pointer and length
        variables updated to match every time the stream is flushed or closed.
        The updating is the part that has no home: it is a hook that runs
        inside stream_flush_output, and struct stream has no field to hang one
        on and no code path that would call it.

        Faking it with a memfd, the way fmemopen above is built, fails at
        exactly the promise: the caller's pointer would be updated only at
        close, so a program that flushes and reads the buffer -- which is the
        normal way the function is used -- would read nothing.

        The honest version is three things together: one function pointer in
        struct stream called on flush, one on close, and open_memstream
        supplying both. That is a small edit to stream.c and it would also be
        what makes fmemopen's write modes work, and it should be one change
        rather than two. Until then this family says no and says why.
*/

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SPOOL
#endif // STANDARD_SKIP_SPOOL

#ifndef STANDARD_SKIP_PROCESS
/* ---- process.c ---- */
/*
        Experimental C standard library

        directories, the exec family, sleeping, and the small POSIX names
        that sit beside them

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_PROCESS
#define STANDARD_MODERN_C_STANDARD_PROCESS

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason src/standard/error.c states at the same place: core.c
        includes this umbrella, library.c sets KERNEL_MODE from __MODULE__,
        and a module that pulled a second struct dirent, a second DIR and a
        second nanosleep in beside the ones <linux/...> already declares would
        not compile. The three families that shipped without this guard were
        each right on their own and wrong together.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        Where this file sits, and what it is allowed to assume.

        It is included last in src/compiler_memory.c, after error.c,
        allocator.c, text.c, stdlib.c, clock.c, math.c, stream.c and format.c,
        and it depends on all but two of them: getdents64, openat, close,
        lseek, readlink, lstat, getcwd, execve and errno itself come from
        error.c, malloc and free from allocator.c, qsort and abort from
        stdlib.c, CLOCK_REALTIME from clock.c, and fdopen from stream.c.
        Placed any earlier it would see a different errno -- error.c does
        `#undef errno` and then defines it as a call to __errno_location --
        and every wrapper here would set a variable nothing reads.

        Nothing in this file is a new trap. Every syscall it needs already has
        a wrapper in error.c with the translation done once, so what is here
        is the part that is genuinely C: a record stream turned into one entry
        at a time, a PATH walk with the errno rules POSIX gives it, a
        canonicaliser, and an option parser.

        Almost everything is static. A spark program is one translation unit,
        so a directory reader that nothing opens is deleted by the compiler
        and a program that never calls scandir does not carry qsort on its
        account. The exceptions are the four getopt globals and __assert_fail,
        which have to be real symbols because they are what a program and a
        compiler respectively reach for by name.
*/

//      -- what the numbers are ---------------------------------------------

/*
        The bounds, all of them the kernel's own and none of them chosen here.

        PATH_MAX is Linux's 4096 and is what every path buffer below is sized
        to; a path longer than that is refused by the kernel anyway, so a
        buffer of that size cannot lose a path the kernel would have accepted.
        NAME_MAX is 255, which is what d_name holds plus its terminator.

        Guarded one at a time because src/sh/file.c already spells its own
        FILE_PATH_MAX and FILE_NAME_MAX, and because a program that included
        real headers before this one has already been told the same numbers.
*/
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef MAXNAMLEN
#define MAXNAMLEN NAME_MAX
#endif

/*
        The d_type values. DT_DIR, DT_REG, DT_LNK, DT_FIFO, DT_SOCK, DT_CHR
        and DT_BLK are already in src/platform/any.inc and are not repeated;
        the two it does not carry are added here, guarded the same way, so
        that a program switching on d_type has the whole set.
*/
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

#ifndef DT_WHT
#define DT_WHT 14
#endif

//      O_TMPFILE is the create bit error.c already names, or-ed with
//      O_DIRECTORY, whose value differs on arm64 and which library.c already
//      spells correctly for each machine. Writing it out here rather than
//      hard-coding 020200000 is what keeps tmpfile working on arm64.
#ifndef O_TMPFILE
#define O_TMPFILE (O_TMPFILE_CREATE | O_DIRECTORY)
#endif

//      clock_nanosleep's one flag: the request is a point on the clock and
//      not a span from now.
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/*
        How deep a chain of symbolic links realpath will follow before it
        decides the chain is a loop. Linux itself stops at forty and answers
        ELOOP, and matching the kernel means a path this refuses is a path the
        kernel would have refused too.
*/
#define PROCESS_SYMLINK_DEPTH 40

/*
        The getdents64 buffer, which is the one size in this file that is a
        judgement rather than a constant somebody else fixed.

        Thirty two kilobytes is four pages more than glibc's default, and the
        reason to be generous is that every refill is a syscall while every
        entry taken out of a full buffer is a pointer add. A directory of a
        thousand short names fits in one trap here. It is malloc'd with the
        DIR rather than put on the stack because a spark program's stack is
        the one the kernel gave _start and a caller that opens a directory
        inside a deep recursion should not have to know how big this is.
*/
#define PROCESS_DIRECTORY_BUFFER 32768

/*
        The kernel's record header: an inode, an offset, a two byte length and
        a one byte type, and then the name. Nineteen bytes, and the same on
        all three machines because linux_dirent64 is not one of the structures
        asm-generic rewrote.
*/
#define PROCESS_DIRENT_NAME 256
#define PROCESS_DIRENT_HEADER \
        (sizeof(p64) + sizeof(b64) + sizeof(p16) + sizeof(p8))

//      How many words execl and its two relatives will assemble before they
//      give up. A command line longer than this is past what any caller of
//      the variadic forms writes by hand.
#define PROCESS_ARGUMENT_MAX 1024

/*
        Reading a wait status, which error.c leaves undone.

        error.c has wait, waitpid and wait4 and the three option flags, but
        not the seven macros that say what the number they wrote actually
        means, so every caller in this tree decodes it by hand. The encoding
        is the kernel's and is the same on all three machines: the low seven
        bits are the terminating signal, 0x7f in them means stopped, the next
        eight bits are the exit code or the stopping signal, and bit seven is
        the core dump flag.

        WIFEXITED is written against the low seven bits rather than as
        "status & 0xff" so that a status of 0x7f -- stopped -- is not read as
        an exit, and WTERMSIG masks 0x7f rather than 0xff for the same
        reason. library.c's wait_status_code turns the whole thing into the
        single number a shell reports; these are the pieces underneath it.
*/
#ifndef WIFEXITED
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WTERMSIG(status) ((status) & 0x7f)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) (((b8)(((status) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WCOREDUMP(status) ((status) & 0x80)
#endif

//      -- directories ------------------------------------------------------

/*
        struct dirent, which is the kernel's record and glibc's structure at
        the same time.

        linux_dirent64 is an inode, an offset, a length, a type and a
        variable-length name; glibc's struct dirent on a 64 bit Linux is the
        same four fields followed by char d_name[256]. The two agree byte for
        byte over the header, which is why readdir below can hand back a
        pointer straight into the buffer the kernel filled instead of copying
        every entry into a structure of its own. sizeof is 280 either way.

        The name field is declared at its full 256 bytes because that is the
        ABI a program compiled against real headers expects, and because
        readdir_r has to copy into one. It is emphatically not how much of a
        record in the buffer may be read: the kernel writes exactly d_reclen
        bytes and the last record in a full buffer ends where the data ends,
        so anything that copies out of a record bounds itself by d_reclen and
        never by sizeof(d_name). Getting that backwards reads past what
        getdents64 wrote.
*/
typedef struct dirent
{
        p64 d_ino;
        b64 d_off;
        p16 d_reclen;
        p8 d_type;
        p8 d_name[PROCESS_DIRENT_NAME];
} process_dirent;

/*
        What an open directory is.

        A descriptor, the buffer the kernel fills, how much of it is valid,
        how far into it readdir has walked, and the offset telldir hands back.
        The buffer is last and everything before it is eight bytes wide or
        packs to it, because getdents64 writes records that must be eight byte
        aligned and a misaligned p64 load of d_ino is a fault on riscv64 and
        an unaligned access trap on arm64 with strict alignment. malloc gives
        sixteen byte alignment, so the buffer starts aligned and the kernel's
        own padding keeps every record after it aligned too.

        position is the offset of the entry readdir returned last, which is
        what glibc's telldir reports and is the thing that makes the usual
        idiom work: telldir before reading an entry names that entry, because
        it is the offset the previous one recorded, and seekdir back to it
        followed by readdir hands the same entry over again.
*/
typedef struct process_directory
{
        b32 descriptor;
        b32 process_directory_padding;
        positive used;
        positive at;
        b64 position;
        p8 buffer[PROCESS_DIRECTORY_BUFFER];
} DIR;

/*
        The one place a directory is built, so that opendir and fdopendir
        cannot drift apart.

        fdopendir owns the descriptor it is given -- closedir closes it, and
        there is no dup -- which is what POSIX says and what a caller that
        opened the descriptor with its own flags depends on. It is checked
        with fstat first, because handing a regular file to getdents64 gives
        ENOTDIR from the kernel on the first readdir rather than from
        fdopendir, and a program that tested the return of fdopendir would
        never see it.
*/
static DIR address_to process_directory_open(b32 descriptor)
{
        DIR address_to folder;
        struct stat facts;

        if (fstat(descriptor, address_of facts) < 0)
                return null;

        if (!S_ISDIR(facts.st_mode))
        {
                errno = ENOTDIR;
                return null;
        }

        folder = (DIR address_to)malloc(sizeof(DIR));

        if (is_null(folder))
        {
                errno = ENOMEM;
                return null;
        }

        folder->descriptor = descriptor;
        folder->process_directory_padding = 0;
        folder->used = 0;
        folder->at = 0;
        folder->position = 0;

        return folder;
}

/*
        opendir opens the descriptor itself and closes it again if anything
        after that fails, because a caller who gets a null pointer back has no
        way to close a descriptor it was never told about, and a program that
        opens a thousand directories and checks the result would still die of
        EMFILE.

        O_CLOEXEC because a directory handle held open across an exec is a
        leak into a program that did not ask for it, and O_DIRECTORY so that
        the refusal for a regular file comes from open rather than from the
        first read.
*/
static DIR address_to opendir(string_address path)
{
        b32 descriptor;
        DIR address_to folder;
        b32 saved;

        if (is_null(path))
        {
                errno = EINVAL;
                return null;
        }

        descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (descriptor < 0)
                return null;

        folder = process_directory_open(descriptor);

        if (is_null(folder))
        {
                saved = errno;
                close(descriptor);
                errno = saved;
                return null;
        }

        return folder;
}

static DIR address_to fdopendir(b32 descriptor)
{
        if (descriptor < 0)
        {
                errno = EBADF;
                return null;
        }

        return process_directory_open(descriptor);
}

static b32 closedir(DIR address_to folder)
{
        b32 descriptor;

        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        descriptor = folder->descriptor;
        free(folder);

        return close(descriptor);
}

static b32 dirfd(DIR address_to folder)
{
        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        return folder->descriptor;
}

/*
        Reading one record out of the stream, which is the substance of this
        half of the file.

        getdents64 does not hand back entries, it hands back a block of
        variable-length records packed end to end, each one saying in
        d_reclen how far the next one starts. There is no library routine for
        walking that and there could not be: the stride is data. This is one
        of the four hand-written loops in this file and the only one in the
        directory half.

        Every step is a pointer add. The refill happens when the walk has
        reached the end of what the kernel wrote, and a refill that returns
        zero is the end of the directory -- readdir answers null with errno
        untouched there, which is precisely how a caller tells the end from a
        failure.

        The reclen check is not defensive dressing. A record claiming zero
        bytes would make this loop forever and a record claiming more than the
        buffer holds would walk off the end, and neither is something the
        Linux kernel does -- but this pointer comes from a file system driver
        and the cost of being sure is one compare.
*/
static process_dirent address_to readdir(DIR address_to folder)
{
        process_dirent address_to record;
        b32 taken;

        if (is_null(folder))
        {
                errno = EINVAL;
                return null;
        }

        while (folder->at >= folder->used)
        {
                taken = getdents64(folder->descriptor, folder->buffer,
                                   PROCESS_DIRECTORY_BUFFER);

                if (taken < 0)
                        return null;

                if (taken == 0)
                        return null;

                folder->used = (positive)taken;
                folder->at = 0;
        }

        record = (process_dirent address_to)(folder->buffer + folder->at);

        if (record->d_reclen <= PROCESS_DIRENT_HEADER ||
            folder->at + record->d_reclen > folder->used)
        {
                folder->at = folder->used;
                errno = EIO;
                return null;
        }

        folder->at += record->d_reclen;
        folder->position = record->d_off;

        return record;
}

/*
        readdir_r, which copies rather than pointing and reports through its
        return value rather than through errno.

        The copy is bounded by d_reclen and not by sizeof(process_dirent),
        because the record in the buffer is only d_reclen bytes long and the
        last one in a full buffer ends exactly where the kernel stopped
        writing. A copy of 280 bytes off that record reads past the data.

        A name too long for the caller's structure is ENAMETOOLONG rather than
        a truncation, which is what glibc answers and is the only safe
        answer: the caller sized its buffer from the header and has nowhere to
        put the rest.
*/
static b32 readdir_r(DIR address_to folder, process_dirent address_to into,
                     process_dirent address_to address_to result)
{
        process_dirent address_to record;
        b32 saved;

        if (is_null(folder) || is_null(into) || is_null(result))
                return EINVAL;

        saved = errno;
        errno = 0;
        record = readdir(folder);

        if (is_null(record))
        {
                if (errno != 0)
                {
                        b32 reason = errno;
                        errno = saved;
                        return reason;
                }

                errno = saved;
                address_to result = null;
                return 0;
        }

        errno = saved;

        if (record->d_reclen > PROCESS_DIRENT_HEADER + NAME_MAX + 1)
        {
                address_to result = null;
                return ENAMETOOLONG;
        }

        memory_copy(into, record, record->d_reclen);
        address_to result = into;

        return 0;
}

/*
        rewinddir, telldir and seekdir, which are three views of one number.

        The buffer has to be dropped on both of the two that move, because it
        holds records from wherever the descriptor used to be and the entry
        after a seek is the one the kernel reads next, not the one still
        sitting in memory.

        telldir cannot fail and has nothing to report, which is why it is the
        only one of the three that does not touch errno.
*/
static fn rewinddir(DIR address_to folder)
{
        if (is_null(folder))
                return;

        lseek(folder->descriptor, 0, SEEK_SET);
        folder->used = 0;
        folder->at = 0;
        folder->position = 0;
}

static bipolar telldir(DIR address_to folder)
{
        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        return (bipolar)folder->position;
}

static fn seekdir(DIR address_to folder, bipolar where)
{
        if (is_null(folder))
                return;

        if (lseek(folder->descriptor, where, SEEK_SET) < 0)
                return;

        folder->used = 0;
        folder->at = 0;
        folder->position = where;
}

/*
        strverscmp, which is what versionsort is made of.

        This is GNU's algorithm and not an approximation of it, transcribed
        from the state machine glibc uses, because "sorts version numbers
        sensibly" is not a specification and two implementations that both
        sound reasonable disagree on ordinary inputs. The whole of the
        behaviour is in the two tables: which state a digit, a zero or
        anything else moves to, and what the answer is once the two strings
        have diverged.

        The four states are running text, an integral part being compared, a
        fractional part, and a fractional part that is still all zeroes. The
        distinction that matters is that "1.010" sorts before "1.09", because
        a run of digits that begins with a zero is a fraction and compares
        left to right, while a run that begins with one to nine is an integer
        and the longer run is the larger number.

        This is the second of the four hand-written loops here. There is no
        library routine for it and could not be: the comparison is a state
        machine over two strings at once, not a scan of one. byte_is_digit
        does the digit test, which is the part that would otherwise have been
        a hand-rolled range compare.
*/
#define PROCESS_VERSION_NORMAL 0
#define PROCESS_VERSION_INTEGER 3
#define PROCESS_VERSION_FRACTION 6
#define PROCESS_VERSION_ZEROES 9

#define PROCESS_VERSION_COMPARE 2
#define PROCESS_VERSION_LENGTH 3

static const p8 process_version_next[] = {
        //      state         other                    digit                    zero
        /* normal   */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_INTEGER, PROCESS_VERSION_ZEROES,
        /* integer  */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_INTEGER, PROCESS_VERSION_INTEGER,
        /* fraction */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_FRACTION, PROCESS_VERSION_FRACTION,
        /* zeroes   */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_FRACTION, PROCESS_VERSION_ZEROES};

static const b8 process_version_answer[] = {
        //      left over right: other/other other/digit other/zero digit/other
        //      digit/digit digit/zero zero/other zero/digit zero/zero
        /* normal   */
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_LENGTH, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        /* integer  */
        PROCESS_VERSION_COMPARE, -1, -1,
        1, PROCESS_VERSION_LENGTH, PROCESS_VERSION_LENGTH,
        1, PROCESS_VERSION_LENGTH, PROCESS_VERSION_LENGTH,
        /* fraction */
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        /* zeroes   */
        PROCESS_VERSION_COMPARE, 1, 1,
        -1, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        -1, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE};

static b32 strverscmp(string_address left, string_address right)
{
        string_address one = left;
        string_address two = right;
        p8 first;
        p8 second;
        b32 state;
        b32 difference;

        if (one == two)
                return 0;

        first = address_to one++;
        second = address_to two++;

        state = PROCESS_VERSION_NORMAL + (first == '0') +
                (byte_is_digit(first) != 0);

        while ((difference = (b32)first - (b32)second) == 0)
        {
                if (first == end)
                        return 0;

                state = (b32)process_version_next[state];
                first = address_to one++;
                second = address_to two++;
                state += (first == '0') + (byte_is_digit(first) != 0);
        }

        state = (b32)process_version_answer[state * 3 + (second == '0') +
                                            (byte_is_digit(second) != 0)];

        if (state == PROCESS_VERSION_COMPARE)
                return difference;

        if (state != PROCESS_VERSION_LENGTH)
                return state;

        //      Both runs are integers and both had the same digits up to
        //      here, so whichever run is longer is the larger number.
        while (byte_is_digit(address_to one++))
                if (!byte_is_digit(address_to two++))
                        return 1;

        return byte_is_digit(address_to two) ? -1 : difference;
}

/*
        The two orderings scandir is normally given.

        alphasort is strcoll in POSIX and strcmp in the C locale, and the C
        locale is the only one there is here, so it is string_compare -- the
        assembly, not a loop. versionsort is strverscmp above.

        Both take a pointer to a pointer, because that is the shape qsort
        hands a comparator when the array is an array of pointers, and it is
        the shape POSIX declares.
*/
static b32 alphasort(process_dirent address_to const address_to left,
                     process_dirent address_to const address_to right)
{
        return string_compare((address_to left)->d_name,
                              (address_to right)->d_name);
}

static b32 versionsort(process_dirent address_to const address_to left,
                       process_dirent address_to const address_to right)
{
        return strverscmp((address_to left)->d_name,
                          (address_to right)->d_name);
}

/*
        scandir: every entry that passes a test, in an order the caller picks.

        Each kept entry is copied into its own allocation sized to the name it
        actually has rather than to the 256 bytes the structure declares,
        which is what glibc does and is the difference between a directory of
        ten thousand short names costing 2.8 megabytes and costing about a
        third of that. The caller frees each entry and then the vector, so
        both have to come from the same allocator the program's free reaches,
        which is why these are malloc and not memory_take.

        The vector doubles. Sixteen to begin with because most directories a
        program scans are small, and doubling because the alternative --
        counting the directory first and then reading it again -- races
        against anything writing into it.

        On any failure everything allocated so far is released before the -1,
        because a scandir that half succeeded and leaked the half is worse
        than one that failed.
*/
#define PROCESS_SCANDIR_FIRST 16

static b32 scandir(string_address path,
                   process_dirent address_to address_to address_to list,
                   b32 (address_to keep)(const process_dirent address_to),
                   b32 (address_to order)(process_dirent address_to const address_to,
                                          process_dirent address_to const address_to))
{
        DIR address_to folder;
        process_dirent address_to record;
        process_dirent address_to address_to found = null;
        positive room = 0;
        positive count = 0;
        positive length;
        positive wanted;
        positive at;
        b32 saved;

        if (is_null(list))
        {
                errno = EINVAL;
                return -1;
        }

        folder = opendir(path);

        if (is_null(folder))
                return -1;

        while (1)
        {
                errno = 0;
                record = readdir(folder);

                if (is_null(record))
                {
                        if (errno != 0)
                                goto process_scandir_failed;

                        break;
                }

                if (!is_null(keep) && !keep(record))
                        continue;

                if (count == (positive)__INT_MAX__ ||
                    !memory_resize_reserve(address_of found, address_of room,
                        (count + 1) * sizeof(found[0]),
                        PROCESS_SCANDIR_FIRST * sizeof(found[0])))
                {
                        errno = count == (positive)__INT_MAX__ ? EOVERFLOW : ENOMEM;
                        goto process_scandir_failed;
                }

                length = string_length(record->d_name);
                wanted = PROCESS_DIRENT_HEADER + length + 1;

                found[count] = (process_dirent address_to)malloc(wanted);

                if (is_null(found[count]))
                {
                        errno = ENOMEM;
                        goto process_scandir_failed;
                }

                //      The header and the name and its terminator, and
                //      nothing past it. d_reclen is rewritten to what was
                //      actually allocated so a caller that trusts it does not
                //      read into the next block.
                memory_copy(found[count], record, wanted);
                found[count]->d_reclen = (p16)wanted;
                count++;
        }

        closedir(folder);

        //      A directory that matched nothing still hands back a vector, so
        //      that free(list) is always the right thing to do afterwards.
        if (is_null(found))
        {
                found = (process_dirent address_to address_to)
                        malloc(sizeof(found[0]));

                if (is_null(found))
                {
                        errno = ENOMEM;
                        return -1;
                }
        }

        if (!is_null(order) && count > 1)
                qsort(found, count, sizeof(found[0]),
                      (stdlib_compare)order);

        address_to list = found;

        return (b32)count;

process_scandir_failed:
        saved = errno;

        for (at = 0; at < count; at++)
                free(found[at]);

        free(found);
        closedir(folder);
        errno = saved;

        return -1;
}

//      -- the exec family ---------------------------------------------------

/*
        execv and execvpe are the two ends of this, and everything else is one
        of them with the arguments arranged differently.

        execve itself is error.c's, so nothing here traps. What is here is the
        PATH walk, which is the whole reason execvp is a different function
        from execv.
*/
static b32 execv(string_address path, string_address address_to arguments)
{
        return execve(path, arguments, stdlib_environment_list());
}

/*
        The shell retry, which is the rule people leave out.

        A file that is executable but is not something the kernel can load --
        a shell script with no #! line, most often -- fails execve with
        ENOEXEC. Every Unix since the seventh edition has answered that by
        running /bin/sh on it, and a program that relies on execvp to run a
        bare script relies on this. The new argument list is the shell, then
        the path that failed, then everything the caller passed after argv[0].

        The bound is the one divergence from glibc: glibc builds the new list
        in a variable-length array sized from the caller's argc and this uses
        a fixed one, because a spark program's stack is whatever the kernel
        gave _start and an argument list long enough to matter would take the
        stack out rather than fail. Past the bound the ENOEXEC stands, which
        is the honest answer rather than a crash.
*/
static fn process_execute_shell(string_address path,
                                string_address address_to arguments,
                                string_address address_to environment)
{
        string_address words[PROCESS_ARGUMENT_MAX];
        positive count = 0;
        positive at;

        while (!is_null(arguments[count]))
        {
                count++;

                if (count + 2 >= PROCESS_ARGUMENT_MAX)
                        return;
        }

        words[0] = (string_address) "/bin/sh";
        words[1] = path;

        for (at = 1; at < count; at++)
                words[at + 1] = arguments[at];

        words[count + 1] = null;

        execve(words[0], words, environment);
}

/*
        execvpe: the PATH walk, and the errno rules that make it useful.

        A name with a slash in it is not a PATH lookup at all and is handed
        straight to execve, which is what POSIX says and what every shell
        relies on.

        Otherwise each colon-separated piece of PATH gets the name joined onto
        it and gets one execve. What happens when that fails is the substance:

          - EACCES is remembered and the walk continues, because a directory
            earlier on PATH containing an unreadable file of the right name
            must not stop the search -- but if nothing later works, EACCES and
            not ENOENT is what the caller is told, because "there is one but
            you may not run it" is a different problem from "there is none".
          - ENOENT, ENOTDIR and ESTALE mean this directory simply does not
            have it, and the walk continues with nothing remembered.
          - ENODEV and ETIMEDOUT are what some network file systems answer
            instead of ENOENT; glibc skips them for that reason and so does
            this.
          - ENOEXEC gets the shell retry above, and if that returns too then
            the file was found and could not be run, which is an answer and
            not a reason to keep looking.
          - anything else means a file was found and something went wrong
            running it, which the caller wants to hear about immediately.

        All six of those were measured against glibc 2.44 on the build machine
        rather than taken from a table -- a directory of stubs, one
        unreadable, one a script with no #!, one missing, one runnable, walked
        by both implementations.

        PATH unset is "/bin:/usr/bin", which is what confstr(_CS_PATH)
        answers on the build machine and is what glibc's own execvp falls back
        to. It is deliberately not the shell's "/bin:/usr/bin:/", which is a
        different string for a different question.

        An empty piece of PATH means the working directory, which is POSIX and
        is why the name is used unjoined there.

        The PATH split is string_first_of, which is the assembly, and the join
        is path_join, which is also the assembly and already gets the
        separator right whether or not the directory ended in one. Neither is
        a loop here.
*/
#define PROCESS_DEFAULT_PATH "/bin:/usr/bin"

static bool process_execute_keep_looking(b32 reason, bool address_to denied)
{
        switch (reason)
        {
        case EACCES:
                address_to denied = true;
                return true;

        case ENOENT:
        case ENOTDIR:
        case ESTALE:
        case ENODEV:
        case ETIMEDOUT:
                return true;

        default:
                return false;
        }
}

static b32 execvpe(string_address name, string_address address_to arguments,
                   string_address address_to environment)
{
        p8 candidate[PATH_MAX];
        string_address search;
        string_address segment;
        bool denied = false;
        positive name_length;

        if (is_null(name) || string_get(name) == end)
        {
                errno = ENOENT;
                return -1;
        }

        if (!is_null(string_first_of(name, '/')))
        {
                execve(name, arguments, environment);

                if (errno == ENOEXEC)
                        process_execute_shell(name, arguments, environment);

                return -1;
        }

        name_length = string_length(name);

        if (name_length >= PATH_MAX)
        {
                errno = ENAMETOOLONG;
                return -1;
        }

        search = getenv((string_address) "PATH");

        if (is_null(search))
                search = (string_address)PROCESS_DEFAULT_PATH;

        segment = search;

        while (1)
        {
                string_address next = string_first_of_or_end(segment, ':');
                positive length = (positive)(next - segment);

                if (length == 0)
                {
                        //      An empty piece is the working directory, and
                        //      the name relative to it is the name itself.
                        string_copy(candidate, name);
                }
                else if (length + name_length + 2 <= PATH_MAX)
                {
                        string_address tail = memory_copy_apart_end(
                                candidate, segment, length);

                        if (tail[-1] != '/')
                                *tail++ = '/';
                        memory_copy_apart(tail, name, name_length + 1);
                }
                else
                {
                        //      Too long to be a path the kernel would take,
                        //      so it cannot be where the program is.
                        goto process_execute_next;
                }

                execve(candidate, arguments, environment);

                if (errno == ENOEXEC)
                {
                        process_execute_shell(candidate, arguments,
                                              environment);
                        return -1;
                }

                if (!process_execute_keep_looking(errno, address_of denied))
                        return -1;

        process_execute_next:
                if (*next == end)
                        break;

                segment = next + 1;
        }

        errno = denied ? EACCES : ENOENT;

        return -1;
}

static b32 execvp(string_address name, string_address address_to arguments)
{
        return execvpe(name, arguments, stdlib_environment_list());
}

/*
        The three variadic spellings.

        Each one walks its own argument list into a vector and then calls the
        vector form, which is the only way to build one: a va_list cannot be
        handed to execve. The list ends at the first null, and execle takes
        one more argument after that null, which is the environment.

        A list longer than the bound is refused with E2BIG rather than
        truncated, because a truncated argument list is a different command.
*/
/*
        The list is handed over by address, and that is not a style choice.

        A va_list is an array type on x86_64, so passing one to a helper
        decays to a pointer and the helper's advances are visible to the
        caller afterwards. On arm64 and riscv64 it is a structure passed by
        value, so they are not: the caller's list is still sitting on the
        first variadic argument when the helper returns. execle is the one
        routine here that reads an argument after the helper has run -- the
        environment, past the terminating null -- and with a by-value list it
        read the wrong one and handed execve a pointer that was not a vector
        at all. That failed with EFAULT on two machines out of three and
        worked on the third, which is exactly the shape of bug three
        architecture parity exists to catch, and it was caught that way.
*/
static positive process_execute_gather(string_address words[],
                                       string_address first,
                                       var_args address_to list)
{
        positive count = 0;

        words[count++] = first;

        if (is_null(first))
                return count;

        while (count < PROCESS_ARGUMENT_MAX)
        {
                words[count] = var_list_get(address_to list, string_address);

                if (is_null(words[count]))
                        return count + 1;

                count++;
        }

        return 0;
}

#define PROCESS_EXECL(name, vector)                                          \
        static b32 name(string_address path, string_address first, ...)      \
        {                                                                    \
                string_address words[PROCESS_ARGUMENT_MAX];                  \
                var_args list;                                               \
                var_list(list, first);                                       \
                positive count = process_execute_gather(                     \
                    words, first, address_of list);                           \
                var_list_end(list);                                          \
                if (!count)                                                  \
                {                                                            \
                        errno = E2BIG;                                        \
                        return -1;                                            \
                }                                                            \
                return vector(path, words);                                  \
        }

PROCESS_EXECL(execl, execv)
PROCESS_EXECL(execlp, execvp)
#undef PROCESS_EXECL

static b32 execle(string_address path, string_address first, ...)
{
        string_address words[PROCESS_ARGUMENT_MAX];
        string_address address_to environment;
        var_args list;
        positive count;

        var_list(list, first);
        count = process_execute_gather(words, first, address_of list);

        if (count == 0)
        {
                var_list_end(list);
                errno = E2BIG;
                return -1;
        }

        environment = var_list_get(list, string_address address_to);
        var_list_end(list);

        return execve(path, words, environment);
}

//      -- sleeping ---------------------------------------------------------

/*
        nanosleep, and why the remaining time needs no code here.

        POSIX says that a sleep cut short by a signal reports how much of it
        was left, and the Linux kernel already writes that into the second
        argument before it returns EINTR. So the contract is kept by passing
        the pointer through and not by measuring anything: there is no clock
        read before, no subtraction after, and no window in which a second
        signal could make the answer wrong.

        The structure is library.c's timespec, whose fields are p64. clock.c
        already explains why: it is a duration handed to this call and a
        duration is never negative. A caller that puts a negative second count
        in it gets the same bits the kernel would have seen from a signed
        field, and the kernel answers EINVAL to both.
*/
static b32 nanosleep(timespec address_to request, timespec address_to remaining)
{
        return error_result(system_call_2(syscall(nanosleep),
                                         (positive)request,
                                         (positive)remaining));
}

/*
        clock_nanosleep is the one call in this file that does not use errno,
        and that is not an oversight.

        POSIX says it returns the error number directly and leaves errno
        alone, which is the opposite of every other name here and is the
        reason it cannot go through error_result. A program that writes

            if (clock_nanosleep(...) != 0)

        and then reads errno is reading whatever the last failure left there.

        TIMER_ABSTIME makes the request a point on the named clock rather than
        a span from now, and an absolute sleep writes nothing into remaining
        even when a signal cuts it short -- there is nothing to say, because
        the deadline has not moved.
*/
static b32 clock_nanosleep(clockid_t which, b32 flags,
                           timespec address_to request,
                           timespec address_to remaining)
{
        bipolar answer = system_call_4(syscall(clock_nanosleep),
                                       (positive)which, (positive)flags,
                                       (positive)request,
                                       (positive)remaining);

        return system_failed(answer) ? (b32) - answer : 0;
}

/*
        sleep, which cannot be called sleep here.

        library.c already exports an assembly routine of that name taking a
        timespec, three places in this tree call it, and the symbol is in
        every built object. POSIX's sleep takes an unsigned count of seconds
        and returns the seconds it did not sleep, which is a different
        function of a different type with the same name.

        So the C one is a routine with its own name and an opt-in macro that
        renames it, exactly as stdlib.c does for exit under
        STANDARD_EXIT_RUNS_HANDLERS. It is off by default and must stay off:
        CHECK_clock in test/checks.c and CHECK_stream_buffering in test/checks.c both call the
        assembly sleep with a pointer, and a macro that was on by default
        would compile those into passing a pointer as a second count without
        a word of complaint.

        The body is glibc's, deliberately. It sleeps through clock_nanosleep
        with the same structure as both the request and the remainder, so that
        an interrupted sleep leaves the remainder where the return value is
        computed from, and it rounds a part-second remainder up -- a sleep
        with a hundred nanoseconds left reports one second left, because
        reporting zero would tell a caller looping on the result that it had
        finished.
*/
static positive process_sleep_seconds(positive seconds)
{
        timespec span;

        span.tv_sec = seconds;
        span.tv_nsec = 0;

        if (clock_nanosleep(CLOCK_REALTIME, 0, address_of span,
                            address_of span) == 0)
                return 0;

        return span.tv_sec + (span.tv_nsec > 0);
}

#ifdef STANDARD_SLEEP_IS_POSIX
#define sleep(seconds) process_sleep_seconds(seconds)
#endif

/*
        usleep, which is obsolete and is still what half the world writes.

        A count above a million is not an error here, because it is not one in
        glibc either since it stopped being XSI -- the seconds and the
        remainder are split out and the sleep is the whole of it.
*/
static b32 usleep(p32 microseconds)
{
        timespec span;

        span.tv_sec = microseconds / 1000000u;
        span.tv_nsec = (microseconds % 1000000u) * 1000u;

        return nanosleep(address_of span, null);
}

//      -- names in the file system ------------------------------------------

/*
        realpath, and the reason it is a walk rather than a call.

        There is no syscall that answers this. The kernel resolves a path
        every time it is handed one, but it never says what it resolved to,
        so the only way to know is to do the resolution here: take the
        components one at a time, look at each with lstat, and splice a
        symbolic link's target in front of whatever is left whenever one
        turns up.

        Every component must exist. That is what the interface promises and
        what makes the answer usable -- a path that resolved through a
        directory that is not there would be a guess -- so a missing piece is
        ENOENT and a piece that is a file where a directory was needed is
        ENOTDIR, both of which come out of lstat and are passed through.

        `..` is where a naive walk goes wrong. Popping the last component off
        the answer is right, but only after checking that what is being
        popped out of is a directory: /etc/passwd/.. is ENOTDIR and not /etc,
        because the kernel would say so and a program using this to decide
        whether a path is inside a tree would be told the wrong thing. The
        mode of the last resolved component is kept for exactly that test.

        The loop over components is the fourth and last hand-written loop
        here. It is not a scan for a byte -- string_first_of finds the
        separator and that is the assembly -- it is the state that has to be
        carried from one component to the next, and there is no routine for
        that.

        A null second argument allocates, which is the GNU behaviour and is by
        far the more common call. The buffer a caller supplies must be
        PATH_MAX bytes, because there is no way to tell it how much was
        needed and no way to ask.
*/
static string_address realpath(string_address path, string_address into)
{
        p8 answer[PATH_MAX];
        p8 rest[PATH_MAX];
        p8 link[PATH_MAX];
        p8 merged[PATH_MAX];
        struct stat facts;
        positive answer_length = 0;
        positive at = 0;
        positive followed = 0;
        bool last_was_directory = true;
        b32 saved;

        if (is_null(path))
        {
                errno = EINVAL;
                return null;
        }

        if (string_get(path) == end)
        {
                errno = ENOENT;
                return null;
        }

        if (string_length(path) >= PATH_MAX)
        {
                errno = ENAMETOOLONG;
                return null;
        }

        if (path[0] == '/')
        {
                answer[0] = '/';
                answer[1] = end;
                answer_length = 1;
                string_copy(rest, path + 1);
        }
        else
        {
                if (is_null(getcwd(answer, PATH_MAX)))
                        return null;

                answer_length = string_length(answer);
                string_copy(rest, path);
        }

        while (rest[at] != end)
        {
                positive piece;

                //      Skip the separators between components, which also
                //      makes "a//b" and "a/b" the same path.
                if (rest[at] == '/')
                {
                        // A slash requires the preceding component to be a
                        // directory, including a trailing slash or slash-dot.
                        if (!last_was_directory)
                        {
                                errno = ENOTDIR;
                                return null;
                        }
                        at++;
                        continue;
                }

                piece = (positive)(string_first_of_or_end(rest + at, '/') -
                                   (rest + at));

                if (piece == 1 && rest[at] == '.')
                {
                        at += piece;
                        continue;
                }

                if (piece == 2 && rest[at] == '.' && rest[at + 1] == '.')
                {
                        //      Root's parent is root, which is what the
                        //      kernel does too.
                        if (answer_length > 1)
                        {
                                answer_length = path_head_copy(answer, PATH_MAX,
                                                               answer);
                        }

                        at += piece;
                        continue;
                }

                {
                        p8 held[PATH_MAX];

                        if (piece >= PATH_MAX)
                        {
                                errno = ENAMETOOLONG;
                                return null;
                        }

                        memory_copy(held, rest + at, piece);
                        held[piece] = end;

                        //      Remember where the answer was, so that a
                        //      symbolic link can be undone by shortening it
                        //      again rather than by rebuilding it.
                        {
                                positive was = answer_length;

                                answer_length = path_join(answer, PATH_MAX,
                                                          answer, held);

                                if (answer_length == 0 ||
                                    answer_length >= PATH_MAX - 1)
                                {
                                        errno = ENAMETOOLONG;
                                        return null;
                                }

                                if (lstat(answer, address_of facts) < 0)
                                        return null;

                                if (S_ISLNK(facts.st_mode))
                                {
                                        bipolar wrote;

                                        if (++followed > PROCESS_SYMLINK_DEPTH)
                                        {
                                                errno = ELOOP;
                                                return null;
                                        }

                                        wrote = readlink(answer, link,
                                                         PATH_MAX - 1);

                                        if (wrote < 0)
                                                return null;

                                        link[wrote] = end;

                                        //      What is left of the original
                                        //      path goes after the link's
                                        //      target, and the walk starts
                                        //      again from the front of the
                                        //      spliced string.
                                        at += piece;

                                        if (string_length(link) +
                                                    string_length(rest + at) +
                                                    2 >
                                            PATH_MAX)
                                        {
                                                errno = ENAMETOOLONG;
                                                return null;
                                        }

                                        {
                                                p8 address_to tail =
                                                        string_copy_max_end(
                                                                merged, link,
                                                                PATH_MAX - 1);

                                                if (rest[at] != end)
                                                {
                                                        address_to tail++ = '/';
                                                        string_copy_max_end(
                                                                tail,
                                                                rest + at,
                                                                PATH_MAX - 1 -
                                                                        (positive)(tail -
                                                                                   merged));
                                                }
                                        }

                                        string_copy(rest, merged);
                                        at = 0;

                                        if (rest[0] == '/')
                                        {
                                                answer[0] = '/';
                                                answer[1] = end;
                                                answer_length = 1;
                                        }
                                        else
                                        {
                                                answer[was] = end;
                                                answer_length = was;
                                        }

                                        last_was_directory = true;
                                        continue;
                                }

                                last_was_directory =
                                        S_ISDIR(facts.st_mode) ? true : false;
                                at += piece;
                        }
                }
        }

        if (is_null(into))
        {
                into = (string_address)malloc(answer_length + 1);

                if (is_null(into))
                {
                        errno = ENOMEM;
                        return null;
                }

                memory_copy(into, answer, answer_length + 1);
                return into;
        }

        saved = errno;
        memory_copy(into, answer, answer_length + 1);
        errno = saved;

        return into;
}

/*
        basename and dirname, which are already written and are in assembly.

        path_tail_copy and path_head_copy in library.c implement exactly these
        two rules -- trailing separators go except for root, a path with no
        directory has "." for its head -- with the bound checked and the
        answer's length returned. There is nothing to write here but the
        buffer and the two edge cases POSIX names: a null pointer and an empty
        string both answer ".".

        They are the XPG forms from <libgen.h> and not the GNU basename from
        <string.h>, which is a different function: that one never strips a
        trailing slash and never answers ".". This is the one a program that
        includes libgen.h expects.

        POSIX allows either modifying the argument or returning static
        storage, and static storage is chosen because the alternative writes
        into a string literal for every caller who passes one. The next call
        overwrites it, which POSIX says it may.

        Note that library.c also has path_basename, which is a different
        routine again: it writes to a writer rather than to a buffer, and it
        is not what either of these wants.
*/
static p8 process_basename_storage[PATH_MAX];
static p8 process_dirname_storage[PATH_MAX];

static string_address basename(string_address path)
{
        if (is_null(path) || string_get(path) == end)
        {
                process_basename_storage[0] = '.';
                process_basename_storage[1] = end;
                return process_basename_storage;
        }

        path_tail_copy(process_basename_storage, PATH_MAX, path);

        return process_basename_storage;
}

static string_address dirname(string_address path)
{
        if (is_null(path) || string_get(path) == end)
        {
                process_dirname_storage[0] = '.';
                process_dirname_storage[1] = end;
                return process_dirname_storage;
        }

        path_head_copy(process_dirname_storage, PATH_MAX, path);

        /*
                The one place path_head_copy and glibc disagree, and POSIX is
                on glibc's side.

                XBD 4.13 says a pathname beginning with exactly two slashes
                may name something the implementation chooses, and a single
                or triple slash may not -- so "//" and "//usr" have "//" for
                their directory part where "/usr" and "///usr" have "/".
                path_head_copy answers "/" for all four, which is right for
                every caller it has in this tree and is not what a program
                that came from glibc expects.

                It is corrected here rather than in library.c because the
                assembly is shared with path handling that does not want the
                distinction, and because the correction is exactly this: a
                head of "/" out of a path whose first two bytes are slashes
                and whose third is not.
        */
        if (process_dirname_storage[0] == '/' &&
            process_dirname_storage[1] == end && path[0] == '/' &&
            path[1] == '/' && path[2] != '/')
        {
                process_dirname_storage[1] = '/';
                process_dirname_storage[2] = end;
        }

        return process_dirname_storage;
}

//      -- getopt ------------------------------------------------------------

/*
        The four globals, which are the interface as much as the function is.

        They are real symbols and not statics, because a program that parses
        options in one file and reports the bad one in another reaches for
        optopt by name, and because an object compiled against real headers
        expects to link against these.

        optind starts at one because argv[0] is the program's own name.
        opterr starts at one because the default is to complain. optopt starts
        at '?' because that is what glibc initialises it to, and a program
        that reads it before the first call sees the same thing here.
*/
string_address optarg = null;
b32 optind = 1;
b32 opterr = 1;
b32 optopt = '?';

//      Where inside the current argument the scan is. A cluster like -abc is
//      three calls into one element, and this is what remembers that.
static string_address process_getopt_place = null;

/*
        Whether getopt has ever been called, which exists to reproduce one
        piece of glibc's observable behaviour exactly.

        glibc declares optopt as '?' and then, on the first call, copies it
        back out of a private structure whose corresponding field was zero
        filled -- so a program that reads optopt before calling getopt sees
        '?' there, and one that reads it after a call that reported no error
        sees 0. POSIX says nothing about either, because optopt is only
        defined after '?' or ':' comes back. Matching both is three lines and
        makes the differential against glibc exact rather than nearly so.
*/
static bool process_getopt_started = false;

/*
        getopt, and the one place it deliberately differs from glibc.

        glibc permutes argv by default: it moves non-options to the end so
        that `prog file -v` sees -v. POSIX says the opposite -- the first
        non-option ends the options -- and this implements POSIX, because
        permuting rewrites the caller's argv and a program that reads argv
        itself afterwards would find it rearranged underneath it.

        The two ways of asking glibc for the POSIX behaviour are a leading '+'
        in the option string and POSIXLY_CORRECT in the environment, and both
        were used to compare against: the differential harness ran every case
        through glibc with POSIXLY_CORRECT set and again with a '+' in front,
        and this agrees with both on all of them. A leading '+' is accepted
        and ignored here, so the same option string works either way.

        Everything else is glibc's behaviour and was measured, not assumed:

          - a leading ':' means silence and makes a missing argument report
            ':' rather than '?'.
          - a leading '-' makes a non-option come back as return value 1 with
            optarg pointing at it, which is the GNU RETURN_IN_ORDER mode.
          - "x:" wants an argument, attached or in the next element; "x::"
            takes one only when it is attached, and leaves optarg null
            otherwise without consuming anything.
          - optind is incremented when the last character of an element is
            reached, not when the element is finished, which is what makes -a
            and -abc leave optind in the same relative place.
          - the two diagnostics are glibc's own wording, character for
            character, so a script matching on them does not have to know
            which library it is reading.

        The parse is the fourth hand-written loop's sibling and is a state
        machine over one string; string_first_of finds the character in the
        option string, which is the scan, and the rest is the state.
*/
static b32 getopt(b32 count, string_address address_to words,
                  string_address options)
{
        string_address specification;
        bool silent;
        bool in_order = false;
        p8 letter;

        if (count < 1 || is_null(words) || is_null(options))
                return -1;

        //      Every call begins with no argument recorded, so that a '?' or
        //      a -1 never leaves the previous option's argument standing
        //      where a caller would read it. glibc clears it in the same
        //      place and for the same reason.
        optarg = null;

        if (!process_getopt_started)
        {
                process_getopt_started = true;
                optopt = 0;
        }

        //      Zero is the GNU request for a full restart, and one is the
        //      ordinary one. Both begin again at the first argument.
        if (optind == 0)
        {
                optind = 1;
                process_getopt_place = null;
        }

        if (options[0] == '-')
        {
                in_order = true;
                options++;
        }
        else if (options[0] == '+')
        {
                options++;
        }

        silent = options[0] == ':';

        if (is_null(process_getopt_place) || string_get(process_getopt_place) == end)
        {
                if (optind >= count || is_null(words[optind]))
                {
                        process_getopt_place = null;
                        return -1;
                }

                if (words[optind][0] != '-' || words[optind][1] == end)
                {
                        //      A bare "-" is an operand and not an option.
                        if (!in_order)
                        {
                                process_getopt_place = null;
                                return -1;
                        }

                        optarg = words[optind++];
                        return 1;
                }

                //      "--" ends the options and is itself consumed.
                if (words[optind][1] == '-' && words[optind][2] == end)
                {
                        optind++;
                        process_getopt_place = null;
                        return -1;
                }

                process_getopt_place = words[optind] + 1;
        }

        letter = address_to process_getopt_place++;
        specification = string_first_of(options, letter);

        //      optind moves on as the last character of the element is taken,
        //      so that everything below can ask whether there is another
        //      element without also having to know where in this one it is.
        if (string_get(process_getopt_place) == end)
                optind++;

        if (letter == ':' || is_null(specification))
        {
                optopt = letter;

                if (opterr && !silent)
                        string_format(log_error, "%s: invalid option -- '%s'\n",
                                      words[0], (p8[]){letter, end});

                return '?';
        }

        if (specification[1] != ':')
        {
                //      No argument wanted, and nothing else to do.
                return letter;
        }

        if (specification[2] == ':')
        {
                //      Optional, and only in the same element.
                if (string_get(process_getopt_place) != end)
                {
                        optarg = process_getopt_place;
                        optind++;
                }

                process_getopt_place = null;

                return letter;
        }

        if (string_get(process_getopt_place) != end)
        {
                optarg = process_getopt_place;
                optind++;
        }
        else if (optind == count)
        {
                optopt = letter;
                process_getopt_place = null;

                if (opterr && !silent)
                        string_format(log_error, "%s: option requires an argument -- '%s'\n",
                                      words[0], (p8[]){letter, end});

                return silent ? ':' : '?';
        }
        else
        {
                optarg = words[optind++];
        }

        process_getopt_place = null;

        return letter;
}

//      -- assert ------------------------------------------------------------

/*
        assert, which is one macro and one routine that never returns.

        The routine is called __assert_fail because that is the symbol every
        compiler and every real <assert.h> emits a call to, so an object built
        against glibc's headers and linked against this finds it. The
        signature is glibc's for the same reason.

        The sentence is glibc's too, word for word and punctuation for
        punctuation:

            prog: file:42: function: Assertion `x > 0' failed.

        The program's name comes from argv[0] with the directories taken off,
        which is what glibc's __progname is, and it is left out entirely when
        there is no argv -- glibc prints no bare ": " for an empty name and
        neither does this.

        It goes to descriptor two through log_error, which flushes anything
        the buffered log is holding first. That ordering is the point: a
        program that printed a line and then tripped an assertion must have
        the line appear before the diagnostic, and abort is coming
        immediately afterwards so nothing else will get the chance to flush
        it.

        NDEBUG turns the macro into nothing, and it has to be nothing that is
        still an expression -- a bare 0 would warn where an assert is used as
        a statement in a comma expression, and a do/while would not compile
        where one is used as an expression.
*/
COLD pub DEAD_END fn __assert_fail(string_address claim, string_address file,
                                  p32 line, string_address function)
{
        p8 name[PATH_MAX];
        string_address program = program_argument(0);

        name[0] = end;

        if (!is_null(program) && string_get(program) != end)
                path_tail_copy(name, PATH_MAX, program);

        if (name[0] != end)
                string_format(log_error, (string_address) "%s: ", name);

        string_format(log_error, (string_address) "%s:%p: ", is_null(file) ? (string_address) "" : file,
                      (positive)line);

        if (!is_null(function))
                string_format(log_error, (string_address) "%s: ", function);

        string_format(log_error, (string_address) "Assertion `%s' failed.\n",
                      is_null(claim) ? (string_address) "" : claim);

        abort();
}

#undef assert

#ifdef NDEBUG
#define assert(claim) ((fn)0)
#else
#define assert(claim)                                                    \
        ((claim) ? (fn)0                                                 \
                 : __assert_fail((string_address)#claim,                 \
                                 (string_address)__FILE__, (p32)__LINE__, \
                                 (string_address)__func__))
#endif

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_PROCESS
#endif // STANDARD_SKIP_PROCESS

#endif // STANDARD_MODERN_C_STANDARD
