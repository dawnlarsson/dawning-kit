/*
        The common Linux mount interface.

        This file deliberately owns no dispatch.  A shell builtin and a
        multicall executable call storage_mount_command or
        storage_umount_command with the same argc/argv and writer, so neither
        path has an option parser, an fstab parser, or syscall policy of its
        own.

        The table loaders in storage_discovery.c are public to the rest of the
        storage family.  Mount consumes that one parsed view; blkid-backed
        identity resolution enters through the source resolver without
        teaching this file about on-disk signatures.
*/

#include "../compiler_memory.c"

#define STORAGE_MS_RDONLY       1
#define STORAGE_MS_NOSUID       2
#define STORAGE_MS_NODEV        4
#define STORAGE_MS_NOEXEC       8
#define STORAGE_MS_SYNCHRONOUS  16
#define STORAGE_MS_REMOUNT      32
#define STORAGE_MS_MANDLOCK     64
#define STORAGE_MS_DIRSYNC      128
#define STORAGE_MS_NOSYMFOLLOW  256
#define STORAGE_MS_NOATIME      1024
#define STORAGE_MS_NODIRATIME   2048
#define STORAGE_MS_BIND         4096
#define STORAGE_MS_MOVE         8192
#define STORAGE_MS_REC          16384
#define STORAGE_MS_SILENT       32768
#define STORAGE_MS_UNBINDABLE   (1UL << 17)
#define STORAGE_MS_PRIVATE      (1UL << 18)
#define STORAGE_MS_SLAVE        (1UL << 19)
#define STORAGE_MS_SHARED       (1UL << 20)
#define STORAGE_MS_RELATIME     (1UL << 21)
#define STORAGE_MS_STRICTATIME  (1UL << 24)
#define STORAGE_MS_LAZYTIME     (1UL << 25)

#define STORAGE_MNT_FORCE       1
#define STORAGE_MNT_DETACH      2
#define STORAGE_MNT_EXPIRE      4
#define STORAGE_UMOUNT_NOFOLLOW 8

#define STORAGE_ERROR_NO_ENTRY  2
#define STORAGE_ERROR_IO        5
#define STORAGE_ERROR_NO_MEMORY 12
#define STORAGE_ERROR_BUSY      16
#define STORAGE_ERROR_INVALID   22

#define STORAGE_BIND_CHANGEABLE (STORAGE_MS_RDONLY | STORAGE_MS_NOSUID | \
                                 STORAGE_MS_NODEV | STORAGE_MS_NOEXEC | \
                                 STORAGE_MS_NOATIME | STORAGE_MS_NODIRATIME | \
                                 STORAGE_MS_RELATIME | STORAGE_MS_NOSYMFOLLOW)

#define storage_word(word, wanted) string_equals((word), (wanted))

static bool storage_prefix(string_address word, string_address prefix)
{
        positive count = string_length(prefix);

        return !string_compare_max(word, prefix, count);
}

typedef struct
{
        p8 address_to text;
        positive room;
        positive used;
} storage_mount_word;

static fn storage_mount_word_free(storage_mount_word address_to word)
{
        memory_release((address_any address_to)address_of word->text,
                       address_of word->room, address_of word->used, 1);
}

static bool storage_mount_tag(storage_mount_word address_to word,
                              string_address tag, string_address value)
{
        positive tag_length = string_length(tag);
        positive value_length = string_length(value);
        positive wanted;

        if (tag_length > positive_max - 2 ||
            value_length > positive_max - tag_length - 2)
                return false;
        wanted = tag_length + value_length + 2;
        if (!memory_reserve((address_any address_to)address_of word->text,
                            address_of word->room, word->used, wanted, 1, 64))
                return false;

        memory_copy_apart(word->text, tag, tag_length);
        word->text[tag_length] = '=';
        memory_copy_apart(word->text + tag_length + 1, value,
                          value_length + 1);
        word->used = wanted;
        return true;
}

typedef struct
{
        positive flags;
        positive mentioned;
        positive propagation;
        p8 address_to data;
        positive data_room;
        positive data_used;
        bool noauto;
        bool nofail;
        bool unsupported_loop;
} storage_mount_options;

static fn storage_options_free(storage_mount_options address_to options)
{
        memory_release((address_any address_to)address_of options->data,
                       address_of options->data_room,
                       address_of options->data_used, 1);
        memory_fill(options, 0, sizeof(*options));
}

static bool storage_data_add(storage_mount_options address_to options,
                             string_address item, positive length)
{
        positive extra = length + (options->data_used ? 1 : 0) + 1;

        if (extra > positive_max - options->data_used ||
            !memory_reserve((address_any address_to)address_of options->data,
                            address_of options->data_room,
                            options->data_used,
                            options->data_used + extra, 1, 64))
                return false;
        if (options->data_used)
                options->data[options->data_used++] = ',';
        memory_copy_apart(options->data + options->data_used, item, length);
        options->data_used += length;
        options->data[options->data_used] = 0;
        return true;
}

static bool storage_option_name(string_address item, positive length,
                                string_address name)
{
        return length == string_length(name) && !memory_compare(item, name, length);
}

static bool storage_options_parse(storage_mount_options address_to out,
                                  string_address list)
{
        p8 address_to at = list;

        while (at && *at)
        {
                p8 address_to token_end = string_first_of_or_end(at, ',');
                positive length = (positive)(token_end - at);
                positive set = 0;
                positive clear = 0;
                bool consume = true;

                /* getopt/libmount accept redundant commas in -o lists. */
                if (!length)
                {
                        at = *token_end ? token_end + 1 : null;
                        continue;
                }

#define STORAGE_OPTION(name, set_bits, clear_bits)                 \
                if (storage_option_name(at, length, name))         \
                { set = (set_bits); clear = (clear_bits); }

                STORAGE_OPTION("ro", STORAGE_MS_RDONLY, 0)
                else STORAGE_OPTION("rw", 0, STORAGE_MS_RDONLY)
                else STORAGE_OPTION("suid", 0, STORAGE_MS_NOSUID)
                else STORAGE_OPTION("nosuid", STORAGE_MS_NOSUID, 0)
                else STORAGE_OPTION("dev", 0, STORAGE_MS_NODEV)
                else STORAGE_OPTION("nodev", STORAGE_MS_NODEV, 0)
                else STORAGE_OPTION("exec", 0, STORAGE_MS_NOEXEC)
                else STORAGE_OPTION("noexec", STORAGE_MS_NOEXEC, 0)
                else STORAGE_OPTION("sync", STORAGE_MS_SYNCHRONOUS, 0)
                else STORAGE_OPTION("async", 0, STORAGE_MS_SYNCHRONOUS)
                else STORAGE_OPTION("dirsync", STORAGE_MS_DIRSYNC, 0)
                else STORAGE_OPTION("mand", STORAGE_MS_MANDLOCK, 0)
                else STORAGE_OPTION("nomand", 0, STORAGE_MS_MANDLOCK)
                else STORAGE_OPTION("atime", 0, STORAGE_MS_NOATIME)
                else STORAGE_OPTION("noatime", STORAGE_MS_NOATIME, 0)
                else STORAGE_OPTION("diratime", 0, STORAGE_MS_NODIRATIME)
                else STORAGE_OPTION("nodiratime", STORAGE_MS_NODIRATIME, 0)
                else STORAGE_OPTION("relatime", STORAGE_MS_RELATIME,
                                    STORAGE_MS_STRICTATIME)
                else STORAGE_OPTION("norelatime", 0, STORAGE_MS_RELATIME)
                else STORAGE_OPTION("strictatime", STORAGE_MS_STRICTATIME,
                                    STORAGE_MS_RELATIME)
                else STORAGE_OPTION("nostrictatime", 0, STORAGE_MS_STRICTATIME)
                else STORAGE_OPTION("lazytime", STORAGE_MS_LAZYTIME, 0)
                else STORAGE_OPTION("nolazytime", 0, STORAGE_MS_LAZYTIME)
                else STORAGE_OPTION("symfollow", 0, STORAGE_MS_NOSYMFOLLOW)
                else STORAGE_OPTION("nosymfollow", STORAGE_MS_NOSYMFOLLOW, 0)
                else STORAGE_OPTION("bind", STORAGE_MS_BIND, 0)
                else STORAGE_OPTION("rbind", STORAGE_MS_BIND | STORAGE_MS_REC, 0)
                else STORAGE_OPTION("move", STORAGE_MS_MOVE, 0)
                else STORAGE_OPTION("remount", STORAGE_MS_REMOUNT, 0)
                else STORAGE_OPTION("silent", STORAGE_MS_SILENT, 0)
                else STORAGE_OPTION("loud", 0, STORAGE_MS_SILENT)
                else if (storage_option_name(at, length, "shared"))
                        out->propagation = STORAGE_MS_SHARED;
                else if (storage_option_name(at, length, "rshared"))
                        out->propagation = STORAGE_MS_SHARED | STORAGE_MS_REC;
                else if (storage_option_name(at, length, "slave"))
                        out->propagation = STORAGE_MS_SLAVE;
                else if (storage_option_name(at, length, "rslave"))
                        out->propagation = STORAGE_MS_SLAVE | STORAGE_MS_REC;
                else if (storage_option_name(at, length, "private"))
                        out->propagation = STORAGE_MS_PRIVATE;
                else if (storage_option_name(at, length, "rprivate"))
                        out->propagation = STORAGE_MS_PRIVATE | STORAGE_MS_REC;
                else if (storage_option_name(at, length, "unbindable"))
                        out->propagation = STORAGE_MS_UNBINDABLE;
                else if (storage_option_name(at, length, "runbindable"))
                        out->propagation = STORAGE_MS_UNBINDABLE | STORAGE_MS_REC;
                else if (storage_option_name(at, length, "noauto"))
                        out->noauto = true;
                else if (storage_option_name(at, length, "nofail"))
                        out->nofail = true;
                else if (storage_option_name(at, length, "loop"))
                        out->unsupported_loop = true;
                else if (storage_option_name(at, length, "defaults") ||
                         storage_option_name(at, length, "auto") ||
                         storage_option_name(at, length, "user") ||
                         storage_option_name(at, length, "users") ||
                         storage_option_name(at, length, "owner") ||
                         storage_option_name(at, length, "group") ||
                         storage_option_name(at, length, "nouser") ||
                         storage_option_name(at, length, "_netdev") ||
                         storage_prefix(at, "x-") || storage_prefix(at, "X-") ||
                         storage_prefix(at, "comment="))
                        ;
                else
                        consume = false;

#undef STORAGE_OPTION

                out->mentioned |= set | clear;
                out->flags = (out->flags | set) & ~clear;
                if (!consume && !storage_data_add(out, at, length))
                        return false;
                at = *token_end ? token_end + 1 : null;
        }
        return true;
}

static bool storage_options_merge(storage_mount_options address_to into,
                                  storage_mount_options address_to extra)
{
        positive operations = STORAGE_MS_BIND | STORAGE_MS_MOVE |
                              STORAGE_MS_REMOUNT | STORAGE_MS_REC;

        into->flags = (into->flags & ~extra->mentioned) |
                      (extra->flags & extra->mentioned) |
                      (extra->flags & operations);
        into->mentioned |= extra->mentioned;
        if (extra->propagation)
                into->propagation = extra->propagation;
        into->noauto |= extra->noauto;
        into->nofail |= extra->nofail;
        into->unsupported_loop |= extra->unsupported_loop;

        return !extra->data_used ||
               storage_data_add(into, extra->data, extra->data_used);
}

/*
        Since Linux 2.6.26, an ordinary remount resets unspecified VFS flags.
        A bind remount has the same trap for the subset it can change.  Read
        the live flags first and apply only the options the caller mentioned;
        otherwise `remount,ro` quietly clears nosuid/nodev/noexec.
*/
static bool storage_remount_options(string_address target,
                                    storage_mount_options address_to asked,
                                    storage_mount_options address_to effective)
{
        storage_mount_table table;
        bool loaded;
        bool parsed = true;

        memory_fill(effective, 0, sizeof(*effective));
        loaded = storage_mount_table_load(address_of table, null);

        if (loaded)
        {
                storage_mount address_to live =
                    storage_mount_find_target(address_of table, target);

                if (live)
                        parsed = storage_options_parse(effective,
                                                       live->options);

                storage_mount_table_release(address_of table);
        }

        if (!parsed || !storage_options_merge(effective, asked))
        {
                storage_options_free(effective);
                return false;
        }

        effective->flags |= STORAGE_MS_REMOUNT;
        effective->mentioned |= STORAGE_MS_REMOUNT;
        return true;
}

static bipolar storage_source(string_address source,
                              string_address address_to resolved,
                              p8 address_to path, positive room)
{
        *resolved = source;

        if (storage_prefix(source, "UUID=") || storage_prefix(source, "LABEL=") ||
            storage_prefix(source, "PARTUUID=") ||
            storage_prefix(source, "PARTLABEL="))
        {
                if (!storage_resolve_tag(source, path, room))
                        return -STORAGE_ERROR_NO_ENTRY;
                *resolved = path;
        }
        return 0;
}

static bipolar storage_mount_one(string_address source, string_address target,
                                 string_address type,
                                 storage_mount_options address_to options)
{
        string_address resolved;
        p8 resolved_path[4096];
        storage_identity identity;
        storage_mount_options effective;
        bool effective_live = false;
        bipolar answer = storage_source(source, address_of resolved,
                                        resolved_path, sizeof(resolved_path));

        if (answer)
                return answer;
        if (options->unsupported_loop)
                return -STORAGE_ERROR_INVALID;

        /* mount(2) itself does not implement util-linux's `-t auto`. */
        if ((!type || storage_word(type, (string_address)"auto")) &&
            !(options->flags & (STORAGE_MS_BIND | STORAGE_MS_MOVE |
                                STORAGE_MS_REMOUNT)) &&
            storage_probe_device(resolved, address_of identity))
                type = identity.type;

        if (options->flags & STORAGE_MS_MOVE)
                return system_call_5(syscall(mount), (positive)resolved,
                                     (positive)target, 0, STORAGE_MS_MOVE, 0);

        if (options->propagation &&
            !(options->flags & ~(STORAGE_MS_REC)))
                return system_call_5(syscall(mount), 0, (positive)target, 0,
                                     options->propagation, 0);

        if (options->flags & STORAGE_MS_BIND)
        {
                if (options->flags & STORAGE_MS_REMOUNT)
                {
                        if (!storage_remount_options(target, options,
                                                     address_of effective))
                                return -STORAGE_ERROR_NO_MEMORY;

                        effective_live = true;
                        answer = system_call_5(
                            syscall(mount), 0, (positive)target, 0,
                            STORAGE_MS_BIND | STORAGE_MS_REMOUNT |
                                (effective.flags & STORAGE_BIND_CHANGEABLE),
                            0);
                }
                else
                {
                        positive bind_flags = STORAGE_MS_BIND |
                                              (options->flags & STORAGE_MS_REC);
                        answer = system_call_5(syscall(mount),
                                               (positive)resolved,
                                               (positive)target, 0,
                                               bind_flags, 0);
                        if (answer)
                                return answer;

                        /* bind(2) ignores VFS restrictions on the first call. */
                        if (options->mentioned & STORAGE_BIND_CHANGEABLE)
                        {
                                if (!storage_remount_options(
                                        target, options, address_of effective))
                                        return -STORAGE_ERROR_NO_MEMORY;

                                effective_live = true;
                                answer = system_call_5(
                                    syscall(mount), 0, (positive)target, 0,
                                    STORAGE_MS_BIND | STORAGE_MS_REMOUNT |
                                        (effective.flags &
                                         STORAGE_BIND_CHANGEABLE),
                                    0);
                        }
                }
        }
        else
        {
                storage_mount_options address_to used = options;

                if (options->flags & STORAGE_MS_REMOUNT)
                {
                        if (!storage_remount_options(target, options,
                                                     address_of effective))
                                return -STORAGE_ERROR_NO_MEMORY;

                        effective_live = true;
                        used = address_of effective;
                }

                answer = system_call_5(
                    syscall(mount), (positive)resolved, (positive)target,
                    (positive)type, used->flags,
                    (positive)(used->data_used ? used->data : null));
        }

        if (effective_live)
                storage_options_free(address_of effective);

        if (!answer && options->propagation)
                answer = system_call_5(syscall(mount), 0, (positive)target, 0,
                                       options->propagation, 0);
        return answer;
}

static fn storage_mount_error(writer diagnostic, string_address program,
                              string_address source, string_address target,
                              bipolar error)
{
        b32 number = error < 0 ? (b32)-(error + 1) + 1 : (b32)error;

        string_format(diagnostic, "%s: %s on %s failed: %s\n", program,
                      source ? source : (string_address)"none",
                      target ? target : (string_address)"none",
                      strerror(number));
}

static b32 storage_mount_fstab_record(string_address program,
                                      storage_fstab address_to record,
                                      storage_mount_options address_to extra,
                                      string_address type_filter, bool explicit,
                                      writer diagnostic)
{
        storage_mount_options options;
        string_address selected_type = record->type;
        bipolar answer;
        bool tolerated;

        memory_fill(address_of options, 0, sizeof(options));
        if (!storage_options_parse(address_of options, record->options) ||
            (extra && !storage_options_merge(address_of options, extra)))
        {
                storage_options_free(address_of options);
                string_format(diagnostic, "%s: no memory\n", program);
                return 1;
        }

        /* With one fstab operand, util-linux treats a single positive -t as
           an override.  Under -a it is a filter. */
        if (explicit && type_filter &&
            !string_first_of(type_filter, ',') &&
            !(type_filter[0] == 'n' && type_filter[1] == 'o' &&
              type_filter[2]))
                selected_type = type_filter;

        if ((!explicit && options.noauto) ||
            (!explicit && (storage_word(record->type, "swap") ||
                           storage_word(record->type, "ignore"))) ||
            (!explicit && !storage_type_match(type_filter, record->type)) ||
            (explicit && selected_type == record->type &&
             !storage_type_match(type_filter, record->type)))
        {
                storage_options_free(address_of options);
                return 0;
        }

        answer = storage_mount_one(record->source, record->target, selected_type,
                                   address_of options);
        tolerated = answer && options.nofail && !explicit;
        if (answer && !tolerated)
                storage_mount_error(diagnostic, program, record->source,
                                    record->target, answer);
        storage_options_free(address_of options);
        return answer && !tolerated ? 1 : 0;
}

static b32 storage_mount_fstab(string_address program, string_address wanted,
                              bool all, storage_mount_options address_to extra,
                              string_address type_filter, string_address fstab,
                              writer diagnostic)
{
        storage_fstab_table table;
        bool loaded = storage_fstab_table_load(address_of table, fstab, false,
                                               diagnostic);
        b32 failed = 0;
        bool found = false;
        storage_mount_table active;
        bool have_active = false;

        if (!loaded)
                return 1;

        if (all)
                have_active = storage_mount_table_load(address_of active, null);

        for (positive at = 0; at < table.count; at++)
        {
                storage_fstab address_to record = table.entry + at;
                if (!all && !storage_word(wanted, record->source) &&
                    !storage_word(wanted, record->target))
                        continue;
                found = true;
                if (all && have_active &&
                    storage_mount_find_target(address_of active,
                                              record->target) &&
                    !storage_option_has(record->options,
                                        (string_address)"remount"))
                        continue;
                failed |= storage_mount_fstab_record(program, record,
                                                     extra, type_filter,
                                                     !all, diagnostic);
                if (!all)
                        break;
        }

        if (!all && !found)
        {
                string_format(diagnostic, "%s: %s not found in %s\n", program,
                              wanted, fstab);
                failed = 1;
        }
        if (have_active)
                storage_mount_table_release(address_of active);
        storage_fstab_table_release(address_of table);
        return failed;
}

static b32 storage_mount_list(writer write, writer diagnostic,
                              string_address type_filter)
{
        storage_mount_table table;
        bool loaded = storage_mount_table_load(address_of table, diagnostic);

        if (!loaded)
                return 1;

        for (positive at = 0; at < table.count; at++)
        {
                storage_mount address_to record = table.entry + at;
                if (storage_type_match(type_filter, record->type))
                {
                        if (*record->filesystem_options)
                                string_format(write, "%s on %s type %s (%s,%s)\n",
                                              record->source, record->target,
                                              record->type, record->options,
                                              record->filesystem_options);
                        else
                                string_format(write, "%s on %s type %s (%s)\n",
                                              record->source, record->target,
                                              record->type, record->options);
                }
        }
        storage_mount_table_release(address_of table);
        return 0;
}

b32 storage_mount_command(positive argc, string_address address_to argv,
                          writer write, writer diagnostic)
{
        string_address type = null;
        string_address fstab = (string_address)"/etc/fstab";
        string_address named_source = null;
        string_address named_target = null;
        string_address operand[2] = {null, null};
        storage_mount_word tag_source;
        storage_mount_options options;
        positive operands = 0;
        bool all = false;
        positive at = 1;
        b32 status = 1;

        memory_fill(address_of tag_source, 0, sizeof(tag_source));
        memory_fill(address_of options, 0, sizeof(options));

        while (at < argc)
        {
                string_address word = argv[at++];
                if (storage_word(word, "--"))
                        break;

                /* Short options are a getopt-style stream. Flags may be
                   clustered; an option which takes a value consumes the
                   rest of its word, or the following argv word when there
                   is no rest. This is one policy for -vn, -vtnfs, -ofoo,
                   and their unclustered spellings rather than four parsers. */
                if (word[0] == '-' && word[1] && word[1] != '-')
                {
                        string_address letter = word + 1;

                        while (*letter)
                        {
                                p8 option = *letter++;
                                string_address value;

                                if (option == 'a')
                                        all = true;
                                else if (option == 'r')
                                {
                                        if (!storage_options_parse(
                                                address_of options,
                                                (string_address)"ro"))
                                                goto no_memory;
                                }
                                else if (option == 'w')
                                {
                                        if (!storage_options_parse(
                                                address_of options,
                                                (string_address)"rw"))
                                                goto no_memory;
                                }
                                else if (option == 'B')
                                {
                                        options.flags |= STORAGE_MS_BIND;
                                        options.mentioned |= STORAGE_MS_BIND;
                                }
                                else if (option == 'R')
                                {
                                        options.flags |= STORAGE_MS_BIND |
                                                         STORAGE_MS_REC;
                                        options.mentioned |= STORAGE_MS_BIND |
                                                             STORAGE_MS_REC;
                                }
                                else if (option == 'M')
                                {
                                        options.flags |= STORAGE_MS_MOVE;
                                        options.mentioned |= STORAGE_MS_MOVE;
                                }
                                else if (option == 'v' || option == 'n' ||
                                         option == 'c' || option == 'i' ||
                                         option == 's')
                                        ;
                                else if (option == 't' || option == 'o' ||
                                         option == 'T' || option == 'L' ||
                                         option == 'U')
                                {
                                        if (*letter)
                                        {
                                                value = letter;
                                                letter += string_length(letter);
                                        }
                                        else
                                        {
                                                if (at >= argc)
                                                        goto missing_option;
                                                value = argv[at++];
                                        }

                                        if (option == 't')
                                                type = value;
                                        else if (option == 'o')
                                        {
                                                if (!storage_options_parse(
                                                        address_of options,
                                                        value))
                                                        goto no_memory;
                                        }
                                        else if (option == 'T')
                                                fstab = value;
                                        else
                                        {
                                                if (!storage_mount_tag(
                                                        address_of tag_source,
                                                        option == 'L' ?
                                                          (string_address)"LABEL" :
                                                          (string_address)"UUID",
                                                        value))
                                                        goto no_memory;
                                                named_source = tag_source.text;
                                        }
                                }
                                else
                                {
                                        string_format(diagnostic,
                                                      "mount: unknown option: -%c\n",
                                                      option);
                                        goto done;
                                }
                        }
                        continue;
                }

                if (storage_word(word, "--all"))
                        all = true;
                else if (storage_word(word, "--types"))
                {
                        if (at >= argc)
                                goto missing_option;
                        type = argv[at++];
                }
                else if (storage_prefix(word, "--types="))
                        type = word + sizeof("--types=") - 1;
                else if (storage_word(word, "--options"))
                {
                        if (at >= argc)
                                goto missing_option;
                        if (!storage_options_parse(address_of options, argv[at++]))
                                goto no_memory;
                }
                else if (storage_prefix(word, "--options="))
                {
                        if (!storage_options_parse(address_of options,
                                                   word + sizeof("--options=") - 1))
                                goto no_memory;
                }
                else if (storage_word(word, "--fstab"))
                {
                        if (at >= argc)
                                goto missing_option;
                        fstab = argv[at++];
                }
                else if (storage_word(word, "--label"))
                {
                        if (at >= argc)
                                goto missing_option;
                        if (!storage_mount_tag(address_of tag_source,
                                               (string_address)"LABEL",
                                               argv[at++]))
                                goto no_memory;
                        named_source = tag_source.text;
                }
                else if (storage_prefix(word, "--label="))
                {
                        if (!storage_mount_tag(address_of tag_source,
                                               (string_address)"LABEL",
                                               word + sizeof("--label=") - 1))
                                goto no_memory;
                        named_source = tag_source.text;
                }
                else if (storage_word(word, "--uuid"))
                {
                        if (at >= argc)
                                goto missing_option;
                        if (!storage_mount_tag(address_of tag_source,
                                               (string_address)"UUID",
                                               argv[at++]))
                                goto no_memory;
                        named_source = tag_source.text;
                }
                else if (storage_prefix(word, "--uuid="))
                {
                        if (!storage_mount_tag(address_of tag_source,
                                               (string_address)"UUID",
                                               word + sizeof("--uuid=") - 1))
                                goto no_memory;
                        named_source = tag_source.text;
                }
                else if (storage_word(word, "--source"))
                {
                        if (at >= argc)
                                goto missing_option;
                        named_source = argv[at++];
                }
                else if (storage_prefix(word, "--source=") &&
                         word[sizeof("--source=") - 1])
                        named_source = word + sizeof("--source=") - 1;
                else if (storage_word(word, "--target"))
                {
                        if (at >= argc)
                                goto missing_option;
                        named_target = argv[at++];
                }
                else if (storage_prefix(word, "--target=") &&
                         word[sizeof("--target=") - 1])
                        named_target = word + sizeof("--target=") - 1;
                else if (storage_word(word, "--read-only"))
                {
                        if (!storage_options_parse(address_of options,
                                                   (string_address)"ro"))
                                goto no_memory;
                }
                else if (storage_word(word, "--read-write"))
                {
                        if (!storage_options_parse(address_of options,
                                                   (string_address)"rw"))
                                goto no_memory;
                }
                else if (storage_word(word, "--bind"))
                {
                        options.flags |= STORAGE_MS_BIND;
                        options.mentioned |= STORAGE_MS_BIND;
                }
                else if (storage_word(word, "--rbind"))
                {
                        options.flags |= STORAGE_MS_BIND | STORAGE_MS_REC;
                        options.mentioned |= STORAGE_MS_BIND | STORAGE_MS_REC;
                }
                else if (storage_word(word, "--move"))
                {
                        options.flags |= STORAGE_MS_MOVE;
                        options.mentioned |= STORAGE_MS_MOVE;
                }
                else if (storage_word(word, "--make-shared"))
                        options.propagation = STORAGE_MS_SHARED;
                else if (storage_word(word, "--make-rshared"))
                        options.propagation = STORAGE_MS_SHARED | STORAGE_MS_REC;
                else if (storage_word(word, "--make-private"))
                        options.propagation = STORAGE_MS_PRIVATE;
                else if (storage_word(word, "--make-rprivate"))
                        options.propagation = STORAGE_MS_PRIVATE | STORAGE_MS_REC;
                else if (storage_word(word, "--make-slave"))
                        options.propagation = STORAGE_MS_SLAVE;
                else if (storage_word(word, "--make-rslave"))
                        options.propagation = STORAGE_MS_SLAVE | STORAGE_MS_REC;
                else if (storage_word(word, "--make-unbindable"))
                        options.propagation = STORAGE_MS_UNBINDABLE;
                else if (storage_word(word, "--make-runbindable"))
                        options.propagation = STORAGE_MS_UNBINDABLE | STORAGE_MS_REC;
                else if (storage_word(word, "--verbose"))
                        ;
                else if (storage_word(word, "--no-mtab") ||
                         storage_word(word, "--no-canonicalize") ||
                         storage_word(word, "--internal-only") ||
                         storage_word(word, "--sloppy"))
                        ;
                else if (*word == '-' && word[1])
                {
                        string_format(diagnostic, "mount: unknown option: %s\n", word);
                        goto done;
                }
                else if (operands < 2)
                        operand[operands++] = word;
                else
                {
                        string_format(diagnostic, "mount: too many operands\n");
                        goto done;
                }
        }

        while (at < argc)
        {
                if (operands >= 2)
                {
                        string_format(diagnostic, "mount: too many operands\n");
                        goto done;
                }
                operand[operands++] = argv[at++];
        }

        if (named_source)
        {
                if (operands > 1 || (named_target && operands))
                {
                        string_format(diagnostic, "mount: too many operands\n");
                        goto done;
                }
                if (operands == 1)
                {
                        operand[1] = operand[0];
                        operand[0] = named_source;
                        operands = 2;
                }
                else if (named_target)
                {
                        operand[0] = named_source;
                        operand[1] = named_target;
                        operands = 2;
                }
                else
                {
                        operand[0] = named_source;
                        operands = 1;
                }
        }
        else if (named_target)
        {
                if (operands > 1)
                {
                        string_format(diagnostic, "mount: too many operands\n");
                        goto done;
                }
                if (operands == 1)
                {
                        operand[1] = named_target;
                        operands = 2;
                }
                else
                {
                        operand[0] = named_target;
                        operands = 1;
                }
        }

        if (all)
        {
                if (operands)
                {
                        string_format(diagnostic, "mount: -a takes no operands\n");
                        goto done;
                }
                status = storage_mount_fstab((string_address)"mount", null, true,
                                             address_of options, type, fstab,
                                             diagnostic);
                goto done;
        }
        if (!operands)
        {
                status = storage_mount_list(write, diagnostic, type);
                goto done;
        }
        if (operands == 1)
        {
                /* Propagation changes name only a target and never consult
                   fstab. Remount can infer source/type from mountinfo. */
                if (options.propagation &&
                    !(options.flags & (STORAGE_MS_BIND | STORAGE_MS_MOVE |
                                       STORAGE_MS_REMOUNT)))
                {
                        bipolar answer = storage_mount_one((string_address)"none",
                                                           operand[0], null,
                                                           address_of options);
                        if (answer)
                                storage_mount_error(diagnostic,
                                                    (string_address)"mount",
                                                    null, operand[0], answer);
                        status = answer ? 1 : 0;
                }
                else if (options.flags & STORAGE_MS_REMOUNT)
                {
                        storage_mount_table table;
                        if (storage_mount_table_load(address_of table, diagnostic))
                        {
                                storage_mount address_to live =
                                    storage_mount_find_target(address_of table,
                                                              operand[0]);
                                bipolar answer;

                                if (!live)
                                {
                                        string_format(diagnostic,
                                                      "mount: %s is not mounted\n",
                                                      operand[0]);
                                        status = 1;
                                }
                                else
                                {
                                        answer = storage_mount_one(live->source,
                                                                   live->target,
                                                                   live->type,
                                                                   address_of options);
                                        if (answer)
                                                storage_mount_error(
                                                    diagnostic,
                                                    (string_address)"mount",
                                                    live->source, live->target,
                                                    answer);
                                        status = answer ? 1 : 0;
                                }
                                storage_mount_table_release(address_of table);
                        }
                }
                else
                        status = storage_mount_fstab((string_address)"mount",
                                                     operand[0], false,
                                                     address_of options, type,
                                                     fstab, diagnostic);
                goto done;
        }
        else
        {
                bipolar answer;

                answer = storage_mount_one(operand[0], operand[1], type,
                                           address_of options);
                if (answer)
                        storage_mount_error(diagnostic,
                                            (string_address)"mount", operand[0],
                                            operand[1], answer);
                status = answer ? 1 : 0;
                goto done;
        }

missing_option:
        string_format(diagnostic, "mount: option requires an argument\n");
        goto done;
no_memory:
        string_format(diagnostic, "mount: no memory\n");
done:
        storage_options_free(address_of options);
        storage_mount_word_free(address_of tag_source);
        return status;
}

static bool storage_path_below(string_address path, string_address root)
{
        positive length = string_length(root);

        while (length > 1 && root[length - 1] == '/')
                length--;

        if (string_compare_max(path, root, length))
                return false;
        if (!path[length])
                return true;
        return (length == 1 && root[0] == '/') || path[length] == '/';
}

static string_address storage_umount_target(storage_mount_table address_to table,
                                            string_address asked)
{
        string_address found = asked;

        /* Last wins: stacked mounts are unmounted from the top. */
        for (positive at = 0; at < table->count; at++)
                if (storage_word(table->entry[at].source, asked) ||
                    storage_word(table->entry[at].target, asked))
                        found = table->entry[at].target;
        return found;
}

static b32 storage_umount_one(writer diagnostic, string_address program,
                             string_address target, positive flags,
                             bool read_only)
{
        bipolar answer = system_call_2(syscall(umount2), (positive)target, flags);

        if (answer && read_only && answer == -STORAGE_ERROR_BUSY)
        {
                storage_mount_options remount;

                memory_fill(address_of remount, 0, sizeof(remount));
                remount.flags = STORAGE_MS_REMOUNT | STORAGE_MS_RDONLY;
                remount.mentioned = STORAGE_MS_REMOUNT | STORAGE_MS_RDONLY;
                answer = storage_mount_one((string_address)"none", target,
                                           null, address_of remount);
        }
        if (answer)
        {
                b32 number = answer < 0 ? (b32)-(answer + 1) + 1 :
                                         (b32)answer;
                string_format(diagnostic, "%s: %s failed: %s\n", program,
                              target, strerror(number));
        }
        return answer ? 1 : 0;
}

static b32 storage_umount_recursive(writer diagnostic, string_address program,
                                    storage_mount_table address_to table,
                                    string_address root, string_address types,
                                    positive flags, bool read_only)
{
        b32 failed = 0;
        positive longest = positive_max;

        /* Repeated longest-path selection avoids another allocation and
           guarantees children leave before their parent even if proc changes
           record order. Equal lengths are distinct siblings. */
        for (;;)
        {
                positive selected = positive_max;
                positive selected_length = 0;

                for (positive at = 0; at < table->count; at++)
                {
                        storage_mount address_to record = table->entry + at;
                        positive length;

                        if (!record->target)
                                continue;
                        length = string_length(record->target);
                        if (length < longest &&
                            length > selected_length &&
                            storage_path_below(record->target, root) &&
                            storage_type_match(types, record->type))
                        {
                                selected = at;
                                selected_length = length;
                        }
                }
                if (selected == positive_max)
                        break;

                longest = selected_length;
                /* All siblings at this depth. */
                for (positive at = 0; at < table->count; at++)
                {
                        storage_mount address_to record = table->entry + at;
                        if (record->target &&
                            string_length(record->target) == selected_length &&
                            storage_path_below(record->target, root) &&
                            storage_type_match(types, record->type))
                        {
                                failed |= storage_umount_one(diagnostic, program,
                                                             record->target, flags,
                                                             read_only);
                                record->target = null;
                        }
                }
        }
        return failed;
}

b32 storage_umount_command(positive argc, string_address address_to argv,
                           writer write, writer diagnostic)
{
        positive flags = STORAGE_UMOUNT_NOFOLLOW;
        string_address types = null;
        string_address address_to operand = null;
        positive operand_room = 0;
        positive operands = 0;
        bool all = false;
        bool recursive = false;
        bool read_only = false;
        positive at = 1;
        storage_mount_table table;
        bool loaded;
        b32 failed = 0;

        (void)write;
        while (at < argc)
        {
                string_address word = argv[at++];
                if (storage_word(word, "--"))
                        break;

                if (word[0] == '-' && word[1] && word[1] != '-')
                {
                        string_address letter = word + 1;

                        while (*letter)
                        {
                                p8 option = *letter++;

                                if (option == 'a')
                                        all = true;
                                else if (option == 'l')
                                        flags |= STORAGE_MNT_DETACH;
                                else if (option == 'f')
                                        flags |= STORAGE_MNT_FORCE;
                                else if (option == 'R')
                                        recursive = true;
                                else if (option == 'r')
                                        read_only = true;
                                else if (option == 'v' || option == 'n' ||
                                         option == 'c' || option == 'i')
                                        ;
                                else if (option == 't')
                                {
                                        if (*letter)
                                        {
                                                types = letter;
                                                letter += string_length(letter);
                                        }
                                        else
                                        {
                                                if (at >= argc)
                                                        goto missing_option;
                                                types = argv[at++];
                                        }
                                }
                                else
                                {
                                        string_format(diagnostic,
                                                      "umount: unknown option: -%c\n",
                                                      option);
                                        goto failed_early;
                                }
                        }
                        continue;
                }

                if (storage_word(word, "--all"))
                        all = true;
                else if (storage_word(word, "--lazy"))
                        flags |= STORAGE_MNT_DETACH;
                else if (storage_word(word, "--force"))
                        flags |= STORAGE_MNT_FORCE;
                else if (storage_word(word, "--recursive"))
                        recursive = true;
                else if (storage_word(word, "--read-only"))
                        read_only = true;
                else if (storage_word(word, "--types"))
                {
                        if (at >= argc)
                                goto missing_option;
                        types = argv[at++];
                }
                else if (storage_prefix(word, "--types="))
                        types = word + sizeof("--types=") - 1;
                else if (storage_word(word, "--verbose"))
                        ;
                else if (storage_word(word, "--no-mtab") ||
                         storage_word(word, "--no-canonicalize") ||
                         storage_word(word, "--internal-only"))
                        ;
                else if (*word == '-' && word[1])
                {
                        string_format(diagnostic, "umount: unknown option: %s\n", word);
                        goto failed_early;
                }
                else
                {
                        if (!memory_reserve(
                                (address_any address_to)address_of operand,
                                address_of operand_room, operands, operands + 1,
                                sizeof(operand[0]), 16))
                        {
                                string_format(diagnostic, "umount: no memory\n");
                                goto failed_early;
                        }
                        operand[operands++] = word;
                }
        }
        while (at < argc)
        {
                if (!memory_reserve(
                        (address_any address_to)address_of operand,
                        address_of operand_room, operands, operands + 1,
                        sizeof(operand[0]), 16))
                {
                        string_format(diagnostic, "umount: no memory\n");
                        goto failed_early;
                }
                operand[operands++] = argv[at++];
        }

        if (!all && !operands)
        {
                string_format(diagnostic, "umount: missing operand\n");
                goto failed_early;
        }

        if (all && operands)
        {
                string_format(diagnostic,
                              "umount: -a takes no operands\n");
                goto failed_early;
        }

        loaded = storage_mount_table_load(address_of table, diagnostic);
        if (!loaded)
                goto failed_early;

        if (all)
        {
                /* Never dismantle the root mount. */
                for (positive i = table.count; i; i--)
                {
                        storage_mount address_to record = table.entry + i - 1;
                        if (!storage_word(record->target, "/") &&
                            storage_type_match(types, record->type))
                                failed |= storage_umount_one(
                                    diagnostic, (string_address)"umount",
                                    record->target, flags, read_only);
                }
        }

        for (positive i = 0; i < operands; i++)
        {
                string_address target = storage_umount_target(address_of table,
                                                               operand[i]);
                if (recursive)
                        failed |= storage_umount_recursive(diagnostic,
                                                           (string_address)"umount",
                                                           address_of table, target,
                                                           types, flags, read_only);
                else
                        failed |= storage_umount_one(
                            diagnostic, (string_address)"umount", target,
                            flags, read_only);
        }

        storage_mount_table_release(address_of table);
        memory_release((address_any address_to)address_of operand,
                       address_of operand_room, address_of operands,
                       sizeof(operand[0]));
        return failed;

missing_option:
        string_format(diagnostic, "umount: option requires an argument\n");
failed_early:
        memory_release((address_any address_to)address_of operand,
                       address_of operand_room, address_of operands,
                       sizeof(operand[0]));
        return 1;
}
