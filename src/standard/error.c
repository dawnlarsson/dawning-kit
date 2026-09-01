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

//      41 and 58 are holes in Linux's numbering, and stay holes. glibc
//      answers "Unknown error 41" for both and so does this.
#define ERROR_HIGHEST 133

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
        line src/test/run uses. x86_64 native, qemu-aarch64 and qemu-riscv64
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

/*
        Telling a failed trap from a large answer, once, so that sixty
        wrappers cannot each get it slightly wrong.

        Linux returns an error as the negated errno in the same register that
        carries the result, and every errno is between 1 and 4095, so a return
        in [-4095, -1] is a failure and everything else is a result. Testing
        the sign instead is wrong twice over: several of these routines are
        declared to return `positive` in library.c, where `result < 0` is a
        comparison the compiler folds to false and deletes, and mmap
        legitimately returns addresses whose top bit is set.

        Written as one unsigned compare against the window rather than two
        signed ones. A value reinterpreted as unsigned is at or above
        0xfffffffffffff001 exactly when it is in the window.
*/
#define ERROR_WINDOW ((positive) - 4095)

static bool error_failed(bipolar result)
{
        return (positive)result >= ERROR_WINDOW;
}

//      The three shapes a POSIX return takes, so the sign handling and the
//      errno store appear once each rather than at every call.

//      int-returning calls: a handle, a count that fits, a plain success.
static b32 error_whole(bipolar result)
{
        if (error_failed(result))
        {
                errno = (b32) - result;
                return -1;
        }

        return (b32)result;
}

//      Register-width calls, where truncating to int would lose a large read,
//      file offset or address. Pointer results cast back at their boundary.
static bipolar error_wide(bipolar result)
{
        if (error_failed(result))
        {
                errno = (b32) - result;
                return -1;
        }

        return result;
}

/*
        The messages, taken from glibc and not written here.

        Copied out of GNU libc 2.44 on the build machine by a program that
        linked it and printed strerror(i) for every i from 0 to 140, so the
        wording is what a user of any other Linux program sees rather than
        what seemed reasonable to whoever typed this table. 41 and 58 are
        holes in Linux's numbering; glibc answers "Unknown error 41" for them
        and the null entries below make this answer the same.

        A table of pointers rather than a packed block of text with an offset
        array: the pointers cost eight bytes an entry in .data and the packed
        form costs a two byte offset, but the packed form also costs an add
        and a load at every lookup, and this table is consulted when something
        has already gone wrong. Nothing here is on a path that is measured.
*/
static string_address const error_messages[ERROR_HIGHEST + 1] = {
        (string_address) "Success",
        (string_address) "Operation not permitted",
        (string_address) "No such file or directory",
        (string_address) "No such process",
        (string_address) "Interrupted system call",
        (string_address) "Input/output error",
        (string_address) "No such device or address",
        (string_address) "Argument list too long",
        (string_address) "Exec format error",
        (string_address) "Bad file descriptor",
        (string_address) "No child processes",
        (string_address) "Resource temporarily unavailable",
        (string_address) "Cannot allocate memory",
        (string_address) "Permission denied",
        (string_address) "Bad address",
        (string_address) "Block device required",
        (string_address) "Device or resource busy",
        (string_address) "File exists",
        (string_address) "Invalid cross-device link",
        (string_address) "No such device",
        (string_address) "Not a directory",
        (string_address) "Is a directory",
        (string_address) "Invalid argument",
        (string_address) "Too many open files in system",
        (string_address) "Too many open files",
        (string_address) "Inappropriate ioctl for device",
        (string_address) "Text file busy",
        (string_address) "File too large",
        (string_address) "No space left on device",
        (string_address) "Illegal seek",
        (string_address) "Read-only file system",
        (string_address) "Too many links",
        (string_address) "Broken pipe",
        (string_address) "Numerical argument out of domain",
        (string_address) "Numerical result out of range",
        (string_address) "Resource deadlock avoided",
        (string_address) "File name too long",
        (string_address) "No locks available",
        (string_address) "Function not implemented",
        (string_address) "Directory not empty",
        (string_address) "Too many levels of symbolic links",
        null,
        (string_address) "No message of desired type",
        (string_address) "Identifier removed",
        (string_address) "Channel number out of range",
        (string_address) "Level 2 not synchronized",
        (string_address) "Level 3 halted",
        (string_address) "Level 3 reset",
        (string_address) "Link number out of range",
        (string_address) "Protocol driver not attached",
        (string_address) "No CSI structure available",
        (string_address) "Level 2 halted",
        (string_address) "Invalid exchange",
        (string_address) "Invalid request descriptor",
        (string_address) "Exchange full",
        (string_address) "No anode",
        (string_address) "Invalid request code",
        (string_address) "Invalid slot",
        null,
        (string_address) "Bad font file format",
        (string_address) "Device not a stream",
        (string_address) "No data available",
        (string_address) "Timer expired",
        (string_address) "Out of streams resources",
        (string_address) "Machine is not on the network",
        (string_address) "Package not installed",
        (string_address) "Object is remote",
        (string_address) "Link has been severed",
        (string_address) "Advertise error",
        (string_address) "Srmount error",
        (string_address) "Communication error on send",
        (string_address) "Protocol error",
        (string_address) "Multihop attempted",
        (string_address) "RFS specific error",
        (string_address) "Bad message",
        (string_address) "Value too large for defined data type",
        (string_address) "Name not unique on network",
        (string_address) "File descriptor in bad state",
        (string_address) "Remote address changed",
        (string_address) "Can not access a needed shared library",
        (string_address) "Accessing a corrupted shared library",
        (string_address) ".lib section in a.out corrupted",
        (string_address) "Attempting to link in too many shared libraries",
        (string_address) "Cannot exec a shared library directly",
        (string_address) "Invalid or incomplete multibyte or wide character",
        (string_address) "Interrupted system call should be restarted",
        (string_address) "Streams pipe error",
        (string_address) "Too many users",
        (string_address) "Socket operation on non-socket",
        (string_address) "Destination address required",
        (string_address) "Message too long",
        (string_address) "Protocol wrong type for socket",
        (string_address) "Protocol not available",
        (string_address) "Protocol not supported",
        (string_address) "Socket type not supported",
        (string_address) "Operation not supported",
        (string_address) "Protocol family not supported",
        (string_address) "Address family not supported by protocol",
        (string_address) "Address already in use",
        (string_address) "Cannot assign requested address",
        (string_address) "Network is down",
        (string_address) "Network is unreachable",
        (string_address) "Network dropped connection on reset",
        (string_address) "Software caused connection abort",
        (string_address) "Connection reset by peer",
        (string_address) "No buffer space available",
        (string_address) "Transport endpoint is already connected",
        (string_address) "Transport endpoint is not connected",
        (string_address) "Cannot send after transport endpoint shutdown",
        (string_address) "Too many references: cannot splice",
        (string_address) "Connection timed out",
        (string_address) "Connection refused",
        (string_address) "Host is down",
        (string_address) "No route to host",
        (string_address) "Operation already in progress",
        (string_address) "Operation now in progress",
        (string_address) "Stale file handle",
        (string_address) "Structure needs cleaning",
        (string_address) "Not a XENIX named type file",
        (string_address) "No XENIX semaphores available",
        (string_address) "Is a named type file",
        (string_address) "Remote I/O error",
        (string_address) "Disk quota exceeded",
        (string_address) "No medium found",
        (string_address) "Wrong medium type",
        (string_address) "Operation canceled",
        (string_address) "Required key not available",
        (string_address) "Key has expired",
        (string_address) "Key has been revoked",
        (string_address) "Key was rejected by service",
        (string_address) "Owner died",
        (string_address) "State not recoverable",
        (string_address) "Operation not possible due to RF-kill",
        (string_address) "Memory page has hardware error",
};

/*
        The longest message above is "Invalid or incomplete multibyte or wide
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

//      True when the number names a message, which is not the same as being
//      in range: 41 and 58 are in range and name nothing.
static bool error_message_known(b32 number)
{
        return number >= 0 && number <= ERROR_HIGHEST
               && !is_null(error_messages[number]);
}

/*
        The message half of perror, and the engine under both strerror forms.

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
static b32 error_message_into(string_address into, positive size, b32 number)
{
        p8 built[ERROR_MESSAGE_MAX];
        string_address source;
        positive length;
        positive room;

        if (error_message_known(number))
        {
                source = error_messages[number];

                if (is_null(into) || size == 0)
                        return ERANGE;

                length = string_length(source);

                //      Apart rather than memory_copy: the destination is the
                //      caller's buffer and the source is the table, which is
                //      static const and cannot be written through, so the
                //      overlap test memory_copy opens with is a test whose
                //      answer is already known. setenv in the sibling file
                //      spells the same shape the same way.
                if (length < size)
                {
                        memory_copy_apart(into, source, length + 1);
                        return 0;
                }

                room = size - 1;
                memory_copy_apart(into, source, room);
                into[room] = end;

                return ERANGE;
        }

        //      Both the length and the copy are constant here, so the copy
        //      folds to straight-line stores and no scan of the prefix
        //      happens at all.
        memory_copy_apart(built, (string_address)ERROR_UNKNOWN_PREFIX,
                          ERROR_UNKNOWN_PREFIX_LENGTH);
        length = ERROR_UNKNOWN_PREFIX_LENGTH +
                 bipolar_into_string(built + ERROR_UNKNOWN_PREFIX_LENGTH,
                                     (bipolar)number);

        if (is_null(into) || size == 0)
                return EINVAL;

        room = length < size ? length : size - 1;

        //      `built` is this function's own frame and `into` is the
        //      caller's buffer, so these two cannot be the same memory.
        memory_copy_apart(into, built, room);
        into[room] = end;

        return EINVAL;
}

/*
        strerror_r, in its POSIX shape rather than its GNU one.

        The two disagree about the return type: POSIX has
        int strerror_r(int, char *, size_t) and GNU has
        char *strerror_r(int, char *, size_t), and glibc picks between them by
        whether _GNU_SOURCE is defined. There is no _GNU_SOURCE here and no
        feature test macro machinery to hang one on, so the choice has to be
        made once and defended.

        POSIX, because its contract is checkable. The POSIX form always writes
        the buffer it was handed and reports whether the whole message fit;
        the GNU form is permitted to return a pointer to static storage and
        never touch the buffer at all, so a caller cannot tell from the return
        whether the buffer holds anything, and a test cannot pin the behaviour
        without knowing which message came from where. A program that wants
        the GNU shape can write

            strerror_r(number, buffer, size), buffer

        and get it, which is not true in the other direction.
*/
static b32 strerror_r(b32 number, string_address into, positive size)
{
        return error_message_into(into, size, number);
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
        if (error_message_known(number))
                return error_messages[number];

        error_message_into(error_unknown_text, sizeof error_unknown_text,
                           number);

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

        error_message_into(line + at, ERROR_MESSAGE_MAX, errno);
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
        in error_whole and error_wide, so the only thing a reader has to check
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

#define ERROR_ENTRY(name, returned, parameters, translate, call)             \
        static returned name parameters { return translate(call); }

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
                return error_whole(system_open_at_mode(                     \
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
        return error_whole(system_open_at_mode(
            ERROR_AT_HERE, path, O_WRONLY | O_CREAT | O_TRUNC, mode));
}

ERROR_ENTRY(close, b32, (b32 handle), error_whole,
            system_close(handle))

/*
        read and write return ssize_t and not int, and the difference is
        reachable: a single read of more than two gigabytes is refused by
        Linux, but a caller that truncates the count to int has already lost
        the answer for anything above that, and pread on a large file has an
        offset that certainly does not fit.
*/
ERROR_ENTRY(read, bipolar, (b32 handle, address_any buffer, positive count),
            error_wide, system_call_3(syscall(read), (positive)handle,
                                      (positive)buffer, count))
ERROR_ENTRY(write, bipolar,
            (b32 handle, const address_any buffer, positive count), error_wide,
            system_call_3(syscall(write), (positive)handle, (positive)buffer,
                          count))
ERROR_ENTRY(pread, bipolar,
            (b32 handle, address_any buffer, positive count, bipolar offset),
            error_wide, system_call_4(syscall(pread64), (positive)handle,
                                      (positive)buffer, count, (positive)offset))
ERROR_ENTRY(pwrite, bipolar,
            (b32 handle, const address_any buffer, positive count,
             bipolar offset),
            error_wide, system_call_4(syscall(pwrite64), (positive)handle,
                                      (positive)buffer, count, (positive)offset))
ERROR_ENTRY(lseek, bipolar, (b32 handle, bipolar offset, b32 whence),
            error_wide, system_call_3(syscall(lseek), (positive)handle,
                                      (positive)offset, (positive)whence))
ERROR_ENTRY(dup, b32, (b32 handle), error_whole,
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
ERROR_ENTRY(dup3, b32, (b32 from, b32 to, b32 flags), error_whole,
            system_duplicate(from, to, flags))

static b32 dup2(b32 from, b32 to)
{
        if (from == to)
        {
                //      F_GETFD is 1 everywhere here, and asking for it is the
                //      cheapest way to find out whether the descriptor exists.
                if (error_failed(system_call_3(syscall(fcntl),
                                               (positive)from, 1, 0)))
                {
                        errno = EBADF;
                        return -1;
                }

                return to;
        }

        return dup3(from, to, 0);
}

ERROR_ENTRY(pipe2, b32, (b32 address_to pair, b32 flags), error_whole,
            system_pipe(pair, flags))
ERROR_ENTRY(pipe, b32, (b32 address_to pair), error_whole,
            system_pipe(pair, 0))

var_list_entry(fcntl, b32, (b32 handle, b32 command, ...), command,
               error_whole(system_call_3(
                   syscall(fcntl), (positive)handle, (positive)command,
                   var_list_get(_variadic_list, positive))))
var_list_entry(ioctl, b32, (b32 handle, positive request, ...), request,
               error_whole(system_call_3(
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
        bipolar answer = system_call_3(syscall(ioctl), (positive)handle,
                                       0x5401, (positive)attributes);

        if (error_failed(answer))
        {
                errno = (b32) - answer;
                return 0;
        }

        return 1;
}

//      -- names in the file system ----------------------------------------

ERROR_ENTRY(fstatat, b32,
            (b32 directory, string_address path, stat_address into, b32 flags),
            error_whole, system_call_4(syscall(newfstatat),
                                       (positive)directory, (positive)path,
                                       (positive)into, (positive)flags))
ERROR_ENTRY(stat, b32, (string_address path, stat_address into), error_whole,
            system_call_4(syscall(newfstatat), ERROR_AT_HERE, (positive)path,
                          (positive)into, 0))
ERROR_ENTRY(lstat, b32, (string_address path, stat_address into), error_whole,
            system_call_4(syscall(newfstatat), ERROR_AT_HERE, (positive)path,
                          (positive)into, AT_SYMLINK_NOFOLLOW))

/*
        fstat has a syscall of its own on all three, and is not newfstatat
        with AT_EMPTY_PATH: the empty-path form needs a pointer to an empty
        string and the plain form does not, and the plain form is one number
        on every machine here.
*/
ERROR_ENTRY(fstat, b32, (b32 handle, stat_address into), error_whole,
            system_call_2(syscall(fstat), (positive)handle, (positive)into))
ERROR_ENTRY(unlinkat, b32,
            (b32 directory, string_address path, b32 flags), error_whole,
            system_remove_at(directory, path, flags))
ERROR_ENTRY(unlink, b32, (string_address path), error_whole,
            system_remove_at(ERROR_AT_HERE, path, 0))
ERROR_ENTRY(rmdir, b32, (string_address path), error_whole,
            system_remove_at(ERROR_AT_HERE, path, AT_REMOVEDIR))
ERROR_ENTRY(mkdirat, b32,
            (b32 directory, string_address path, p32 mode), error_whole,
            system_call_3(syscall(mkdirat), (positive)directory,
                          (positive)path, (positive)mode))
ERROR_ENTRY(mkdir, b32, (string_address path, p32 mode), error_whole,
            system_call_3(syscall(mkdirat), ERROR_AT_HERE, (positive)path,
                          (positive)mode))

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
            error_whole, system_call_5(syscall(renameat2),
                                       (positive)from_directory,
                                       (positive)from, (positive)to_directory,
                                       (positive)to, (positive)flags))
ERROR_ENTRY(rename, b32, (string_address from, string_address to), error_whole,
            system_call_5(syscall(renameat2), ERROR_AT_HERE, (positive)from,
                          ERROR_AT_HERE, (positive)to, 0))
ERROR_ENTRY(link, b32, (string_address from, string_address to), error_whole,
            system_call_5(syscall(linkat), ERROR_AT_HERE, (positive)from,
                          ERROR_AT_HERE, (positive)to, 0))
ERROR_ENTRY(symlink, b32, (string_address target, string_address path),
            error_whole, system_call_3(syscall(symlinkat), (positive)target,
                                       ERROR_AT_HERE, (positive)path))

/*
        readlink returns the number of bytes it placed and does not terminate
        them, which is the trap in this call and the reason it is spelled out
        here: a caller that hands it a buffer of exactly the link's length
        gets that length back and a string with no end on it.
*/
ERROR_ENTRY(readlink, bipolar,
            (string_address path, string_address into, positive size),
            error_wide, system_call_4(syscall(readlinkat), ERROR_AT_HERE,
                                      (positive)path, (positive)into, size))
ERROR_ENTRY(readlinkat, bipolar,
            (b32 directory, string_address path, string_address into,
             positive size),
            error_wide, system_call_4(syscall(readlinkat),
                                      (positive)directory, (positive)path,
                                      (positive)into, size))

/*
        access asks the kernel with the real user and group rather than the
        effective ones, which is what the name has always meant and what makes
        it the wrong call for a security decision. faccessat on asm-generic
        takes three arguments and no flags; the four argument form with
        AT_EACCESS is faccessat2, which is a much newer number and is not
        used here.
*/
ERROR_ENTRY(access, b32, (string_address path, b32 mode), error_whole,
            system_call_3(syscall(faccessat), ERROR_AT_HERE, (positive)path,
                          (positive)mode))

static b32 faccessat(b32 directory, string_address path, b32 mode, b32 flags)
{
        (void)flags;
        return error_whole(system_call_3(syscall(faccessat),
                                         (positive)directory, (positive)path,
                                         (positive)mode));
}

//      fchmodat on asm-generic is three arguments; the flags-taking form is
//      fchmodat2 and is newer than the floor this targets.
ERROR_ENTRY(chmod, b32, (string_address path, p32 mode), error_whole,
            system_call_3(syscall(fchmodat), ERROR_AT_HERE, (positive)path,
                          (positive)mode))
ERROR_ENTRY(fchmod, b32, (b32 handle, p32 mode), error_whole,
            system_call_2(syscall(fchmod), (positive)handle, (positive)mode))
ERROR_ENTRY(chown, b32, (string_address path, p32 owner, p32 group),
            error_whole, system_call_5(syscall(fchownat), ERROR_AT_HERE,
                                       (positive)path, (positive)owner,
                                       (positive)group, 0))
ERROR_ENTRY(lchown, b32, (string_address path, p32 owner, p32 group),
            error_whole, system_call_5(syscall(fchownat), ERROR_AT_HERE,
                                       (positive)path, (positive)owner,
                                       (positive)group, AT_SYMLINK_NOFOLLOW))
ERROR_ENTRY(fchown, b32, (b32 handle, p32 owner, p32 group), error_whole,
            system_call_3(syscall(fchown), (positive)handle, (positive)owner,
                          (positive)group))
ERROR_ENTRY(truncate, b32, (string_address path, bipolar length), error_whole,
            system_call_2(syscall(truncate), (positive)path, (positive)length))
ERROR_ENTRY(ftruncate, b32, (b32 handle, bipolar length), error_whole,
            system_call_2(syscall(ftruncate), (positive)handle,
                          (positive)length))
ERROR_ENTRY(fsync, b32, (b32 handle), error_whole,
            system_call_1(syscall(fsync), (positive)handle))
ERROR_ENTRY(fdatasync, b32, (b32 handle), error_whole,
            system_call_1(syscall(fdatasync), (positive)handle))
ERROR_ENTRY(chdir, b32, (string_address path), error_whole,
            system_call_1(syscall(chdir), (positive)path))
ERROR_ENTRY(fchdir, b32, (b32 handle), error_whole,
            system_call_1(syscall(fchdir), (positive)handle))

/*
        getcwd is the one call here whose C shape and syscall shape disagree
        about what success looks like.

        The kernel returns the number of bytes it wrote, including the
        terminator. C returns the buffer, or a null pointer with errno set --
        and ERANGE specifically when the buffer was too small, which the
        kernel already reports as -ERANGE. So the translation is not
        error_whole's: the count is discarded and the buffer comes back.

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

        if (error_failed(wrote))
        {
                errno = (b32) - wrote;
                return null;
        }

        return into;
}

ERROR_ENTRY(getdents64, b32, (b32 handle, address_any into, positive size),
            error_whole, system_call_3(syscall(getdents64), (positive)handle,
                                       (positive)into, size))

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
        return (address_any)error_wide(
            system_call_6(syscall(mmap), (positive)hint, length,
                          (positive)protection, (positive)flags,
                          (positive)handle, (positive)offset));
}

ERROR_ENTRY(munmap, b32, (address_any address, positive length), error_whole,
            system_call_2(syscall(munmap), (positive)address, length))
ERROR_ENTRY(mprotect, b32,
            (address_any address, positive length, b32 protection), error_whole,
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

ERROR_ENTRY(kill, b32, (b32 process, b32 signal), error_whole,
            system_call_2(syscall(kill), (positive)process, (positive)signal))
ERROR_ENTRY(execve, b32,
            (string_address path, string_address address_to arguments,
             string_address address_to environment),
            error_whole, system_call_3(syscall(execve), (positive)path,
                                       (positive)arguments,
                                       (positive)environment))

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
ERROR_ENTRY(fork, b32, (void), error_whole,
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
            error_whole, system_call_4(syscall(wait4), (positive)process,
                                       ERROR_STATUS_ADDRESS(status),
                                       (positive)options, (positive)usage))
ERROR_ENTRY(waitpid, b32,
            (b32 process, b32 address_to status, b32 options), error_whole,
            system_call_4(syscall(wait4), (positive)process,
                          ERROR_STATUS_ADDRESS(status), (positive)options, 0))
ERROR_ENTRY(wait, b32, (b32 address_to status), error_whole,
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

ERROR_ENTRY(setsid, b32, (void), error_whole, system_call(syscall(setsid)))
ERROR_ENTRY(sync, b32, (void), error_whole, system_call(syscall(sync)))

#undef ERROR_ENTRY

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_ERROR
