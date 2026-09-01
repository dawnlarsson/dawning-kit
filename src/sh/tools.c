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
#define DD_LCASE 0x080
#define DD_UCASE 0x100
#define DD_SWAB 0x200

#define DD_FULLBLOCK 0x001
#define DD_APPEND 0x001
#define DD_O_APPEND 02000

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

static fn dd_say_number(positive value)
{
        p8 digits[24];
        positive length = positive_into(digits, value);

        system_write_all(2, digits, length);
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
        text_error_raw("+");
        dd_say_number(dd_in_partial);
        text_error_raw(" records in\n");
        dd_say_number(dd_out_full);
        text_error_raw("+");
        dd_say_number(dd_out_partial);
        text_error_raw(" records out\n");

        if (dd_status_level == DD_STATUS_NOXFER)
                return;

        p8 si[32];
        p8 iec[32];
        positive si_length = positive_into_human_nearest_string(si, dd_written,
                                                                 false);
        positive iec_length = positive_into_human_nearest_string(iec, dd_written,
                                                                  true);

        dd_say_number(dd_written);
        text_error_raw(dd_written == 1 ? " byte" : " bytes");

        if (!dd_bare(si, si_length))
        {
                text_error_raw(" (");
                text_error_raw(si);

                if (!dd_bare(iec, iec_length))
                {
                        text_error_raw(", ");
                        text_error_raw(iec);
                }

                text_error_raw(")");
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

        text_error_raw(" copied, ");

        positive whole = elapsed / 1000000000u;
        positive rest = elapsed % 1000000000u;

        dd_say_number(whole);
        text_error_raw(".");

        p8 fraction[9];
        positive fraction_length = positive_into_padded(fraction, rest, 9, '0');

        system_write_all(2, fraction, fraction_length);

        text_error_raw(" s, ");

        p8 rate[32];
        positive per = elapsed >= 1000000000u
                           ? dd_written / (elapsed / 1000000000u)
                           : dd_written * (1000000000u / elapsed);

        positive_into_human_nearest_string(rate, per, false);
        text_error_raw(rate);
        text_error_raw("/s\n");
}

static bool dd_digits(string_address address_to text, positive address_to value)
{
        string_address at = address_to text;
        positive made = 0;
        bool any = false;

        while (byte_is_digit(string_get(at)))
        {
                positive digit = (positive)(string_get(at) - '0');

                if (made > (positive_max - digit) / 10)
                        return false;

                made = made * 10 + digit;
                at++;
                any = true;
        }

        if (!any)
                return false;

        address_to text = at;
        address_to value = made;
        return true;
}

static bool dd_size(string_address text, positive address_to out)
{
        positive total = 1;
        string_address at = text;

        if (!string_get(at))
                return false;

        while (1)
        {
                positive value;

                if (!dd_digits(address_of at, address_of value))
                        return false;

                positive power = file_size_power(string_get(at), false);
                positive multiple = 1;

                if (power)
                        at++;
                else switch (string_get(at))
                {
                case 'b': multiple = 512; at++; break;
                case 'c': at++; break;
                case 'w': multiple = 2; at++; break;
                case 'B': at++; break;
                }

                if (power)
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
                        {
                                if (multiple > positive_max / base)
                                        return false;

                                multiple *= base;
                        }
                }

                if (value && multiple > positive_max / value)
                        return false;

                positive piece = value * multiple;

                if (piece && total > positive_max / piece)
                        return false;

                total *= piece;

                if (string_get(at) != 'x')
                        break;

                at++;
        }

        if (string_get(at))
                return false;

        address_to out = total;

        return true;
}

// A final B on count, skip or seek changes the unit from blocks to bytes.
// It is still part of the ordinary size grammar (3KB is 3000), so parsing is
// shared and only this last-byte fact is carried separately.
static bool dd_quantity(string_address text, positive address_to out,
                        bool address_to bytes)
{
        positive length = string_length(text);

        address_to bytes = length && text[length - 1] == 'B';
        return dd_size(text, out);
}

static bool dd_operand(string_address argument, string_address name,
                       string_address address_to value)
{
        positive length = string_length(name);

        if (string_compare_max(argument, name, length) ||
            argument[length] != '=')
                return false;

        address_to value = argument + length + 1;

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

/*
        Every output path has the same failure contract. Keeping it here
        prevents regrouped blocks, the final partial block and seek padding
        from quietly accepting a short write while the equal-size fast path
        reports it.
*/
static positive dd_output(positive handle, string_address name,
                          p8 address_to bytes, positive length, bool copied)
{
        positive wrote = system_write_all(handle, bytes, length);

        if (copied)
                dd_written += wrote;

        if (wrote != length)
        {
                text_flush();
                text_error_raw("dd: error writing '");
                text_error_raw(name ? name : (string_address) "standard output");
                text_error_raw("'\n");
        }

        return wrote;
}

// Five failure paths say the same sentence: dd, what went wrong, which
// file, and the kernel's reason. One writer keeps their shapes from
// drifting apart.
static fn dd_complain(string_address verb, string_address name,
                      string_address fallback, bipolar code)
{
        text_flush();
        text_error_raw("dd: ");
        text_error_raw(verb);
        text_error_raw(" '");
        text_error_raw(name ? name : fallback);
        text_error_raw("': ");
        text_error_raw(file_reason(code));
        text_error_raw("\n");
}

// swab is one conversion over the byte stream, not one conversion per read.
// An odd byte therefore waits for the first byte of the next input record.
static positive dd_swab(p8 address_to into, p8 address_to from, positive length,
                        bool address_to pending, p8 address_to held)
{
        positive in = 0;
        positive out = 0;

        if (address_to pending && length)
        {
                into[out++] = from[in++];
                into[out++] = address_to held;
                address_to pending = false;
        }

        while (in + 1 < length)
        {
                into[out++] = from[in + 1];
                into[out++] = from[in];
                in += 2;
        }

        if (in < length)
        {
                address_to held = from[in];
                address_to pending = true;
        }

        return out;
}

// A short read is not the end of the input, and a partial record is not an
// error: both are counted and the next block is asked for.
static b32 tools_dd(void)
{
        string_address input = null;
        string_address output = null;
        positive ibs = 512;
        positive obs = 512;
        positive bs = 0;
        positive count = TEXT_UNSET;
        positive skip = 0;
        positive seek = 0;
        positive cbs = 0;
        positive conv = 0;
        positive iflags = 0;
        positive oflags = 0;
        bool bs_set = false;
        bool count_set = false;
        bool count_bytes = false;
        bool skip_bytes = false;
        bool seek_bytes = false;
        bool count_bytes_flag = false;
        bool skip_bytes_flag = false;
        bool seek_bytes_flag = false;
        b32 status = 0;

        text_begin("dd");

        dd_in_full = dd_in_partial = dd_out_full = dd_out_partial = 0;
        dd_written = 0;
        dd_status_level = DD_STATUS_ALL;
        dd_info_asked = 0;
        text_arena_used = 0;

        {
                p64 wall[2] = {0, 0};

                system_call_2(syscall(clock_gettime), 1, (positive)wall);
                dd_started = (positive)wall[0] * 1000000000u + (positive)wall[1];
        }

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = program_argument(i);
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
                        if (!dd_size(value, address_of bs))
                                return text_error(argument, "invalid number"), 1;

                        bs_set = true;
                }
                else if (dd_operand(argument, "count", address_of value))
                {
                        if (!dd_quantity(value, address_of count,
                                         address_of count_bytes))
                                return text_error(argument, "invalid number"), 1;

                        count_set = true;
                }
                else if (dd_operand(argument, "skip", address_of value) ||
                         dd_operand(argument, "iseek", address_of value))
                {
                        if (!dd_quantity(value, address_of skip,
                                         address_of skip_bytes))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "seek", address_of value) ||
                         dd_operand(argument, "oseek", address_of value))
                {
                        if (!dd_quantity(value, address_of seek,
                                         address_of seek_bytes))
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
                                else if (dd_word(address_of at, "lcase"))
                                        conv |= DD_LCASE;
                                else if (dd_word(address_of at, "ucase"))
                                        conv |= DD_UCASE;
                                else if (dd_word(address_of at, "swab"))
                                        conv |= DD_SWAB;
                                else
                                        return text_error(at, "invalid conversion"), 1;
                        }
                }
                else if (dd_operand(argument, "iflag", address_of value))
                {
                        string_address at = value;

                        if (!string_get(at))
                                return text_error(value, "invalid input flag"), 1;

                        while (string_get(at))
                                if (dd_word(address_of at, "fullblock"))
                                        iflags |= DD_FULLBLOCK;
                                else if (dd_word(address_of at, "count_bytes"))
                                        count_bytes_flag = true;
                                else if (dd_word(address_of at, "skip_bytes"))
                                        skip_bytes_flag = true;
                                else
                                        return text_error(at, "invalid input flag"), 1;
                }
                else if (dd_operand(argument, "oflag", address_of value))
                {
                        string_address at = value;

                        if (!string_get(at))
                                return text_error(value, "invalid output flag"), 1;

                        while (string_get(at))
                                if (dd_word(address_of at, "append"))
                                        oflags |= DD_APPEND;
                                else if (dd_word(address_of at, "seek_bytes"))
                                        seek_bytes_flag = true;
                                else
                                        return text_error(at, "invalid output flag"), 1;
                }
                else if (dd_operand(argument, "cbs", address_of value))
                {
                        // cbs has no effect until block or unblock is chosen,
                        // but it remains a number and nonsense must not pass.
                        if (!dd_size(value, address_of cbs))
                                return text_error(argument, "invalid number"), 1;
                }
                else
                {
                        text_error(argument, "unrecognized operand");
                        return 1;
                }
        }

        (void)cbs;

        if (bs_set)
                ibs = obs = bs;

        count_bytes |= count_bytes_flag;
        skip_bytes |= skip_bytes_flag;
        seek_bytes |= seek_bytes_flag;

        if ((conv & DD_LCASE) && (conv & DD_UCASE))
                return text_error(null, "cannot combine lcase and ucase"), 1;

        if (!ibs || !obs || ibs > positive_max - 31 || obs > positive_max - 31)
        {
                text_error(null, "invalid number");
                return 1;
        }

        if ((count_set && count > (positive)bipolar_max) ||
            (skip && skip > (positive)bipolar_max / (skip_bytes ? 1 : ibs)) ||
            (seek && seek > (positive)bipolar_max / (seek_bytes ? 1 : obs)))
        {
                text_error(null, "offset too large");
                return 1;
        }

        positive in_handle = 0;
        positive out_handle = 1;

        if (input)
        {
                bipolar opened = text_open_handle(input, FILE_READ, 0);

                if (opened < 0)
                {
                        dd_complain("failed to open", input,
                                    "standard input", opened);
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

                if (oflags & DD_APPEND)
                        flags |= DD_O_APPEND;

                if (!(conv & DD_NOTRUNC))
                        flags |= O_TRUNC;

                bipolar opened = text_open_handle(output, flags, 0666);

                if (opened < 0)
                {
                        dd_complain("failed to open", output,
                                    "standard output", opened);
                        return 1;
                }

                out_handle = (positive)opened;
        }

        p8 address_to ibuf = (p8 address_to)text_arena_take(ibs + 16);
        p8 address_to obuf = ibs == obs && !(conv & DD_SWAB)
                                 ? ibuf
                                 : (p8 address_to)text_arena_take(obs + 16);
        p8 address_to converted = conv & DD_SWAB
                                      ? (p8 address_to)text_arena_take(ibs + 16)
                                      : ibuf;

        if (!ibuf || !obuf || !converted)
                return 1;

        dd_listen(DD_SIGNAL_INFO);

        if (skip)
        {
                positive want = skip_bytes ? skip : skip * ibs;
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
                                bipolar got = system_read_retry(in_handle, ibuf, ask);

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
                        text_error_raw("dd: '");
                        text_error_raw(input ? input : (string_address) "standard input");
                        text_error_raw("': cannot skip to specified offset\n");
                }
        }

        if (seek)
        {
                positive want = seek_bytes ? seek : seek * obs;
                bipolar landed = system_call_3(syscall(lseek), out_handle, want, 0);

                if (landed < 0)
                {
                        text_flush();
                        text_error_raw("dd: '");
                        text_error_raw(output ? output
                                              : (string_address) "standard output");
                        text_error_raw("': cannot seek\n");
                        status = 1;
                }
                else if (!(conv & DD_NOTRUNC))
                {
                        bipolar truncated = system_call_2(syscall(ftruncate), out_handle,
                                                          want);

                        if (truncated < 0)
                        {
                                text_error(output, "cannot truncate");
                                status = 1;
                        }
                }
        }

        positive held = 0;
        b32 result = status;
        positive partial_before = 0;
        positive input_bytes = 0;
        bool swab_pending = false;
        p8 swab_held = 0;

        while (count != 0 && !result)
        {
                if (dd_info_asked)
                {
                        dd_info_asked = 0;
                        dd_summary();
                }

                if (!count_bytes && count != TEXT_UNSET &&
                    dd_in_full + dd_in_partial >= count)
                        break;

                positive ask = ibs;

                if (count_bytes && count != TEXT_UNSET)
                {
                        if (input_bytes >= count)
                                break;

                        if (ask > count - input_bytes)
                                ask = count - input_bytes;
                }

                if (conv & (DD_SYNC | DD_NOERROR))
                        memory_fill(ibuf, 0, ibs);

                bipolar got;

                if (iflags & DD_FULLBLOCK)
                {
                        positive gathered = 0;

                        while (gathered < ask)
                        {
                                got = system_read_retry(in_handle, ibuf + gathered,
                                                        ask - gathered);

                                if (got <= 0)
                                        break;

                                gathered += (positive)got;
                        }

                        if (gathered)
                                got = (bipolar)gathered;
                }
                else
                {
                        got = system_read_retry(in_handle, ibuf, ask);
                }

                if (!got)
                        break;

                if (got < 0)
                {
                        dd_complain("error reading", input, "standard input",
                                    got);

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

                input_bytes += read_bytes;

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

                if (conv & DD_LCASE)
                        memory_to_lower_ascii(ibuf, read_bytes);

                if (conv & DD_UCASE)
                        memory_to_upper_ascii(ibuf, read_bytes);

                p8 address_to output_bytes = ibuf;

                if (conv & DD_SWAB)
                {
                        read_bytes = dd_swab(converted, ibuf, read_bytes,
                                             address_of swab_pending,
                                             address_of swab_held);
                        output_bytes = converted;
                }

                if (ibuf == obuf)
                {
                        positive wrote = dd_output(out_handle, output, obuf,
                                                   read_bytes, true);

                        if (wrote != read_bytes)
                        {
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

                        memory_copy_apart(obuf + held, output_bytes + at, take);
                        held += take;
                        at += take;

                        if (held < obs)
                                continue;

                        positive wrote = dd_output(out_handle, output, obuf, obs,
                                                   true);
                        held = 0;

                        if (wrote != obs)
                        {
                                if (wrote)
                                        dd_out_partial++;

                                result = 1;
                                break;
                        }

                        dd_out_full++;
                }

                if (result)
                        break;
        }

        if (swab_pending)
                obuf[held++] = swab_held;

        if (held)
        {
                positive wrote = dd_output(out_handle, output, obuf, held, true);

                if (wrote)
                        dd_out_partial++;

                if (wrote != held)
                        result = 1;
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
                        dd_complain("fdatasync failed for", output,
                                    "standard output", done);
                        result = 1;
                }
        }

        if (conv & DD_FSYNC)
        {
                bipolar done = system_call_1(syscall(fsync), out_handle);

                if (done < 0)
                {
                        dd_complain("fsync failed for", output,
                                    "standard output", done);
                        result = 1;
                }
        }

        if (out_handle != 1 && system_call_1(syscall(close), out_handle) < 0)
        {
                text_error(output, "close failed");
                result = 1;
        }

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

#define DIFF_LARGE (positive_max / 8)

static bool diff_icase;
static positive diff_space;
static bool diff_blank_lines;
static bool diff_brief;
static bool diff_recursive;
static bool diff_new_file;
static bool diff_new_file_left;
static bool diff_text;
static bool diff_identical;
static bool diff_trailing;
static bool diff_strip_cr;
static bool diff_tabs;
static positive diff_style;
static positive diff_context = 3;
static string_address diff_labels[2];
static positive diff_label_count;
static bool diff_style_seen;
static p8 address_to diff_switches;
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

static bool diff_slurp(diff_side address_to side, string_address path,
                       bool allow_missing)
{
        bipolar handle = 0;

        side->base = null;
        side->size = 0;
        side->incomplete = false;
        side->missing = false;
        side->modified_seconds = 0;
        side->modified_nanoseconds = 0;

        if (path && !string_equals(path, "-"))
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
                        if (allow_missing && handle == -ERROR_NO_ENTRY)
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

                bipolar got = system_read_retry((positive)handle, block,
                                                TEXT_READ_MAX);

                if (got < 0)
                {
                        if (handle > 0)
                                system_call_1(syscall(close), handle);

                        text_error(path ? path : (string_address) "standard input",
                                   "Read error");
                        return false;
                }

                if (!got)
                        break;

                have += (positive)got;

                if ((positive)got < TEXT_READ_MAX)
                        break;
        }

        if (handle > 0)
                system_call_1(syscall(close), handle);

        if (diff_strip_cr && have > 1)
        {
                positive read = 0;
                positive write = 0;

                while (read < have)
                {
                        if (start[read] == '\r' && read + 1 < have &&
                            start[read + 1] == '\n')
                        {
                                read++;
                                continue;
                        }

                        start[write++] = start[read++];
                }

                have = write;
        }

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

// Lines -----------------------------------------------------

static p8 diff_fold(p8 value)
{
        if (diff_icase)
                return (p8)byte_to_lower(value);

        return value;
}

/*
        One line's worth of bytes, in the shape the ignore flags leave it.

        The walk stops at the newline every line in the buffer has, so the
        caller never has to know where a line ends.
*/
typedef struct
{
        p8 address_to at;
        p8 address_to stop;
        p8 held;
        positive column;
        positive tab_left;
        bool done;
} diff_scan;

static fn diff_scan_open(diff_scan address_to scan, p8 address_to line)
{
        scan->at = line;
        scan->stop = (p8 address_to)string_first_of_or_end(line, '\n');

        if (diff_trailing)
                while (scan->stop > line && byte_is_space(scan->stop[-1]))
                        scan->stop--;

        scan->done = false;
        scan->column = 0;
        scan->tab_left = 0;
}

static bool diff_scan_next(diff_scan address_to scan, p8 address_to out)
{
        if (scan->done)
                return false;

        if (scan->tab_left)
        {
                scan->tab_left--;
                scan->column++;
                address_to out = ' ';
                return true;
        }

        if (diff_space == DIFF_SPACE_ALL)
        {
                while (scan->at < scan->stop && byte_is_space(address_to scan->at))
                        scan->at++;

                if (scan->at == scan->stop)
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
                if (scan->at == scan->stop)
                {
                        scan->done = true;
                        return false;
                }

                if (byte_is_space(address_to scan->at))
                {
                        while (scan->at < scan->stop && byte_is_space(address_to scan->at))
                                scan->at++;

                        if (scan->at == scan->stop)
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

        if (scan->at == scan->stop)
        {
                scan->done = true;
                return false;
        }

        if (diff_tabs && address_to scan->at == '\t')
        {
                positive spaces = 8 - scan->column % 8;

                scan->at++;
                scan->column++;
                scan->tab_left = spaces - 1;
                address_to out = ' ';
                return true;
        }

        address_to out = diff_fold(address_to scan->at);
        scan->at++;
        scan->column++;

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

static PURE positive diff_hash(diff_side address_to side, bipolar middle)
{
        positive length = diff_line_length(side, middle);

        // The common exact-line path is already a bounded span. Short lines
        // stay inline; past the measured break-even the four-byte polynomial
        // floor removes three quarters of the dependent hash updates.
        if (length >= 24 && diff_space == DIFF_SPACE_NONE && !diff_trailing &&
            !diff_tabs && !diff_icase)
                return memory_hash_33(diff_line(side, middle), length) * 2 +
                       (diff_stub(side, middle) ? 1 : 0);

        diff_scan scan;
        positive value = 5381;
        p8 one;

        diff_scan_open(address_of scan, diff_line(side, middle));

        while (diff_scan_next(address_of scan, address_of one))
                value = value * 33 + one;

        return value * 2 + (diff_stub(side, middle) ? 1 : 0);
}

static PURE bool diff_same(diff_side address_to a, bipolar i, diff_side address_to b,
                      bipolar j)
{
        diff_scan left, right;
        p8 one, two;

        if (diff_stub(a, i) != diff_stub(b, j))
                return false;

        // With no whitespace normalization the lines remain exact bounded
        // spans; case folding only changes which block comparator proves it.
        if (diff_space == DIFF_SPACE_NONE && !diff_trailing && !diff_tabs)
        {
                positive length = diff_line_length(a, i);

                if (length != diff_line_length(b, j))
                        return false;

                return !(diff_icase
                              ? memory_compare_ascii_case(diff_line(a, i),
                                                          diff_line(b, j), length)
                              : memory_compare(diff_line(a, i), diff_line(b, j),
                                               length));
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

// The loop this used to be is one count of leading zeros; the or with one
// keeps a zero from asking about a word with nothing set.
static positive diff_floor_log2(positive value)
{
        return positive_bits - 1 - bits_leading_zeros(value | 1);
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
static PURE bool diff_trivial(positive from, positive to)
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
        positive_to_string(text_put, value);
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
static PURE positive diff_hunk_end(positive start)
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

// Every one-line verdict diff gives is the same sentence around " and ",
// with a different head and tail.
static fn diff_announce(string_address head, string_address left,
                        string_address right, string_address tail)
{
        text_put_string(head);
        text_put_string(left);
        text_put_string(" and ");
        text_put_string(right);
        text_put_string(tail);
        text_flush();
}

static fn diff_identical_output(string_address left, string_address right)
{
        if (diff_identical)
                diff_announce("Files ", left, right, " are identical\n");
}

// One pair of files -----------------------------------------

static b32 diff_pair(string_address left, string_address right)
{
        diff_side address_to a = diff_files + 0;
        diff_side address_to b = diff_files + 1;

        memory_fill(a, 0, sizeof(diff_side));
        memory_fill(b, 0, sizeof(diff_side));

        if (!diff_slurp(a, left, diff_new_file || diff_new_file_left) ||
            !diff_slurp(b, right, diff_new_file))
                return 2;

        // Whether either file looks like something to diff by lines at all.
        if (!diff_text &&
            (memory_first_of(a->base, 0, a->size) ||
             memory_first_of(b->base, 0, b->size)))
        {
            if (a->size == b->size && !memory_compare(a->base, b->base, a->size))
                {
                        diff_identical_output(left, right);
                        return 0;
                }

                diff_announce(diff_brief ? "Files " : "Binary files ", left,
                              right, " differ\n");

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
        positive bytes = memory_common_prefix(a->base, b->base, shortest);

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

        memory_fill(diff_buckets, (b8)-1,
                    diff_bucket_count * sizeof(diff_buckets[0]));

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
        {
                diff_identical_output(left, right);
                return 0;
        }

        if (diff_brief)
        {
                diff_announce("Files ", left, right, " differ\n");

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

/*
        The names in one directory, with the bytes taken from the arena.

        A walk that is going to recurse cannot keep its names in a buffer of
        its own, because the level below would write over them and the level
        above would go on comparing whatever landed there.
*/
typedef struct
{
        string_address address_to at;
        positive count;
        positive room;
} diff_names;

/*
        Every growing table in this file doubles the same way, out of the
        text arena. The arena cannot take a block back, so growth allocates
        the doubled room, copies what is used, and abandons the old block;
        the abandoned halves sum to less than the final table. One grower
        keeps six tables from writing that walk out by hand.
*/
static bool tools_grow(address_any table, positive address_to room,
                       positive used, positive need, positive unit,
                       positive first)
{
        if (need <= address_to room)
                return true;

        positive larger = address_to room ? address_to room : first;

        while (larger < need)
        {
                if (larger > positive_max / 2 ||
                    larger * 2 > positive_max / unit)
                        return false;

                larger *= 2;
        }

        address_any grown = text_arena_take(larger * unit);

        if (!grown)
                return false;

        if (used)
                memory_copy_apart(grown,
                                  address_to(address_any address_to)table,
                                  used * unit);

        address_to(address_any address_to)table = grown;
        address_to room = larger;
        return true;
}

static bool diff_name_add(diff_names address_to names, string_address value)
{
        if (!tools_grow(address_of names->at, address_of names->room,
                        names->count, names->count + 1,
                        sizeof(string_address), 32))
                return false;

        names->at[names->count++] = value;
        return true;
}

static bool diff_names_sort(diff_names address_to names)
{
        if (names->count < 2)
                return true;

        string_address address_to spare =
            (string_address address_to)text_arena_take(
                names->count * sizeof(string_address));

        if (!spare)
                return false;

        string_address address_to from = names->at;
        string_address address_to into = spare;

        for (positive width = 1; width < names->count; width *= 2)
        {
                for (positive base = 0; base < names->count; base += width * 2)
                {
                        positive middle = min(base + width, names->count);
                        positive stop = min(middle + width, names->count);
                        positive left = base;
                        positive right = middle;
                        positive out = base;

                        while (left < middle && right < stop)
                                into[out++] = string_compare(from[left], from[right]) <= 0
                                                  ? from[left++]
                                                  : from[right++];

                        /* Once either run is exhausted the other is already
                           ordered. Hand the whole pointer tail to the bulk
                           floor instead of paying one scalar load/store and
                           one taken branch per name. */
                        positive tail = left < middle ? middle - left
                                                       : stop - right;
                        string_address address_to rest = left < middle
                                                             ? from + left
                                                             : from + right;

                        if (tail)
                                memory_copy_apart(into + out, rest,
                                                  tail * sizeof(string_address));
                }

                string_address address_to swapped = from;

                from = into;
                into = swapped;
        }

        names->at = from;
        return true;
}

static string_address diff_path(string_address directory, string_address name)
{
        positive head = string_length(directory);
        positive tail = string_length(name);
        positive slash = head && directory[head - 1] != '/';

        if (tail > positive_max - slash - 1 ||
            head > positive_max - tail - slash - 1)
                return null;

        positive room = head + tail + slash + 1;
        p8 address_to joined = (p8 address_to)text_arena_take(room);

        if (!joined)
                return null;

        path_join(joined, room, directory, name);
        return (string_address)joined;
}

static bool diff_gather(string_address path, diff_names address_to names,
                        bool allow_missing)
{
        file_walk walk;

        names->at = null;
        names->count = 0;
        names->room = 0;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                if (allow_missing)
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

                positive length = string_length(entry->d_name);
                p8 address_to at = (p8 address_to)text_arena_take(length + 1);

                if (!at)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                memory_copy_apart(at, entry->d_name, length + 1);

                if (!diff_name_add(names, (string_address)at))
                {
                        file_walk_close(address_of walk);
                        return false;
                }
        }

        file_walk_close(address_of walk);

        return diff_names_sort(names);
}

static b32 diff_walk(string_address left, string_address right, positive depth);

/*
        A name present in one directory only. With the new-file flags it is
        compared against the nothing on the other side; otherwise it is
        announced. A path that could not be made says so through failed,
        because that must stop the walk while an ordinary difference must
        not.
*/
static b32 diff_one_sided(string_address left, string_address right,
                          string_address inside, string_address name,
                          bool compare, positive depth,
                          bool address_to failed)
{
        if (compare)
        {
                string_address only_left = diff_path(left, name);
                string_address only_right = diff_path(right, name);

                if (!only_left || !only_right)
                {
                        address_to failed = true;
                        return 2;
                }

                return diff_walk(only_left, only_right, depth + 1);
        }

        text_put_string("Only in ");
        text_put_string(inside);
        text_put_string(": ");
        text_put_string(name);
        text_put_character('\n');
        text_flush();

        return 1;
}

static b32 diff_directories(string_address left, string_address right, positive depth)
{
        if (depth >= FILE_MAX_DEPTH)
                return 2;

        diff_names names[2];

        if (!diff_gather(left, names + 0, diff_new_file || diff_new_file_left) ||
            !diff_gather(right, names + 1, diff_new_file))
                return 2;

        positive i = 0, j = 0;
        b32 worst = 0;

        while (i < names[0].count || j < names[1].count)
        {
                /* The two gathered name tables have to survive the whole
                   directory, but paths and file-diff workspaces belong only
                   to this child. Rewind them after the child returns so a
                   wide tree costs its maximum file, not the sum of every
                   file visited before it. */
                positive child_mark = text_arena_used;
                b32 order = i >= names[0].count
                                ? 1
                                : j >= names[1].count
                                      ? -1
                                      : (b32)string_compare(names[0].at[i],
                                                            names[1].at[j]);

                if (order)
                {
                        bool failed = false;
                        b32 one = order < 0
                                      ? diff_one_sided(left, right, left,
                                                       names[0].at[i],
                                                       diff_new_file, depth,
                                                       address_of failed)
                                      : diff_one_sided(left, right, right,
                                                       names[1].at[j],
                                                       diff_new_file ||
                                                           diff_new_file_left,
                                                       depth,
                                                       address_of failed);

                        if (failed)
                                return 2;

                        if (worst < one)
                                worst = one;

                        text_arena_used = child_mark;

                        if (order < 0)
                                i++;
                        else
                                j++;

                        continue;
                }

                string_address one_left = diff_path(left, names[0].at[i]);
                string_address one_right = diff_path(right, names[1].at[j]);

                if (!one_left || !one_right)
                        return 2;

                bool left_directory = file_is_directory_through(one_left);
                bool right_directory = file_is_directory_through(one_right);

                if (left_directory && right_directory && !diff_recursive)
                {
                        diff_announce("Common subdirectories: ", one_left,
                                      one_right, "\n");
                }
                else
                {
                        b32 one = diff_walk(one_left, one_right, depth + 1);

                        if (worst < one)
                                worst = one;
                }

                text_arena_used = child_mark;

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

        /*
                Two names for the same regular inode cannot differ. Avoiding
                the pair of complete reads is especially important here:
                diff keeps both inputs for its line algorithm, so an otherwise
                trivial same-file comparison could consume the entire arena.
        */
        if (left_here && right_here && !left_directory && !right_directory)
        {
                file_facts left_facts;
                file_facts right_facts;

                if (file_look_at(left, address_of left_facts) &&
                    file_look_at(right, address_of right_facts) &&
                    (left_facts.mode & MODE_FORMAT) == MODE_FILE &&
                    (right_facts.mode & MODE_FORMAT) == MODE_FILE &&
                    file_same_identity(address_of left_facts,
                                       address_of right_facts))
                {
                        diff_identical_output(left, right);
                        return 0;
                }
        }

        bool absent_directory_is_empty =
            !(left_here && right_here) &&
            (diff_new_file || (diff_new_file_left && !left_here && right_here));

        if ((left_directory || right_directory) &&
            (left_directory == right_directory || absent_directory_is_empty))
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
        positive pair_mark = text_arena_used;

        diff_titled = depth > 0;

        b32 one = diff_pair(left, right);

        diff_titled = titled;
        text_arena_used = pair_mark;

        return one;
}

static const file_long diff_longs[] = {
    {(string_address) "normal", 'z'},
    {(string_address) "unified", 'v'},
    {(string_address) "brief", 'q'},
    {(string_address) "report-identical-files", 's'},
    {(string_address) "recursive", 'r'},
    {(string_address) "new-file", 'N'},
    {(string_address) "unidirectional-new-file", 'O'},
    {(string_address) "no-ignore-file-name-case", 'J'},
    {(string_address) "ignore-case", 'i'},
    {(string_address) "ignore-tab-expansion", 'E'},
    {(string_address) "ignore-all-space", 'w'},
    {(string_address) "ignore-space-change", 'b'},
    {(string_address) "ignore-trailing-space", 'Z'},
    {(string_address) "ignore-blank-lines", 'B'},
    {(string_address) "text", 'a'},
    {(string_address) "strip-trailing-cr", 'R'},
    {(string_address) "speed-large-files", 'h'},
    {(string_address) "label", 'L'},
    {null, 0},
};

static bool diff_context_set(string_address value)
{
        positive context;
        string_address at = value;

        if (!value)
        {
                diff_context = 3;
                return true;
        }

        if (!dd_digits(address_of at, address_of context) || string_get(at) ||
            context > (positive_max - 1) / 2)
        {
                text_error(value, "invalid context length");
                return false;
        }

        diff_context = context;
        return true;
}

// Labels, context values and output styles need arrival order; one flag word
// and one value per letter cannot represent any of those three contracts.
static bool diff_option_seen(p8 letter, string_address value)
{
        if (letter == 'L')
        {
                if (diff_label_count >= 2)
                {
                        text_error(value, "too many file label options");
                        return false;
                }

                diff_labels[diff_label_count++] = value;
                return true;
        }

        if (letter == 'u' || letter == 'U' || letter == 'v' || letter == 'z')
        {
                positive style = letter == 'z' ? DIFF_NORMAL : DIFF_UNIFIED;

                if (diff_style_seen && diff_style != style)
                {
                        text_error(null, "conflicting output style options");
                        return false;
                }

                diff_style = style;
                diff_style_seen = true;

                if (style == DIFF_UNIFIED && (letter == 'U' || letter == 'v'))
                        return diff_context_set(value);

                return true;
        }

        return true;
}

static b32 tools_diff(void)
{
        file_taking taking = {
            .program = (string_address) "diff",
            .allowed = (string_address) "BELNUZabiqrsuw",
            .valued = (string_address) "LU",
            .optional = (string_address) "v",
            .longs = diff_longs,
            .seen = diff_option_seen,
        };

        text_begin("diff");

        diff_brief = false;
        diff_style = DIFF_NORMAL;
        diff_context = 3;
        diff_labels[0] = diff_labels[1] = null;
        diff_label_count = 0;
        diff_style_seen = false;
        diff_switches_used = 0;
        diff_titled = false;
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(2);

        positive flags = taking.flags;
        b32 first = (b32)taking.first;

        positive switches_room = 1;

        for (positive i = 1; i < taking.first; i++)
                switches_room += string_length(program_argument((b32)i)) + 1;

        diff_switches = (p8 address_to)text_arena_take(switches_room);

        if (!diff_switches)
                return text_done(2);

        diff_icase = (flags & FILE_FLAG('i')) != 0;
        diff_blank_lines = (flags & FILE_FLAG('B')) != 0;
        diff_recursive = (flags & FILE_FLAG('r')) != 0;
        diff_new_file = (flags & FILE_FLAG('N')) != 0;
        diff_new_file_left = (flags & FILE_FLAG('O')) != 0;
        diff_text = (flags & FILE_FLAG('a')) != 0;
        diff_identical = (flags & FILE_FLAG('s')) != 0;
        diff_trailing = (flags & FILE_FLAG('Z')) != 0;
        diff_strip_cr = (flags & FILE_FLAG('R')) != 0;
        diff_tabs = (flags & FILE_FLAG('E')) != 0;
        diff_brief = (flags & FILE_FLAG('q')) != 0;
        diff_space = (flags & FILE_FLAG('w'))   ? DIFF_SPACE_ALL
                     : (flags & FILE_FLAG('b')) ? DIFF_SPACE_CHANGE
                                                : DIFF_SPACE_NONE;

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

                diff_switches[diff_switches_used++] = ' ';
                memory_copy_apart(diff_switches + diff_switches_used, word, length);
                diff_switches_used += length;

                if (word[length - 1] == 'U' && i + 1 < taking.first)
                {
                        string_address context = program_argument((b32)(i + 1));
                        positive context_length = string_length(context);

                        diff_switches[diff_switches_used++] = ' ';
                        memory_copy_apart(diff_switches + diff_switches_used,
                                         context, context_length);
                        diff_switches_used += context_length;
                }
        }

        if (text_argument_count - first != 2)
        {
                text_error(null, "missing operand");
                return text_done(2);
        }

        string_address left = program_argument(first);
        string_address right = program_argument(first + 1);
        string_address joined;

        // diff dir file and diff file dir both mean the same file inside the
        // directory, which is the one place the two names are not the pair.
        if (file_is_directory_through(left) && !file_is_directory_through(right))
        {
                joined = diff_path(left, file_last_component(right));

                if (!joined)
                        return text_done(2);

                left = joined;
        }
        else if (!file_is_directory_through(left) && file_is_directory_through(right))
        {
                joined = diff_path(right, file_last_component(left));

                if (!joined)
                        return text_done(2);

                right = joined;
        }

        if (string_equals(left, "-") && string_equals(right, "-"))
        {
                diff_identical_output(left, right);
                diff_result = 0;
        }
        else
        {
                diff_result = diff_walk(left, right, 0);
        }

        text_done(diff_result);

        // A diagnostic utility distinguishes a difference (1) from being
        // unable to report one (2). A failed final buffered flush is the
        // latter even when the comparison itself completed.
        return text_out_failed ? 2 : diff_result;
}

// ps --------------------------------------------------------

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
#define PS_FIELD_SID 14
#define PS_FIELD_PGID 15
#define PS_FIELD_NLWP 16
#define PS_FIELD_ETIMES 17
#define PS_FIELD_COUNT 18

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
    {"tty", "TT", 2, false},        {"uid", "UID", 5, true},
    {"c", "C", 2, true},            {"stime", "STIME", 5, false},
    {"sid", "SID", 7, true},         {"pgid", "PGID", 7, true},
    {"nlwp", "NLWP", 4, true},       {"etimes", "ELAPSED", 7, true}};

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
        string_address comm;
        string_address args;
        string_address user;
} ps_process;

static ps_process address_to ps_list;
static positive ps_count;
static positive ps_room_processes;
static positive ps_clock = 100;
static positive ps_now;
static positive ps_own_tty;
static positive ps_own_uid;
static positive ps_wall;
static positive ps_page_kb = 4;

static positive ps_read_file(string_address path, p8 address_to into, positive limit)
{
        bipolar got = file_slurp(path, into, limit);

        return got > 0 ? (positive)got : 0;
}

static p8 address_to ps_read_growing(string_address path, positive first,
                                     positive address_to length)
{
        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
                return null;

        positive room = first ? first : 64;
        positive used = 0;
        positive arena_mark = text_arena_used;
        p8 address_to bytes = (p8 address_to)text_arena_take(room);

        if (!bytes)
        {
                system_call_1(syscall(close), (positive)handle);
                return null;
        }

        for (;;)
        {
                if (used + 1 >= room)
                {
                        if (room > positive_max / 2)
                        {
                                system_call_1(syscall(close), (positive)handle);
                                text_arena_used = arena_mark;
                                return null;
                        }

                        positive larger = room * 2;

                        /* This buffer is the newest arena object. Rewind and
                           extend it in place: its bytes remain at the same
                           address, so growth needs neither another retained
                           block nor a copy of everything read so far. */
                        text_arena_used = arena_mark;
                        p8 address_to grown =
                            (p8 address_to)text_arena_take(larger);

                        if (!grown)
                        {
                                system_call_1(syscall(close), (positive)handle);
                                text_arena_used = arena_mark;
                                return null;
                        }

                        bytes = grown;
                        room = larger;
                }

                bipolar got = system_read_retry((positive)handle, bytes + used,
                                                 room - used - 1);

                if (got < 0)
                {
                        system_call_1(syscall(close), (positive)handle);
                        text_arena_used = arena_mark;
                        return null;
                }

                if (!got)
                        break;

                used += (positive)got;
        }

        system_call_1(syscall(close), (positive)handle);
        bytes[used] = end;
        /* Give the unused half of the last doubling step back before ps
           starts retaining the next process. */
        text_arena_used = arena_mark + ((used + 1 + 15) & ~(positive)15);
        address_to length = used;
        return bytes;
}

static ps_process address_to ps_process_add()
{
        if (!tools_grow(address_of ps_list, address_of ps_room_processes,
                        ps_count, ps_count + 1, sizeof(ps_process), 128))
                return null;

        ps_process address_to made = ps_list + ps_count;

        memory_fill(made, 0, sizeof(ps_process));
        return made;
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
        positive taken;
        bipolar value = string_bipolar(here, address_of taken);

        address_to at = here + taken;
        return value;
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

// The name behind a numeric user id. file.c already reads /etc/passwd and
// remembers the last answer, so ps only keeps a copy that lives as long as
// the arena; the fallback for an unknown id is the number spelled out.
static string_address ps_name_of(positive uid)
{
        p8 name[FILE_NAME_MAX];
        positive length;

        if (file_user_name(uid, name, FILE_NAME_MAX))
                length = string_length(name);
        else
                length = positive_into(name, uid);

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return null;

        memory_copy_apart_end(made, name, length);
        return (string_address)made;
}

static bool ps_gather()
{
        p8 block[8192];
        file_walk walk;

        ps_count = 0;
        ps_room_processes = 0;
        ps_page_kb = 4;

        /*
                rss in /proc/PID/stat is counted in native pages, not in the
                4 KiB pages most machines happen to use. AT_PAGESZ is the
                kernel's architecture-specific answer and /proc/self/auxv is
                available anywhere the rest of this implementation is.
        */
        {
                p8 auxv[512];
                positive got = ps_read_file("/proc/self/auxv", auxv, sizeof(auxv));
                positive pair = 2 * sizeof(positive);

                for (positive at = 0; at + pair <= got; at += pair)
                {
                        positive type;
                        positive value;

                        memory_copy_apart(address_of type, auxv + at,
                                         sizeof(positive));
                        memory_copy_apart(address_of value,
                                         auxv + at + sizeof(positive),
                                         sizeof(positive));

                        if (type == 6 && value >= 1024)
                        {
                                ps_page_kb = value / 1024;
                                break;
                        }
                }
        }

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

        while ((entry = file_walk_next(address_of walk)))
        {
                if (!byte_is_digit(entry->d_name[0]))
                        continue;

                p8 path[64];

                positive base = path_join(path, sizeof(path), "/proc", entry->d_name);

                string_copy(path + base, "/stat");

                if (!ps_read_file(path, block, sizeof(block)))
                        continue;

                ps_process address_to one = ps_process_add();

                if (!one)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                string_address at = block;

                one->pid = ps_take(address_of at);

                at = string_first_of_or_end(at, '(') + 1;

                string_address close = at;
                string_address last = string_last_of(at, ')');

                if (!last)
                        continue;

                positive length = (positive)(last - close);
                p8 address_to command =
                    (p8 address_to)text_arena_take(length + 1);

                if (!command)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                memory_copy_apart_end(command, close, length);
                one->comm = (string_address)command;

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
                {
                        positive pages = ps_take(address_of at);

                        one->rss = pages > positive_max / ps_page_kb
                                       ? positive_max
                                       : pages * ps_page_kb;
                }

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

                one->user = ps_name_of(one->uid);

                if (!one->user)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                string_copy(path + base, "/cmdline");

                positive got = 0;
                p8 address_to arguments = ps_read_growing(path, 256,
                                                          address_of got);

                if (arguments && got)
                {
                        for (positive i = 0; i < got; i++)
                                arguments[i] = arguments[i] ? arguments[i] : ' ';

                        while (got && arguments[got - 1] == ' ')
                                got--;

                        arguments[got] = end;
                        one->args = (string_address)arguments;
                }
                else
                {
                        positive length = string_length(one->comm);
                        p8 address_to fallback =
                            (p8 address_to)text_arena_take(length + 3);

                        if (!fallback)
                        {
                                file_walk_close(address_of walk);
                                return false;
                        }

                        fallback[0] = '[';
                        memory_copy_apart(fallback + 1, one->comm, length);
                        fallback[1 + length] = ']';
                        fallback[2 + length] = end;
                        one->args = (string_address)fallback;
                }

                ps_count++;
        }

        file_walk_close(address_of walk);

        /* Bottom-up merge sort avoids quadratic startup on large /proc trees. */
        if (ps_count > 1)
        {
                if (ps_count > positive_max / sizeof(ps_process))
                        return false;

                ps_process address_to spare =
                    (ps_process address_to)text_arena_take(
                        ps_count * sizeof(ps_process));

                if (!spare)
                        return false;

                ps_process address_to from = ps_list;
                ps_process address_to into = spare;

                for (positive width = 1; width < ps_count;)
                {
                        for (positive base = 0; base < ps_count; base += 2 * width)
                        {
                                positive left = base;
                                positive middle = base + width < ps_count
                                                      ? base + width
                                                      : ps_count;
                                positive right = base + 2 * width < ps_count
                                                     ? base + 2 * width
                                                     : ps_count;
                                positive one = left;
                                positive two = middle;

                                while (one < middle && two < right)
                                {
                                        if (from[one].pid <= from[two].pid)
                                                into[left++] = from[one++];
                                        else
                                                into[left++] = from[two++];
                                }

                                /* A ps record is much wider than a pointer;
                                   after one run empties, copying the remaining
                                   contiguous records in one assembly call is
                                   cheaper than repeating structure copies. */
                                positive tail = one < middle ? middle - one
                                                              : right - two;
                                ps_process address_to rest = one < middle
                                                                 ? from + one
                                                                 : from + two;

                                if (tail)
                                        memory_copy_apart(into + left, rest,
                                                          tail * sizeof(ps_process));
                        }

                        ps_process address_to swap = from;
                        from = into;
                        into = swap;

                        if (width > ps_count / 2)
                                break;

                        width *= 2;
                }

                if (from != ps_list)
                        memory_copy_apart(ps_list, from,
                                         ps_count * sizeof(ps_process));
        }

        return true;
}

/*
        A field is drawn into a small buffer first, because a column is padded
        by how wide what it drew turned out to be and the shared output buffer
        can empty itself between one byte and the next.
*/
static p8 address_to ps_room;
static positive ps_room_used;
static positive ps_room_size;
static bool ps_failed;

static bool ps_room_add(positive extra)
{
        if (ps_room_used == positive_max ||
            extra > positive_max - ps_room_used - 1 ||
            !tools_grow(address_of ps_room, address_of ps_room_size,
                        ps_room_used, ps_room_used + extra + 1, 1, 64))
        {
                ps_failed = true;
                return false;
        }

        return true;
}

static fn ps_byte(p8 value)
{
        if (ps_room_add(1))
                ps_room[ps_room_used++] = value;
}

static fn ps_bytes(address_any value, positive length)
{
        if (!ps_room_add(length))
                return;

        memory_copy_apart(ps_room + ps_room_used, value, length);
        ps_room_used += length;
}

static fn ps_text(string_address value)
{
        if (value)
                ps_bytes(value, string_length(value));
}

static fn ps_digits(positive value)
{
        if (ps_room_add(20))
        {
                ps_room_used += positive_into(ps_room + ps_room_used, value);
                return;
        }

        p8 have[24];
        positive length = positive_into(have, value);

        ps_bytes(have, length);
}

static fn ps_pair(positive value)
{
        p8 pair[2];
        positive length = positive_into_pair(pair, value);

        ps_bytes(pair, length);
}

static fn ps_put_time(positive ticks)
{
        positive seconds = ticks / ps_clock;

        positive_to_padded(ps_bytes, seconds / 3600, 2, '0', 0);
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
                positive_to_padded(ps_bytes, rest / 3600, 2, '0', 0);
        }
        else
        {
                positive_to_padded(ps_bytes, (rest / 60) % 60, 2, '0', 0);
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
                ps_text(one->user);
                break;
        case PS_FIELD_UID: ps_digits(one->uid); break;
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
                        positive_to_padded(ps_bytes, hour, 2, '0', 0);
                        ps_byte(':');
                        ps_pair(minute);
                }
                else if (year == year_now)
                {
                        ps_text(months[month - 1]);
                        positive_to_padded(ps_bytes, day, 2, '0', 0);
                }
                else
                {
                        ps_digits((positive)year);
                }

                break;
        }
        case PS_FIELD_SID: ps_digits(one->session); break;
        case PS_FIELD_PGID: ps_digits(one->pgrp); break;
        case PS_FIELD_NLWP: ps_digits(one->threads); break;
        case PS_FIELD_ETIMES:
        {
                positive began = one->start / ps_clock;

                ps_digits(ps_now > began ? ps_now - began : 0);
                break;
        }
        default: break;
        }

        if (ps_room_add(0))
                ps_room[ps_room_used] = end;
}

static fn ps_column_out(ps_process address_to one, positive field,
                        positive width, bool last)
{
        ps_draw(one, field);

        // A column that something follows is exactly as wide as it says,
        // which is where the reference cuts a long command line off.
        if (!last && ps_room_used > width)
                ps_room_used = width;

        writer_field(text_put, ps_room, ps_room_used,
                     !ps_columns[field].right && last ? ps_room_used : width,
                     ' ', !ps_columns[field].right);

        if (!last)
                text_put_character(' ');
}

typedef struct
{
        positive field;
        string_address header;
        bool custom_header;
} ps_selected;

static bool ps_field_add(ps_selected address_to address_to fields,
                         positive address_to count, positive address_to room,
                         positive value, string_address header,
                         bool custom_header)
{
        if (!tools_grow(fields, room, address_to count, address_to count + 1,
                        sizeof(ps_selected), 16))
                return false;

        (address_to fields)[address_to count].field = value;
        (address_to fields)[address_to count].header = header;
        (address_to fields)[address_to count].custom_header = custom_header;
        address_to count += 1;
        return true;
}

static bool ps_value_add(positive address_to address_to values,
                         positive address_to count, positive address_to room,
                         positive value)
{
        if (!tools_grow(values, room, address_to count, address_to count + 1,
                        sizeof(positive), 16))
                return false;

        (address_to values)[address_to count] = value;
        address_to count += 1;
        return true;
}

static bool ps_value_has(positive address_to values, positive count,
                         positive value)
{
        for (positive i = 0; i < count; i++)
                if (values[i] == value)
                        return true;

        return false;
}

typedef struct
{
        string_address at;
        string_address from;
        positive length;
} ps_list_cursor;

/* A comma-or-space separated span. The caller supplies the bytes that end
   its kind of item because a format name also stops at =, while a command
   name may contain one. A separator-only tail is returned once as an empty
   span: three readers ignore it, while --sort deliberately refuses it. */
static bool ps_list_next(ps_list_cursor address_to list,
                         string_address stops)
{
        if (!string_get(list->at))
                return false;

        list->at += string_span_of_set(list->at, ", ");
        list->from = list->at;
        list->length = string_span_without_set(list->at, stops);
        list->at += list->length;
        return true;
}

static bool ps_pid_list(string_address list,
                        positive address_to address_to values,
                        positive address_to count, positive address_to room,
                        bool duplicate_within_operand)
{
        ps_list_cursor item = {.at = list};
        bool any = false;
        positive before = address_to count;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                if (!item.length)
                        break;

                positive value;
                string_address at = item.from;

                if (!dd_digits(address_of at, address_of value) ||
                    !value || (positive)(at - item.from) != item.length)
                        return false;

                /*
                        procps preserves a duplicate written in one -p list,
                        but repeated selection operands are unioned. Other
                        numeric selectors are sets in both shapes.
                */
                bool seen = ps_value_has(address_to values,
                                         duplicate_within_operand
                                             ? before
                                             : address_to count,
                                         value);

                if (!seen && !ps_value_add(values, count, room, value))
                        return false;

                any = true;
        }

        return any;
}

static bool ps_string_add(string_address address_to address_to values,
                          positive address_to count, positive address_to room,
                          string_address from, positive length)
{
        for (positive i = 0; i < address_to count; i++)
                if (string_length((address_to values)[i]) == length &&
                    !string_compare_max((address_to values)[i], from, length))
                        return true;

        if (!tools_grow(values, room, address_to count, address_to count + 1,
                        sizeof(string_address), 16))
                return false;

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return false;

        memory_copy_apart(made, from, length);
        made[length] = end;
        (address_to values)[address_to count] = (string_address)made;
        address_to count += 1;
        return true;
}

static bool ps_command_list(string_address list,
                            string_address address_to address_to values,
                            positive address_to count, positive address_to room)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                if (!item.length)
                        break;

                if (!ps_string_add(values, count, room, item.from,
                                   item.length))
                        return false;

                any = true;
        }

        return any;
}

static bool ps_command_selected(string_address address_to values,
                                positive count, string_address command)
{
        for (positive i = 0; i < count; i++)
        {
                positive length = string_length(values[i]);

                /* procps compares -C through Linux's 15-byte comm ceiling. */
                if (length > 15)
                        length = 15;

                if (string_length(command) == length &&
                    !string_compare_max(values[i], command, length))
                        return true;
        }

        return false;
}

static bool ps_sort_pid(string_address list, bool address_to reverse)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                string_address from = item.from;
                positive length = item.length;

                if (!length)
                        return false;

                bool descending = false;

                if (string_get(from) == '+' || string_get(from) == '-')
                {
                        descending = string_get(from++) == '-';
                        length--;
                }

                if (length != 3 || string_compare_max(from,
                                                       (string_address)"pid", 3))
                        return false;

                if (!any)
                        address_to reverse = descending;

                any = true;
        }

        return any;
}

static positive ps_pid_matches(positive address_to values, positive count,
                               positive pid)
{
        positive matches = 0;

        for (positive i = 0; i < count; i++)
                if (values[i] == pid)
                        matches++;

        return matches;
}

static bool ps_format_list(string_address list,
                           ps_selected address_to address_to fields,
                           positive address_to count, positive address_to room)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) "=, "))
        {
                string_address name_from = item.from;
                positive name_length = item.length;

                if (!name_length)
                {
                        if (!string_get(name_from))
                                break;

                        return false;
                }

                p8 address_to name =
                    (p8 address_to)text_arena_take(name_length + 1);

                if (!name)
                        return false;

                memory_copy_apart(name, name_from, name_length);
                name[name_length] = end;

                bool custom = string_get(item.at) == '=';
                string_address header = null;

                if (custom)
                {
                        string_address header_from = ++item.at;
                        positive header_length = string_span_without_set(
                            item.at, (string_address) ",");

                        item.at += header_length;
                        p8 address_to made =
                            (p8 address_to)text_arena_take(header_length + 1);

                        if (!made)
                                return false;

                        if (header_length)
                                memory_copy_apart(made, header_from, header_length);

                        made[header_length] = end;
                        header = (string_address)made;
                }

                positive which;
                string_address alias_header = null;

                if (string_equals(name, "cmd"))
                {
                        which = PS_FIELD_ARGS;
                        alias_header = "CMD";
                }
                else if (string_equals(name, "command"))
                {
                        which = PS_FIELD_ARGS;
                        alias_header = "COMMAND";
                }
                else if (string_equals(name, "ucmd"))
                {
                        which = PS_FIELD_COMM;
                        alias_header = "CMD";
                }
                else
                {
                        which = string_table_find(name, ps_columns,
                                                  sizeof(ps_columns[0]),
                                                  PS_FIELD_COUNT);
                }

                if (which == PS_FIELD_COUNT)
                {
                        text_error(name, "unknown user-defined format specifier");
                        return false;
                }

                if (!custom && alias_header)
                {
                        custom = true;
                        header = alias_header;
                }

                if (!ps_field_add(fields, count, room, which, header, custom))
                        return false;

                any = true;
        }

        return any;
}

/*
        The long selections arrive both as --name value and --name=value.
        One reader answers whether the word is this option, and hands back
        the value: null when the word that should have held it is missing,
        which each caller's validator already refuses.
*/
static bool ps_long_value(string_address argument, string_address name,
                          b32 address_to at, string_address address_to value)
{
        positive length = string_length(name);

        if (string_compare_max(argument, name, length))
                return false;

        if (argument[length] == '=')
        {
                address_to value = argument + length + 1;
                return true;
        }

        if (argument[length])
                return false;

        address_to value = program_argument(++(address_to at));
        return true;
}

static b32 tools_ps(void)
{
        ps_selected address_to fields = null;
        positive field_count = 0;
        positive field_room = 0;
        positive address_to selected_pids = null;
        positive selected_count = 0;
        positive selected_room = 0;
        positive address_to selected_ppids = null;
        positive ppid_count = 0;
        positive ppid_room = 0;
        string_address address_to selected_commands = null;
        positive command_count = 0;
        positive command_room = 0;
        bool every = false;
        bool full = false;
        bool no_headers = false;
        bool force_headers = false;
        bool reverse = false;

        text_begin("ps");
        text_arena_used = 0;
        ps_room = null;
        ps_room_used = 0;
        ps_room_size = 0;
        ps_failed = false;

        // Default formats change these three entries for display. Restore
        // the -o table for another invocation in the same process.
        ps_columns[PS_FIELD_TTY].header = "TT";
        ps_columns[PS_FIELD_TTY].width = 2;
        ps_columns[PS_FIELD_COMM].header = "COMMAND";
        ps_columns[PS_FIELD_ARGS].header = "COMMAND";

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = program_argument(i);
                string_address value;

                if (string_equals(argument, "--no-headers"))
                {
                        no_headers = true;
                        force_headers = false;
                        continue;
                }
                else if (string_equals(argument, "--headers"))
                {
                        force_headers = true;
                        no_headers = false;
                        continue;
                }
                else if (ps_long_value(argument, "--format", address_of i,
                                       address_of value))
                {
                        if (!value)
                        {
                                text_error(null, "option requires an argument -- format");
                                return text_done(1);
                        }

                        argument = value;
                }
                else if (ps_long_value(argument, "--pid", address_of i,
                                       address_of value))
                {
                        if (!value ||
                            !ps_pid_list(value, address_of selected_pids,
                                         address_of selected_count,
                                         address_of selected_room, true))
                        {
                                text_error(value, "invalid process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (argument[0] == '-' && argument[1] == 'p')
                {
                        argument += 2;

                        if (!string_get(argument))
                                argument = program_argument(++i);

                        if (!argument ||
                            !ps_pid_list(argument, address_of selected_pids,
                                         address_of selected_count,
                                         address_of selected_room, true))
                        {
                                text_error(argument, "invalid process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (argument[0] == 'p' && byte_is_digit(argument[1]))
                {
                        if (!ps_pid_list(argument + 1, address_of selected_pids,
                                         address_of selected_count,
                                         address_of selected_room, true))
                        {
                                text_error(argument, "invalid process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (ps_long_value(argument, "--ppid", address_of i,
                                       address_of value))
                {
                        if (!value ||
                            !ps_pid_list(value, address_of selected_ppids,
                                         address_of ppid_count,
                                         address_of ppid_room, false))
                        {
                                text_error(value, "invalid parent process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (argument[0] == '-' && argument[1] == 'C')
                {
                        argument += 2;

                        if (!string_get(argument))
                                argument = program_argument(++i);

                        if (!argument ||
                            !ps_command_list(argument,
                                             address_of selected_commands,
                                             address_of command_count,
                                             address_of command_room))
                        {
                                text_error(argument, "invalid command list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (ps_long_value(argument, "--sort", address_of i,
                                       address_of value))
                {
                        if (!value || !ps_sort_pid(value, address_of reverse))
                        {
                                text_error(value, "unsupported sort key");
                                return text_done(1);
                        }

                        continue;
                }
                else if (argument[0] == '-' && argument[1] == 'o' && !argument[2])
                {
                        argument = program_argument(++i);

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
                                case 'w': break;
                                case 'h': no_headers = true; break;
                                case 'a':
                                case 'x':
                                case 'u':
                                        /*
                                                These BSD personalities also
                                                replace the output format.
                                                A broad default listing is a
                                                plausible but wrong `ps aux`,
                                                so refuse them until that
                                                format exists in full.
                                        */
                                        known = false;
                                        break;
                                default: known = false; break;
                                }

                        if (!known)
                        {
                                text_error(argument, "unsupported option");
                                return text_done(1);
                        }

                        continue;
                }

                if (!ps_format_list(argument, address_of fields,
                                    address_of field_count,
                                    address_of field_room))
                        return text_done(1);
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
                // The two listings ps prints with no -o, in the shape -o
                // carries: field, header, and whether the header overrides
                // the column table's.
                static const ps_selected ps_full_preset[] = {
                    {PS_FIELD_USER, "UID", true},  {PS_FIELD_PID, null, false},
                    {PS_FIELD_PPID, null, false},  {PS_FIELD_CPU, null, false},
                    {PS_FIELD_STIME, null, false}, {PS_FIELD_TTY, null, false},
                    {PS_FIELD_TIME, null, false},  {PS_FIELD_ARGS, "CMD", true},
                };
                static const ps_selected ps_plain_preset[] = {
                    {PS_FIELD_PID, null, false},   {PS_FIELD_TTY, null, false},
                    {PS_FIELD_TIME, null, false},  {PS_FIELD_COMM, "CMD", true},
                };
                const ps_selected address_to preset = full ? ps_full_preset
                                                           : ps_plain_preset;
                positive presets = full ? 8 : 4;

                ps_columns[PS_FIELD_TTY].header = "TTY";
                ps_columns[PS_FIELD_TTY].width = 8;
                ps_columns[full ? PS_FIELD_ARGS : PS_FIELD_COMM].header = "CMD";

                for (positive f = 0; f < presets; f++)
                        if (!ps_field_add(address_of fields,
                                          address_of field_count,
                                          address_of field_room,
                                          preset[f].field, preset[f].header,
                                          preset[f].custom_header))
                                return text_done(1);
        }

        bool show_headers = force_headers;

        if (!force_headers && !no_headers)
                for (positive f = 0; f < field_count; f++)
                {
                        string_address header = fields[f].custom_header
                                                    ? fields[f].header
                                                    : ps_columns[fields[f].field].header;

                        if (header && string_get(header))
                        {
                                show_headers = true;
                                break;
                        }
                }

        if (show_headers)
        {
                for (positive f = 0; f < field_count; f++)
                {
                        positive field = fields[f].field;
                        bool last = f + 1 == field_count;
                        string_address header = fields[f].custom_header
                                                    ? fields[f].header
                                                    : ps_columns[field].header;
                        positive width = ps_columns[field].width;
                        positive header_length = header ? string_length(header) : 0;

                        if (header_length > width)
                                width = header_length;

                        string_to_field(text_put,
                                        header ? header : (string_address)"",
                                        !ps_columns[field].right && last ? 0 : width,
                                        ' ', !ps_columns[field].right);

                        if (!last)
                                text_put_character(' ');
                }

                text_put_character('\n');
        }

        bool selectors = selected_count || ppid_count || command_count;
        bool alternate_selectors = ppid_count || command_count;
        bool matched = false;

        for (positive at = 0; at < ps_count; at++)
        {
                positive p = reverse ? ps_count - at - 1 : at;
                ps_process address_to one = ps_list + p;
                positive repeats = 1;

                /*
                        procps gives -p its historical override of -e, but
                        combines -e with -C/--ppid as a union, which is all
                        processes. Preserve that awkward visible distinction.
                */
                if (selectors && !(every && alternate_selectors))
                {
                        repeats = ps_pid_matches(selected_pids, selected_count,
                                                 one->pid);

                        if (!repeats &&
                            (ps_value_has(selected_ppids, ppid_count,
                                          one->ppid) ||
                             ps_command_selected(selected_commands,
                                                 command_count, one->comm)))
                                repeats = 1;

                        if (!repeats)
                                continue;
                }
                else if (!every &&
                         !(one->uid == ps_own_uid && one->tty == ps_own_tty))
                        continue;

                matched = true;

                for (positive repeat = 0; repeat < repeats; repeat++)
                {
                        for (positive f = 0; f < field_count; f++)
                        {
                                positive field = fields[f].field;
                                string_address header = fields[f].custom_header
                                                            ? fields[f].header
                                                            : ps_columns[field].header;
                                positive width = ps_columns[field].width;
                                positive header_length =
                                    header ? string_length(header) : 0;

                                if (header_length > width)
                                        width = header_length;

                                ps_column_out(one, field, width,
                                              f + 1 == field_count);
                        }

                        text_put_character('\n');
                }
        }

        return text_done(ps_failed || (selectors && !matched) ? 1 : 0);
}

#include "util_linux.c"
