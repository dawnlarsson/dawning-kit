/*
        The checksum utilities' optional Linux AF_ALG compatibility backend.

        One AF_ALG transform answers every installed name below.  A regular
        file that fits Linux's one-transfer ceiling goes from the page cache
        into the hash socket in one sendfile call; large regular files stay
        kernel-only through splice.  The shared file-transfer block is the
        uncommon fallback for descriptors a transfer primitive cannot move.

        AF_ALG is deprecated upstream and is not enabled by Moonwater's
        default kernel profiles.  This backend remains useful on distro
        kernels that already expose it and in explicitly controlled images;
        the intended native replacement is a narrow Moonwater fd-hash ABI,
        not the generic unprivileged crypto socket surface.

        There is deliberately no second software MD5, SHA or BLAKE2 stack
        here.  Duplicating those cores would make the multicall image larger
        and give this project two implementations to tune and audit.
*/

#if defined(LINUX)

#define CHECKSUM_AF_ALG 38
#define CHECKSUM_SOCK_SEQPACKET 5
#define CHECKSUM_SPLICE_MOVE 1
#define CHECKSUM_SPLICE_MORE 4
#define CHECKSUM_SPLICE_BLOCK (1 << 20)
#define CHECKSUM_MSG_MORE 0x8000
#define CHECKSUM_ERROR_INTERRUPTED (-4)
#define CHECKSUM_ERROR_IO (-5)

typedef struct
{
        p16 family;
        p8 type[14];
        p32 feature;
        p32 mask;
        p8 name[64];
} checksum_socket_address;

_Static_assert(sizeof(checksum_socket_address) == 88,
               "sockaddr_alg is 88 bytes");

typedef struct
{
        string_address command;
        string_address type;
        string_address kernel;
        string_address label;
        positive bytes;
        bool variable_length;
} checksum_algorithm;

static const checksum_algorithm checksum_algorithms[] = {
    {(string_address) "b2sum", (string_address) "blake2b",
     (string_address) "blake2b-512", (string_address) "BLAKE2b", 64, true},
    {(string_address) "md5sum", (string_address) "md5", (string_address) "md5",
     (string_address) "MD5", 16, false},
    {(string_address) "sha1sum", (string_address) "sha1", (string_address) "sha1",
     (string_address) "SHA1", 20, false},
    {(string_address) "sha224sum", (string_address) "sha224",
     (string_address) "sha224", (string_address) "SHA224", 28, false},
    {(string_address) "sha256sum", (string_address) "sha256",
     (string_address) "sha256", (string_address) "SHA256", 32, false},
    {(string_address) "sha384sum", (string_address) "sha384",
     (string_address) "sha384", (string_address) "SHA384", 48, false},
    {(string_address) "sha512sum", (string_address) "sha512",
     (string_address) "sha512", (string_address) "SHA512", 64, false},
};

/* BLAKE2 alone exposes length; every other sum uses the common tail. */
static const file_long checksum_longs[] = {
    {(string_address) "length", 'l'},
    {(string_address) "binary", 'b'},
    {(string_address) "check", 'c'},
    {(string_address) "ignore-missing", 'i'},
    {(string_address) "quiet", 'q'},
    {(string_address) "status", 's'},
    {(string_address) "strict", 'S'},
    {(string_address) "tag", 'T'},
    {(string_address) "text", 't'},
    {(string_address) "warn", 'w'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static bool checksum_binary;
static bool checksum_warn;
// Which of -b/-t was given last, and whether either was: GNU refuses --tag
// with an explicit --text and both with --check.
static bool checksum_text_given;
static bool checksum_mode_given;
// The last of --status, --warn and --quiet wins, as in GNU.
static p8 checksum_verify_mode;
// Output shapes: NUL-terminated unescaped lines, base64 or raw digests.
static bool checksum_zero;
static bool checksum_base64;
static bool checksum_raw;

static bool checksum_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'b')
        {
                checksum_binary = true;
                checksum_text_given = false;
                checksum_mode_given = true;
        }
        else if (letter == 't')
        {
                checksum_binary = false;
                checksum_text_given = true;
                checksum_mode_given = true;
        }
        if (letter == 'w' || letter == 'q' || letter == 's')
        {
                checksum_warn = letter == 'w';
                checksum_verify_mode = letter;
        }

        return true;
}

static fn checksum_modes_reset()
{
        checksum_binary = false;
        checksum_warn = false;
        checksum_text_given = false;
        checksum_mode_given = false;
        checksum_verify_mode = 0;
        checksum_zero = false;
        checksum_base64 = false;
        checksum_raw = false;
}

/* coreutils' complaint about an option out of place, with its usage hint. */
static b32 checksum_usage_error(string_address command, string_address message)
{
        text_flush();
        return text_done(string_report(writer_stderr, 1,
                                       "%s: %s\nTry '%s --help' for more information.\n",
                                       command, message, command));
}

static fn checksum_base64_put(p8 address_to digest, positive length)
{
        static const p8 alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        for (positive at = 0; at < length; at += 3)
        {
                positive have = min(length - at, (positive)3);
                positive word = (positive)digest[at] << 16 |
                                (have > 1 ? (positive)digest[at + 1] << 8 : 0) |
                                (have > 2 ? (positive)digest[at + 2] : 0);

                text_put_character(alphabet[(word >> 18) & 63]);
                text_put_character(alphabet[(word >> 12) & 63]);
                text_put_character(have > 1 ? alphabet[(word >> 6) & 63] : '=');
                text_put_character(have > 2 ? alphabet[word & 63] : '=');
        }
}

static string_address checksum_called()
{
        string_address called = program_argument(0);
        string_address slash = called ? string_last_of(called, '/') : null;

        return slash ? slash + 1 : called;
}

static const checksum_algorithm address_to checksum_algorithm_find(
    string_address name, bool type)
{
        for (positive i = 0; i < array_count(checksum_algorithms); i++)
                if (string_equals(name, type ? checksum_algorithms[i].type
                                             : checksum_algorithms[i].command))
                        return checksum_algorithms + i;

        return null;
}

static bipolar checksum_kernel_open(const checksum_algorithm address_to algorithm)
{
        checksum_socket_address address;
        bipolar handle;

        memory_fill(address_of address, 0, sizeof(address));
        address.family = CHECKSUM_AF_ALG;
        memory_copy(address.type, "hash", 5);
        memory_copy(address.name, algorithm->kernel,
                    string_length(algorithm->kernel) + 1);

        handle = socket_new(CHECKSUM_AF_ALG,
                            CHECKSUM_SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return handle;

        bipolar bound = socket_bind((b32)handle, address_of address,
                                    sizeof(address));

        if (bound < 0)
        {
                system_close((positive)handle);
                return bound;
        }

        return handle;
}

static bipolar checksum_operation_open(bipolar transform)
{
        bipolar answer;

        do
                answer = socket_accept((b32)transform, null, null,
                                       SOCK_CLOEXEC);
        while (answer == CHECKSUM_ERROR_INTERRUPTED);

        return answer;
}

/* All buffered pieces carry MSG_MORE.  Reading the digest is AF_ALG's
   explicit finalisation, so no one-byte sentinel or empty software block is
   inserted into the stream. */
static bipolar checksum_send_more(bipolar operation, address_any bytes,
                                   positive length)
{
        positive sent = 0;

        while (sent < length)
        {
                bipolar wrote = socket_send((b32)operation,
                                             (p8 address_to)bytes + sent,
                                             length - sent, CHECKSUM_MSG_MORE,
                                             null, 0);

                if (wrote == CHECKSUM_ERROR_INTERRUPTED)
                        continue;
                if (wrote <= 0)
                        return wrote ? wrote : CHECKSUM_ERROR_IO;

                sent += (positive)wrote;
        }

        return 0;
}

/* The fallback uses cp's already-resident transfer block and the library's
   EINTR-safe reader.  It exists for procfs, devices and restricted kernels;
   ordinary files never reach it on a kernel with sendfile or splice. */
static bipolar checksum_buffered(bipolar input, bipolar operation)
{
        for (;;)
        {
                bipolar got = system_read_retry((positive)input, file_transfer,
                                                 FILE_TRANSFER_SIZE);

                if (got < 0)
                        return got;
                if (!got)
                        return 0;

                bipolar sent = checksum_send_more(operation, file_transfer,
                                                   (positive)got);
                if (sent < 0)
                        return sent;
        }
}

/* Large regular files remain in the kernel.  At least one side of splice
   must be a pipe, hence the two moves per block. */
static bipolar checksum_splice(bipolar input, bipolar operation)
{
        b32 ends[2];
        bipolar made;

        do
                made = system_call_2(syscall(pipe2), (positive)ends,
                                     O_CLOEXEC);
        while (made == CHECKSUM_ERROR_INTERRUPTED);

        if (made < 0)
                return made;

        bipolar answer = 0;

        for (;;)
        {
                bipolar taken;

                do
                        taken = system_call_6(
                            syscall(splice), (positive)input, 0,
                            (positive)ends[1], 0, CHECKSUM_SPLICE_BLOCK,
                            CHECKSUM_SPLICE_MOVE | CHECKSUM_SPLICE_MORE);
                while (taken == CHECKSUM_ERROR_INTERRUPTED);

                if (taken <= 0)
                {
                        answer = taken;
                        break;
                }

                positive left = (positive)taken;

                while (left)
                {
                        bipolar moved;

                        do
                                moved = system_call_6(
                                    syscall(splice), (positive)ends[0], 0,
                                    (positive)operation, 0, left,
                                    CHECKSUM_SPLICE_MOVE |
                                        CHECKSUM_SPLICE_MORE);
                        while (moved == CHECKSUM_ERROR_INTERRUPTED);

                        if (moved <= 0)
                        {
                                answer = moved ? moved : CHECKSUM_ERROR_IO;
                                goto done;
                        }

                        left -= (positive)moved;
                }
        }

done:
        system_close((positive)ends[0]);
        system_close((positive)ends[1]);
        return answer;
}

static bipolar checksum_digest_read(bipolar operation,
                                     p8 address_to digest,
                                     positive length)
{
        bipolar got = system_read_retry((positive)operation, digest, length);

        if (got == (bipolar)length)
                return 0;

        return got < 0 ? got : CHECKSUM_ERROR_IO;
}

/* One accepted socket is one digest.  A failed fast path is discarded before
   retrying from the descriptor's original position, so a partial send can
   never become the prefix of the fallback digest. */
static bipolar checksum_hash_descriptor(bipolar transform, bipolar input,
                                         p8 address_to digest,
                                         positive digest_length,
                                         bool starts_at_zero)
{
        bipolar operation = checksum_operation_open(transform);

        if (operation < 0)
                return operation;

        file_facts facts;
        bool regular = system_stat_at((b32)input, (string_address) "",
                                      AT_EMPTY_PATH | AT_NO_AUTOMOUNT,
                                      STATX_BASIC, address_of facts) == 0 &&
                       (facts.mode & MODE_FORMAT) == MODE_FILE;
        bipolar start = regular
                            ? starts_at_zero
                                  ? 0
                                  : system_seek(input, 0, FILE_SEEK_CUR)
                            : -1;
        regular = regular && start >= 0;
        bipolar moved = CHECKSUM_ERROR_IO;

        for (positive attempt = 0; regular && attempt < 2; attempt++)
        {
                if (!attempt)
                {
                        p64 remaining = facts.size > (p64)start
                                            ? facts.size - (p64)start : 0;

                        /* Asking for the ceiling, rather than the sampled
                           size, includes a file that grows before this trap.
                           A ceiling-sized answer was not EOF and is retried
                           through the streaming path. */
                        if (remaining < FILE_KERNEL_COPY_SIZE)
                        {
                                moved = file_send_range_once(
                                    input, null, operation, FILE_KERNEL_COPY_SIZE);
                                if (moved >= (bipolar)remaining &&
                                    moved < FILE_KERNEL_COPY_SIZE)
                                        goto digest;
                                if (moved < 0 && !file_copy_range_fallback(moved))
                                        goto done;
                        }
                }
                else
                {
                        moved = checksum_splice(input, operation);
                        if (!moved)
                                goto digest;
                        if (!file_copy_range_fallback(moved))
                                goto done;
                }

                // Each failed transport discards its accepted digest before
                // rewinding the input, whether the next try is splice or the
                // buffered floor. No partial prefix survives into a retry.
                system_close((positive)operation);
                if (system_seek(input, start, FILE_SEEK_SET) < 0)
                        return attempt ? moved : CHECKSUM_ERROR_IO;

                operation = checksum_operation_open(transform);
                if (operation < 0)
                        return operation;
        }

        moved = checksum_buffered(input, operation);
        if (moved < 0)
                goto done;

digest:
        moved = checksum_digest_read(operation, digest, digest_length);
done:
        system_close((positive)operation);
        return moved;
}

static bipolar checksum_hash_path(bipolar transform, string_address path,
                                   p8 address_to digest,
                                   positive digest_length)
{
        bool standard = !path ||
                        (string_is(path, '-') && !string_get(path + 1));
        bipolar input;

        if (standard)
                input = 0;
        else
        {
                do
                        input = system_open_at(AT_FDCWD, path,
                                               FILE_READ | O_CLOEXEC);
                while (input == CHECKSUM_ERROR_INTERRUPTED);

                if (input < 0)
                        return input;
        }

        bipolar answer = checksum_hash_descriptor(transform, input, digest,
                                                   digest_length, !standard);

        if (!standard)
                system_close((positive)input);

        return answer;
}

static bool checksum_filename_escaped(string_address name)
{
        positive length = string_length(name);

        return memory_first_of(name, '\\', length) ||
               memory_first_of(name, '\n', length) ||
               memory_first_of(name, '\r', length);
}

/* GNU shell-quotes a name in a verification report when a shell would not
   take it whole: blanks, controls, the metacharacters, and a leading # or ~. */
static bool checksum_filename_special(string_address name)
{
        for (string_address at = name; string_get(at); at++)
        {
                p8 byte = string_get(at);

                if (byte <= ' ' || byte >= 127 ||
                    string_first_of("!\"$&'()*;<>?[\\]^`{|}", byte))
                        return true;
                if ((byte == '#' || byte == '~') && at == name)
                        return true;
        }
        return false;
}

static fn checksum_filename_put(string_address name, bool escaped)
{
        if (!escaped)
        {
                text_put_string(name);
                return;
        }

        string_address from = name;

        while (string_get(from))
        {
                string_address stop = string_first_of_set(from, "\\\n\r");

                if (!stop)
                {
                        text_put_string(from);
                        break;
                }

                text_put(from, (positive)(stop - from));
                text_put_character('\\');
                text_put_character(*stop == '\n' ? 'n' : *stop == '\r' ? 'r' : '\\');
                from = stop + 1;
        }
}

/* The check result is a human-facing shell word in coreutils 9.11, not the
   portable backslash record used when a manifest is written.  Keep the shell
   quoting policy in its existing writer and add only the control-byte islands
   that quotearg spells as $'...'.  The decoded name lives in text_line, so a
   temporary terminator can expose each printable span without allocating or
   building a second output buffer. */
fn shell_quoted(writer write, string_address value);

static fn checksum_check_filename_put(string_address name)
{
        if (!checksum_filename_special(name))
        {
                text_put_string(name);
                return;
        }

        p8 address_to step = (p8 address_to)name;

        while (*step)
        {
                p8 address_to stop = step;
                while (*stop && *stop >= ' ' && *stop != 127)
                        stop++;

                if (stop > step)
                {
                        p8 held = *stop;
                        *stop = end;
                        shell_quoted(text_put, step);
                        *stop = held;
                        step = stop;
                }

                if (!*step)
                        break;

                text_put_string("$'");
                do
                {
                        p8 escaped[4];
                        positive length = ls_escape_byte(*step++, escaped,
                                                         false);
                        text_put(escaped, length);
                }
                while (*step && (*step < ' ' || *step == 127));
                text_put_character('\'');
        }
}

static fn checksum_hex_put(p8 address_to digest, positive length)
{
        p8 text[128];
        text_put(text, memory_into_hex(text, digest, length));
}

static fn checksum_digest_put(p8 address_to digest, positive length)
{
        if (checksum_base64)
                checksum_base64_put(digest, length);
        else
                checksum_hex_put(digest, length);
}

static fn checksum_line_put(const checksum_algorithm address_to algorithm,
                            p8 address_to digest, string_address name, bool tagged)
{
        // --raw is the digest's bytes and nothing else.
        if (checksum_raw)
        {
                text_put(digest, algorithm->bytes);
                return;
        }

        // --zero disables the escaping that exists for newline records.
        bool escaped = !checksum_zero && checksum_filename_escaped(name);

        if (escaped)
                text_put_character('\\');

        if (tagged)
        {
                text_put_string(algorithm->label);
                text_put_string(" (");
        }
        else
        {
                checksum_digest_put(digest, algorithm->bytes);
                text_put_character(' ');
                text_put_character(checksum_binary ? '*' : ' ');
        }
        checksum_filename_put(name, escaped);
        if (tagged)
        {
                text_put_string(") = ");
                checksum_digest_put(digest, algorithm->bytes);
        }
        text_put_character(checksum_zero ? '\0' : '\n');
}

/* cksum's collected operands and the named sums' argv tail differ only at
   the input boundary; hashing, errors and escaped line output are shared. */
static b32 checksum_generate(const checksum_algorithm address_to algorithm,
                             bipolar transform, positive first, bool tagged,
                             bool from_files)
{
        positive count = (positive)program_argument_count();
        positive inputs = from_files ? (positive)text_input_count()
                                     : first < count ? count - first : 1;
        b32 answer = 0;

        for (positive i = 0; i < inputs; i++)
        {
                string_address name = from_files ? text_file_name(i)
                    : first < count ? program_argument((b32)(first + i)) : null;
                name = name ? name : (string_address) "-";
                p8 digest[64];
                bipolar hashed = checksum_hash_path(
                    transform, name, digest, algorithm->bytes);

                if (hashed < 0)
                {
                        string_diagnostic(address_of text_diagnostic, 0, name, file_reason(hashed));
                        answer = 1;
                        continue;
                }

                checksum_line_put(algorithm, digest, name, tagged);
        }
        return answer;
}

/* The manifest's name as GNU writes it in a diagnostic: quoted when a
   shell would not take it whole, into a bounded buffer the callers hand to
   the shared formatter. */
static string_address checksum_quoted_name(string_address name, p8 address_to into,
                                           positive room)
{
        if (!checksum_filename_special(name))
                return name;

        positive length = string_length(name);

        if (length + 3 > room)
                return name;

        into[0] = '\'';
        memory_copy_apart(into + 1, name, length);
        into[length + 1] = '\'';
        into[length + 2] = end;
        return (string_address)into;
}

static fn checksum_check_result_put(string_address name,
                                    string_address result)
{
        checksum_check_filename_put(name);
        text_put_string(": ");
        text_put_string(result);
        text_put_character('\n');
}

/* Decode a normal GNU checksum record in place.  Tagged and NUL records are
   refused at option parsing, so the only escapes here are the two GNU emits
   for portable newline-delimited output. */
static bool checksum_line_parse(const checksum_algorithm address_to algorithm,
                                p8 address_to expected,
                                string_address address_to filename)
{
        positive at = 0;

        // A record from a Windows editor ends in CR LF; GNU drops the CR.
        if (text_line_length && text_line[text_line_length - 1] == '\r')
                text_line_length--;

        bool escaped = text_line_length && text_line[0] == '\\';

        if (escaped)
                at++;

        positive digits = algorithm->bytes * 2;
        positive label_length = string_length(algorithm->label);
        positive digest_at = at;
        p8 address_to name;

        if (text_line_length >= at + label_length + 6 + digits &&
            !string_compare_max(text_line + at, algorithm->label, label_length) &&
            text_line[at + label_length] == ' ' &&
            text_line[at + label_length + 1] == '(' &&
            !memory_compare(text_line + text_line_length - digits - 4, ") = ", 4))
        {
                // The BSD tagged record: LABEL (name) = digest.
                name = text_line + at + label_length + 2;
                digest_at = text_line_length - digits;
                text_line[digest_at - 4] = end;
        }
        else
        {
                if (text_line_length < at + digits + 2)
                        return false;

                digest_at = at;
                at += digits;
                // One space, then the mode marker: a second space, or the
                // asterisk of a binary record.
                if (text_line[at] != ' ' ||
                    (text_line[at + 1] != ' ' && text_line[at + 1] != '*'))
                        return false;

                at += 2;

                text_line[text_line_length] = end;
                name = text_line + at;
        }

        for (positive i = 0; i < algorithm->bytes; i++)
        {
                positive high = digit_known(text_line[digest_at + i * 2], 16);
                positive low = digit_known(text_line[digest_at + i * 2 + 1], 16);

                if (high >= 16 || low >= 16)
                        return false;

                expected[i] = (p8)((high << 4) | low);
        }

        if (escaped)
        {
                p8 address_to from = name;
                p8 address_to into = name;

                while (*from)
                {
                        if (*from != '\\')
                        {
                                *into++ = *from++;
                                continue;
                        }

                        from++;
                        if (*from == 'n')
                                *into++ = '\n';
                        else if (*from == 'r')
                                *into++ = '\r';
                        else if (*from == '\\')
                                *into++ = '\\';
                        else
                                return false;

                        from++;
                }

                *into = end;
        }

        address_to filename = name;
        return true;
}

static b32 checksum_verify(const checksum_algorithm address_to algorithm,
                           bipolar transform, file_taking address_to taking)
{
        bool quiet = checksum_verify_mode == 'q';
        bool status = checksum_verify_mode == 's';
        bool strict = (taking->flags & FILE_FLAG('S')) != 0;
        bool ignore_missing = (taking->flags & FILE_FLAG('i')) != 0;
        positive manifests = taking->first < (positive)program_argument_count()
                                 ? (positive)program_argument_count() - taking->first
                                 : 1;
        bool failed = false;

        for (positive m = 0; m < manifests; m++)
        {
                string_address manifest = taking->first <
                                                  (positive)program_argument_count()
                                              ? program_argument((b32)(taking->first + m))
                                              : null;

                if (!text_open(manifest))
                {
                        failed = true;
                        continue;
                }
                if (!manifest || (manifest[0] == '-' && !manifest[1]))
                        manifest = (string_address) "'standard input'";

                positive line = 0;
                bool read_failed = false;
                positive malformed = 0;
                positive formatted = 0;
                positive mismatched = 0;
                positive unreadable = 0;
                positive verified = 0;

                while (text_line_next())
                {
                        p8 expected[64];
                        p8 digest[64];
                        string_address filename;

                        line++;
                        // An empty record is passed over in silence.
                        if (!text_line_length)
                                continue;
                        if (!checksum_line_parse(algorithm, expected,
                                                 address_of filename))
                        {
                                malformed++;
                                if (checksum_warn)
                                {
                                        text_flush();
                                     {
                                        p8 quoted[FILE_PATH_MAX + 4];

                                        string_format(log_error, "%s: %s: %p: improperly formatted %s checksum line\n",
                                                      algorithm->command,
                                                      checksum_quoted_name(manifest, quoted, sizeof(quoted)),
                                                      line, algorithm->label);
                                }
                                }
                                continue;
                        }

                        formatted++;

                        bipolar hashed = checksum_hash_path(
                            transform, filename, digest, algorithm->bytes);

                        if (hashed == -ERROR_NO_ENTRY && ignore_missing)
                                continue;

                        if (hashed < 0)
                        {
                                unreadable++;
                                failed = true;

                                string_diagnostic(address_of text_diagnostic, 0, filename, file_reason(hashed));
                                if (!status)
                                        checksum_check_result_put(
                                            filename,
                                            (string_address) "FAILED open or read");
                                continue;
                        }

                        verified++;
                        if (memory_compare(expected, digest, algorithm->bytes))
                        {
                                mismatched++;
                                failed = true;
                                if (!status)
                                        checksum_check_result_put(
                                            filename, (string_address) "FAILED");
                        }
                        else if (!quiet && !status)
                                checksum_check_result_put(filename,
                                                          (string_address) "OK");
                }

                if (text_input.failed)
                {
                        // The shared reader has already named it; GNU says
                        // nothing further about a manifest it cannot read.
                        failed = true;
                        read_failed = true;
                }

                text_close();

                // Every manifest has its own format/verification contract;
                // a valid earlier file cannot make an empty later one valid.
                if (!status)
                {
                        if (malformed && formatted)
                        {
                                text_flush();
                                string_format(log_error, "%s: WARNING: %p%s\n", algorithm->command, malformed, malformed == 1 ? (string_address) " line is improperly formatted" : (string_address) " lines are improperly formatted");
                        }
                        if (unreadable)
                        {
                                text_flush();
                                string_format(log_error, "%s: WARNING: %p%s\n", algorithm->command, unreadable, unreadable == 1 ? (string_address) " listed file could not be read" : (string_address) " listed files could not be read");
                        }
                        if (mismatched)
                        {
                                text_flush();
                                string_format(log_error, "%s: WARNING: %p%s\n", algorithm->command, mismatched, mismatched == 1 ? (string_address) " computed checksum did NOT match" : (string_address) " computed checksums did NOT match");
                        }
                }

                if (!verified && !unreadable && !read_failed)
                {
                        p8 quoted[FILE_PATH_MAX + 4];

                        failed = true;
                        if (!status || !formatted)
                                string_diagnostic(address_of text_diagnostic, 0,
                                    checksum_quoted_name(manifest, quoted, sizeof(quoted)),
                                    ignore_missing && formatted
                                        ? (string_address) "no file was verified"
                                        : (string_address) "no properly formatted checksum lines found");
                }

                if (strict && malformed)
                        failed = true;
        }

        return failed ? 1 : 0;
}

static b32 checksum_main()
{
        string_address command = checksum_called();
        const checksum_algorithm address_to algorithm =
            checksum_algorithm_find(command, false);

        if (!algorithm)
                return 1;

        text_begin(command);
        checksum_modes_reset();

        file_taking taking = {
            .program = command,
            .allowed = algorithm->variable_length
                           ? (string_address) "bctlwz"
                           : (string_address) "bctwz",
            .valued = algorithm->variable_length
                          ? (string_address) "l"
                          : null,
            .longs = checksum_longs + !algorithm->variable_length,
            .seen = checksum_option_seen,
        };

        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.flags & FILE_FLAG('l'))
                return text_done(string_diagnostic(address_of text_diagnostic, 1, null, "variable BLAKE2 lengths are not supported"));

        bool checking = (taking.flags & FILE_FLAG('c')) != 0;
        bool tagged = (taking.flags & FILE_FLAG('T')) != 0;
        checksum_zero = (taking.flags & FILE_FLAG('z')) != 0;

        // coreutils' own refusals, in its order and words.
        if (checksum_zero && checking)
                return checksum_usage_error(command, "the --zero option is not supported when verifying checksums");
        if (tagged && checking)
                return checksum_usage_error(command, "the --tag option is meaningless when verifying checksums");
        if (checking && checksum_mode_given)
                return checksum_usage_error(command, "the --binary and --text options are meaningless when verifying checksums");
        if (!checking)
        {
                if (taking.flags & FILE_FLAG('i'))
                        return checksum_usage_error(command, "the --ignore-missing option is meaningful only when verifying checksums");
                if (checksum_verify_mode == 's')
                        return checksum_usage_error(command, "the --status option is meaningful only when verifying checksums");
                if (checksum_verify_mode == 'w')
                        return checksum_usage_error(command, "the --warn option is meaningful only when verifying checksums");
                if (checksum_verify_mode == 'q')
                        return checksum_usage_error(command, "the --quiet option is meaningful only when verifying checksums");
                if (taking.flags & FILE_FLAG('S'))
                        return checksum_usage_error(command, "the --strict option is meaningful only when verifying checksums");
        }
        if (tagged && checksum_text_given)
                return checksum_usage_error(command, "--tag does not support --text mode");

        bipolar transform = checksum_kernel_open(algorithm);

        if (transform < 0)
                return text_done(string_diagnostic(address_of text_diagnostic, 1, null, "kernel AF_ALG hash support or requested algorithm is unavailable"));

        b32 answer = 0;

        if (checking)
                answer = checksum_verify(algorithm, transform,
                                         address_of taking);
        else
                answer = checksum_generate(algorithm, transform, taking.first, tagged, false);

        system_close((positive)transform);
        return text_done(answer);
}

#else

/* AF_ALG is a Linux ABI.  Keep non-Linux compilation honest instead of
   substituting an unreviewed software implementation. */
static b32 checksum_main()
{
        return 1;
}

#endif
