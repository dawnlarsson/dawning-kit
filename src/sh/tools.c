/*
        The utilities that are neither text nor files.

        dd copies with a block size and a count, diff says what changed between
        two files, ps says what is running. They share nothing with each other
        beyond not belonging anywhere else.

        The reference is the tool on the machine, not the standard. dd's
        summary, diff's choice of which of two identical lines to call the
        changed one, and the widths ps pads to are all decisions nobody writes
        down twice the same way, so they are taken from the bytes the real
        tool emits and from the source that emits them.
*/

// dd --------------------------------------------------------

#define DD_NOTRUNC 0x001
#define DD_SYNC 0x002
#define DD_NOERROR 0x004
#define DD_FSYNC 0x008
#define DD_FDATASYNC 0x010
#define DD_EXCL 0x020
#define DD_NOCREAT 0x040

#define DD_STATUS_ALL 0
#define DD_STATUS_NOXFER 1
#define DD_STATUS_NONE 2

#define DD_SIGNAL_INFO 10
#define DD_NO_SUCH_CALL 38

static positive dd_in_full;
static positive dd_in_partial;
static positive dd_out_full;
static positive dd_out_partial;
static positive dd_written;
static positive dd_status_level;
static positive dd_started;

// Set in the handler, acted on where a block boundary is, because printing
// the summary from inside the handler would land it in the middle of one.
static volatile b32 dd_info_asked;

static fn dd_info_caught(b32 number)
{
        (void)number;
        dd_info_asked = 1;
}

// Restarting, so a read that a report interrupted goes back to waiting rather
// than coming back short and being counted as a partial record.
static fn dd_listen(b32 number)
{
        positive action[4] = {(positive)dd_info_caught, SIGNAL_CATCH_FLAGS,
                              SIGNAL_CATCH_RESTORER, 0};

        system_call_4(syscall(rt_sigaction), number, (positive)address_of action, 0, 8);
}

static fn dd_say(string_address text)
{
        text_write_raw(2, (address_any)text, string_length(text));
}

static fn dd_say_number(positive value)
{
        p8 digits[24];
        positive have = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (have)
        {
                p8 one = digits[--have];

                text_write_raw(2, address_of one, 1);
        }
}

/*
        gnulib's human_readable, for the one case dd asks it for: a byte count
        with no block scaling, rounded to nearest, autoscaled, with a space
        before the unit. The rounding is what makes 999999 read as 1.0 MB
        rather than 1000.0 kB, and it is done in integers because there is no
        floating point here worth trusting.
*/
static positive dd_human(p8 address_to into, positive n, bool binary)
{
        positive base = binary ? 1024 : 1000;
        positive amount = n;
        positive tenths = 0;
        positive rounding = 0;
        positive exponent = 0;
        p8 letters[11] = {0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y', 'R', 'Q'};
        p8 digits[24];
        positive have = 0;
        positive used = 0;
        positive fraction = 0;
        bool point = false;

        if (base <= amount)
        {
                do
                {
                        positive ten = (amount % base) * 10 + tenths;
                        positive two = (ten % base) * 2 + (rounding >> 1);

                        amount /= base;
                        tenths = ten / base;
                        rounding = two < base ? ((two + rounding) != 0)
                                              : 2 + (base < two + rounding);
                        exponent++;
                }
                while (base <= amount && exponent < 10);

                if (amount < 10)
                {
                        if (2 < rounding + (tenths & 1))
                        {
                                tenths++;
                                rounding = 0;

                                if (tenths == 10)
                                {
                                        amount++;
                                        tenths = 0;
                                }
                        }

                        if (amount < 10)
                        {
                                point = true;
                                fraction = tenths;
                                tenths = 0;
                                rounding = 0;
                        }
                }
        }

        if (5 < tenths + (0 < rounding + (amount & 1)))
        {
                amount++;

                if (amount == base && exponent < 10)
                {
                        exponent++;
                        point = true;
                        fraction = 0;
                        amount = 1;
                }
        }

        if (!amount)
                digits[have++] = '0';

        while (amount)
        {
                digits[have++] = (p8)('0' + amount % 10);
                amount /= 10;
        }

        while (have)
                into[used++] = digits[--have];

        if (point)
        {
                into[used++] = '.';
                into[used++] = (p8)('0' + fraction);
        }

        into[used++] = ' ';

        if (exponent)
                into[used++] = !binary && exponent == 1 ? 'k' : letters[exponent];

        if (binary && exponent)
                into[used++] = 'i';

        into[used++] = 'B';
        into[used] = end;

        return used;
}

// A scaled count with no prefix letter is the plain number again, and dd
// leaves the parenthesis off rather than saying the same thing twice.
static bool dd_bare(p8 address_to text, positive length)
{
        return length >= 2 && text[length - 2] == ' ';
}

static fn dd_summary()
{
        if (dd_status_level == DD_STATUS_NONE)
                return;

        text_flush();

        dd_say_number(dd_in_full);
        dd_say("+");
        dd_say_number(dd_in_partial);
        dd_say(" records in\n");
        dd_say_number(dd_out_full);
        dd_say("+");
        dd_say_number(dd_out_partial);
        dd_say(" records out\n");

        if (dd_status_level == DD_STATUS_NOXFER)
                return;

        p8 si[32];
        p8 iec[32];
        positive si_length = dd_human(si, dd_written, false);
        positive iec_length = dd_human(iec, dd_written, true);

        dd_say_number(dd_written);
        dd_say(dd_written == 1 ? " byte" : " bytes");

        if (!dd_bare(si, si_length))
        {
                dd_say(" (");
                dd_say(si);

                if (!dd_bare(iec, iec_length))
                {
                        dd_say(", ");
                        dd_say(iec);
                }

                dd_say(")");
        }

        /*
                The seconds and the rate are what this machine did, not what
                the other one did, so they are printed in the shape coreutils
                prints them in and nothing here compares them.
        */
        p64 wall[2] = {0, 0};

        system_call_2(syscall(clock_gettime), 1, (positive)wall);

        positive elapsed = (positive)wall[0] * 1000000000u + (positive)wall[1] - dd_started;

        if (!elapsed)
                elapsed = 1;

        dd_say(" copied, ");

        positive whole = elapsed / 1000000000u;
        positive rest = elapsed % 1000000000u;

        dd_say_number(whole);
        dd_say(".");

        for (positive scale = 100000000u; scale; scale /= 10)
        {
                p8 one = (p8)('0' + (rest / scale) % 10);

                text_write_raw(2, address_of one, 1);
        }

        dd_say(" s, ");

        p8 rate[32];
        positive per = elapsed >= 1000000000u
                           ? dd_written / (elapsed / 1000000000u)
                           : dd_written * (1000000000u / elapsed);

        dd_human(rate, per, false);
        dd_say(rate);
        dd_say("/s\n");
}

static bool dd_size(string_address text, positive address_to out)
{
        positive total = 1;
        string_address at = text;

        if (!string_get(at))
                return false;

        while (1)
        {
                positive taken;
                positive value = string_digits(at, address_of taken);

                if (!taken)
                        return false;

                at += taken;

                positive power = 0;
                positive multiple = 1;
                bool scaled = true;

                switch (string_get(at))
                {
                case 'b': multiple = 512; scaled = false; at++; break;
                case 'c': multiple = 1; scaled = false; at++; break;
                case 'w': multiple = 2; scaled = false; at++; break;
                case 'B': multiple = 1; scaled = false; at++; break;
                case 'k':
                case 'K': power = 1; at++; break;
                case 'M':
                case 'm': power = 2; at++; break;
                case 'G':
                case 'g': power = 3; at++; break;
                case 'T':
                case 't': power = 4; at++; break;
                case 'P': power = 5; at++; break;
                case 'E': power = 6; at++; break;
                case 'Z': power = 7; at++; break;
                case 'Y': power = 8; at++; break;
                case 'R': power = 9; at++; break;
                case 'Q': power = 10; at++; break;
                default: scaled = false; break;
                }

                if (scaled)
                {
                        positive base = 1024;

                        // KiB is the binary one spelled out; KB is the
                        // decimal one, and a bare K is binary.
                        if (string_get(at) == 'i' && string_get(at + 1) == 'B')
                        {
                                at += 2;
                        }
                        else if (string_get(at) == 'B' || string_get(at) == 'D')
                        {
                                base = 1000;
                                at++;
                        }

                        for (positive i = 0; i < power; i++)
                                multiple *= base;
                }

                total *= value * multiple;

                if (string_get(at) != 'x' && string_get(at) != '*')
                        break;

                at++;
        }

        if (string_get(at))
                return false;

        address_to out = total;

        return true;
}

static bool dd_operand(string_address argument, string_address name,
                       string_address address_to value)
{
        positive i = 0;

        for (; name[i]; i++)
                if (argument[i] != name[i])
                        return false;

        if (argument[i] != '=')
                return false;

        address_to value = argument + i + 1;

        return true;
}

static bool dd_word(string_address address_to at, string_address name)
{
        string_address here = address_to at;
        positive i = string_length(name);

        if (string_compare_max(here, name, i))
                return false;

        if (here[i] && here[i] != ',')
                return false;

        address_to at = here + i + (here[i] == ',' ? 1 : 0);

        return true;
}

// A short read is not the end of the input, and a partial record is not an
// error: both are counted and the next block is asked for.
static bipolar dd_read(positive handle, p8 address_to into, positive want)
{
        return system_call_3(syscall(read), handle, (positive)into, want);
}

static positive dd_write(positive handle, p8 address_to from, positive length)
{
        positive done = 0;

        while (done < length)
        {
                bipolar wrote = system_call_3(syscall(write), handle,
                                              (positive)(from + done), length - done);

                if (wrote <= 0)
                        break;

                done += (positive)wrote;
        }

        return done;
}

static b32 tools_dd(void)
{
        string_address input = null;
        string_address output = null;
        positive ibs = 512;
        positive obs = 512;
        positive count = TEXT_UNSET;
        positive skip = 0;
        positive seek = 0;
        positive conv = 0;
        b32 status = 0;

        text_begin("dd");

        dd_in_full = dd_in_partial = dd_out_full = dd_out_partial = 0;
        dd_written = 0;
        dd_status_level = DD_STATUS_ALL;
        dd_info_asked = 0;

        {
                p64 wall[2] = {0, 0};

                system_call_2(syscall(clock_gettime), 1, (positive)wall);
                dd_started = (positive)wall[0] * 1000000000u + (positive)wall[1];
        }

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);
                string_address value;

                if (dd_operand(argument, "if", address_of value))
                        input = value;
                else if (dd_operand(argument, "of", address_of value))
                        output = value;
                else if (dd_operand(argument, "ibs", address_of value))
                {
                        if (!dd_size(value, address_of ibs))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "obs", address_of value))
                {
                        if (!dd_size(value, address_of obs))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "bs", address_of value))
                {
                        if (!dd_size(value, address_of ibs))
                                return text_error(argument, "invalid number"), 1;

                        obs = ibs;
                }
                else if (dd_operand(argument, "count", address_of value))
                {
                        if (!dd_size(value, address_of count))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "skip", address_of value))
                {
                        if (!dd_size(value, address_of skip))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "seek", address_of value))
                {
                        if (!dd_size(value, address_of seek))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "status", address_of value))
                {
                        if (string_equals(value, "none"))
                                dd_status_level = DD_STATUS_NONE;
                        else if (string_equals(value, "noxfer"))
                                dd_status_level = DD_STATUS_NOXFER;
                        else if (string_equals(value, "progress"))
                                dd_status_level = DD_STATUS_ALL;
                        else
                                return text_error(value, "invalid status level"), 1;
                }
                else if (dd_operand(argument, "conv", address_of value))
                {
                        string_address at = value;

                        while (string_get(at))
                        {
                                if (dd_word(address_of at, "notrunc"))
                                        conv |= DD_NOTRUNC;
                                else if (dd_word(address_of at, "sync"))
                                        conv |= DD_SYNC;
                                else if (dd_word(address_of at, "noerror"))
                                        conv |= DD_NOERROR;
                                else if (dd_word(address_of at, "fdatasync"))
                                        conv |= DD_FDATASYNC;
                                else if (dd_word(address_of at, "fsync"))
                                        conv |= DD_FSYNC;
                                else if (dd_word(address_of at, "excl"))
                                        conv |= DD_EXCL;
                                else if (dd_word(address_of at, "nocreat"))
                                        conv |= DD_NOCREAT;
                                else
                                        return text_error(at, "invalid conversion"), 1;
                        }
                }
                else if (dd_operand(argument, "iflag", address_of value) ||
                         dd_operand(argument, "oflag", address_of value) ||
                         dd_operand(argument, "cbs", address_of value))
                {
                        // Accepted and ignored: nothing here buffers by
                        // record or opens with anything but the defaults.
                }
                else
                {
                        text_error(argument, "unrecognized operand");
                        return 1;
                }
        }

        if (!ibs || !obs)
        {
                text_error(null, "invalid number");
                return 1;
        }

        positive in_handle = 0;
        positive out_handle = 1;

        if (input)
        {
                bipolar opened = text_open_handle(input, FILE_READ, 0);

                if (opened < 0)
                {
                        text_flush();
                        dd_say("dd: failed to open '");
                        dd_say(input);
                        dd_say("': ");
                        dd_say(file_reason(opened));
                        dd_say("\n");
                        return 1;
                }

                in_handle = (positive)opened;
        }

        if (output)
        {
                positive flags = 01;

                if (!(conv & DD_NOCREAT))
                        flags |= 0100;

                if (conv & DD_EXCL)
                        flags |= 0200;

                if (!seek && !(conv & DD_NOTRUNC))
                        flags |= O_TRUNC;

                bipolar opened = text_open_handle(output, flags, 0666);

                if (opened < 0)
                {
                        text_flush();
                        dd_say("dd: failed to open '");
                        dd_say(output);
                        dd_say("': ");
                        dd_say(file_reason(opened));
                        dd_say("\n");
                        return 1;
                }

                out_handle = (positive)opened;
        }

        p8 address_to ibuf = (p8 address_to)text_arena_take(ibs + 16);
        p8 address_to obuf = ibs == obs ? ibuf : (p8 address_to)text_arena_take(obs + 16);

        if (!ibuf || !obuf)
                return 1;

        dd_listen(DD_SIGNAL_INFO);

        if (skip)
        {
                positive want = skip * ibs;
                bipolar landed = system_call_3(syscall(lseek), in_handle, want, 0);
                bool short_of_it = false;

                if (landed >= 0)
                {
                        bipolar stop = system_call_3(syscall(lseek), in_handle, 0, 2);

                        if (stop >= 0)
                        {
                                system_call_3(syscall(lseek), in_handle, (positive)landed, 0);

                                // A size of zero is what a file that has no
                                // size to report says, so it is not a file
                                // that is too short.
                                if (stop > 0 && (positive)stop < want)
                                        short_of_it = true;
                        }
                }
                else
                {
                        positive left = want;

                        while (left)
                        {
                                positive ask = left < ibs ? left : ibs;
                                bipolar got = dd_read(in_handle, ibuf, ask);

                                if (got <= 0)
                                        break;

                                left -= (positive)got;
                        }

                        short_of_it = left != 0;
                }

                // Asked to skip past what is there. Not fatal, and said only
                // where a summary would have been said.
                if (short_of_it && dd_status_level != DD_STATUS_NONE)
                {
                        text_flush();
                        dd_say("dd: '");
                        dd_say(input ? input : (string_address) "standard input");
                        dd_say("': cannot skip to specified offset\n");
                }
        }

        if (seek)
        {
                positive want = seek * obs;
                bipolar landed = system_call_3(syscall(lseek), out_handle, want, 0);

                if (landed < 0)
                {
                        memory_fill(obuf, 0, obs);

                        for (positive i = 0; i < seek; i++)
                                dd_write(out_handle, obuf, obs);
                }
                else if (!(conv & DD_NOTRUNC))
                {
                        system_call_2(syscall(ftruncate), out_handle, want);
                }
        }

        positive held = 0;
        b32 result = 0;
        positive partial_before = 0;

        while (count != 0)
        {
                if (dd_info_asked)
                {
                        dd_info_asked = 0;
                        dd_summary();
                }

                if (count != TEXT_UNSET && dd_in_full + dd_in_partial >= count)
                        break;

                if (conv & (DD_SYNC | DD_NOERROR))
                        memory_fill(ibuf, 0, ibs);

                bipolar got = dd_read(in_handle, ibuf, ibs);

                if (!got)
                        break;

                if (got < 0)
                {
                        text_flush();
                        dd_say("dd: error reading '");
                        dd_say(input ? input : (string_address) "standard input");
                        dd_say("': ");
                        dd_say(file_reason(got));
                        dd_say("\n");

                        if (!(conv & DD_NOERROR))
                        {
                                result = 1;
                                break;
                        }

                        dd_summary();

                        positive bad = ibs - partial_before;

                        if (system_call_3(syscall(lseek), in_handle, bad, 1) < 0)
                                result = 1;

                        if ((conv & DD_SYNC) && !partial_before)
                                got = 0;
                        else
                                continue;
                }

                positive read_bytes = (positive)got;

                if (read_bytes < ibs)
                {
                        dd_in_partial++;
                        partial_before = read_bytes;

                        if (conv & DD_SYNC)
                                read_bytes = ibs;
                }
                else
                {
                        dd_in_full++;
                        partial_before = 0;
                }

                if (ibuf == obuf)
                {
                        positive wrote = dd_write(out_handle, obuf, read_bytes);

                        dd_written += wrote;

                        if (wrote != read_bytes)
                        {
                                text_flush();
                                dd_say("dd: error writing '");
                                dd_say(output ? output : (string_address) "standard output");
                                dd_say("'\n");

                                if (wrote)
                                        dd_out_partial++;

                                result = 1;
                                break;
                        }

                        if (read_bytes == ibs)
                                dd_out_full++;
                        else
                                dd_out_partial++;

                        continue;
                }

                // The input block regrouped into output blocks, which is what
                // dd is for whenever ibs and obs differ.
                for (positive at = 0; at < read_bytes;)
                {
                        positive take = obs - held;

                        if (take > read_bytes - at)
                                take = read_bytes - at;

                        memory_copy_fast(obuf + held, ibuf + at, take);
                        held += take;
                        at += take;

                        if (held < obs)
                                continue;

                        positive wrote = dd_write(out_handle, obuf, obs);

                        dd_written += wrote;
                        held = 0;

                        if (wrote != obs)
                        {
                                result = 1;
                                break;
                        }

                        dd_out_full++;
                }

                if (result)
                        break;
        }

        if (held)
        {
                positive wrote = dd_write(out_handle, obuf, held);

                dd_written += wrote;

                if (wrote)
                        dd_out_partial++;
        }

        /*
                A stream that cannot be synced says so and is a failure. A
                pipe answers the narrower call with "invalid argument", and
                dd asks the wider one rather than giving up, which is why the
                complaint about a pipe names fsync even when nobody asked for
                it.
        */
        if (conv & DD_FDATASYNC)
        {
                bipolar done = system_call_1(syscall(fdatasync), out_handle);

                if (done == -ERROR_INVALID || done == -DD_NO_SUCH_CALL)
                {
                        conv |= DD_FSYNC;
                }
                else if (done < 0)
                {
                        text_flush();
                        dd_say("dd: fdatasync failed for '");
                        dd_say(output ? output : (string_address) "standard output");
                        dd_say("': ");
                        dd_say(file_reason(done));
                        dd_say("\n");

                        result = 1;
                }
        }

        if (conv & DD_FSYNC)
        {
                bipolar done = system_call_1(syscall(fsync), out_handle);

                if (done < 0)
                {
                        text_flush();
                        dd_say("dd: fsync failed for '");
                        dd_say(output ? output : (string_address) "standard output");
                        dd_say("': ");
                        dd_say(file_reason(done));
                        dd_say("\n");

                        result = 1;
                }
        }

        if (out_handle != 1)
                system_call_1(syscall(close), out_handle);

        if (in_handle != 0)
                system_call_1(syscall(close), in_handle);

        text_flush();
        dd_summary();

        return result;
}

// diff ------------------------------------------------------

#define DIFF_SPACE_NONE 0
#define DIFF_SPACE_CHANGE 4
#define DIFF_SPACE_ALL 5

#define DIFF_NORMAL 0
#define DIFF_UNIFIED 1

#define DIFF_LARGE ((positive)1 << 60)

static bool diff_icase;
static positive diff_space;
static bool diff_blank_lines;
static bool diff_brief;
static bool diff_recursive;
static bool diff_new_file;
static bool diff_text;
static positive diff_style;
static positive diff_context = 3;
static string_address diff_labels[2];
static p8 diff_switches[256];
static positive diff_switches_used;
static b32 diff_result;
static bool diff_titled;

typedef struct
{
        p8 address_to base;
        positive size;
        bool incomplete;
        positive lines;
        positive address_to at;
        positive prefix;
        positive count;
        b32 address_to class;
        p8 address_to changed;
        b32 address_to kept;
        positive address_to real;
        positive keeps;
        b64 modified_seconds;
        positive modified_nanoseconds;
        bool missing;
} diff_side;

static diff_side diff_files[2];

static fn diff_writer(address_any data, positive length)
{
        text_put(data, length ? length : string_length((string_address)data));
}

// Reading ---------------------------------------------------

static bool diff_slurp(diff_side address_to side, string_address path)
{
        bipolar handle = 0;

        side->base = null;
        side->size = 0;
        side->incomplete = false;
        side->missing = false;
        side->modified_seconds = 0;
        side->modified_nanoseconds = 0;

        if (path)
        {
                file_facts facts;

                if (file_look_at(path, address_of facts))
                {
                        side->modified_seconds = facts.modified.seconds;
                        side->modified_nanoseconds = facts.modified.nanoseconds;
                }

                handle = text_open_handle(path, FILE_READ, 0);

                if (handle < 0)
                {
                        if (diff_new_file && handle == -ERROR_NO_ENTRY)
                        {
                                side->base = (p8 address_to)text_arena_take(16);
                                side->at = (positive address_to)text_arena_take(2 * sizeof(positive));

                                if (!side->base || !side->at)
                                        return false;

                                side->at[0] = 0;
                                side->missing = true;

                                return true;
                        }

                        text_flush();
                        text_error_raw("diff: ");
                        text_error_raw(path);
                        text_error_raw(": ");
                        text_error_raw(file_reason(handle));
                        text_error_raw("\n");
                        return false;
                }
        }

        p8 address_to start = null;
        positive have = 0;

        while (1)
        {
                p8 address_to block = (p8 address_to)text_arena_take(TEXT_READ_MAX);

                if (!block)
                        return false;

                if (!start)
                        start = block;

                bipolar got = system_call_3(syscall(read), (positive)handle,
                                            (positive)block, TEXT_READ_MAX);

                if (got <= 0)
                        break;

                have += (positive)got;

                if ((positive)got < TEXT_READ_MAX)
                        break;
        }

        if (handle > 0)
                system_call_1(syscall(close), handle);

        // One byte for the newline the file may not have, and one so the
        // scan below can look one past the end without care.
        p8 address_to tail = (p8 address_to)text_arena_take(16);

        if (!tail)
                return false;

        if (!start)
                start = tail;

        side->base = start;
        side->size = have;

        if (have && start[have - 1] != '\n')
        {
                start[have++] = '\n';
                side->incomplete = true;
                side->size = have;
        }

        positive lines = memory_count(start, have, '\n');

        side->lines = lines;
        side->at = (positive address_to)text_arena_take((lines + 2) * sizeof(positive));

        if (!side->at)
                return false;

        positive which = 0;
        positive from = 0;

        // The last byte is a newline by now, so every hunt below finds one.
        while (from < have)
        {
                p8 address_to cut = (p8 address_to)memory_first_of(start + from, '\n',
                                                                   have - from);

                side->at[which++] = from;
                from = (positive)(cut - start) + 1;
        }

        side->at[which] = have;

        return true;
}

// Whether a file looks like something to diff by lines at all.
static bool diff_binary(diff_side address_to side)
{
        if (diff_text)
                return false;

        return memory_first_of(side->base, 0, side->size) != null;
}

// Lines -----------------------------------------------------

static p8 diff_fold(p8 value)
{
        if (diff_icase && value >= 'A' && value <= 'Z')
                return (p8)(value + 32);

        return value;
}

static bool diff_white(p8 value)
{
        return value == ' ' || value == '\t' || value == '\n' || value == '\v' ||
               value == '\f' || value == '\r';
}

/*
        One line's worth of bytes, in the shape the ignore flags leave it.

        The walk stops at the newline every line in the buffer has, so the
        caller never has to know where a line ends.
*/
typedef struct
{
        p8 address_to at;
        p8 held;
        bool done;
} diff_scan;

static fn diff_scan_open(diff_scan address_to scan, p8 address_to line)
{
        scan->at = line;
        scan->done = false;
}

static bool diff_scan_next(diff_scan address_to scan, p8 address_to out)
{
        if (scan->done)
                return false;

        if (diff_space == DIFF_SPACE_ALL)
        {
                while (address_to scan->at != '\n' && diff_white(address_to scan->at))
                        scan->at++;

                if (address_to scan->at == '\n')
                {
                        scan->done = true;
                        return false;
                }

                address_to out = diff_fold(address_to scan->at);
                scan->at++;

                return true;
        }

        if (diff_space == DIFF_SPACE_CHANGE)
        {
                if (address_to scan->at == '\n')
                {
                        scan->done = true;
                        return false;
                }

                if (diff_white(address_to scan->at))
                {
                        while (diff_white(address_to scan->at) && address_to scan->at != '\n')
                                scan->at++;

                        if (address_to scan->at == '\n')
                        {
                                scan->done = true;
                                return false;
                        }

                        address_to out = ' ';

                        return true;
                }

                address_to out = diff_fold(address_to scan->at);
                scan->at++;

                return true;
        }

        if (address_to scan->at == '\n')
        {
                scan->done = true;
                return false;
        }

        address_to out = diff_fold(address_to scan->at);
        scan->at++;

        return true;
}

static p8 address_to diff_line(diff_side address_to side, bipolar middle)
{
        return side->base + side->at[(bipolar)side->prefix + middle];
}

// Without the newline every line in the buffer ends with.
static positive diff_line_length(diff_side address_to side, bipolar middle)
{
        positive line = side->prefix + (positive)middle;

        return side->at[line + 1] - side->at[line] - 1;
}

/*
        The last line of a file that has no newline of its own cannot be the
        same line as a complete one, however the bytes read -- which is the
        distinction GNU keeps by putting it in a bucket of its own. With -b or
        -w the trailing white space is already gone, and the distinction with
        it.
*/
static bool diff_stub(diff_side address_to side, bipolar middle)
{
        return side->incomplete && diff_space == DIFF_SPACE_NONE &&
               side->prefix + (positive)middle == side->lines - 1;
}

static positive diff_hash(diff_side address_to side, bipolar middle)
{
        diff_scan scan;
        positive value = 5381;
        p8 one;

        diff_scan_open(address_of scan, diff_line(side, middle));

        while (diff_scan_next(address_of scan, address_of one))
                value = value * 33 + one;

        return value * 2 + (diff_stub(side, middle) ? 1 : 0);
}

static bool diff_same(diff_side address_to a, bipolar i, diff_side address_to b,
                      bipolar j)
{
        diff_scan left, right;
        p8 one, two;

        if (diff_stub(a, i) != diff_stub(b, j))
                return false;

        // Nothing folded and nothing skipped: the two lines are the same
        // line when the bytes are, and the bytes are a block compare.
        if (!diff_icase && diff_space == DIFF_SPACE_NONE)
        {
                positive length = diff_line_length(a, i);

                return length == diff_line_length(b, j) &&
                       !memory_compare(diff_line(a, i), diff_line(b, j), length);
        }

        diff_scan_open(address_of left, diff_line(a, i));
        diff_scan_open(address_of right, diff_line(b, j));

        while (1)
        {
                bool more_left = diff_scan_next(address_of left, address_of one);
                bool more_right = diff_scan_next(address_of right, address_of two);

                if (!more_left || !more_right)
                        return more_left == more_right;

                if (one != two)
                        return false;
        }
}

// The classes, shared across both files so a number means the same thing
// on either side of the comparison.
typedef struct
{
        positive hash;
        b32 side;
        positive line;
} diff_class;

static diff_class address_to diff_classes;
static positive diff_class_count;
static b32 address_to diff_buckets;
static positive diff_bucket_count;

static bool diff_classify(diff_side address_to side, b32 which)
{
        side->class = (b32 address_to)text_arena_take((side->count + 1) * sizeof(b32));

        if (!side->class)
                return false;

        for (positive i = 0; i < side->count; i++)
        {
                positive hash = diff_hash(side, (bipolar)i);
                positive slot = hash & (diff_bucket_count - 1);
                b32 found = -1;

                while (diff_buckets[slot] >= 0)
                {
                        diff_class address_to have = diff_classes + diff_buckets[slot];

                        if (have->hash == hash &&
                            diff_same(diff_files + have->side, (bipolar)have->line, side, (bipolar)i))
                        {
                                found = diff_buckets[slot];
                                break;
                        }

                        slot = (slot + 1) & (diff_bucket_count - 1);
                }

                if (found < 0)
                {
                        found = (b32)diff_class_count;
                        diff_classes[diff_class_count].hash = hash;
                        diff_classes[diff_class_count].side = which;
                        diff_classes[diff_class_count].line = i;
                        diff_class_count++;
                        diff_buckets[slot] = found;
                }

                side->class[i] = found;
        }

        return true;
}

// Discarding ------------------------------------------------

static positive diff_floor_log2(positive value)
{
        positive bits = 0;

        while (value > 1)
        {
                value >>= 1;
                bits++;
        }

        return bits;
}

/*
        A line that matches nothing on the other side is a deletion whatever
        the matcher would have said, so it is taken out before the matcher
        runs. A line that matches a great many is only provisionally taken
        out, and put back unless it sits in the middle of a run of the first
        kind: this is GNU's discard_confusing_lines, and without it the
        matcher pairs up the wrong one of several identical lines.
*/
static bool diff_discard()
{
        positive total = diff_class_count + 1;
        positive address_to counts[2];
        p8 address_to marks[2];

        for (b32 f = 0; f < 2; f++)
        {
                counts[f] = (positive address_to)text_arena_take(total * sizeof(positive));

                if (!counts[f])
                        return false;

                memory_fill(counts[f], 0, total * sizeof(positive));

                for (positive i = 0; i < diff_files[f].count; i++)
                        counts[f][diff_files[f].class[i]]++;
        }

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                positive bound = side->count;

                marks[f] = (p8 address_to)text_arena_take(bound + 1);

                if (!marks[f])
                        return false;

                memory_fill(marks[f], 0, bound + 1);

                positive many = 5;

                if (bound >= 64)
                        many <<= (diff_floor_log2(bound) >> 1) - 3;

                for (positive i = 0; i < bound; i++)
                {
                        positive matches = counts[1 - f][side->class[i]];

                        if (!matches)
                                marks[f][i] = 1;
                        else if (matches > many)
                                marks[f][i] = 2;
                }
        }

        for (b32 f = 0; f < 2; f++)
        {
                positive bound = diff_files[f].count;
                p8 address_to mark = marks[f];

                for (positive i = 0; i < bound; i++)
                {
                        if (mark[i] == 2)
                        {
                                mark[i] = 0;
                                continue;
                        }

                        if (!mark[i])
                                continue;

                        positive provisional = 0;
                        positive j = i;

                        while (j < bound && mark[j])
                        {
                                if (mark[j] == 2)
                                        provisional++;

                                j++;
                        }

                        while (j > i && mark[j - 1] == 2)
                        {
                                j--;
                                mark[j] = 0;
                                provisional--;
                        }

                        positive length = j - i;

                        if ((length >> 2) < provisional)
                        {
                                while (j > i)
                                        if (mark[--j] == 2)
                                                mark[j] = 0;

                                continue;
                        }

                        positive least = length < 4
                                             ? 2
                                             : ((positive)1 << ((diff_floor_log2(length) >> 1) - 1)) + 1;
                        positive run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (mark[i + j] != 2)
                                {
                                        run = 0;
                                }
                                else if (least == ++run)
                                {
                                        j -= run;
                                }
                                else if (least < run)
                                {
                                        mark[i + j] = 0;
                                }
                        }

                        run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (j >= 8 && mark[i + j] == 1)
                                        break;

                                if (mark[i + j] == 2)
                                {
                                        run = 0;
                                        mark[i + j] = 0;
                                }
                                else if (!mark[i + j])
                                {
                                        run = 0;
                                }
                                else
                                {
                                        run++;
                                }

                                if (run == 3)
                                        break;
                        }

                        i += length - 1;
                        run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (j >= 8 && mark[i - j] == 1)
                                        break;

                                if (mark[i - j] == 2)
                                {
                                        run = 0;
                                        mark[i - j] = 0;
                                }
                                else if (!mark[i - j])
                                {
                                        run = 0;
                                }
                                else
                                {
                                        run++;
                                }

                                if (run == 3)
                                        break;
                        }
                }
        }

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                positive bound = side->count;

                side->kept = (b32 address_to)text_arena_take((bound + 1) * sizeof(b32));
                side->real = (positive address_to)text_arena_take((bound + 1) * sizeof(positive));

                if (!side->kept || !side->real)
                        return false;

                positive keeps = 0;

                for (positive i = 0; i < bound; i++)
                        if (!marks[f][i])
                        {
                                side->kept[keeps] = side->class[i];
                                side->real[keeps++] = i;
                        }
                        else
                        {
                                side->changed[i] = 1;
                        }

                side->keeps = keeps;
        }

        return true;
}

// The matcher -----------------------------------------------

static b32 address_to diff_forward;
static b32 address_to diff_backward;
static positive diff_middle;

static fn diff_meet(positive xoff, positive xlim, positive yoff, positive ylim,
                    positive address_to xmid, positive address_to ymid)
{
        b32 address_to xv = diff_files[0].kept;
        b32 address_to yv = diff_files[1].kept;
        bipolar low = (bipolar)xoff - (bipolar)ylim;
        bipolar high = (bipolar)xlim - (bipolar)yoff;
        bipolar front = (bipolar)xoff - (bipolar)yoff;
        bipolar back = (bipolar)xlim - (bipolar)ylim;
        bipolar front_low = front, front_high = front;
        bipolar back_low = back, back_high = back;
        bool odd = (front - back) & 1;
        positive shift = diff_middle;

        diff_forward[shift + front] = (b32)xoff;
        diff_backward[shift + back] = (b32)xlim;

        while (1)
        {
                if (front_low > low)
                        diff_forward[shift + --front_low - 1] = -1;
                else
                        front_low++;

                if (front_high < high)
                        diff_forward[shift + ++front_high + 1] = -1;
                else
                        front_high--;

                for (bipolar d = front_high; d >= front_low; d -= 2)
                {
                        bipolar lower = diff_forward[shift + d - 1];
                        bipolar upper = diff_forward[shift + d + 1];
                        bipolar x = lower < upper ? upper : lower + 1;
                        bipolar y = x - d;

                        while (x < (bipolar)xlim && y < (bipolar)ylim &&
                               xv[x] == yv[y])
                        {
                                x++;
                                y++;
                        }

                        diff_forward[shift + d] = (b32)x;

                        if (odd && back_low <= d && d <= back_high &&
                            diff_backward[shift + d] <= x)
                        {
                                address_to xmid = (positive)x;
                                address_to ymid = (positive)y;
                                return;
                        }
                }

                if (back_low > low)
                        diff_backward[shift + --back_low - 1] = 0x7fffffff;
                else
                        back_low++;

                if (back_high < high)
                        diff_backward[shift + ++back_high + 1] = 0x7fffffff;
                else
                        back_high--;

                for (bipolar d = back_high; d >= back_low; d -= 2)
                {
                        bipolar lower = diff_backward[shift + d - 1];
                        bipolar upper = diff_backward[shift + d + 1];
                        bipolar x = lower < upper ? lower : upper - 1;
                        bipolar y = x - d;

                        while ((bipolar)xoff < x && (bipolar)yoff < y &&
                               xv[x - 1] == yv[y - 1])
                        {
                                x--;
                                y--;
                        }

                        diff_backward[shift + d] = (b32)x;

                        if (!odd && front_low <= d && d <= front_high &&
                            x <= diff_forward[shift + d])
                        {
                                address_to xmid = (positive)x;
                                address_to ymid = (positive)y;
                                return;
                        }
                }
        }
}

// The high half is looped rather than recursed, which is what keeps the
// depth off a stack that has no room to grow.
static fn diff_compare(positive xoff, positive xlim, positive yoff, positive ylim)
{
        b32 address_to xv = diff_files[0].kept;
        b32 address_to yv = diff_files[1].kept;

        while (1)
        {
                while (xoff < xlim && yoff < ylim && xv[xoff] == yv[yoff])
                {
                        xoff++;
                        yoff++;
                }

                while (xoff < xlim && yoff < ylim && xv[xlim - 1] == yv[ylim - 1])
                {
                        xlim--;
                        ylim--;
                }

                if (xoff == xlim)
                {
                        while (yoff < ylim)
                                diff_files[1].changed[diff_files[1].real[yoff++]] = 1;

                        return;
                }

                if (yoff == ylim)
                {
                        while (xoff < xlim)
                                diff_files[0].changed[diff_files[0].real[xoff++]] = 1;

                        return;
                }

                positive xmid, ymid;

                diff_meet(xoff, xlim, yoff, ylim, address_of xmid, address_of ymid);
                diff_compare(xoff, xmid, yoff, ymid);

                xoff = xmid;
                yoff = ymid;
        }
}

/*
        Where a run of changed lines sits when either end of it could be the
        changed one.

        A minimal edit script is not unique, and this is the rule that makes
        ours the same one GNU prints: merge backwards into whatever came
        before, then slide forward as far as the lines allow, and finally
        pull back to line up with a run on the other side.
*/
static fn diff_shift()
{
        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                diff_side address_to other = diff_files + (1 - f);
                p8 address_to changed = side->changed;
                p8 address_to opposite = other->changed;
                b32 address_to equivalent = side->class;
                bipolar i = 0;
                bipolar j = 0;
                bipolar bound = (bipolar)side->count;

                while (1)
                {
                        while (i < bound && !changed[i])
                        {
                                while (opposite[j++])
                                        ;

                                i++;
                        }

                        if (i == bound)
                                break;

                        bipolar start = i;

                        while (changed[++i])
                                ;

                        while (opposite[j])
                                j++;

                        bipolar length, matching;

                        do
                        {
                                length = i - start;

                                while (start && equivalent[start - 1] == equivalent[i - 1])
                                {
                                        changed[--start] = 1;
                                        changed[--i] = 0;

                                        while (changed[start - 1])
                                                start--;

                                        while (opposite[--j])
                                                ;
                                }

                                matching = opposite[j - 1] ? i : bound;

                                while (i != bound && equivalent[start] == equivalent[i])
                                {
                                        changed[start++] = 0;
                                        changed[i++] = 1;

                                        while (changed[i])
                                                i++;

                                        while (opposite[++j])
                                                matching = i;
                                }
                        }
                        while (length != i - start);

                        while (matching < i)
                        {
                                changed[--start] = 1;
                                changed[--i] = 0;

                                while (opposite[--j])
                                        ;
                        }
                }
        }
}

// The script ------------------------------------------------

typedef struct
{
        positive line0;
        positive line1;
        positive deleted;
        positive inserted;
        bool ignore;
} diff_change;

static diff_change address_to diff_script;
static positive diff_script_count;

static bool diff_build()
{
        positive room = diff_files[0].count + diff_files[1].count + 2;

        diff_script = (diff_change address_to)text_arena_take(room * sizeof(diff_change));

        if (!diff_script)
                return false;

        diff_script_count = 0;

        positive i0 = 0, i1 = 0;
        positive len0 = diff_files[0].count, len1 = diff_files[1].count;
        p8 address_to c0 = diff_files[0].changed;
        p8 address_to c1 = diff_files[1].changed;

        while (i0 < len0 || i1 < len1)
        {
                if ((i0 < len0 && c0[i0]) || (i1 < len1 && c1[i1]))
                {
                        positive line0 = i0, line1 = i1;

                        while (i0 < len0 && c0[i0])
                                i0++;

                        while (i1 < len1 && c1[i1])
                                i1++;

                        diff_script[diff_script_count].line0 = line0;
                        diff_script[diff_script_count].line1 = line1;
                        diff_script[diff_script_count].deleted = i0 - line0;
                        diff_script[diff_script_count].inserted = i1 - line1;
                        diff_script[diff_script_count].ignore = false;
                        diff_script_count++;
                }

                i0++;
                i1++;
        }

        return true;
}

// Whether every line a change touches is one -B was told to skip over.
static bool diff_trivial(positive from, positive to)
{
        if (!diff_blank_lines)
                return false;

        for (positive c = from; c < to; c++)
        {
                diff_change address_to one = diff_script + c;

                for (positive i = 0; i < one->deleted; i++)
                {
                        p8 address_to line = diff_line(diff_files + 0, (bipolar)(one->line0 + i));

                        if (address_to line != '\n')
                                return false;
                }

                for (positive i = 0; i < one->inserted; i++)
                {
                        p8 address_to line = diff_line(diff_files + 1, (bipolar)(one->line1 + i));

                        if (address_to line != '\n')
                                return false;
                }
        }

        return true;
}

static fn diff_mark_ignorable()
{
        for (positive c = 0; c < diff_script_count; c++)
                diff_script[c].ignore = diff_trivial(c, c + 1);
}

// Output ----------------------------------------------------

static fn diff_number(positive value)
{
        text_put_number(value, 0);
}

static fn diff_range(diff_side address_to side, bipolar first, bipolar last,
                     p8 separator)
{
        bipolar low = first + (bipolar)side->prefix + 1;
        bipolar high = last + (bipolar)side->prefix + 1;

        if (high > low)
        {
                diff_number((positive)low);
                text_put_character(separator);
                diff_number((positive)high);
        }
        else
        {
                diff_number((positive)high);
        }
}

static fn diff_unified_range(diff_side address_to side, bipolar first, bipolar last)
{
        bipolar low = first + (bipolar)side->prefix + 1;
        bipolar high = last + (bipolar)side->prefix + 1;

        if (high <= low)
        {
                diff_number((positive)high);

                if (high < low)
                        text_put_string(",0");

                return;
        }

        diff_number((positive)low);
        text_put_character(',');
        diff_number((positive)(high - low + 1));
}

static fn diff_put_line(diff_side address_to side, bipolar middle, string_address flag)
{
        p8 address_to line = diff_line(side, middle);
        positive where = (positive)((bipolar)side->prefix + middle);
        positive length = side->at[where + 1] - side->at[where];
        bool whole = !(side->incomplete && where == side->lines - 1);

        if (flag)
                text_put_string(flag);

        text_put(line, length - 1);

        if (whole)
                text_put_character('\n');
        else
                text_put_string("\n\\ No newline at end of file\n");
}

static fn diff_title(string_address left, string_address right)
{
        if (!diff_titled)
                return;

        text_put_string("diff");
        text_put(diff_switches, diff_switches_used);
        text_put_character(' ');
        text_put_string(left);
        text_put_character(' ');
        text_put_string(right);
        text_put_character('\n');
}

static fn diff_label(string_address mark, diff_side address_to side,
                     string_address name, string_address label)
{
        text_put_string(mark);
        text_put_character(' ');

        if (label)
        {
                text_put_string(label);
                text_put_character('\n');
                return;
        }

        text_put_string(name);
        text_put_character('\t');
        file_stamp(diff_writer, side->modified_seconds, side->modified_nanoseconds);
        text_put_character('\n');
}

static fn diff_normal_output()
{
        for (positive c = 0; c < diff_script_count; c++)
        {
                diff_change address_to one = diff_script + c;

                if (diff_trivial(c, c + 1))
                        continue;

                bipolar first0 = (bipolar)one->line0;
                bipolar last0 = (bipolar)(one->line0 + one->deleted) - 1;
                bipolar first1 = (bipolar)one->line1;
                bipolar last1 = (bipolar)(one->line1 + one->inserted) - 1;

                diff_range(diff_files + 0, first0, last0, ',');
                text_put_character(one->deleted && one->inserted
                                       ? 'c'
                                       : one->deleted ? 'd' : 'a');
                diff_range(diff_files + 1, first1, last1, ',');
                text_put_character('\n');

                for (positive i = 0; i < one->deleted; i++)
                        diff_put_line(diff_files + 0, first0 + (bipolar)i, "< ");

                if (one->deleted && one->inserted)
                        text_put_string("---\n");

                for (positive i = 0; i < one->inserted; i++)
                        diff_put_line(diff_files + 1, first1 + (bipolar)i, "> ");
        }
}

// A hunk runs on while the gap to the next change is smaller than the
// context it would print on either side of it.
static positive diff_hunk_end(positive start)
{
        positive c = start;

        while (1)
        {
                positive top0 = diff_script[c].line0 + diff_script[c].deleted;

                if (c + 1 >= diff_script_count)
                        return c;

                positive threshold = diff_script[c + 1].ignore ? diff_context
                                                              : diff_context * 2 + 1;

                if (threshold <= diff_script[c + 1].line0 - top0)
                        return c;

                c++;
        }
}

static fn diff_unified_output(string_address left, string_address right)
{
        if (diff_blank_lines)
                diff_mark_ignorable();

        positive c = 0;
        bool headed = false;

        while (c < diff_script_count)
        {
                positive last = diff_hunk_end(c);
                bool anything = false;

                for (positive k = c; k <= last; k++)
                        if (!diff_script[k].ignore)
                                anything = true;

                if (!anything)
                {
                        c = last + 1;
                        continue;
                }

                bipolar first0 = (bipolar)diff_script[c].line0;
                bipolar first1 = (bipolar)diff_script[c].line1;
                bipolar last0 = (bipolar)(diff_script[last].line0 +
                                          diff_script[last].deleted) - 1;
                bipolar last1 = (bipolar)(diff_script[last].line1 +
                                          diff_script[last].inserted) - 1;

                /*
                        The context around a hunk comes out of the whole file,
                        not out of the middle the matcher was given, so these
                        walk back into the identical head and on into the
                        identical tail. That is where the negative index is
                        from and why none of this is unsigned.
                */
                bipolar floor0 = -(bipolar)diff_files[0].prefix;
                bipolar ceiling0 = (bipolar)diff_files[0].lines - (bipolar)diff_files[0].prefix;
                bipolar ceiling1 = (bipolar)diff_files[1].lines - (bipolar)diff_files[1].prefix;

                first0 -= (bipolar)diff_context;
                first1 -= (bipolar)diff_context;

                if (first0 < floor0)
                        first0 = floor0;

                if (first1 < floor0)
                        first1 = floor0;

                if (last0 < ceiling0 - (bipolar)diff_context)
                        last0 += (bipolar)diff_context;
                else
                        last0 = ceiling0 - 1;

                if (last1 < ceiling1 - (bipolar)diff_context)
                        last1 += (bipolar)diff_context;
                else
                        last1 = ceiling1 - 1;

                if (!headed)
                {
                        diff_title(left, right);
                        diff_label("---", diff_files + 0, left, diff_labels[0]);
                        diff_label("+++", diff_files + 1, right, diff_labels[1]);
                        headed = true;
                }

                text_put_string("@@ -");
                diff_unified_range(diff_files + 0, first0, last0);
                text_put_string(" +");
                diff_unified_range(diff_files + 1, first1, last1);
                text_put_string(" @@\n");

                positive at = c;
                bipolar i = first0;
                bipolar j = first1;

                while (i <= last0 || j <= last1)
                {
                        if (at > last || i < (bipolar)diff_script[at].line0)
                        {
                                diff_put_line(diff_files + 0, i++, " ");
                                j++;
                                continue;
                        }

                        for (positive k = 0; k < diff_script[at].deleted; k++)
                                diff_put_line(diff_files + 0, i++, "-");

                        for (positive k = 0; k < diff_script[at].inserted; k++)
                                diff_put_line(diff_files + 1, j++, "+");

                        at++;
                }

                c = last + 1;
        }
}

// One pair of files -----------------------------------------

static b32 diff_pair(string_address left, string_address right)
{
        diff_side address_to a = diff_files + 0;
        diff_side address_to b = diff_files + 1;

        memory_fill(a, 0, sizeof(diff_side));
        memory_fill(b, 0, sizeof(diff_side));

        if (!diff_slurp(a, left) || !diff_slurp(b, right))
                return 2;

        if (diff_binary(a) || diff_binary(b))
        {
                if (a->size == b->size && !memory_compare(a->base, b->base, a->size))
                        return 0;

                text_put_string(diff_brief ? "Files " : "Binary files ");
                text_put_string(left);
                text_put_string(" and ");
                text_put_string(right);
                text_put_string(" differ\n");
                text_flush();

                return 1;
        }

        /*
                The identical head and tail are taken off before anything
                else, in bytes and then rounded back to whole lines, because
                that is where GNU takes them off and the matcher that runs
                after sees a different problem without it.
        */
        positive horizon = diff_style == DIFF_UNIFIED ? diff_context : 0;
        positive prefix = 0;
        positive shortest = a->size < b->size ? a->size : b->size;
        positive bytes = 0;

        while (bytes < shortest && a->base[bytes] == b->base[bytes])
                bytes++;

        /*
                The newline a file did not have is in the buffer anyway, and
                a head that runs past where one file really ended is a head
                that includes a byte only one of them wrote. Backing off one
                byte here is what keeps an incomplete last line from being
                trimmed away as identical to a complete one.
        */
        {
                positive real_a = a->size - (a->incomplete ? 1 : 0);
                positive real_b = b->size - (b->incomplete ? 1 : 0);

                if ((real_a < bytes) != (real_b < bytes))
                        bytes--;
        }

        while (bytes && a->base[bytes - 1] != '\n')
                bytes--;

        while (prefix < a->lines && a->at[prefix] < bytes)
                prefix++;

        /*
                A context style keeps back as many lines as it is going to
                print around a hunk, because a line inside the identical head
                is a line the matcher is not allowed to move a run of changes
                into. Without it -u and the plain format disagree about which
                of two identical lines is the changed one, and only in the
                cases where a hunk reaches the edge of what was trimmed.
        */
        prefix = prefix > horizon ? prefix - horizon : 0;
        bytes = a->at[prefix];

        positive suffix = 0;

        if (a->incomplete == b->incomplete)
        {
                positive tail = 0;

                while (tail < a->size - bytes && tail < b->size - bytes &&
                       a->base[a->size - 1 - tail] == b->base[b->size - 1 - tail])
                        tail++;

                positive stop_a = a->size - tail;
                positive stop_b = b->size - tail;

                // The identical tail has to begin a line in both files, and
                // it is the one that does not that decides: a tail that
                // starts inside a line on either side costs a whole line.
                positive drop = horizon +
                                !((!stop_a || a->base[stop_a - 1] == '\n') &&
                                  (!stop_b || b->base[stop_b - 1] == '\n'));

                while (drop && stop_a != a->size)
                {
                        drop--;

                        while (stop_a < a->size && a->base[stop_a++] != '\n')
                                ;
                }

                while (suffix < a->lines - prefix &&
                       a->at[a->lines - 1 - suffix] >= stop_a)
                        suffix++;

                if (suffix > b->lines - prefix)
                        suffix = b->lines - prefix;
        }

        a->prefix = prefix;
        b->prefix = prefix;
        a->count = a->lines - prefix - suffix;
        b->count = b->lines - prefix - suffix;

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                p8 address_to room = (p8 address_to)text_arena_take(side->count + 4);

                if (!room)
                        return 2;

                memory_fill(room, 0, side->count + 4);
                side->changed = room + 1;
        }

        positive total = a->count + b->count + 4;

        diff_bucket_count = 8;

        while (diff_bucket_count < total * 2)
                diff_bucket_count <<= 1;

        diff_classes = (diff_class address_to)text_arena_take(total * sizeof(diff_class));
        diff_buckets = (b32 address_to)text_arena_take(diff_bucket_count * sizeof(b32));

        if (!diff_classes || !diff_buckets)
                return 2;

        for (positive i = 0; i < diff_bucket_count; i++)
                diff_buckets[i] = -1;

        diff_class_count = 0;

        if (!diff_classify(a, 0) || !diff_classify(b, 1))
                return 2;

        if (!diff_discard())
                return 2;

        positive diagonals = a->keeps + b->keeps + 3;

        diff_forward = (b32 address_to)text_arena_take((diagonals + 4) * sizeof(b32));
        diff_backward = (b32 address_to)text_arena_take((diagonals + 4) * sizeof(b32));

        if (!diff_forward || !diff_backward)
                return 2;

        diff_middle = b->keeps + 2;

        diff_compare(0, a->keeps, 0, b->keeps);
        diff_shift();

        if (!diff_build())
                return 2;

        bool changes = false;

        if (diff_blank_lines)
        {
                for (positive c = 0; c < diff_script_count; c++)
                        if (!diff_trivial(c, c + 1))
                                changes = true;
        }
        else
        {
                changes = diff_script_count != 0;
        }

        if (!changes)
                return 0;

        if (diff_brief)
        {
                text_put_string("Files ");
                text_put_string(left);
                text_put_string(" and ");
                text_put_string(right);
                text_put_string(" differ\n");
                text_flush();

                return 1;
        }

        if (diff_style == DIFF_UNIFIED)
        {
                diff_unified_output(left, right);
        }
        else
        {
                diff_title(left, right);
                diff_normal_output();
        }

        text_flush();

        return 1;
}

// Directories -----------------------------------------------

#define DIFF_NAMES_MAX 2048

/*
        The names in one directory, with the bytes taken from the arena.

        A walk that is going to recurse cannot keep its names in a buffer of
        its own, because the level below would write over them and the level
        above would go on comparing whatever landed there.
*/
static bool diff_gather(string_address path, string_address address_to names,
                        positive address_to count)
{
        file_walk walk;

        address_to count = 0;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                if (diff_new_file)
                        return true;

                text_flush();
                text_error_raw("diff: ");
                text_error_raw(path);
                text_error_raw(": No such file or directory\n");
                return false;
        }

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                if (address_to count >= DIFF_NAMES_MAX)
                        break;

                positive length = string_length(entry->d_name);
                p8 address_to at = (p8 address_to)text_arena_take(length + 1);

                if (!at)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                memory_copy_fast(at, entry->d_name, length + 1);
                names[address_to count] = at;
                address_to count += 1;
        }

        file_walk_close(address_of walk);

        for (positive i = 1; i < address_to count; i++)
        {
                string_address one = names[i];
                positive j = i;

                while (j && string_compare(names[j - 1], one) > 0)
                {
                        names[j] = names[j - 1];
                        j--;
                }

                names[j] = one;
        }

        return true;
}

static b32 diff_walk(string_address left, string_address right, positive depth);

static b32 diff_directories(string_address left, string_address right, positive depth)
{
        if (depth >= FILE_MAX_DEPTH)
                return 2;

        string_address names[2][DIFF_NAMES_MAX];
        positive counts[2];

        if (!diff_gather(left, names[0], address_of counts[0]) ||
            !diff_gather(right, names[1], address_of counts[1]))
                return 2;

        positive i = 0, j = 0;
        b32 worst = 0;

        while (i < counts[0] || j < counts[1])
        {
                b32 order = i >= counts[0]
                                ? 1
                                : j >= counts[1]
                                      ? -1
                                      : (b32)string_compare(names[0][i], names[1][j]);

                if (order < 0)
                {
                        if (diff_new_file)
                        {
                                p8 only_left[TEXT_PATH_MAX];
                                p8 only_right[TEXT_PATH_MAX];

                                file_join(only_left, TEXT_PATH_MAX, left, names[0][i]);
                                file_join(only_right, TEXT_PATH_MAX, right, names[0][i]);

                                b32 one = diff_walk(only_left, only_right, depth + 1);

                                if (worst < one)
                                        worst = one;
                        }
                        else
                        {
                                text_put_string("Only in ");
                                text_put_string(left);
                                text_put_string(": ");
                                text_put_string(names[0][i]);
                                text_put_character('\n');
                                text_flush();

                                if (worst < 1)
                                        worst = 1;
                        }

                        i++;
                        continue;
                }

                if (order > 0)
                {
                        if (diff_new_file)
                        {
                                p8 only_left[TEXT_PATH_MAX];
                                p8 only_right[TEXT_PATH_MAX];

                                file_join(only_left, TEXT_PATH_MAX, left, names[1][j]);
                                file_join(only_right, TEXT_PATH_MAX, right, names[1][j]);

                                b32 one = diff_walk(only_left, only_right, depth + 1);

                                if (worst < one)
                                        worst = one;
                        }
                        else
                        {
                                text_put_string("Only in ");
                                text_put_string(right);
                                text_put_string(": ");
                                text_put_string(names[1][j]);
                                text_put_character('\n');
                                text_flush();

                                if (worst < 1)
                                        worst = 1;
                        }

                        j++;
                        continue;
                }

                p8 one_left[TEXT_PATH_MAX];
                p8 one_right[TEXT_PATH_MAX];

                file_join(one_left, TEXT_PATH_MAX, left, names[0][i]);
                file_join(one_right, TEXT_PATH_MAX, right, names[1][j]);

                bool left_directory = file_is_directory_through(one_left);
                bool right_directory = file_is_directory_through(one_right);

                if (left_directory && right_directory && !diff_recursive)
                {
                        text_put_string("Common subdirectories: ");
                        text_put_string(one_left);
                        text_put_string(" and ");
                        text_put_string(one_right);
                        text_put_character('\n');
                        text_flush();
                }
                else
                {
                        b32 one = diff_walk(one_left, one_right, depth + 1);

                        if (worst < one)
                                worst = one;
                }

                i++;
                j++;
        }

        return worst;
}

static b32 diff_walk(string_address left, string_address right, positive depth)
{
        bool left_here = file_exists(AT_FDCWD, left);
        bool right_here = file_exists(AT_FDCWD, right);
        bool left_directory = left_here && file_is_directory_through(left);
        bool right_directory = right_here && file_is_directory_through(right);

        if ((left_directory || right_directory) &&
            (left_directory == right_directory || (diff_new_file && !(left_here && right_here))))
                return diff_directories(left, right, depth);

        if (left_directory != right_directory && left_here && right_here)
        {
                text_put_string("File ");
                text_put_string(left);
                text_put_string(" is a ");
                text_put_string(left_directory ? "directory" : "regular file");
                text_put_string(" while file ");
                text_put_string(right);
                text_put_string(" is a ");
                text_put_string(right_directory ? "directory" : "regular file");
                text_put_character('\n');
                text_flush();

                return 1;
        }

        bool titled = diff_titled;

        diff_titled = depth > 0;

        b32 one = diff_pair(left, right);

        diff_titled = titled;

        return one;
}

static string_address diff_basename(string_address path)
{
        string_address last = string_last_of(path, '/');

        return last ? last + 1 : path;
}

static const file_long diff_longs[] = {
    {(string_address) "unified", 'u'},
    {(string_address) "brief", 'q'},
    {(string_address) "recursive", 'r'},
    {(string_address) "new-file", 'N'},
    {(string_address) "ignore-case", 'i'},
    {(string_address) "ignore-all-space", 'w'},
    {(string_address) "ignore-space-change", 'b'},
    {(string_address) "ignore-blank-lines", 'B'},
    {(string_address) "text", 'a'},
    {(string_address) "label", 'L'},
    {null, 0},
};

// -L comes twice, once for each side, and one value per letter cannot hold
// two: the labels are taken as the options arrive.
static bool diff_label_seen(p8 letter, string_address value)
{
        if (letter != 'L')
                return true;

        if (!diff_labels[0])
                diff_labels[0] = value;
        else
                diff_labels[1] = value;

        return true;
}

static b32 tools_diff(void)
{
        file_taking taking = {
            .program = (string_address) "diff",
            .allowed = (string_address) "BLNabiqruw",
            .valued = (string_address) "L",
            .longs = diff_longs,
            .seen = diff_label_seen,
        };

        text_begin("diff");

        diff_brief = false;
        diff_style = DIFF_NORMAL;
        diff_context = 3;
        diff_labels[0] = diff_labels[1] = null;
        diff_switches_used = 0;
        diff_titled = false;

        if (!file_take(address_of taking))
                return text_done(2);

        positive flags = taking.flags;
        b32 first = (b32)taking.first;

        diff_icase = (flags & FILE_FLAG('i')) != 0;
        diff_blank_lines = (flags & FILE_FLAG('B')) != 0;
        diff_recursive = (flags & FILE_FLAG('r')) != 0;
        diff_new_file = (flags & FILE_FLAG('N')) != 0;
        diff_text = (flags & FILE_FLAG('a')) != 0;
        diff_brief = (flags & FILE_FLAG('q')) != 0;
        diff_space = (flags & FILE_FLAG('w'))   ? DIFF_SPACE_ALL
                     : (flags & FILE_FLAG('b')) ? DIFF_SPACE_CHANGE
                                                : DIFF_SPACE_NONE;

        if (flags & FILE_FLAG('u'))
                diff_style = DIFF_UNIFIED;

        // What the header of a piece of a recursive diff repeats is the
        // options as they were typed, not a canonical spelling -- and a label
        // written as its own word is not one of them.
        for (positive i = 1; i < taking.first; i++)
        {
                string_address word = program_argument((b32)i);
                positive length = string_length(word);

                if (!string_is(word, '-') || length < 2 ||
                    (string_is(word + 1, '-') && length == 2))
                        continue;

                if (diff_switches_used + length + 2 >= sizeof(diff_switches))
                        break;

                diff_switches[diff_switches_used++] = ' ';

                for (string_address at = word; string_get(at); at++)
                        diff_switches[diff_switches_used++] = string_get(at);
        }

        if (text_argument_count - first != 2)
        {
                text_error(null, "missing operand");
                return text_done(2);
        }

        string_address left = text_argument(first);
        string_address right = text_argument(first + 1);
        p8 joined[TEXT_PATH_MAX];

        // diff dir file and diff file dir both mean the same file inside the
        // directory, which is the one place the two names are not the pair.
        if (file_is_directory_through(left) && !file_is_directory_through(right))
        {
                file_join(joined, TEXT_PATH_MAX, left, diff_basename(right));
                left = joined;
        }
        else if (!file_is_directory_through(left) && file_is_directory_through(right))
        {
                file_join(joined, TEXT_PATH_MAX, right, diff_basename(left));
                right = joined;
        }

        diff_result = diff_walk(left, right, 0);

        return text_done(diff_result);
}

// ps --------------------------------------------------------

#define PS_MAX 8192
#define PS_ARGS_MAX 512

#define PS_FIELD_PID 0
#define PS_FIELD_PPID 1
#define PS_FIELD_USER 2
#define PS_FIELD_COMM 3
#define PS_FIELD_ARGS 4
#define PS_FIELD_STAT 5
#define PS_FIELD_TIME 6
#define PS_FIELD_ETIME 7
#define PS_FIELD_RSS 8
#define PS_FIELD_VSZ 9
#define PS_FIELD_TTY 10
#define PS_FIELD_UID 11
#define PS_FIELD_CPU 12
#define PS_FIELD_STIME 13
#define PS_FIELD_COUNT 14

typedef struct
{
        string_address name;
        string_address header;
        positive width;
        bool right;
} ps_column;

static ps_column ps_columns[PS_FIELD_COUNT] = {
    {"pid", "PID", 7, true},        {"ppid", "PPID", 7, true},
    {"user", "USER", 8, false},     {"comm", "COMMAND", 15, false},
    {"args", "COMMAND", 27, false}, {"stat", "STAT", 4, false},
    {"time", "TIME", 8, true},      {"etime", "ELAPSED", 11, true},
    {"rss", "RSS", 5, true},        {"vsz", "VSZ", 6, true},
    {"tty", "TT", 2, false},        {"uid", "UID", 8, false},
    {"c", "C", 2, true},            {"stime", "STIME", 5, false}};

typedef struct
{
        positive pid;
        positive ppid;
        positive uid;
        positive tty;
        positive rss;
        positive vsz;
        positive utime;
        positive stime;
        positive start;
        positive pgrp;
        positive session;
        bipolar tpgid;
        bipolar nice;
        positive threads;
        p8 state[8];
        p8 comm[64];
        p8 args[256];
        p8 user[36];
} ps_process;

static ps_process ps_list[PS_MAX];
static positive ps_count;
static positive ps_clock = 100;
static positive ps_now;
static positive ps_own_tty;
static positive ps_own_uid;
static positive ps_wall;

static positive ps_read_file(string_address path, p8 address_to into, positive limit)
{
        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
                return 0;

        positive have = 0;

        while (have + 1 < limit)
        {
                bipolar got = system_call_3(syscall(read), (positive)handle,
                                            (positive)(into + have), limit - 1 - have);

                if (got <= 0)
                        break;

                have += (positive)got;
        }

        system_call_1(syscall(close), handle);
        into[have] = end;

        return have;
}

// The bytes a /proc field is made of, and the ones between two of them.
static b8 ps_blank_bytes[STRING_SET_BYTES];
static b8 ps_field_bytes[STRING_SET_BYTES];

static positive ps_take(string_address address_to at)
{
        string_address here = address_to at + string_span(address_to at, ps_blank_bytes);
        positive taken;
        positive value = string_digits(here, address_of taken);

        address_to at = here + taken;

        return value;
}

static bipolar ps_signed(string_address address_to at)
{
        string_address here = address_to at + string_span(address_to at, ps_blank_bytes);
        bool minus = false;

        if (string_get(here) == '-')
        {
                minus = true;
                here++;
        }

        address_to at = here;

        bipolar value = (bipolar)ps_take(at);

        return minus ? -value : value;
}

static fn ps_pass(string_address address_to at, positive fields)
{
        string_address here = address_to at;

        for (positive i = 0; i < fields; i++)
        {
                here += string_span(here, ps_blank_bytes);
                here += string_span(here, ps_field_bytes);
        }

        address_to at = here;
}

// The name behind a numeric user id, from the file the system keeps it in.
static p8 ps_password[1 << 18];
static positive ps_password_size;

static fn ps_name_of(positive uid, p8 address_to into, positive limit)
{
        positive at = 0;

        while (at < ps_password_size)
        {
                positive line = at;
                positive stop = (positive)(string_first_of_or_end(ps_password + at, '\n') -
                                           ps_password);

                positive fields = 0;
                positive start = line;
                positive found = 0;
                positive name_end = 0;

                for (positive i = line; i <= stop; i++)
                        if (i == stop || ps_password[i] == ':')
                        {
                                if (fields == 0)
                                {
                                        found = start;
                                        name_end = i;
                                }

                                if (fields == 2)
                                {
                                        positive value = string_digits_max(
                                            ps_password + start, i - start, null);

                                        if (value == uid)
                                        {
                                                positive length = name_end - found;

                                                if (length >= limit)
                                                        length = limit - 1;

                                                memory_copy(into, ps_password + found, length);
                                                into[length] = end;

                                                return;
                                        }

                                        break;
                                }

                                fields++;
                                start = i + 1;
                        }

                at = stop + 1;
        }

        positive length = 0;
        p8 digits[24];
        positive have = 0;
        positive value = uid;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (have && length + 1 < limit)
                into[length++] = digits[--have];

        into[length] = end;
}

static bool ps_gather()
{
        p8 block[8192];
        file_walk walk;

        ps_count = 0;
        ps_password_size = ps_read_file("/etc/passwd", ps_password, sizeof(ps_password));

        string_set_add(ps_blank_bytes, " \t");
        memory_fill(ps_field_bytes, 1, sizeof(ps_field_bytes));
        ps_field_bytes[0] = 0;
        ps_field_bytes[' '] = 0;
        ps_field_bytes['\t'] = 0;

        {
                p8 uptime[64];

                if (ps_read_file("/proc/uptime", uptime, sizeof(uptime)))
                {
                        string_address at = uptime;

                        ps_now = ps_take(address_of at);
                        ps_wall = (positive)file_now();
                }
        }

        if (!file_walk_open(address_of walk, AT_FDCWD, "/proc"))
                return false;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)) && ps_count < PS_MAX)
        {
                if (!text_digit(entry->d_name[0]))
                        continue;

                p8 path[64];

                string_copy(path, "/proc/");
                string_copy(path + string_length(path), entry->d_name);

                positive base = string_length(path);

                string_copy(path + base, "/stat");

                if (!ps_read_file(path, block, sizeof(block)))
                        continue;

                ps_process address_to one = ps_list + ps_count;

                memory_fill(one, 0, sizeof(ps_process));

                string_address at = block;

                one->pid = ps_take(address_of at);

                at = string_first_of_or_end(at, '(') + 1;

                string_address close = at;
                string_address last = string_last_of(at, ')');

                if (!last)
                        continue;

                positive length = (positive)(last - close);

                if (length > sizeof(one->comm) - 1)
                        length = sizeof(one->comm) - 1;

                memory_copy(one->comm, close, length);
                one->comm[length] = end;

                at = last + 2;

                one->state[0] = string_get(at);
                one->state[1] = end;

                at += 2;

                /*
                        The state is the third field and the command before it
                        can hold a space or a bracket of its own, which is why
                        the walk starts from the last close bracket rather
                        than counting from the front.
                */
                one->ppid = ps_take(address_of at);
                one->pgrp = ps_take(address_of at);
                one->session = ps_take(address_of at);
                one->tty = ps_take(address_of at);
                one->tpgid = ps_signed(address_of at);
                ps_pass(address_of at, 5);
                one->utime = ps_take(address_of at);
                one->stime = ps_take(address_of at);
                ps_pass(address_of at, 2);
                ps_take(address_of at);
                one->nice = ps_signed(address_of at);
                one->threads = ps_take(address_of at);
                ps_pass(address_of at, 1);
                one->start = ps_take(address_of at);
                one->vsz = ps_take(address_of at) / 1024;
                one->rss = ps_take(address_of at) * 4;

                positive mark = 1;

                if (one->nice < 0)
                        one->state[mark++] = '<';
                else if (one->nice > 0)
                        one->state[mark++] = 'N';

                if (one->session == one->pid)
                        one->state[mark++] = 's';

                if (one->threads > 1)
                        one->state[mark++] = 'l';

                if (one->tpgid == (bipolar)one->pgrp)
                        one->state[mark++] = '+';

                one->state[mark] = end;

                string_copy(path + base, "/status");

                if (ps_read_file(path, block, sizeof(block)))
                {
                        string_address seek = block;

                        while (string_get(seek))
                        {
                                if (seek[0] == 'U' && seek[1] == 'i' && seek[2] == 'd' &&
                                    seek[3] == ':')
                                {
                                        string_address value = seek + 4;

                                        one->uid = ps_take(address_of value);
                                        break;
                                }

                                seek = string_first_of_or_end(seek, '\n');

                                if (string_get(seek))
                                        seek++;
                        }
                }

                ps_name_of(one->uid, one->user, sizeof(one->user));

                string_copy(path + base, "/cmdline");

                positive got = ps_read_file(path, block, sizeof(one->args));

                if (got)
                {
                        for (positive i = 0; i < got; i++)
                                one->args[i] = block[i] ? block[i] : ' ';

                        while (got && one->args[got - 1] == ' ')
                                got--;

                        one->args[got] = end;
                }
                else
                {
                        positive length = string_length(one->comm);

                        one->args[0] = '[';
                        memory_copy_fast(one->args + 1, one->comm, length);
                        one->args[1 + length] = ']';
                        one->args[2 + length] = end;
                }

                ps_count++;
        }

        file_walk_close(address_of walk);

        for (positive i = 1; i < ps_count; i++)
        {
                ps_process one = ps_list[i];
                positive j = i;

                while (j && ps_list[j - 1].pid > one.pid)
                {
                        ps_list[j] = ps_list[j - 1];
                        j--;
                }

                ps_list[j] = one;
        }

        return true;
}

/*
        A field is drawn into a small buffer first, because a column is padded
        by how wide what it drew turned out to be and the shared output buffer
        can empty itself between one byte and the next.
*/
static p8 ps_room[512];
static positive ps_room_used;

static fn ps_byte(p8 value)
{
        if (ps_room_used + 1 < sizeof(ps_room))
                ps_room[ps_room_used++] = value;
}

static fn ps_text(string_address value)
{
        while (string_get(value))
                ps_byte(string_get(value++));
}

static fn ps_digits(positive value)
{
        p8 have[24];
        positive length = 0;

        if (!value)
                have[length++] = '0';

        while (value)
        {
                have[length++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (length)
                ps_byte(have[--length]);
}

static fn ps_pair(positive value)
{
        ps_byte((p8)('0' + (value / 10) % 10));
        ps_byte((p8)('0' + value % 10));
}

static fn ps_wide(positive value, positive width)
{
        p8 have[24];
        positive length = 0;
        positive scratch = value;

        if (!scratch)
                have[length++] = '0';

        while (scratch)
        {
                have[length++] = (p8)('0' + scratch % 10);
                scratch /= 10;
        }

        for (positive i = length; i < width; i++)
                ps_byte('0');

        while (length)
                ps_byte(have[--length]);
}

static fn ps_put_time(positive ticks)
{
        positive seconds = ticks / ps_clock;

        ps_wide(seconds / 3600, 2);
        ps_byte(':');
        ps_pair((seconds / 60) % 60);
        ps_byte(':');
        ps_pair(seconds % 60);
}

static fn ps_put_elapsed(positive seconds)
{
        positive days = seconds / 86400;
        positive rest = seconds % 86400;

        if (days)
        {
                ps_digits(days);
                ps_byte('-');
                ps_pair(rest / 3600);
        }
        else if (rest >= 3600)
        {
                ps_wide(rest / 3600, 2);
        }
        else
        {
                ps_wide((rest / 60) % 60, 2);
                ps_byte(':');
                ps_pair(rest % 60);
                return;
        }

        ps_byte(':');
        ps_pair((rest / 60) % 60);
        ps_byte(':');
        ps_pair(rest % 60);
}

static fn ps_put_tty(positive tty)
{
        if (!tty)
        {
                ps_byte('?');
                return;
        }

        positive major = (tty >> 8) & 0xfff;
        positive minor = (tty & 0xff) | ((tty >> 12) & 0xfff00);

        if (major == 136)
        {
                ps_text("pts/");
                ps_digits(minor);
                return;
        }

        ps_text("tty");
        ps_digits(minor);
}

static fn ps_draw(ps_process address_to one, positive field)
{
        ps_room_used = 0;

        switch (field)
        {
        case PS_FIELD_PID: ps_digits(one->pid); break;
        case PS_FIELD_PPID: ps_digits(one->ppid); break;
        case PS_FIELD_USER:
        case PS_FIELD_UID: ps_text(one->user); break;
        case PS_FIELD_COMM: ps_text(one->comm); break;
        case PS_FIELD_ARGS: ps_text(one->args); break;
        case PS_FIELD_STAT: ps_text(one->state); break;
        case PS_FIELD_TIME: ps_put_time(one->utime + one->stime); break;
        case PS_FIELD_ETIME:
        {
                positive began = one->start / ps_clock;

                ps_put_elapsed(ps_now > began ? ps_now - began : 0);
                break;
        }
        case PS_FIELD_RSS: ps_digits(one->rss); break;
        case PS_FIELD_VSZ: ps_digits(one->vsz); break;
        case PS_FIELD_TTY: ps_put_tty(one->tty); break;
        case PS_FIELD_CPU:
        {
                positive lived = ps_now > one->start / ps_clock
                                     ? ps_now - one->start / ps_clock
                                     : 0;

                ps_digits(lived ? (one->utime + one->stime) / ps_clock * 100 / lived : 0);
                break;
        }
        case PS_FIELD_STIME:
        {
                b64 began = (b64)ps_wall - (b64)ps_now + (b64)(one->start / ps_clock);
                b64 year, year_now;
                positive month, day, hour, minute, second;
                positive month_now, day_now, hour_now, minute_now, second_now;
                string_address months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

                file_split_moment(began, address_of year, address_of month,
                                  address_of day, address_of hour,
                                  address_of minute, address_of second);
                file_split_moment((b64)ps_wall, address_of year_now, address_of month_now,
                                  address_of day_now, address_of hour_now,
                                  address_of minute_now, address_of second_now);

                if (year == year_now && month == month_now && day == day_now)
                {
                        ps_wide(hour, 2);
                        ps_byte(':');
                        ps_pair(minute);
                }
                else if (year == year_now)
                {
                        ps_text(months[month - 1]);
                        ps_wide(day, 2);
                }
                else
                {
                        ps_digits((positive)year);
                }

                break;
        }
        default: break;
        }

        ps_room[ps_room_used] = end;
}

static fn ps_column_out(ps_process address_to one, positive field, bool last)
{
        positive width = ps_columns[field].width;

        ps_draw(one, field);

        // A column that something follows is exactly as wide as it says,
        // which is where the reference cuts a long command line off.
        if (!last && ps_room_used > width)
                ps_room_used = width;

        if (ps_columns[field].right)
        {
                for (positive i = ps_room_used; i < width; i++)
                        text_put_character(' ');

                text_put(ps_room, ps_room_used);
        }
        else
        {
                text_put(ps_room, ps_room_used);

                if (!last)
                        for (positive i = ps_room_used; i < width; i++)
                                text_put_character(' ');
        }

        if (!last)
                text_put_character(' ');
}

static b32 tools_ps(void)
{
        positive fields[32];
        positive field_count = 0;
        bool every = false;
        bool full = false;

        text_begin("ps");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] == 'o' && !argument[2])
                {
                        argument = text_argument(++i);

                        if (!argument)
                        {
                                text_error(null, "option requires an argument -- o");
                                return text_done(1);
                        }
                }
                else if (argument[0] == '-' && argument[1] == 'o')
                {
                        argument += 2;
                }
                else if (argument[0] == 'o' && argument[1])
                {
                        argument += 1;
                }
                else
                {
                        string_address at = argument;

                        if (string_get(at) == '-')
                                at++;

                        bool known = string_get(at) != end;

                        for (; string_get(at); at++)
                                switch (string_get(at))
                                {
                                case 'e':
                                case 'A': every = true; break;
                                case 'f': full = true; break;
                                case 'a':
                                case 'x':
                                case 'u': every = true; break;
                                default: known = false; break;
                                }

                        if (!known)
                        {
                                text_error(argument, "unsupported option");
                                return text_done(1);
                        }

                        continue;
                }

                string_address at = argument;

                while (string_get(at) && field_count < 32)
                {
                        p8 name[24];
                        positive used = 0;

                        while (string_get(at) && string_get(at) != ',' &&
                               string_get(at) != ' ' && used + 1 < sizeof(name))
                                name[used++] = string_get(at++);

                        name[used] = end;

                        while (string_get(at) == ',' || string_get(at) == ' ')
                                at++;

                        positive which = TEXT_UNSET;

                        for (positive f = 0; f < PS_FIELD_COUNT; f++)
                                if (string_equals(name, ps_columns[f].name))
                                {
                                        which = f;
                                        break;
                                }

                        if (which == TEXT_UNSET)
                        {
                                text_error(name, "unknown user-defined format specifier");
                                return text_done(1);
                        }

                        fields[field_count++] = which;
                }
        }

        if (!ps_gather())
        {
                text_error("/proc", "cannot read");
                return text_done(1);
        }

        {
                p8 block[4096];

                if (ps_read_file("/proc/self/stat", block, sizeof(block)))
                {
                        string_address last = string_last_of(block, ')');

                        if (last)
                        {
                                string_address at = last + 2;
                                ps_pass(address_of at, 4);
                                ps_own_tty = ps_take(address_of at);
                        }
                }
        }

        ps_own_uid = (positive)system_call(syscall(geteuid));

        /*
                The two listings ps has of its own are not -o spelled out:
                the terminal is eight columns wide and headed TTY rather than
                two and TT, and the command is headed CMD. The table above is
                what -o asks for, so the two that differ are set here.
        */
        if (!field_count)
        {
                ps_columns[PS_FIELD_TTY].header = "TTY";
                ps_columns[PS_FIELD_TTY].width = 8;

                if (full)
                {
                        ps_columns[PS_FIELD_ARGS].header = "CMD";

                        fields[field_count++] = PS_FIELD_UID;
                        fields[field_count++] = PS_FIELD_PID;
                        fields[field_count++] = PS_FIELD_PPID;
                        fields[field_count++] = PS_FIELD_CPU;
                        fields[field_count++] = PS_FIELD_STIME;
                        fields[field_count++] = PS_FIELD_TTY;
                        fields[field_count++] = PS_FIELD_TIME;
                        fields[field_count++] = PS_FIELD_ARGS;
                }
                else
                {
                        ps_columns[PS_FIELD_COMM].header = "CMD";

                        fields[field_count++] = PS_FIELD_PID;
                        fields[field_count++] = PS_FIELD_TTY;
                        fields[field_count++] = PS_FIELD_TIME;
                        fields[field_count++] = PS_FIELD_COMM;
                }
        }

        for (positive f = 0; f < field_count; f++)
        {
                positive field = fields[f];
                bool last = f + 1 == field_count;
                positive width = ps_columns[field].width;
                positive length = string_length(ps_columns[field].header);

                if (ps_columns[field].right)
                {
                        for (positive i = length; i < width; i++)
                                text_put_character(' ');

                        text_put_string(ps_columns[field].header);
                }
                else
                {
                        text_put_string(ps_columns[field].header);

                        if (!last)
                                for (positive i = length; i < width; i++)
                                        text_put_character(' ');
                }

                if (!last)
                        text_put_character(' ');
        }

        text_put_character('\n');

        for (positive p = 0; p < ps_count; p++)
        {
                ps_process address_to one = ps_list + p;

                if (!every && !(one->uid == ps_own_uid && one->tty == ps_own_tty))
                        continue;

                for (positive f = 0; f < field_count; f++)
                        ps_column_out(one, fields[f], f + 1 == field_count);

                text_put_character('\n');
        }

        return text_done(0);
}
