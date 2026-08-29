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
address_any realloc(address_any block, positive size);
fn free(address_any block);

#define stream_allocate(size) malloc(size)
#define stream_reallocate(block, size) realloc((block), (size))
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
*/
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define EOF (-1)
#define BUFSIZ 4096

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
b32 stream_at_end(stream address_to handle);
b32 stream_failed(stream address_to handle);
fn stream_clear_state(stream address_to handle);
b32 stream_set_buffering(stream address_to handle, string_address buffer,
                         b32 mode, sized size);
fn stream_set_buffer(stream address_to handle, string_address buffer);
b32 stream_descriptor(stream address_to handle);
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

/*
        The traps, one line each.

        These exist so that the reason for every syscall in the file is
        visible at the call site rather than buried in an argument list of
        casts. read goes through system_read_retry because a stream read that
        loses to a signal is not an end of file and must not be reported as
        one; write goes through system_write_all because a short write to a
        pipe is not an error either and the caller of fwrite has no way to
        resume from one.
*/
static bipolar stream_trap_read(b32 descriptor, address_any into,
                                positive length)
{
        return system_read_retry((positive)descriptor, into, length);
}

static positive stream_trap_write(b32 descriptor, address_any from,
                                  positive length)
{
        return system_write_all((positive)descriptor, from, length);
}

static bipolar stream_trap_seek(b32 descriptor, bipolar offset, b32 whence)
{
        return system_call_3(syscall(lseek), (positive)(bipolar)descriptor,
                             (positive)offset, (positive)(bipolar)whence);
}

static bipolar stream_trap_close(b32 descriptor)
{
        return system_call_1(syscall(close), (positive)(bipolar)descriptor);
}

static bipolar stream_trap_open(string_address path, b32 flags, b32 permissions)
{
        return system_call_4(syscall(openat), (positive)(bipolar)AT_FDCWD,
                             (positive)path, (positive)(bipolar)flags,
                             (positive)(bipolar)permissions);
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

        return system_call_3(syscall(ioctl), (positive)(bipolar)descriptor,
                             stream_terminal_attributes_request,
                             (positive)attributes) == 0;
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

        handle->buffer = (p8 address_to)stream_allocate(BUFSIZ);

        if (handle->buffer == null)
        {
                handle->flags |= STREAM_UNBUFFERED;
                handle->buffer = handle->single;
                handle->buffer_size = 1;
                return;
        }

        handle->flags |= STREAM_BUFFER_OURS;
        handle->buffer_size = BUFSIZ;
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
                stream_trap_seek(handle->descriptor, -(bipolar)unread, SEEK_CUR);

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

        if (mode == null)
                return false;

        for (index = 1; mode[index] != end; index++)
        {
                if (mode[index] == '+')
                        update = true;
                else if (mode[index] == 'x')
                        exclusive = true;
                else if (mode[index] == 'e')
                        close_on_exec = true;
        }

        if (mode[0] == 'r')
        {
                address_to open_flags = update ? stream_open_read_write
                                               : stream_open_read_only;
                address_to stream_flags = update
                                                  ? STREAM_READABLE | STREAM_WRITABLE
                                                  : STREAM_READABLE;
        }
        else if (mode[0] == 'w')
        {
                address_to open_flags = (update ? stream_open_read_write
                                                : stream_open_write_only) |
                                        stream_open_create | stream_open_truncate;
                address_to stream_flags = update
                                                  ? STREAM_READABLE | STREAM_WRITABLE
                                                  : STREAM_WRITABLE;
        }
        else if (mode[0] == 'a')
        {
                address_to open_flags = (update ? stream_open_read_write
                                                : stream_open_write_only) |
                                        stream_open_create | stream_open_append;
                address_to stream_flags = (update
                                                   ? STREAM_READABLE | STREAM_WRITABLE
                                                   : STREAM_WRITABLE) |
                                          STREAM_APPEND;
        }
        else
                return false;

        if (exclusive)
                address_to open_flags |= stream_open_exclusive;

        if (close_on_exec)
                address_to open_flags |= O_CLOEXEC;

        return true;
}

// Newest first, because the order fflush walks the list in is not observable
// and prepending is the only insertion with nothing to go wrong.
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
        An append stream starts at the end of the file.

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
        if (handle->flags & STREAM_APPEND)
                stream_trap_seek(handle->descriptor, 0, SEEK_END);
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

        handle = (stream address_to)stream_allocate(sizeof(stream));

        if (handle == null)
        {
                stream_trap_close((b32)descriptor);
                return null;
        }

        memory_zero(handle, sizeof(stream));
        handle->descriptor = (b32)descriptor;
        handle->flags = stream_flags | STREAM_STRUCT_OURS;
        stream_land_at_end(handle);
        stream_register(handle);
        return handle;
}

/*
        fdopen: the same stream around a descriptor somebody else opened.

        No open call, so the create and truncate bits in the mode are simply
        not acted on -- the descriptor already is what it is. The access mode
        is not verified against the descriptor either: asking the kernel would
        mean fcntl, and a mode that lies produces EBADF from the first read or
        write, which is the same diagnosis one call later.
*/
stream address_to stream_adopt(b32 descriptor, string_address mode)
{
        b32 open_flags = 0;
        p32 stream_flags = 0;
        stream address_to handle;

        if (descriptor < 0 || !stream_read_mode(mode, address_of open_flags,
                                                address_of stream_flags))
                return null;

        handle = (stream address_to)stream_allocate(sizeof(stream));

        if (handle == null)
                return null;

        memory_zero(handle, sizeof(stream));
        handle->descriptor = descriptor;
        handle->flags = stream_flags | STREAM_STRUCT_OURS;
        stream_land_at_end(handle);
        stream_register(handle);
        return handle;
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

        handle->buffer = null;
        handle->buffer_size = 0;
        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
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
static fn stream_flush_at_exit_one(stream address_to handle, positive here)
{
        if (handle->write_used != 0 && handle->owner != here)
                return;

        stream_flush_output(handle);
}

static fn stream_flush_at_exit(void)
{
        positive here = stdlib_process_identity();
        stream address_to walk;

        stream_flush_at_exit_one(address_of stream_standard_output, here);
        stream_flush_at_exit_one(address_of stream_standard_error, here);

        for (walk = stream_open_list; walk != null; walk = walk->next)
                stream_flush_at_exit_one(walk, here);
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
        found = (p8 address_to)memory_first_of(handle->buffer + handle->read_head,
                                               (b8)delimiter, available);

        if (found != null)
        {
                take = (positive)(found - (handle->buffer + handle->read_head)) + 1;

                if (take <= take_limit)
                        address_to found_delimiter = true;
        }
        else
                take = available;

        if (take > take_limit)
                take = take_limit;

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
        {
                p8 address_to fresh = (p8 address_to)stream_allocate(120);

                if (fresh == null)
                        return -1;

                address_to held = fresh;
                address_to capacity = 120;
        }

        while (!found && !ended)
        {
                positive room;
                positive taken;

                if (filled + 1 >= (positive)address_to capacity)
                {
                        positive want = memory_growth((positive)address_to capacity,
                                                      filled + 2, 120);
                        p8 address_to grown;

                        if (want == 0)
                                return -1;

                        grown = (p8 address_to)stream_reallocate(address_to held,
                                                                 want);

                        if (grown == null)
                                return -1;

                        address_to held = grown;
                        address_to capacity = (sized)want;
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

        Three things have to happen and the order of the first two matters.
        Staged output goes to the file before the position moves, or it lands
        wherever the seek left the offset. Buffered input is thrown away
        without seeking back, because the absolute seek that follows makes the
        position right regardless -- but a relative seek has to have the
        buffered bytes subtracted from its offset first, since the caller's
        idea of "here" is behind the kernel's by exactly what the buffer and
        the pushback are holding.

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

        if (whence == SEEK_CUR)
                offset -= (bipolar)((handle->read_tail - handle->read_head) +
                                    handle->pushback_used);

        stream_drop_input(handle, false);

        landed = stream_trap_seek(handle->descriptor, offset, whence);

        if (landed < 0)
                return -1;

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

        position += (bipolar)handle->write_used;
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
b32 stream_at_end(stream address_to handle)
{
        if (handle == null)
                return 0;

        return (handle->flags & STREAM_AT_END) != 0;
}

b32 stream_failed(stream address_to handle)
{
        if (handle == null)
                return 0;

        return (handle->flags & STREAM_FAILED) != 0;
}

fn stream_clear_state(stream address_to handle)
{
        if (handle == null)
                return;

        handle->flags &= ~(STREAM_AT_END | STREAM_FAILED);
}

b32 stream_descriptor(stream address_to handle)
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

        handle->buffer = null;
        handle->buffer_size = 0;
        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
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
b32 feof(stream address_to handle)
        __attribute__((alias("stream_at_end"), used));
b32 ferror(stream address_to handle)
        __attribute__((alias("stream_failed"), used));
fn clearerr(stream address_to handle)
        __attribute__((alias("stream_clear_state"), used));
b32 setvbuf(stream address_to handle, string_address buffer, b32 mode, sized size)
        __attribute__((alias("stream_set_buffering"), used));
fn setbuf(stream address_to handle, string_address buffer)
        __attribute__((alias("stream_set_buffer"), used));
b32 fileno(stream address_to handle)
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
