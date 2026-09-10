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

#define STORAGE_ERROR_PERMISSION 1
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

#define storage_prefix(word, prefix)                                        \
        (!string_compare_max((word), (string_address)(prefix),              \
                             sizeof(prefix) - 1))

typedef byte_store storage_mount_word;

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
        if (!byte_store_reserve(word, wanted, 64))
                return false;

        memory_copy_apart(word->bytes, tag, tag_length);
        word->bytes[tag_length] = '=';
        memory_copy_apart(word->bytes + tag_length + 1, value,
                          value_length + 1);
        word->used = wanted;
        return true;
}

typedef struct
{
        positive flags;
        positive mentioned;
        positive propagation;
        byte_store data;
        bool noauto;
        bool nofail;
        bool unsupported_loop;
        bool fake;
        bool verbose;
} storage_mount_options;

static fn storage_options_free(storage_mount_options address_to options)
{
        byte_store_release(address_of options->data);
        memory_fill(options, 0, sizeof(*options));
}

static bool storage_data_add(storage_mount_options address_to options,
                             string_address item, positive length)
{
        positive extra = (options->data.used ? 1 : 0) + 1;

        if (length > positive_max - extra)
                return false;
        extra += length;

        if (extra > positive_max - options->data.used ||
            !byte_store_reserve(address_of options->data,
                                options->data.used + extra, 64))
                return false;
        if (options->data.used)
                options->data.bytes[options->data.used++] = ',';
        memory_copy_apart(options->data.bytes + options->data.used,
                          item, length);
        options->data.used += length;
        options->data.bytes[options->data.used] = 0;
        return true;
}

typedef struct {
        string_address name;
        p32 set_length;
        p32 clear_action;
} storage_mount_option;

enum { STORAGE_OPTION_FLAGS, STORAGE_OPTION_PROPAGATION,
       STORAGE_OPTION_NOAUTO, STORAGE_OPTION_NOFAIL, STORAGE_OPTION_LOOP };

#define STORAGE_OPTION_LENGTH_SHIFT 26
#define STORAGE_OPTION_FLAG_MASK (((p32)1 << STORAGE_OPTION_LENGTH_SHIFT) - 1)
#define STORAGE_OPTION_ACTION_SHIFT 28
#define O(name, set, clear, action)                                         \
        {(string_address)(name),                                            \
         (p32)(set) | ((p32)(sizeof(name) - 1)                              \
                        << STORAGE_OPTION_LENGTH_SHIFT),                    \
         (p32)(clear) | ((p32)(action) << STORAGE_OPTION_ACTION_SHIFT)}

/* Length and action live above Linux's highest mount flag, leaving the hot
   table at two words and one pointer per spelling. */
static const storage_mount_option storage_mount_option_table[] = {
    O("ro", STORAGE_MS_RDONLY, 0, STORAGE_OPTION_FLAGS),
    O("rw", 0, STORAGE_MS_RDONLY, STORAGE_OPTION_FLAGS),
    O("suid", 0, STORAGE_MS_NOSUID, STORAGE_OPTION_FLAGS),
    O("nosuid", STORAGE_MS_NOSUID, 0, STORAGE_OPTION_FLAGS),
    O("dev", 0, STORAGE_MS_NODEV, STORAGE_OPTION_FLAGS),
    O("nodev", STORAGE_MS_NODEV, 0, STORAGE_OPTION_FLAGS),
    O("exec", 0, STORAGE_MS_NOEXEC, STORAGE_OPTION_FLAGS),
    O("noexec", STORAGE_MS_NOEXEC, 0, STORAGE_OPTION_FLAGS),
    O("sync", STORAGE_MS_SYNCHRONOUS, 0, STORAGE_OPTION_FLAGS),
    O("async", 0, STORAGE_MS_SYNCHRONOUS, STORAGE_OPTION_FLAGS),
    O("dirsync", STORAGE_MS_DIRSYNC, 0, STORAGE_OPTION_FLAGS),
    O("mand", STORAGE_MS_MANDLOCK, 0, STORAGE_OPTION_FLAGS),
    O("nomand", 0, STORAGE_MS_MANDLOCK, STORAGE_OPTION_FLAGS),
    O("atime", 0, STORAGE_MS_NOATIME, STORAGE_OPTION_FLAGS),
    O("noatime", STORAGE_MS_NOATIME, 0, STORAGE_OPTION_FLAGS),
    O("diratime", 0, STORAGE_MS_NODIRATIME, STORAGE_OPTION_FLAGS),
    O("nodiratime", STORAGE_MS_NODIRATIME, 0, STORAGE_OPTION_FLAGS),
    O("relatime", STORAGE_MS_RELATIME, STORAGE_MS_STRICTATIME, STORAGE_OPTION_FLAGS),
    O("norelatime", 0, STORAGE_MS_RELATIME, STORAGE_OPTION_FLAGS),
    O("strictatime", STORAGE_MS_STRICTATIME, STORAGE_MS_RELATIME, STORAGE_OPTION_FLAGS),
    O("nostrictatime", 0, STORAGE_MS_STRICTATIME, STORAGE_OPTION_FLAGS),
    O("lazytime", STORAGE_MS_LAZYTIME, 0, STORAGE_OPTION_FLAGS),
    O("nolazytime", 0, STORAGE_MS_LAZYTIME, STORAGE_OPTION_FLAGS),
    O("symfollow", 0, STORAGE_MS_NOSYMFOLLOW, STORAGE_OPTION_FLAGS),
    O("nosymfollow", STORAGE_MS_NOSYMFOLLOW, 0, STORAGE_OPTION_FLAGS),
    O("bind", STORAGE_MS_BIND, 0, STORAGE_OPTION_FLAGS),
    O("rbind", STORAGE_MS_BIND | STORAGE_MS_REC, 0, STORAGE_OPTION_FLAGS),
    O("move", STORAGE_MS_MOVE, 0, STORAGE_OPTION_FLAGS),
    O("remount", STORAGE_MS_REMOUNT, 0, STORAGE_OPTION_FLAGS),
    O("silent", STORAGE_MS_SILENT, 0, STORAGE_OPTION_FLAGS),
    O("loud", 0, STORAGE_MS_SILENT, STORAGE_OPTION_FLAGS),
    O("shared", STORAGE_MS_SHARED, 0, STORAGE_OPTION_PROPAGATION),
    O("rshared", STORAGE_MS_SHARED | STORAGE_MS_REC, 0, STORAGE_OPTION_PROPAGATION),
    O("slave", STORAGE_MS_SLAVE, 0, STORAGE_OPTION_PROPAGATION),
    O("rslave", STORAGE_MS_SLAVE | STORAGE_MS_REC, 0, STORAGE_OPTION_PROPAGATION),
    O("private", STORAGE_MS_PRIVATE, 0, STORAGE_OPTION_PROPAGATION),
    O("rprivate", STORAGE_MS_PRIVATE | STORAGE_MS_REC, 0, STORAGE_OPTION_PROPAGATION),
    O("unbindable", STORAGE_MS_UNBINDABLE, 0, STORAGE_OPTION_PROPAGATION),
    O("runbindable", STORAGE_MS_UNBINDABLE | STORAGE_MS_REC, 0, STORAGE_OPTION_PROPAGATION),
    O("noauto", 0, 0, STORAGE_OPTION_NOAUTO),
    O("nofail", 0, 0, STORAGE_OPTION_NOFAIL),
    O("loop", 0, 0, STORAGE_OPTION_LOOP),
    O("defaults", 0, 0, STORAGE_OPTION_FLAGS),
    O("auto", 0, 0, STORAGE_OPTION_FLAGS),
    O("user", 0, 0, STORAGE_OPTION_FLAGS),
    O("users", 0, 0, STORAGE_OPTION_FLAGS),
    O("owner", 0, 0, STORAGE_OPTION_FLAGS),
    O("group", 0, 0, STORAGE_OPTION_FLAGS),
    O("nouser", 0, 0, STORAGE_OPTION_FLAGS),
    O("_netdev", 0, 0, STORAGE_OPTION_FLAGS),
};
#undef O

static bool storage_options_parse(storage_mount_options address_to out,
                                  string_address list)
{
        string_address cursor = list;
        string_address at;
        positive length;

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                positive set = 0;
                positive clear = 0;
                bool consume = false;

                /* getopt/libmount accept redundant commas in -o lists. */
                if (!length)
                        continue;

                for (positive i = 0; i < array_count(storage_mount_option_table); i++)
                {
                        const storage_mount_option address_to option =
                            storage_mount_option_table + i;
                        positive option_length = option->set_length >>
                                                 STORAGE_OPTION_LENGTH_SHIFT;

                        if (length != option_length ||
                            memory_compare(at, option->name, length))
                                continue;

                        positive action = option->clear_action >>
                                          STORAGE_OPTION_ACTION_SHIFT;

                        set = option->set_length & STORAGE_OPTION_FLAG_MASK;
                        clear = option->clear_action & STORAGE_OPTION_FLAG_MASK;
                        if (action == STORAGE_OPTION_PROPAGATION)
                        {
                                /* Propagation is its own mount(2) call once
                                   the mount exists; carried in the flags it
                                   would turn `bind,rshared` into an rbind. */
                                out->propagation = set;
                                set = 0;
                        }
                        else if (action == STORAGE_OPTION_NOAUTO)
                                out->noauto = true;
                        else if (action == STORAGE_OPTION_NOFAIL)
                                out->nofail = true;
                        else if (action == STORAGE_OPTION_LOOP)
                                out->unsupported_loop = true;
                        consume = true;
                        break;
                }

                if (!consume)
                        consume = (length >= 2 &&
                                   ((at[0] == 'x' || at[0] == 'X') &&
                                    at[1] == '-')) ||
                                  (length >= 8 &&
                                   !memory_compare(at, "comment=", 8));

                out->mentioned |= set | clear;
                out->flags = (out->flags | set) & ~clear;
                if (!consume && !storage_data_add(out, at, length))
                        return false;
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

        return !extra->data.used ||
               storage_data_add(into, extra->data.bytes, extra->data.used);
}

/*      The absolute spelling of a word that names something, and null for
        one that does not -- `tmpfs` is not a path and stays as it was
        written, which is how the reference reports both. */
static p8 address_to storage_mount_canonical(string_address word,
                                             positive address_to room)
{
        bipolar handle = system_open_at(AT_FDCWD, word,
                                        STORAGE_OPEN_PATH | O_CLOEXEC);
        p8 address_to resolved;

        if (handle < 0)
                return null;
        resolved = storage_fd_path(handle, room);
        system_close(handle);
        return resolved;
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
                /*      The table spells its targets absolutely, so a word
                        the caller wrote as `b` finds nothing there and the
                        remount then asks for flags the kernel locked when
                        it handed this mount over: EPERM, where the
                        reference keeps nosuid and nodev and succeeds. */
                positive room = 0;
                p8 address_to resolved = storage_mount_canonical(target,
                                                                 address_of room);
                storage_mount address_to live =
                    storage_mount_find_target(address_of table,
                                              resolved ? (string_address)resolved
                                                       : target);

                if (live)
                        parsed = storage_options_parse(effective,
                                                       live->options);

                if (resolved)
                        memory_free(resolved, room);
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
        /* --fake does everything but the mount-related system calls. */
        if (options->fake)
                return 0;

        /* mount(2) itself does not implement util-linux's `-t auto`. */
        if ((!type || storage_word(type, (string_address)"auto")) &&
            !(options->flags & (STORAGE_MS_BIND | STORAGE_MS_MOVE |
                                STORAGE_MS_REMOUNT)) &&
            storage_probe_device(resolved, address_of identity))
                type = identity.type;

        if (options->flags & STORAGE_MS_MOVE)
                return system_mount(resolved, target, 0, STORAGE_MS_MOVE, 0);

        /* A propagation change alone names only a target; combined with a
           real mount it follows that mount as a second call below. */
        if (options->propagation && !type &&
            storage_word(source, (string_address)"none") &&
            !(options->flags & ~(STORAGE_MS_REC)))
                return system_mount(0, target, 0,
                                     options->propagation, 0);

        bool bind = (options->flags & STORAGE_MS_BIND) != 0;

        if (bind && !(options->flags & STORAGE_MS_REMOUNT))
        {
                positive bind_flags = STORAGE_MS_BIND |
                                      (options->flags & STORAGE_MS_REC);
                answer = system_mount(resolved, target, 0, bind_flags, 0);
                if (answer)
                        return answer;
        }

        /* bind(2) ignores VFS restrictions on the first call. Both kinds of
           remount merge the requested changes with the live options. */
        if ((options->flags & STORAGE_MS_REMOUNT) ||
            (bind && (options->mentioned & STORAGE_BIND_CHANGEABLE)))
        {
                if (!storage_remount_options(target, options,
                                             address_of effective))
                        return -STORAGE_ERROR_NO_MEMORY;
                effective_live = true;
        }

        if (bind)
        {
                if (effective_live)
                        answer = system_call_5(
                            syscall(mount), 0, (positive)target, 0,
                            STORAGE_MS_BIND | STORAGE_MS_REMOUNT |
                                (effective.flags & STORAGE_BIND_CHANGEABLE),
                            0);
        }
        else
        {
                storage_mount_options address_to used =
                    effective_live ? address_of effective : options;

                answer = system_call_5(
                    syscall(mount), (positive)resolved, (positive)target,
                    (positive)type, used->flags,
                    (positive)(used->data.used ? used->data.bytes : null));
        }

        if (effective_live)
                storage_options_free(address_of effective);

        if (!answer && options->propagation)
                answer = system_mount(0, target, 0,
                                       options->propagation, 0);
        return answer;
}


/*      ignored says the record was passed over rather than mounted: noauto,
        swap, a type the filter excludes. mount -a counts those in neither
        column, and the difference is the whole exit status -- one failure
        beside one ignored record is 32, "all failed", not 64. */


static b32 storage_mount_fstab_record(string_address program,
                                      storage_fstab address_to record,
                                      storage_mount_options address_to extra,
                                      string_address type_filter, bool explicit,
                                      writer diagnostic, writer write,
                                      bool address_to ignored)
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
                return string_report(diagnostic, 1, "%s: no memory\n", program);
        }
        if (extra)
        {
                options.fake = extra->fake;
                options.verbose = extra->verbose;
        }
        address_to ignored = false;

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
                if (options.verbose && write)
                        string_format(write, "%s: ignored\n", record->target);
                storage_options_free(address_of options);
                address_to ignored = true;
                return 0;
        }

        answer = storage_mount_one(record->source, record->target, selected_type,
                                   address_of options);
        tolerated = answer && options.nofail && !explicit;
        if (!answer && options.verbose && write)
                string_format(write, "%s: successfully mounted\n", record->target);
        if (answer && !tolerated)
                string_format(diagnostic, "%s: %s on %s failed: %s\n", program,
                              (record->source) ? (record->source) : (string_address)"none",
                              (record->target) ? (record->target) : (string_address)"none",
                              strerror(answer < 0 ? (b32)-(answer + 1) + 1 : (b32)answer));
        storage_options_free(address_of options);
        return answer && !tolerated ? 32 : 0;
}

static b32 storage_mount_fstab(string_address program, string_address wanted,
                              bool all, storage_mount_options address_to extra,
                              string_address type_filter, string_address fstab,
                              writer diagnostic, writer write)
{
        storage_fstab_table table;
        bool loaded = storage_fstab_table_load(address_of table, fstab, false,
                                               diagnostic);
        b32 failed = 0;
        bool found = false;
        positive mounted = 0;
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
                {
                        if (extra && extra->verbose && write)
                                string_format(write, "%s: already mounted\n",
                                              record->target);
                        continue;
                }
                bool ignored = false;
                b32 one = storage_mount_fstab_record(program, record, extra,
                                                     type_filter, !all,
                                                     diagnostic, write,
                                                     address_of ignored);
                failed |= one;
                if (!one && !ignored)
                        mounted++;
                if (!all)
                        break;
        }

        /* util-linux: 64 when some of -a succeeded, 32 when a mount failed. */
        if (all && failed && mounted)
                failed = 64;
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
                        string_format(write, "%s on %s type %s (", record->source,
                                      record->target, record->type);
                        storage_combined_options_write(write, record, false,
                                                       false);
                        if (write)
                                write(str(")\n"));
                }
        }
        storage_mount_table_release(address_of table);
        return 0;
}

b32 storage_mount_command(positive argc, string_address address_to argv,
                          writer write, writer diagnostic)
{
        static const storage_argument_name arguments[] = {
            STORAGE_ARGUMENT("all", 'a'),
            STORAGE_ARGUMENT("types", 't'),
            STORAGE_ARGUMENT("options", 'o'),
            STORAGE_ARGUMENT("fstab", 'T'),
            STORAGE_ARGUMENT("label", 'L'),
            STORAGE_ARGUMENT("uuid", 'U'),
            STORAGE_ARGUMENT("source", 'S'),
            STORAGE_ARGUMENT("target", 'X'),
            STORAGE_ARGUMENT("read-only", 'r'),
            STORAGE_ARGUMENT("read-write", 'w'),
            STORAGE_ARGUMENT("bind", 'B'),
            STORAGE_ARGUMENT("rbind", 'R'),
            STORAGE_ARGUMENT("move", 'M'),
            STORAGE_ARGUMENT("make-shared", '1'),
            STORAGE_ARGUMENT("make-rshared", '2'),
            STORAGE_ARGUMENT("make-private", '3'),
            STORAGE_ARGUMENT("make-rprivate", '4'),
            STORAGE_ARGUMENT("make-slave", '5'),
            STORAGE_ARGUMENT("make-rslave", '6'),
            STORAGE_ARGUMENT("make-unbindable", '7'),
            STORAGE_ARGUMENT("make-runbindable", '8'),
            STORAGE_ARGUMENT("verbose", 'v'),
            STORAGE_ARGUMENT("no-mtab", 'n'),
            STORAGE_ARGUMENT("no-canonicalize", 'c'),
            STORAGE_ARGUMENT("internal-only", 'i'),
            STORAGE_ARGUMENT("sloppy", 's'),
            STORAGE_ARGUMENT("fake", 'f'),
            STORAGE_ARGUMENT("show-labels", 'l'),
            STORAGE_ARGUMENT("fork", 'F'),
            STORAGE_ARGUMENT("ro", 'r'),
            STORAGE_ARGUMENT("rw", 'w'),
        };
        string_address type = null;
        string_address fstab = (string_address)"/etc/fstab";
        string_address named_source = null;
        string_address named_target = null;
        string_address operand[2] = {null, null};
        storage_mount_word tag_source;
        storage_mount_options options;
        positive operands = 0;
        bool all = false;
        /*  --no-canonicalize keeps the target as it was written, in what is
            done and in what --verbose says was done. */
        bool canonical = true;
        argument_cursor taking = {.argc = argc, .argv = argv, .at = 1};
        string_address value;
        b32 option;
        b32 status = 1;

        memory_fill(address_of tag_source, 0, sizeof(tag_source));
        memory_fill(address_of options, 0, sizeof(options));

        while ((option = storage_argument_next(
                    address_of taking, (string_address)"arwBRMvncistoflFTLU",
                    (string_address)"toTLUSX", arguments,
                    array_count(arguments), address_of value)) !=
               ARGUMENT_END)
        {
                if (option == ARGUMENT_OPERAND)
                {
                        if (operands >= 2)
                        {
                                string_format(diagnostic,
                                              "mount: too many operands\n");
                                goto done;
                        }
                        operand[operands++] = value;
                }
                else if (option == ARGUMENT_MISSING)
                        goto missing_option;
                else if (option == ARGUMENT_UNKNOWN)
                {
                        if (taking.letters)
                        {
                                p8 letter[2] = {(p8)taking.letters[-1], end};
                                string_format(diagnostic,
                                              "mount: invalid option -- '%s'\n"
                                              "Try 'mount --help' for more information.\n",
                                              letter);
                        }
                        else
                                string_format(diagnostic,
                                              "mount: unrecognized option '%s'\n"
                                              "Try 'mount --help' for more information.\n",
                                              taking.word);
                        goto done;
                }
                else if (option == 'a')
                        all = true;
                else if (option == 'f')
                        options.fake = true;
                else if (option == 'v')
                        options.verbose = true;
                else if (option == 'F')
                        ; /* One device at a time: nothing to fork for. */
                else if (option == 'r' || option == 'w')
                {
                        if (!storage_options_parse(
                                address_of options,
                                option == 'r' ? (string_address)"ro" :
                                                (string_address)"rw"))
                                goto no_memory;
                }
                else if (option == 'B' || option == 'R')
                {
                        positive flags = STORAGE_MS_BIND |
                            (option == 'R' ? STORAGE_MS_REC : 0);
                        options.flags |= flags;
                        options.mentioned |= flags;
                }
                else if (option == 'M')
                {
                        options.flags |= STORAGE_MS_MOVE;
                        options.mentioned |= STORAGE_MS_MOVE;
                }
                else if (option == 'c')
                        canonical = false;
                else if (option == 't')
                        type = value;
                else if (option == 'o')
                {
                        if (!storage_options_parse(address_of options, value))
                                goto no_memory;
                }
                else if (option == 'T')
                        fstab = value;
                else if (option == 'L' || option == 'U')
                {
                        if (!storage_mount_tag(
                                address_of tag_source,
                                option == 'L' ? (string_address)"LABEL" :
                                                (string_address)"UUID",
                                value))
                                goto no_memory;
                        named_source = tag_source.bytes;
                }
                else if (option == 'S')
                        named_source = value;
                else if (option == 'X')
                        named_target = value;
                else if (option >= '1' && option <= '8')
                {
                        static const positive propagation[] = {
                            STORAGE_MS_SHARED,
                            STORAGE_MS_SHARED | STORAGE_MS_REC,
                            STORAGE_MS_PRIVATE,
                            STORAGE_MS_PRIVATE | STORAGE_MS_REC,
                            STORAGE_MS_SLAVE,
                            STORAGE_MS_SLAVE | STORAGE_MS_REC,
                            STORAGE_MS_UNBINDABLE,
                            STORAGE_MS_UNBINDABLE | STORAGE_MS_REC,
                        };
                        options.propagation = propagation[option - '1'];
                }
        }

        {
                static const storage_exclusive_pair attach[] = {
                    {'B', (string_address)"bind"},
                    {'M', (string_address)"move"},
                    {'R', (string_address)"rbind"}};

                if (storage_exclusive_refuse(
                        diagnostic, (string_address)"mount", argc, argv,
                        arguments, array_count(arguments),
                        (string_address)"toTLUSX", attach,
                        array_count(attach)))
                        goto done;
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
                                             diagnostic, write);
                goto done;
        }
        if (!operands)
        {
                /* Listing is what mount does with no operand; asking it to
                   change a mount and naming none is a usage error. */
                if (options.mentioned || options.propagation ||
                    options.data.used || named_source || named_target)
                {
                        string_format(diagnostic, "mount: bad usage\n"
                                      "Try 'mount --help' for more information.\n");
                        status = 1;
                        goto done;
                }
                status = storage_mount_list(write, diagnostic, type);
                goto done;
        }
        if (operands == 1)
        {
                /* Propagation changes name only a target and never consult
                   fstab. Remount can infer source/type from mountinfo. */
                /*  A propagation change stands alone even beside other
                    options, so long as one of them is the recursion the
                    change itself asked for: the reference reads the rest as
                    attributes of that one operation and never looks the
                    target up in fstab. */
                if (options.propagation &&
                    (!(options.flags & (STORAGE_MS_BIND | STORAGE_MS_MOVE |
                                        STORAGE_MS_REMOUNT)) ||
                     ((options.flags | options.propagation) &
                      STORAGE_MS_REC)))
                {
                        bipolar answer = storage_mount_one((string_address)"none",
                                                           operand[0], null,
                                                           address_of options);
                        if (answer)
                                string_format(diagnostic, "%s: %s on %s failed: %s\n", (string_address)"mount",
                                              (null) ? (null) : (string_address)"none",
                                              (operand[0]) ? (operand[0]) : (string_address)"none",
                                              strerror(answer < 0 ? (b32)-(answer + 1) + 1 : (b32)answer));
                        else if (options.verbose)
                        {
                                /* util-linux names the followed, absolute
                                   spelling here, not the word it was given. */
                                positive room = 0;
                                p8 address_to resolved = null;
                                bipolar handle = system_open_at(
                                    AT_FDCWD, operand[0],
                                    STORAGE_OPEN_PATH | O_CLOEXEC);
                                if (handle >= 0)
                                {
                                        resolved = storage_fd_path(handle, address_of room);
                                        system_close(handle);
                                }
                                string_format(write,
                                              "mount: %s propagation flags changed.\n",
                                              resolved ? (string_address)resolved
                                                       : operand[0]);
                                if (resolved)
                                        memory_free(resolved, room);
                        }
                        status = answer ? 32 : 0;
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
                                        status = 32;
                                }
                                else
                                {
                                        answer = storage_mount_one(live->source,
                                                                   live->target,
                                                                   live->type,
                                                                   address_of options);
                                        if (answer)
                                                string_format(diagnostic, "%s: %s on %s failed: %s\n", (string_address)"mount",
                                                              (live->source) ? (live->source) : (string_address)"none",
                                                              (live->target) ? (live->target) : (string_address)"none",
                                                              strerror(answer < 0 ? (b32)-(answer + 1) + 1 : (b32)answer));
                                        status = answer ? 32 : 0;
                                }
                                storage_mount_table_release(address_of table);
                        }
                }
                else
                        status = storage_mount_fstab((string_address)"mount",
                                                     operand[0], false,
                                                     address_of options, type,
                                                     fstab, diagnostic, write);
                goto done;
        }
        else
        {
                bipolar answer;
                p8 tag_path[4096];

                /* util-linux answers 32 for a mount(2) that failed, 1 for
                   an invocation it could not make sense of, such as a tag
                   no device carries. */
                if ((storage_prefix(operand[0], "UUID=") ||
                     storage_prefix(operand[0], "LABEL=") ||
                     storage_prefix(operand[0], "PARTUUID=") ||
                     storage_prefix(operand[0], "PARTLABEL=")) &&
                    !storage_resolve_tag(operand[0], tag_path, sizeof(tag_path)))
                {
                        string_format(diagnostic, "mount: %s: can't find %s.\n",
                                      operand[1], operand[0]);
                        status = 1;
                        goto done;
                }
                /*  --no-canonicalize hands the target to the kernel as it
                    was written, and the mount API does not follow a trailing
                    symlink there: the reference refuses such a target where
                    the canonical spelling would have reached the directory
                    under it. */
                if (!canonical && !options.fake)
                {
                        file_facts itself;

                        if (file_look(AT_FDCWD, operand[1], AT_SYMLINK_NOFOLLOW,
                                      address_of itself) &&
                            (itself.mode & MODE_FORMAT) == MODE_LINK)
                        {
                                string_format(diagnostic,
                                              "mount: %s on %s failed: %s\n",
                                              operand[0], operand[1],
                                              strerror(STORAGE_ERROR_INVALID));
                                status = 32;
                                goto done;
                        }
                }
                /*  A move and a remount asked for together: the reference
                    takes the remount, which leaves the move undone, and
                    calls that a subsequent operation that failed -- the
                    usage answer 1, not the 32 a refused mount(2) leaves. A
                    target that is not there at all never gets that far. */
                if ((options.flags & STORAGE_MS_MOVE) &&
                    (options.flags & STORAGE_MS_REMOUNT) && !options.fake)
                {
                        positive room = 0;
                        p8 address_to shown = canonical
                            ? storage_mount_canonical(operand[1],
                                                      address_of room)
                            : null;

                        if (shown || !canonical)
                        {
                                string_format(diagnostic,
                                              "mount: %s: filesystem was mounted,"
                                              " but any subsequent operation"
                                              " failed: Invalid argument.\n",
                                              shown ? (string_address)shown
                                                    : operand[1]);
                                if (shown)
                                        memory_free(shown, room);
                                status = 1;
                                goto done;
                        }
                }
                answer = storage_mount_one(operand[0], operand[1], type,
                                           address_of options);
                if (answer)
                        string_format(diagnostic, "%s: %s on %s failed: %s\n", (string_address)"mount",
                                      (operand[0]) ? (operand[0]) : (string_address)"none",
                                      (operand[1]) ? (operand[1]) : (string_address)"none",
                                      strerror(answer < 0 ? (b32)-(answer + 1) + 1 : (b32)answer));
                else if (options.verbose)
                {
                        /*  The reference names both words the way it
                            resolved them: the canonical path where the word
                            is one, and the word itself where it is not, so
                            `tmpfs` stays `tmpfs` while `c` becomes its
                            absolute spelling. --no-canonicalize resolves
                            neither. And it says which operation it did:
                            moved, bound, or mounted -- with a propagation
                            change reported on a line of its own after it. */
                        positive shown_room = 0;
                        positive source_room = 0;
                        p8 address_to shown = canonical
                            ? storage_mount_canonical(operand[1],
                                                      address_of shown_room)
                            : null;
                        p8 address_to source_shown = canonical
                            ? storage_mount_canonical(operand[0],
                                                      address_of source_room)
                            : null;
                        string_address target = shown ? (string_address)shown
                                                      : operand[1];
                        string_address source = source_shown
                            ? (string_address)source_shown : operand[0];

                        if (options.flags & STORAGE_MS_MOVE)
                                string_format(write, "mount: %s moved to %s.\n",
                                              source, target);
                        else if (options.flags & STORAGE_MS_BIND)
                                string_format(write, "mount: %s bound on %s.\n",
                                              source, target);
                        else
                                string_format(write, "mount: %s mounted on %s.\n",
                                              source, target);
                        if (options.propagation &&
                            !(options.flags & (STORAGE_MS_MOVE |
                                               STORAGE_MS_BIND)))
                                string_format(write,
                                              "mount: %s propagation flags changed.\n",
                                              target);
                        if (shown)
                                memory_free(shown, shown_room);
                        if (source_shown)
                                memory_free(source_shown, source_room);
                }
                status = answer ? 32 : 0;
                goto done;
        }

missing_option:
        string_format(diagnostic, "mount: option requires an argument\n");
        goto done;
no_memory:
        string_format(diagnostic, "mount: no memory\n");
done:
        storage_options_free(address_of options);
        byte_store_release(address_of tag_source);
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

/* The mount a spelling names, or null when the table has none. */
static PURE storage_mount address_to storage_umount_target(
    storage_mount_table address_to table, string_address asked)
{
        storage_mount address_to found = null;

        /* Last wins: stacked mounts are unmounted from the top. */
        for (positive at = 0; at < table->count; at++)
                if (table->entry[at].target &&
                    (storage_word(table->entry[at].source, asked) ||
                     storage_word(table->entry[at].target, asked)))
                        found = table->entry + at;
        return found;
}

/*      Every operational refusal umount(8) reports is 32; 1 is reserved for
        usage -- a spelling that was never a mount point under --recursive or
        --all-targets, which do their own table lookup and say so before any
        syscall. A plain umount of a path the table does not hold still takes
        umount(2), which is why "not mounted" and "no mount point specified"
        both leave 32 behind. */
/*      util-linux says what went wrong in its own words rather than the
        kernel's: a path that is nothing to the mount table is "not mounted",
        one the kernel refuses to let go is "target is busy", and a path the
        kernel never saw at all is "no mount point specified". */
static PURE string_address storage_umount_reason(b32 number)
{
        if (number == STORAGE_ERROR_PERMISSION)
                return (string_address)"must be superuser to unmount.";
        if (number == STORAGE_ERROR_BUSY)
                return (string_address)"target is busy.";
        if (number == STORAGE_ERROR_INVALID)
                return (string_address)"not mounted.";
        if (number == STORAGE_ERROR_NO_ENTRY)
                return (string_address)"no mount point specified.";
        return null;
}

/*      --read-only remounts what it could not unmount, and it needs the
        source to do it: the reference reads that from the mount table, so a
        spelling the table never answered for -- `--no-canonicalize` naming a
        symlink, say -- keeps the busy refusal instead of quietly turning the
        filesystem read-only under the caller. */
static b32 storage_umount_one(writer diagnostic, string_address program,
                             string_address target, string_address type,
                             string_address source,
                             positive flags, bool read_only,
                             bool verbose, bool quiet, bool fake)
{
        bipolar answer = fake ? 0
            : system_call_2(syscall(umount2), (positive)target, flags);

        if (answer && read_only && source && answer == -STORAGE_ERROR_BUSY)
        {
                storage_mount_options remount;

                memory_fill(address_of remount, 0, sizeof(remount));
                remount.flags = STORAGE_MS_REMOUNT | STORAGE_MS_RDONLY;
                remount.mentioned = STORAGE_MS_REMOUNT | STORAGE_MS_RDONLY;
                answer = storage_mount_one(source, target,
                                           null, address_of remount);
        }
        if (answer)
        {
                b32 number = answer < 0 ? (b32)-(answer + 1) + 1 :
                                         (b32)answer;
                string_address reason = storage_umount_reason(number);
                /*  --quiet is only about the path that was never a mount
                    point; every other refusal is still said. */
                if (!(quiet && number == STORAGE_ERROR_INVALID))
                {
                        if (reason)
                                string_format(diagnostic, "%s: %s: %s\n",
                                              program, target, reason);
                        else
                                string_format(diagnostic, "%s: %s failed: %s\n",
                                              program, target,
                                              strerror(number));
                }
                return 32;
        }
        if (verbose)
                string_format(diagnostic, "%s: %s (%s) unmounted\n", program,
                              target, type ? type : (string_address)"none");
        return 0;
}

#define STORAGE_MOUNT_OPEN_NOFOLLOW 0400000

static b32 storage_umount_recursive(writer diagnostic, string_address program,
                                    storage_mount_table address_to table,
                                    string_address root, string_address shown,
                                    string_address types,
                                    positive flags, bool read_only, bool verbose,
                                    bool quiet, bool fake)
{
        b32 failed = 0;
        positive longest = positive_max;
        bool found = false;

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
                                found = true;
                                failed |= storage_umount_one(diagnostic, program,
                                                             record->target,
                                                             record->type,
                                                             record->source, flags,
                                                             read_only, verbose,
                                                             quiet, fake);
                                record->target = null;
                        }
                }
        }
        if (!found)
        {
                /*  The recursive walk says "not mounted" without the stop
                    the single form puts after it, and --quiet is about this
                    sentence as it is about the other. */
                if (!quiet)
                        string_format(diagnostic, "%s: %s: not mounted\n",
                                      program, shown);
                return 1;
        }
        return failed;
}

b32 storage_umount_command(positive argc, string_address address_to argv,
                           writer write, writer diagnostic)
{
        static const storage_argument_name arguments[] = {
            STORAGE_ARGUMENT("all", 'a'),
            STORAGE_ARGUMENT("lazy", 'l'),
            STORAGE_ARGUMENT("force", 'f'),
            STORAGE_ARGUMENT("recursive", 'R'),
            STORAGE_ARGUMENT("read-only", 'r'),
            STORAGE_ARGUMENT("types", 't'),
            STORAGE_ARGUMENT("verbose", 'v'),
            STORAGE_ARGUMENT("no-mtab", 'n'),
            STORAGE_ARGUMENT("no-canonicalize", 'c'),
            STORAGE_ARGUMENT("internal-only", 'i'),
            STORAGE_ARGUMENT("all-targets", 'A'),
            STORAGE_ARGUMENT("quiet", 'q'),
            STORAGE_ARGUMENT("detach-loop", 'd'),
            STORAGE_ARGUMENT("test-opts", 'O'),
            STORAGE_ARGUMENT("fake", 'F'),
        };
        /*  umount(8) takes no UMOUNT_NOFOLLOW: only its set-user-id path
            does, and then it chdirs to the parent first. So a symlink named
            to umount(2) is followed by the kernel, which is how
            `umount --no-canonicalize clink` reaches the mount under it. */
        positive flags = 0;
        string_address types = null;
        string_address address_to operand = null;
        positive operand_room = 0;
        positive operands = 0;
        bool all = false;
        bool all_targets = false;
        bool recursive = false;
        bool read_only = false;
        bool verbose = false;
        bool canonical = true;
        bool quiet = false;
        bool loop_detach = false;
        bool fake = false;
        argument_cursor taking = {.argc = argc, .argv = argv, .at = 1};
        string_address value;
        b32 option;
        storage_mount_table table;
        bool loaded;
        b32 failed = 0;

        (void)write;
        while ((option = storage_argument_next(
                    address_of taking, (string_address)"alfRrvncitAqdO",
                    (string_address)"tO", arguments, array_count(arguments),
                    address_of value)) != ARGUMENT_END)
        {
                if (option == ARGUMENT_OPERAND)
                {
                        if (!array_store_reserve(operand, operand_room,
                                                 operands, operands + 1, 16))
                        {
                                string_format(diagnostic,
                                              "umount: no memory\n");
                                goto failed_early;
                        }
                        operand[operands++] = value;
                }
                else if (option == ARGUMENT_MISSING)
                        goto missing_option;
                else if (option == ARGUMENT_UNKNOWN)
                {
                        if (taking.letters)
                        {
                                p8 letter[2] = {(p8)taking.letters[-1], end};
                                string_format(diagnostic,
                                              "umount: invalid option -- '%s'\n"
                                              "Try 'umount --help' for more information.\n",
                                              letter);
                        }
                        else
                                string_format(diagnostic,
                                              "umount: unrecognized option '%s'\n"
                                              "Try 'umount --help' for more information.\n",
                                              taking.word);
                        goto failed_early;
                }
                else if (option == 'a')
                        all = true;
                else if (option == 'A')
                        all_targets = true;
                else if (option == 'q')
                        quiet = true;
                else if (option == 'F')
                        fake = true;
                else if (option == 'd')
                        loop_detach = true;
                else if (option == 'O' || option == 'n')
                        ; /* Every mount matches an empty option filter, and
                             there is no mtab to leave alone. */
                else if (option == 'l')
                        flags |= STORAGE_MNT_DETACH;
                else if (option == 'f')
                        flags |= STORAGE_MNT_FORCE;
                else if (option == 'R')
                        recursive = true;
                else if (option == 'r')
                        read_only = true;
                else if (option == 'v')
                        verbose = true;
                else if (option == 'c')
                        canonical = false;
                else if (option == 't')
                        types = value;
        }

        {
                static const storage_exclusive_pair reach[] = {
                    {'a', (string_address)"all"},
                    {'A', (string_address)"all-targets"}};
                static const storage_exclusive_pair depth[] = {
                    {'r', (string_address)"read-only"},
                    {'R', (string_address)"recursive"}};

                if (storage_exclusive_refuse(
                        diagnostic, (string_address)"umount", argc, argv,
                        arguments, array_count(arguments),
                        (string_address)"tO", reach, array_count(reach)) ||
                    storage_exclusive_refuse(
                        diagnostic, (string_address)"umount", argc, argv,
                        arguments, array_count(arguments),
                        (string_address)"tO", depth, array_count(depth)))
                        goto failed_early;
        }

        if (!all && !operands)
        {
                string_format(diagnostic, "umount: missing operand\n");
                goto failed_early;
        }

        if (recursive && types)
        {
                string_format(diagnostic,
                              "umount: options --recursive and --types cannot be combined\n");
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
                                    record->target, record->type,
                                    record->source, flags,
                                    read_only, verbose, quiet, fake);
                }
        }

        for (positive i = 0; i < operands; i++)
        {
                /* util-linux looks a spelling up by its canonical path, so a
                   relative or symlinked target names the same mount. */
                positive resolved_room = 0;
                p8 address_to resolved = null;
                {
                        /* The table holds absolute targets, so a relative
                           word is made absolute even under
                           --no-canonicalize; that option says not to follow
                           the last symlink, not to leave the spelling as it
                           was typed. */
                        bipolar handle = system_open_at(
                            AT_FDCWD, operand[i],
                            STORAGE_OPEN_PATH | O_CLOEXEC |
                                (canonical ? 0 : STORAGE_MOUNT_OPEN_NOFOLLOW));
                        if (handle >= 0)
                        {
                                resolved = storage_fd_path(handle,
                                                           address_of resolved_room);
                                system_close(handle);
                        }
                }
                string_address asked = resolved ? (string_address)resolved
                                                : operand[i];
                b32 answer;
                if (all_targets)
                {
                        /*  --all-targets names one filesystem, by any of its
                            mount points or by its source, and unmounts every
                            mount of that same device -- the newest record
                            first, stopping at the first refusal. A spelling
                            the table does not hold never reaches umount(2),
                            so it is the usage answer 1 and its sentence ends
                            without a stop. */
                        storage_mount address_to chosen =
                            storage_umount_target(address_of table, asked);

                        if (!chosen && asked != operand[i])
                                chosen = storage_umount_target(
                                    address_of table, operand[i]);
                        if (!chosen)
                        {
                                if (!quiet)
                                        string_format(
                                            diagnostic, "umount: %s: %s\n",
                                            operand[i],
                                            resolved ? "not mounted"
                                                     : "not found");
                                answer = 1;
                        }
                        else
                        {
                                string_address device = chosen->device;

                                answer = 0;
                                for (positive at = table.count; at && !answer;
                                     at--)
                                {
                                        storage_mount address_to record =
                                            table.entry + at - 1;

                                        if (!record->target ||
                                            !storage_word(record->device,
                                                          device))
                                                continue;
                                        if (recursive)
                                                answer = storage_umount_recursive(
                                                    diagnostic,
                                                    (string_address)"umount",
                                                    address_of table,
                                                    record->target,
                                                    record->target, types,
                                                    flags, read_only, verbose,
                                                    quiet, fake);
                                        else
                                        {
                                                answer = storage_umount_one(
                                                    diagnostic,
                                                    (string_address)"umount",
                                                    record->target,
                                                    record->type,
                                                    record->source, flags,
                                                    read_only, verbose, quiet,
                                                    fake);
                                                record->target = null;
                                        }
                                }
                        }
                }
                else
                {
                        storage_mount address_to found =
                            storage_umount_target(address_of table, asked);
                        if (!found && asked != operand[i])
                                found = storage_umount_target(address_of table,
                                                              operand[i]);
                        /*  A path the table does not hold is named to the
                            kernel as it was written; one it holds is named by
                            the table's own spelling, which is how the
                            reference names it. */
                        string_address target = found ? found->target
                                                      : operand[i];
                        if (recursive)
                                answer = storage_umount_recursive(
                                    diagnostic, (string_address)"umount",
                                    address_of table,
                                    found ? found->target : asked, operand[i],
                                    types, flags, read_only, verbose, quiet,
                                    fake);
                        else
                        {
                                answer = storage_umount_one(
                                    diagnostic, (string_address)"umount", target,
                                    found ? found->type : null,
                                    found ? found->source : null, flags, read_only,
                                    verbose, quiet, fake);
                                /*  --detach-loop frees the loop device the
                                    source names once the filesystem is gone,
                                    and it needs a source to name one. Nothing
                                    here owns a loop device, so the step is a
                                    no-op -- but where the table never answered
                                    for this target there is no source at all,
                                    and the reference calls that a failure
                                    after a successful unmount. */
                                if (!answer && loop_detach && !found && !fake)
                                {
                                        string_format(
                                            diagnostic,
                                            "umount: %s: filesystem was unmounted,"
                                            " but any subsequent operation failed:"
                                            " Invalid argument.\n",
                                            target);
                                        answer = 1;
                                }
                        }
                }
                /*  Every operand contributes its own answer: two refusals of
                    32 are 64, not 32, and only a total past 255 saturates. */
                failed = failed + answer > 255 ? 255 : failed + answer;
                if (resolved)
                        memory_free(resolved, resolved_room);
        }

        storage_mount_table_release(address_of table);
done_early:
        array_store_release(operand, operand_room, operands);
        return failed;

missing_option:
        string_format(diagnostic, "umount: option requires an argument\n");
failed_early:
        array_store_release(operand, operand_room, operands);
        return 1;
}

#undef STORAGE_ARGUMENT
