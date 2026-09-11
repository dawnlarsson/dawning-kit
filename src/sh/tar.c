/*
        tar -- ustar, GNU and pax archives.

        The kernel does not extract archives. The header checksum is
        memory_sum_bytes; a seekable regular member is copy_file_range
        then sendfile, the same floors cp uses. Compression is a later
        applet, not a guest bind of gzip.
*/

#define TAR_BLOCK 512
#define TAR_PATH 4096
#define TAR_NAME 100
#define TAR_PREFIX 155
#define TAR_CHKSUM 148
#define TAR_CHKSUM_WIDTH 8
#define TAR_SPACE_SUM ((p32)' ' * TAR_CHKSUM_WIDTH)

static p32 tar_header_sum(p8 address_to block)
{
        return memory_sum_bytes(block, TAR_BLOCK) -
               memory_sum_bytes(block + TAR_CHKSUM, TAR_CHKSUM_WIDTH) +
               TAR_SPACE_SUM;
}

static bool tar_header_zero(p8 address_to block)
{
        return memory_sum_bytes(block, TAR_BLOCK) == 0;
}

static bool tar_base256(p8 address_to field, positive width,
                        p64 address_to value)
{
        p64 held = 0;
        positive at;

        if (!width || (field[0] & 0x40))
                return false;

        held = (p64)(field[0] & 0x7f);
        for (at = 1; at < width; at++)
        {
                if (held > (positive_max >> 8))
                        return false;

                held = (held << 8) | field[at];
        }

        address_to value = held;
        return true;
}

static bool tar_field_value(p8 address_to field, positive width,
                            p64 address_to value)
{
        p8 digits[32];
        positive used;
        positive at = 0;
        positive keep;

        if (width && (field[0] & 0x80))
                return tar_base256(field, width, value);

        if (width >= sizeof(digits))
                return false;

        memory_copy(digits, field, width);
        digits[width] = end;

        while (at < width && digits[at] == ' ')
                at++;

        if (at >= width || !digits[at] || digits[at] == ' ')
        {
                address_to value = 0;
                return true;
        }

        keep = string_digits_octal_max(digits + at, positive_max,
                                       address_of used);
        if (!used)
                return false;

        at += used;
        while (at < width && (digits[at] == ' ' || !digits[at]))
                at++;

        if (at < width && digits[at])
                return false;

        address_to value = keep;
        return true;
}

static fn tar_field_put_octal(p8 address_to field, positive width, p64 value)
{
        p8 digits[32];
        positive length;

        memory_fill(field, '0', width);
        if (!width)
                return;

        field[width - 1] = end;
        length = positive_into_base(digits, (positive)value, 8, false);
        if (length >= width)
                length = width - 1;

        memory_copy(field + (width - 1 - length), digits, length);
}

static fn tar_header_put_checksum(p8 address_to block)
{
        tar_field_put_octal(block + TAR_CHKSUM, 7, tar_header_sum(block));
        block[TAR_CHKSUM + 6] = end;
        block[TAR_CHKSUM + 7] = ' ';
}

static bool tar_header_ok(p8 address_to block)
{
        p64 stored;

        if (!tar_field_value(block + TAR_CHKSUM, TAR_CHKSUM_WIDTH,
                             address_of stored))
                return false;

        return stored == tar_header_sum(block);
}

static positive tar_padded(p64 size)
{
        return (positive)((size + (TAR_BLOCK - 1)) & ~(p64)(TAR_BLOCK - 1));
}

static fn tar_field_text(p8 address_to field, positive width,
                         p8 address_to into, positive room)
{
        positive keep = 0;

        while (keep < width && field[keep])
                keep++;

        if (keep >= room)
                keep = room ? room - 1 : 0;

        if (keep)
                memory_copy(into, field, keep);

        if (room)
                into[keep] = end;
}

static bool tar_join_name(p8 address_to prefix, p8 address_to name,
                          p8 address_to into, positive room)
{
        p8 head[TAR_PREFIX + 1];
        p8 leaf[TAR_NAME + 1];

        tar_field_text(name, TAR_NAME, leaf, sizeof(leaf));
        tar_field_text(prefix, TAR_PREFIX, head, sizeof(head));
        if (!head[0])
        {
                string_copy_max_end(into, leaf, room ? room - 1 : 0);
                return into[0] != end;
        }

        return path_join(into, room, head, leaf) && into[0];
}

/*
        Drop leading slashes unless the caller asked to keep them, drop a
        leading run of components, and refuse a path that would walk out of
        the destination with `..`.
*/
static bool tar_safe_path(string_address path, positive strip, bool absolute,
                          p8 address_to into, positive room,
                          bool address_to escaped)
{
        p8 work[TAR_PATH];
        positive used = 0;
        string_address at = path;

        if (!path || !room)
                return false;

        into[0] = end;
        if (escaped)
                address_to escaped = false;

        if (!absolute)
                while (string_is(at, '/'))
                        at++;
        else if (string_is(path, '/'))
        {
                work[0] = '/';
                used = 1;
        }

        while (*at)
        {
                string_address slash = string_first_of(at, '/');
                positive piece = slash ? (positive)(slash - at)
                                       : string_length(at);

                if (!piece)
                {
                        at++;
                        continue;
                }

                if (piece == 1 && string_is(at, '.'))
                {
                        at += slash ? piece + 1 : piece;
                        continue;
                }

                if (piece == 2 && string_is(at, '.') && string_is(at + 1, '.'))
                {
                        if (escaped)
                                address_to escaped = true;

                        return false;
                }

                if (strip)
                {
                        strip--;
                        at += slash ? piece + 1 : piece;
                        continue;
                }

                if (used && used + 1 < room && !(used == 1 && work[0] == '/'))
                        work[used++] = '/';

                if (used + piece >= room)
                        return false;

                memory_copy(work + used, at, piece);
                used += piece;
                at += slash ? piece + 1 : piece;
        }

        if (!used || strip)
                return false;

        work[used] = end;
        string_copy_max_end(into, work, room - 1);
        return true;
}

#ifndef TAR_PARSE_ONLY

#define TAR_CREATE 'c'
#define TAR_LIST 't'
#define TAR_EXTRACT 'x'

struct tar_options
{
        p8 mode;
        string_address archive;
        string_address directory;
        positive strip;
        bool verbose;
        bool absolute;
        positive first;
};

static p8 tar_block[TAR_BLOCK];
static p8 tar_name[TAR_PATH];
static p8 tar_link[TAR_PATH];
static p8 tar_pax_path[TAR_PATH];
static p8 tar_pax_link[TAR_PATH];
static p64 tar_pax_size;
static bool tar_pax_has_path;
static bool tar_pax_has_link;
static bool tar_pax_has_size;
static b32 tar_status;

static fn tar_fail(string_address what, bipolar failed)
{
        string_format(log_error, "tar: %s: %s\n", what, file_reason(failed));
        tar_status = 2;
}

static fn tar_refuse(string_address message)
{
        string_format(log_error, "tar: %s\n", message);
        tar_status = 2;
}

static bool tar_read_block(bipolar handle, p8 address_to block)
{
        bipolar got = system_read_retry((positive)handle, block, TAR_BLOCK);

        if (got == 0)
                return false;

        if (got != TAR_BLOCK)
        {
                tar_refuse("unexpected EOF in archive");
                return false;
        }

        return true;
}

static bool tar_skip(bipolar handle, p64 bytes, bool seekable)
{
        p8 dump[FILE_TRANSFER_SIZE];

        if (!bytes)
                return true;

        if (seekable && system_seek(handle, (bipolar)bytes, FILE_SEEK_CUR) >= 0)
                return true;

        while (bytes)
        {
                positive ask = bytes > sizeof(dump) ? sizeof(dump)
                                                    : (positive)bytes;
                bipolar got = system_read_retry((positive)handle, dump, ask);

                if (got <= 0)
                {
                        tar_refuse("unexpected EOF in archive");
                        return false;
                }

                bytes -= (positive)got;
        }

        return true;
}

static bool tar_copy_out(bipolar in, bipolar out, p64 size)
{
        bool range_copy = true;
        bool send_copy = true;

        if (!size)
                return true;

        return file_copy_stream(in, out, size, true, address_of range_copy,
                                address_of send_copy, null);
}

static fn tar_clear_pax(void)
{
        tar_pax_has_path = false;
        tar_pax_has_link = false;
        tar_pax_has_size = false;
        tar_pax_path[0] = end;
        tar_pax_link[0] = end;
}

static bool tar_pax_apply(p8 address_to body, positive length)
{
        positive at = 0;

        while (at < length)
        {
                positive used;
                positive record = string_digits_max(body + at, positive_max,
                                                    address_of used);
                string_address mark;
                string_address equal;
                positive key;
                positive value;
                positive rest;

                if (!used || at + record > length || record < used + 3)
                        return false;

                mark = body + at + used;
                if (*mark != ' ')
                        return false;

                equal = string_first_of(mark + 1, '=');
                if (!equal)
                        return false;

                key = (positive)(equal - (mark + 1));
                value = (positive)((body + at + record - 1) - (equal + 1));
                rest = record;

                if (body[at + rest - 1] != '\n')
                        return false;

                if (key == 4 && !memory_compare(mark + 1, "path", 4))
                {
                        if (value >= TAR_PATH)
                                return false;

                        memory_copy(tar_pax_path, equal + 1, value);
                        tar_pax_path[value] = end;
                        tar_pax_has_path = true;
                }
                else if (key == 8 && !memory_compare(mark + 1, "linkpath", 8))
                {
                        if (value >= TAR_PATH)
                                return false;

                        memory_copy(tar_pax_link, equal + 1, value);
                        tar_pax_link[value] = end;
                        tar_pax_has_link = true;
                }
                else if (key == 4 && !memory_compare(mark + 1, "size", 4))
                {
                        p8 keep[32];
                        positive digits;

                        if (value >= sizeof(keep))
                                return false;

                        memory_copy(keep, equal + 1, value);
                        keep[value] = end;
                        tar_pax_size = string_digits_max(keep, positive_max,
                                                         address_of digits);
                        tar_pax_has_size = digits == value;
                        if (!tar_pax_has_size)
                                return false;
                }

                at += rest;
        }

        return true;
}

static bool tar_read_payload(bipolar handle, p64 size, p8 address_to into,
                             positive room, bool seekable)
{
        positive padded = tar_padded(size);
        bipolar got;

        if (size >= room)
        {
                tar_refuse("member name is too long");
                return tar_skip(handle, padded, seekable);
        }

        got = size ? system_read_retry((positive)handle, into, (positive)size)
                   : 0;
        if ((p64)got != size)
        {
                tar_refuse("unexpected EOF in archive");
                return false;
        }

        into[size] = end;
        return tar_skip(handle, padded - size, seekable);
}

static bool tar_name_matches(string_address name, string_address wanted)
{
        positive keep = string_length(wanted);

        if (string_equals(name, wanted))
                return true;

        return keep && string_length(name) > keep &&
               !memory_compare(name, wanted, keep) && name[keep] == '/';
}

static bool tar_wanted(string_address name, positive first, positive count)
{
        positive at;

        if (first >= count)
                return true;

        for (at = first; at < count; at++)
                if (tar_name_matches(name, program_argument((b32)at)))
                        return true;

        return false;
}

static bool tar_parents(string_address path, bool last_is_dir)
{
        p8 head[TAR_PATH];

        if (last_is_dir)
                return file_make_parents(path, 0755);

        path_head_copy(head, sizeof(head), path);
        if (!head[0] || string_equals(head, ".") || string_equals(head, "/"))
                return true;

        return file_make_parents(head, 0755);
}

static bool tar_extract_regular(bipolar archive, string_address path,
                                p64 size, positive mode, bool seekable)
{
        bipolar made;

        if (!tar_parents(path, false))
        {
                tar_fail(path, -ERROR_NOT_DIRECTORY);
                return tar_skip(archive, tar_padded(size), seekable);
        }

        made = system_open_at_mode(AT_FDCWD, path, FILE_WRITE | O_CLOEXEC,
                                   mode & 07777);
        if (made < 0)
        {
                tar_fail(path, made);
                return tar_skip(archive, tar_padded(size), seekable);
        }

        if (!tar_copy_out(archive, made, size))
        {
                system_close(made);
                tar_fail(path, -ERROR_INPUT_OUTPUT);
                return tar_skip(archive, tar_padded(size) - size, seekable);
        }

        system_change_mode_at(AT_FDCWD, path, mode & 07777);
        system_close(made);
        return tar_skip(archive, tar_padded(size) - size, seekable);
}

static fn tar_extract_member(bipolar archive, p8 type, string_address path,
                             string_address link, p64 size, p64 mode,
                             p64 major, p64 minor, bool seekable, bool verbose)
{
        bipolar made;
        string_address slash = string_last_of(path, '/');
        bool directory = type == '5' || (slash && !slash[1]);
        bool regular = !directory && (!type || type == '0' || type == '7');

        if (verbose)
                string_format(log_error, "%s\n", path);

        if (directory)
        {
                if (!tar_parents(path, true))
                        tar_fail(path, -ERROR_NOT_DIRECTORY);

                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (type == '2')
        {
                if (!tar_parents(path, false))
                        tar_fail(path, -ERROR_NOT_DIRECTORY);
                else
                {
                        system_remove_at(AT_FDCWD, path, 0);
                        made = system_symbolic_link_at(link, AT_FDCWD, path);
                        if (made < 0)
                                tar_fail(path, made);
                }

                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (type == '1')
        {
                if (!tar_parents(path, false))
                        tar_fail(path, -ERROR_NOT_DIRECTORY);
                else
                {
                        system_remove_at(AT_FDCWD, path, 0);
                        made = system_link_at(AT_FDCWD, link, AT_FDCWD, path, 0);
                        if (made < 0)
                                tar_fail(path, made);
                }

                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (type == '6')
        {
                if (!tar_parents(path, false))
                        tar_fail(path, -ERROR_NOT_DIRECTORY);
                else
                {
                        made = system_call_4(syscall(mknodat), AT_FDCWD,
                                             (positive)path,
                                             (mode & 07777) | MODE_PIPE, 0);
                        if (made < 0)
                                tar_fail(path, made);
                }

                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (type == '3' || type == '4')
        {
                positive kind = type == '3' ? MODE_CHARACTER : MODE_BLOCK;

                if (!tar_parents(path, false))
                        tar_fail(path, -ERROR_NOT_DIRECTORY);
                else
                {
                        made = system_call_4(syscall(mknodat), AT_FDCWD,
                                             (positive)path,
                                             (mode & 07777) | kind,
                                             file_device((p32)major, (p32)minor));
                        if (made < 0)
                                tar_fail(path, made);
                }

                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        if (!regular)
        {
                p8 shown[2];

                shown[0] = type;
                shown[1] = end;
                string_format(log_error, "tar: %s: unknown file type '%s'\n",
                              path, shown);
                tar_status = tar_status ? tar_status : 1;
                tar_skip(archive, tar_padded(size), seekable);
                return;
        }

        tar_extract_regular(archive, path, size, (positive)mode, seekable);
}

static bool tar_member_name(p8 address_to block, p8 address_to into)
{
        if (tar_pax_has_path)
        {
                string_copy_max_end(into, tar_pax_path, TAR_PATH - 1);
                return into[0] != end;
        }

        return tar_join_name(block + 345, block, into, TAR_PATH);
}

static bool tar_member_link(p8 address_to block, p8 address_to into)
{
        if (tar_pax_has_link)
        {
                string_copy_max_end(into, tar_pax_link, TAR_PATH - 1);
                return true;
        }

        tar_field_text(block + 157, TAR_NAME, into, TAR_PATH);
        return true;
}

static b32 tar_read_archive(struct tar_options address_to options)
{
        bipolar handle;
        bool seekable;
        bool listed = false;
        p8 long_name[TAR_PATH];
        p8 long_link[TAR_PATH];
        bool have_long_name = false;
        bool have_long_link = false;
        positive count = (positive)program_argument_count();

        tar_clear_pax();
        long_name[0] = end;
        long_link[0] = end;

        if (!options->archive || string_equals(options->archive, "-"))
                handle = 0;
        else
        {
                handle = system_open_at(AT_FDCWD, options->archive,
                                        FILE_READ | O_CLOEXEC);
                if (handle < 0)
                {
                        tar_fail(options->archive, handle);
                        return tar_status;
                }
        }

        seekable = system_seek(handle, 0, FILE_SEEK_CUR) >= 0;

        if (options->directory)
        {
                bipolar moved = system_change_directory(options->directory);

                if (moved < 0)
                {
                        tar_fail(options->directory, moved);
                        if (handle > 0)
                                system_close(handle);

                        return tar_status;
                }
        }

        while (tar_read_block(handle, tar_block))
        {
                p8 type;
                p64 size = 0;
                p64 mode = 0;
                p64 major = 0;
                p64 minor = 0;
                p8 kept[TAR_PATH];
                p8 kept_link[TAR_PATH];
                bool escaped;

                if (tar_header_zero(tar_block))
                        break;

                if (!tar_header_ok(tar_block))
                {
                        tar_refuse("invalid header checksum");
                        break;
                }

                type = tar_block[156];
                if (!tar_field_value(tar_block + 124, 12, address_of size) ||
                    !tar_field_value(tar_block + 100, 8, address_of mode) ||
                    !tar_field_value(tar_block + 329, 8, address_of major) ||
                    !tar_field_value(tar_block + 337, 8, address_of minor))
                {
                        tar_refuse("invalid header");
                        break;
                }

                if (tar_pax_has_size)
                        size = tar_pax_size;

                if (type == 'L')
                {
                        have_long_name = tar_read_payload(handle, size,
                                                          long_name, TAR_PATH,
                                                          seekable);
                        continue;
                }

                if (type == 'K')
                {
                        have_long_link = tar_read_payload(handle, size,
                                                          long_link, TAR_PATH,
                                                          seekable);
                        continue;
                }

                if (type == 'x' || type == 'g')
                {
                        p8 body[TAR_PATH];

                        if (!tar_read_payload(handle, size, body, TAR_PATH,
                                              seekable) ||
                            !tar_pax_apply(body, (positive)size))
                        {
                                tar_refuse("invalid extended header");
                                break;
                        }

                        continue;
                }

                if (have_long_name)
                        string_copy_max_end(tar_name, long_name, TAR_PATH - 1);
                else if (!tar_member_name(tar_block, tar_name))
                {
                        tar_refuse("member name is too long");
                        break;
                }

                if (have_long_link)
                        string_copy_max_end(tar_link, long_link, TAR_PATH - 1);
                else
                        tar_member_link(tar_block, tar_link);

                have_long_name = false;
                have_long_link = false;
                long_name[0] = end;
                long_link[0] = end;

                if (!tar_wanted(tar_name, options->first, count))
                {
                        tar_skip(handle, tar_padded(size), seekable);
                        tar_clear_pax();
                        continue;
                }

                if (!tar_safe_path(tar_name, options->strip, options->absolute,
                                   kept, TAR_PATH, address_of escaped))
                {
                        string_format(log_error,
                                      escaped ? "tar: %s: member name is unsafe\n"
                                              : "tar: %s: empty name after stripping\n",
                                      tar_name);
                        if (escaped)
                                tar_status = 2;

                        tar_skip(handle, tar_padded(size), seekable);
                        tar_clear_pax();
                        continue;
                }

                if (type == '1')
                        tar_safe_path(tar_link, options->strip, options->absolute,
                                      kept_link, TAR_PATH, null);
                else
                        string_copy_max_end(kept_link, tar_link, TAR_PATH - 1);

                listed = true;
                if (options->mode == TAR_LIST)
                {
                        string_format(log, "%s\n", kept);
                        tar_skip(handle, tar_padded(size), seekable);
                }
                else
                        tar_extract_member(handle, type, kept, kept_link, size,
                                           mode, major, minor, seekable,
                                           options->verbose);

                tar_clear_pax();
        }

        if (handle > 0)
                system_close(handle);

        if (options->first < count && !listed && !tar_status)
                tar_refuse("the requested members were not in the archive");

        log_flush();
        return tar_status;
}

static fn tar_header_ustar(p8 address_to block, string_address name,
                           p8 type, p64 size, p64 mode, p64 mtime,
                           string_address link)
{
        p8 prefix[TAR_PREFIX + 1];
        p8 leaf[TAR_NAME + 1];
        positive length = string_length(name);
        string_address slash;

        memory_fill(block, 0, TAR_BLOCK);
        prefix[0] = end;
        leaf[0] = end;

        if (length < TAR_NAME)
                string_copy_max_end(leaf, name, TAR_NAME);
        else
        {
                slash = string_last_of(name, '/');
                if (!slash || (positive)(slash - name) >= TAR_PREFIX ||
                    string_length(slash + 1) >= TAR_NAME)
                {
                        tar_refuse("member name is too long for ustar");
                        return;
                }

                memory_copy(prefix, name, (positive)(slash - name));
                prefix[slash - name] = end;
                string_copy_max_end(leaf, slash + 1, TAR_NAME);
        }

        memory_copy(block, leaf, string_length(leaf));
        if (prefix[0])
                memory_copy(block + 345, prefix, string_length(prefix));

        tar_field_put_octal(block + 100, 8, mode);
        tar_field_put_octal(block + 108, 8, 0);
        tar_field_put_octal(block + 116, 8, 0);
        tar_field_put_octal(block + 124, 12, size);
        tar_field_put_octal(block + 136, 12, mtime);
        block[156] = type;
        if (link)
                memory_copy(block + 157, link,
                            min(string_length(link), TAR_NAME - 1));

        memory_copy(block + 257, "ustar", 6);
        block[263] = '0';
        block[264] = '0';
        tar_header_put_checksum(block);
}

static bool tar_write_block(bipolar handle, p8 address_to block)
{
        if (system_write_all((positive)handle, block, TAR_BLOCK) != TAR_BLOCK)
        {
                tar_refuse("cannot write archive");
                return false;
        }

        return true;
}

static bool tar_write_padding(bipolar handle, p64 size)
{
        positive pad = tar_padded(size) - (positive)size;

        if (!pad)
                return true;

        memory_fill(tar_block, 0, pad);
        return system_write_all((positive)handle, tar_block, pad) == pad
                   ? true
                   : (tar_refuse("cannot write archive"), false);
}

static b32 tar_add_path(bipolar archive, string_address path, bool verbose);

static b32 tar_add_directory(bipolar archive, string_address path,
                             file_facts address_to facts, bool verbose)
{
        file_walk walk;

        tar_header_ustar(tar_block, path, '5', 0, facts->mode & 07777,
                         (p64)facts->modified.seconds, null);
        if (tar_status || !tar_write_block(archive, tar_block))
                return tar_status;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
                return tar_fail(path, walk.error), tar_status;

        for (;;)
        {
                struct linux_dirent64 address_to entry = file_walk_next(
                    address_of walk);
                p8 child[TAR_PATH];

                if (!entry)
                        break;

                if (file_is_dot(entry->d_name))
                        continue;

                if (!file_path_join(child, path, entry->d_name))
                {
                        tar_refuse("member name is too long");
                        break;
                }

                tar_add_path(archive, child, verbose);
                if (tar_status == 2)
                        break;
        }

        file_walk_close(address_of walk);
        return tar_status;
}

static b32 tar_add_path(bipolar archive, string_address path, bool verbose)
{
        file_facts facts;
        p8 link[TAR_PATH];
        bipolar handle;
        p8 type;

        if (!file_look(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, address_of facts))
        {
                tar_fail(path, -ERROR_NO_ENTRY);
                return tar_status;
        }

        if (verbose)
                string_format(log_error, "%s\n", path);

        if ((facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                return tar_add_directory(archive, path, address_of facts,
                                         verbose);

        if ((facts.mode & MODE_FORMAT) == MODE_SOCKET)
        {
                string_format(log_error, "tar: %s: socket ignored\n", path);
                tar_status = tar_status ? tar_status : 1;
                return tar_status;
        }

        if ((facts.mode & MODE_FORMAT) == MODE_LINK)
        {
                bipolar got = system_read_link_at(AT_FDCWD, path, link,
                                                  TAR_PATH - 1);

                if (got < 0)
                {
                        tar_fail(path, got);
                        return tar_status;
                }

                link[got] = end;
                tar_header_ustar(tar_block, path, '2', 0, facts.mode & 07777,
                                 (p64)facts.modified.seconds, link);
                if (tar_status || !tar_write_block(archive, tar_block))
                        return tar_status;

                return tar_status;
        }

        if ((facts.mode & MODE_FORMAT) == MODE_PIPE)
                type = '6';
        else if ((facts.mode & MODE_FORMAT) == MODE_CHARACTER)
                type = '3';
        else if ((facts.mode & MODE_FORMAT) == MODE_BLOCK)
                type = '4';
        else
                type = '0';

        if (type != '0')
        {
                tar_header_ustar(tar_block, path, type, 0, facts.mode & 07777,
                                 (p64)facts.modified.seconds, null);
                if (tar_status)
                        return tar_status;

                if (type == '3' || type == '4')
                {
                        tar_field_put_octal(tar_block + 329, 8,
                                            facts.rdev_major);
                        tar_field_put_octal(tar_block + 337, 8,
                                            facts.rdev_minor);
                        tar_header_put_checksum(tar_block);
                }

                if (!tar_write_block(archive, tar_block))
                        return tar_status;

                return tar_status;
        }

        tar_header_ustar(tar_block, path, '0', facts.size, facts.mode & 07777,
                         (p64)facts.modified.seconds, null);
        if (tar_status || !tar_write_block(archive, tar_block))
                return tar_status;

        handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        if (handle < 0)
        {
                tar_fail(path, handle);
                return tar_status;
        }

        if (!tar_copy_out(handle, archive, (p64)facts.size))
                tar_fail(path, -ERROR_INPUT_OUTPUT);

        system_close(handle);
        if (!tar_status)
                tar_write_padding(archive, (p64)facts.size);

        return tar_status;
}

static b32 tar_write_archive(struct tar_options address_to options)
{
        bipolar handle;
        positive count = (positive)program_argument_count();
        positive at;

        if (options->first >= count)
        {
                tar_refuse("cowardly refusing to create an empty archive");
                return tar_status;
        }

        if (!options->archive || string_equals(options->archive, "-"))
                handle = 1;
        else
        {
                handle = system_open_at_mode(AT_FDCWD, options->archive,
                                             FILE_WRITE | O_CLOEXEC, 0666);
                if (handle < 0)
                {
                        tar_fail(options->archive, handle);
                        return tar_status;
                }
        }

        if (options->directory)
        {
                bipolar moved = system_change_directory(options->directory);

                if (moved < 0)
                {
                        tar_fail(options->directory, moved);
                        if (handle > 2)
                                system_close(handle);

                        return tar_status;
                }
        }

        for (at = options->first; at < count && tar_status != 2; at++)
                tar_add_path(handle, program_argument((b32)at),
                             options->verbose);

        memory_fill(tar_block, 0, TAR_BLOCK);
        if (!tar_status)
        {
                tar_write_block(handle, tar_block);
                tar_write_block(handle, tar_block);
        }

        if (handle > 2)
                system_close(handle);

        log_flush();
        return tar_status;
}

static bool tar_compression_letter(p8 letter)
{
        return letter == 'z' || letter == 'j' || letter == 'J' ||
               letter == 'Z' || letter == 'a';
}

static bool tar_cluster_letter(p8 letter)
{
        return letter == 'x' || letter == 't' || letter == 'c' ||
               letter == 'f' || letter == 'v' || letter == 'C' ||
               letter == 'P' || tar_compression_letter(letter);
}

static fn tar_refuse_compression(void)
{
        tar_refuse("compression is a separate applet; this tar is uncompressed");
}

static bool tar_is_cluster(string_address word)
{
        if (!word || !*word || string_is(word, '-'))
                return false;

        for (; *word; word++)
                if (!tar_cluster_letter(*word))
                        return false;

        return true;
}

static bool tar_take_letter(struct tar_options address_to options, p8 letter,
                            string_address address_to arguments,
                            positive count, positive address_to at)
{
        if (letter == 'x' || letter == 't' || letter == 'c')
        {
                options->mode = letter;
                return true;
        }

        if (letter == 'v')
        {
                options->verbose = true;
                return true;
        }

        if (letter == 'P')
        {
                options->absolute = true;
                return true;
        }

        if (tar_compression_letter(letter))
        {
                tar_refuse_compression();
                return false;
        }

        if (letter == 'f' || letter == 'C')
        {
                if (address_to at >= count)
                {
                        tar_refuse(letter == 'f' ? "option requires an argument -- 'f'"
                                                 : "option requires an argument -- 'C'");
                        return false;
                }

                if (letter == 'f')
                        options->archive = arguments[address_to at];
                else
                        options->directory = arguments[address_to at];

                address_to at += 1;
                return true;
        }

        p8 shown[2];

        shown[0] = letter;
        shown[1] = end;
        string_format(log_error, "tar: invalid option -- '%s'\n", shown);
        tar_status = 2;
        return false;
}

static bool tar_take_long(struct tar_options address_to options,
                          string_address word, string_address address_to arguments,
                          positive count, positive address_to at)
{
        string_address equal = string_first_of(word, '=');
        string_address name = word + 2;
        string_address value = equal ? equal + 1 : null;
        positive name_length = equal ? (positive)(equal - name)
                                     : string_length(name);

        if (name_length == 4 && !memory_compare(name, "file", 4))
        {
                if (!value)
                {
                        if (address_to at >= count)
                                return tar_refuse("option '--file' requires an argument"),
                                       false;

                        value = arguments[address_to at];
                        address_to at += 1;
                }

                options->archive = value;
                return true;
        }

        if (name_length == 9 && !memory_compare(name, "directory", 9))
        {
                if (!value)
                {
                        if (address_to at >= count)
                                return tar_refuse("option '--directory' requires an argument"),
                                       false;

                        value = arguments[address_to at];
                        address_to at += 1;
                }

                options->directory = value;
                return true;
        }

        if (name_length == 16 && !memory_compare(name, "strip-components", 16))
        {
                positive used;

                if (!value)
                {
                        if (address_to at >= count)
                                return tar_refuse("option '--strip-components' requires an argument"),
                                       false;

                        value = arguments[address_to at];
                        address_to at += 1;
                }

                options->strip = string_digits_max(value, positive_max,
                                                   address_of used);
                if (!used || value[used])
                        return tar_refuse("invalid number of components"), false;

                return true;
        }

        if (name_length == 7 && !memory_compare(name, "verbose", 7))
        {
                options->verbose = true;
                return true;
        }

        if (name_length == 14 && !memory_compare(name, "absolute-names", 14))
        {
                options->absolute = true;
                return true;
        }

        if ((name_length == 4 && !memory_compare(name, "gzip", 4)) ||
            (name_length == 6 && !memory_compare(name, "gunzip", 6)) ||
            (name_length == 5 && !memory_compare(name, "bzip2", 5)) ||
            (name_length == 2 && !memory_compare(name, "xz", 2)) ||
            (name_length == 4 && !memory_compare(name, "zstd", 4)) ||
            (name_length == 8 && !memory_compare(name, "compress", 8)) ||
            (name_length == 14 && !memory_compare(name, "auto-compress", 14)) ||
            (name_length == 21 && !memory_compare(name, "use-compress-program", 21)))
        {
                tar_refuse_compression();
                return false;
        }

        if (name_length == 7 && !memory_compare(name, "extract", 7))
        {
                options->mode = TAR_EXTRACT;
                return true;
        }

        if (name_length == 4 && !memory_compare(name, "list", 4))
        {
                options->mode = TAR_LIST;
                return true;
        }

        if (name_length == 6 && !memory_compare(name, "create", 6))
        {
                options->mode = TAR_CREATE;
                return true;
        }

        if ((name_length == 4 && !memory_compare(name, "help", 4)) ||
            (name_length == 7 && !memory_compare(name, "version", 7)))
        {
                if (name_length == 4)
                        string_format(log, "Usage: tar [-ctx] [-f ARCHIVE] [-C DIR] "
                                           "[--strip-components N] [FILE...]\n");
                else
                        string_format(log, "tar from dawning-kit\n");

                log_flush();
                options->mode = 'h';
                return true;
        }

        string_format(log_error, "tar: unrecognized option '%s'\n", word);
        tar_status = 2;
        return false;
}

static bool tar_parse(struct tar_options address_to options)
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        positive at = 1;

        memory_fill(options, 0, sizeof(*options));
        options->mode = 0;

        if (count > 1 && tar_is_cluster(arguments[1]))
        {
                string_address cluster = arguments[1];

                at = 2;
                for (; *cluster; cluster++)
                        if (!tar_take_letter(options, *cluster, arguments,
                                             count, address_of at))
                                return false;
        }

        while (at < count)
        {
                string_address word = arguments[at];

                if (string_equals(word, "--"))
                {
                        at++;
                        break;
                }

                if (string_length(word) > 2 && word[0] == '-' && word[1] == '-')
                {
                        at++;
                        if (!tar_take_long(options, word, arguments, count,
                                           address_of at))
                                return false;

                        if (options->mode == 'h')
                                return true;

                        continue;
                }

                if (word[0] == '-' && word[1])
                {
                        string_address letters = word + 1;

                        at++;
                        for (; *letters; letters++)
                        {
                                if ((*letters == 'f' || *letters == 'C') &&
                                    letters[1])
                                {
                                        if (*letters == 'f')
                                                options->archive = letters + 1;
                                        else
                                                options->directory = letters + 1;

                                        break;
                                }

                                if (!tar_take_letter(options, *letters,
                                                     arguments, count,
                                                     address_of at))
                                        return false;
                        }

                        continue;
                }

                break;
        }

        options->first = at;
        if (!options->mode)
        {
                tar_refuse("you must specify one of the '-c', '-t', or '-x' options");
                string_format(log_error, "Try 'tar --help' for more information.\n");
                return false;
        }

        return true;
}

static b32 file_tar(void)
{
        struct tar_options options;

        tar_status = 0;
        if (!tar_parse(address_of options))
                return tar_status ? tar_status : 2;

        if (options.mode == 'h')
                return 0;

        if (options.mode == TAR_CREATE)
                return tar_write_archive(address_of options);

        return tar_read_archive(address_of options);
}

#endif /* TAR_PARSE_ONLY */
