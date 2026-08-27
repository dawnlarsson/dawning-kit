#include "../src/sh/file.c"

/*
        ls [-laARtShr1din]

        There is an ls builtin in the shell as well. This is the one with the
        flags, and the builtin should call it rather than grow a second copy:
        a listing is not the shell's business, and a program can be replaced
        on its own.

        Output is one name per line. The system's ls does the same the moment
        it is not writing to a terminal, which is every case a script cares
        about, and columns are a terminal's problem rather than a listing's.

        Times are UTC. Nothing in this tree reads /usr/share/zoneinfo, and a
        listing that quietly used the wrong zone would be worse than one that
        says which zone it used.
*/
#define LS_MAX_ENTRIES 8192
#define LS_ARENA (1 << 20)

typedef struct
{
        positive name;
        positive mode;
        positive links;
        positive owner;
        positive group;
        p64 size;
        b64 modified;
        p32 modified_fraction;
        p64 inode;
        p64 blocks;
        bool known;
} ls_entry;

static ls_entry ls_entries[LS_MAX_ENTRIES];
static positive ls_count;
static p8 ls_arena[LS_ARENA];
static positive ls_used;

static bool ls_long;
static bool ls_hidden;
static bool ls_almost;
static bool ls_recursive;
static bool ls_by_time;
static bool ls_by_size;
static bool ls_human;
static bool ls_reversed;
static bool ls_inode;
static bool ls_numeric;
static bool ls_as_itself;
static bool ls_headings;

static b32 ls_status;
static bool ls_written;
static b64 ls_now;

static positive ls_keep(string_address name)
{
        positive length = string_length(name);

        if (ls_used + length + 1 > LS_ARENA)
                return 0;

        positive at = ls_used;

        for (positive i = 0; i <= length; i++)
                ls_arena[at + i] = string_get(name + i);

        ls_used += length + 1;

        return at;
}

static bipolar ls_order(ls_entry address_to left, ls_entry address_to right)
{
        if (ls_by_time)
        {
                if (left->modified != right->modified)
                        return left->modified > right->modified ? -1 : 1;

                // Two files written in the same second are not the same age,
                // and sorting them by name instead puts them in the wrong
                // order rather than an arbitrary one.
                if (left->modified_fraction != right->modified_fraction)
                        return left->modified_fraction > right->modified_fraction ? -1 : 1;
        }
        else if (ls_by_size)
        {
                if (left->size != right->size)
                        return left->size > right->size ? -1 : 1;
        }

        string_address a = ls_arena + left->name;
        string_address b = ls_arena + right->name;

        while (string_get(a) && string_get(a) == string_get(b))
        {
                a++;
                b++;
        }

        if (string_get(a) == string_get(b))
                return 0;

        return string_get(a) < string_get(b) ? -1 : 1;
}

// Shell sort with Ciura's gaps: no recursion, no second array, and a
// directory of any size this program will hold sorts in well under the
// quadratic time an insertion sort would take on it.
static fn ls_sort()
{
        positive gaps[8] = {701, 301, 132, 57, 23, 10, 4, 1};

        for (positive g = 0; g < 8; g++)
        {
                positive gap = gaps[g];

                for (positive i = gap; i < ls_count; i++)
                {
                        ls_entry held = ls_entries[i];
                        positive j = i;

                        while (j >= gap && ls_order(address_of ls_entries[j - gap],
                                                    address_of held) > 0)
                        {
                                ls_entries[j] = ls_entries[j - gap];
                                j -= gap;
                        }

                        ls_entries[j] = held;
                }
        }
}

static fn ls_size_field(p64 value)
{
        if (ls_human)
                return file_human(log, value);

        file_number(log, value);
}

static positive ls_width_of(p64 value)
{
        p8 text[24];

        return file_digits(text, value);
}

static positive ls_human_width(p64 value)
{
        if (!ls_human)
                return ls_width_of(value);

        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 7)
        {
                divisor *= 1024;
                unit++;
        }

        if (unit == 0)
                return ls_width_of(value);

        positive whole = (value + divisor - 1) / divisor;

        if (whole >= 10)
                return ls_width_of(whole) + 1;

        return 3 + 1;
}

static fn ls_owner_text(positive id, bool group, p8 address_to into)
{
        if (!ls_numeric)
        {
                bool known = group ? file_group_name(id, into, FILE_NAME_MAX)
                                   : file_user_name(id, into, FILE_NAME_MAX);

                if (known)
                        return;
        }

        file_digits(into, id);
}

static fn ls_print(string_address directory)
{
        positive link_width = 1;
        positive size_width = 1;
        positive owner_width = 1;
        positive group_width = 1;
        positive inode_width = 1;
        p64 blocks = 0;

        for (positive i = 0; i < ls_count; i++)
        {
                ls_entry address_to entry = address_of ls_entries[i];

                if (ls_inode && ls_width_of(entry->inode) > inode_width)
                        inode_width = ls_width_of(entry->inode);

                if (!ls_long)
                        continue;

                if (ls_width_of(entry->links) > link_width)
                        link_width = ls_width_of(entry->links);

                if (ls_human_width(entry->size) > size_width)
                        size_width = ls_human_width(entry->size);

                p8 name[FILE_NAME_MAX];

                ls_owner_text(entry->owner, false, name);

                if (string_length(name) > owner_width)
                        owner_width = string_length(name);

                ls_owner_text(entry->group, true, name);

                if (string_length(name) > group_width)
                        group_width = string_length(name);
        }

        if (ls_long)
        {
                for (positive i = 0; i < ls_count; i++)
                        blocks += ls_entries[i].blocks;

                // The kernel counts in 512 byte blocks and ls has always
                // reported in 1024 byte ones.
                blocks /= 2;

                log("total ", 0);

                if (ls_human)
                        file_human(log, blocks * 1024);
                else
                        file_number(log, blocks);

                log("\n", 1);
        }

        for (positive k = 0; k < ls_count; k++)
        {
                positive i = ls_reversed ? ls_count - 1 - k : k;
                ls_entry address_to entry = address_of ls_entries[i];
                string_address name = ls_arena + entry->name;

                if (ls_inode)
                {
                        file_number_padded(log, entry->inode, inode_width);
                        log(" ", 1);
                }

                if (ls_long)
                {
                        p8 letters[12];
                        p8 who[FILE_NAME_MAX];

                        file_mode_letters(letters, entry->mode);
                        log(letters, 10);
                        log(" ", 1);
                        file_number_padded(log, entry->links, link_width);
                        log(" ", 1);

                        ls_owner_text(entry->owner, false, who);
                        file_text_padded(log, who, owner_width);
                        log(" ", 1);

                        ls_owner_text(entry->group, true, who);
                        file_text_padded(log, who, group_width);
                        log(" ", 1);

                        for (positive pad = ls_human_width(entry->size); pad < size_width; pad++)
                                log(" ", 1);

                        ls_size_field(entry->size);
                        log(" ", 1);
                        file_stamp_short(log, entry->modified, ls_now);
                        log(" ", 1);
                }

                log(name, 0);

                if (ls_long && (entry->mode & MODE_FORMAT) == MODE_LINK)
                {
                        p8 where[FILE_PATH_MAX];
                        p8 full[FILE_PATH_MAX];

                        if (directory)
                                file_join(full, FILE_PATH_MAX, directory, name);
                        else
                                string_copy_max(full, name, FILE_PATH_MAX - 1);

                        if (file_link_text(full, where, FILE_PATH_MAX) >= 0)
                        {
                                log(" -> ", 0);
                                log(where, 0);
                        }
                }

                log("\n", 1);
        }
}

static fn ls_add(bipolar directory, string_address path, string_address shown)
{
        if (ls_count >= LS_MAX_ENTRIES)
                return;

        file_facts facts;
        ls_entry address_to entry = address_of ls_entries[ls_count];

        memory_fill(entry, 0, sizeof(ls_entry));
        entry->name = ls_keep(shown);

        if (file_look(directory, path, AT_SYMLINK_NOFOLLOW, address_of facts))
        {
                entry->known = true;
                entry->mode = facts.mode;
                entry->links = facts.hard_links;
                entry->owner = facts.owner;
                entry->group = facts.group;
                entry->size = facts.size;
                entry->modified = facts.modified.seconds;
                entry->modified_fraction = facts.modified.nanoseconds;
                entry->inode = facts.inode;
                entry->blocks = facts.blocks;
        }

        ls_count++;
}

static fn ls_directory(string_address path, bool heading, positive depth);

static fn ls_below(string_address path, positive depth)
{
        // The names of the subdirectories are taken out of the listing before
        // descending, because the listing buffers are about to be filled with
        // whatever is inside the first of them.
        p8 keep[LS_ARENA / 8];
        positive kept = 0;
        positive found = 0;

        for (positive k = 0; k < ls_count; k++)
        {
                positive i = ls_reversed ? ls_count - 1 - k : k;
                ls_entry address_to entry = address_of ls_entries[i];

                if ((entry->mode & MODE_FORMAT) != MODE_DIRECTORY)
                        continue;

                string_address name = ls_arena + entry->name;

                if (file_is_dot(name))
                        continue;

                positive length = string_length(name);

                if (kept + length + 1 > sizeof(keep))
                        break;

                for (positive j = 0; j <= length; j++)
                        keep[kept + j] = string_get(name + j);

                kept += length + 1;
                found++;
        }

        positive at = 0;

        for (positive i = 0; i < found; i++)
        {
                p8 below[FILE_PATH_MAX];

                file_join(below, FILE_PATH_MAX, path, keep + at);
                at += string_length(keep + at) + 1;

                ls_directory(below, true, depth - 1);
        }
}

static fn ls_directory(string_address path, bool heading, positive depth)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(file_fail, "ls: cannot open directory '%s': No such file or directory\n",
                              path);
                ls_status = 1;
                return;
        }

        ls_count = 0;
        ls_used = 0;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (entry->d_name[0] == '.' && !ls_hidden && !ls_almost)
                        continue;

                if (ls_almost && file_is_dot(entry->d_name))
                        continue;

                ls_add(walk.handle, entry->d_name, entry->d_name);
        }

        file_walk_close(address_of walk);

        ls_sort();

        if (heading)
        {
                if (ls_written)
                        log("\n", 1);

                log(path, 0);
                log(":\n", 0);
        }

        ls_written = true;

        ls_print(path);

        // Each level of -R holds a listing and a block of names on the stack,
        // so a tree that links into itself stops here rather than by running
        // out of stack.
        if (ls_recursive && depth > 0)
                ls_below(path, depth);
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "laARtShr1dinF", address_of first);

        ls_now = file_now();

        ls_long = (flags & FILE_FLAG('l')) != 0 || (flags & FILE_FLAG('n')) != 0;
        ls_hidden = (flags & FILE_FLAG('a')) != 0;
        ls_almost = (flags & FILE_FLAG('A')) != 0;
        ls_recursive = (flags & FILE_FLAG('R')) != 0;
        ls_by_time = (flags & FILE_FLAG('t')) != 0;
        ls_by_size = (flags & FILE_FLAG('S')) != 0;
        ls_human = (flags & FILE_FLAG('h')) != 0;
        ls_reversed = (flags & FILE_FLAG('r')) != 0;
        ls_inode = (flags & FILE_FLAG('i')) != 0;
        ls_numeric = (flags & FILE_FLAG('n')) != 0;
        ls_as_itself = (flags & FILE_FLAG('d')) != 0;

        if (first >= count)
        {
                ls_directory((string_address) ".", false, FILE_MAX_DEPTH);
                log_flush();
                return ls_status;
        }

        positive given = count - first;

        if (ls_as_itself)
        {
                ls_count = 0;
                ls_used = 0;

                for (positive i = first; i < count; i++)
                        ls_add(AT_FDCWD, program_argument((b32)i), program_argument((b32)i));

                ls_sort();
                ls_print(null);
                log_flush();

                return ls_status;
        }

        // Everything that is not a directory is listed first, together, and
        // then each directory in turn -- which is the order the system's own
        // ls uses and the only one where a mixed set of operands reads.
        ls_count = 0;
        ls_used = 0;

        positive directories = 0;

        for (positive i = first; i < count; i++)
        {
                string_address path = program_argument((b32)i);

                if (!file_exists(AT_FDCWD, path))
                {
                        string_format(file_fail,
                                      "ls: cannot access '%s': No such file or directory\n",
                                      path);
                        ls_status = 1;
                        continue;
                }

                if (file_is_directory_through(path))
                {
                        directories++;
                        continue;
                }

                ls_add(AT_FDCWD, path, path);
        }

        ls_headings = given > 1 || ls_recursive;

        if (ls_count > 0)
        {
                ls_sort();
                ls_print(null);
                ls_written = true;
        }

        // The directory operands are listed in name order however they were
        // typed, which is what ls does with every other list of names.
        positive order[LS_MAX_ENTRIES];
        positive have = 0;

        for (positive i = first; i < count && have < LS_MAX_ENTRIES; i++)
        {
                string_address path = program_argument((b32)i);

                if (!file_exists(AT_FDCWD, path) || !file_is_directory_through(path))
                        continue;

                order[have++] = i;
        }

        for (positive i = 1; i < have; i++)
        {
                positive held = order[i];
                positive j = i;

                while (j > 0 &&
                       string_compare(program_argument((b32)order[j - 1]),
                                      program_argument((b32)held)) > 0)
                {
                        order[j] = order[j - 1];
                        j--;
                }

                order[j] = held;
        }

        for (positive i = 0; i < have; i++)
                ls_directory(program_argument((b32)order[i]), ls_headings,
                             FILE_MAX_DEPTH);

        log_flush();

        return ls_status;
}
