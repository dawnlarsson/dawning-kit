#include "../src/sh/file.c"

/*
        find [PATH...] [-maxdepth N] [-mindepth N] [-name PATTERN]
             [-type C] [-size N] [-empty] [-print]

        Every test given has to hold, which is the and that find's expression
        language spells by juxtaposition. There is no -exec: running a command
        is the shell's job, and everything here can be piped into one.
*/
#define FIND_TESTS_MAX 32

typedef struct
{
        p8 kind;
        string_address text;
        b64 number;
        p8 unit;
        p8 comparison;
} find_test;

static find_test find_tests[FIND_TESTS_MAX];
static positive find_have;
static positive find_maximum = FILE_MAX_DEPTH;
static positive find_minimum;
static b32 find_status;

static bool find_size_holds(find_test address_to test, file_facts address_to facts)
{
        p64 divisor = 512;

        if (test->unit == 'c')
                divisor = 1;
        else if (test->unit == 'k')
                divisor = 1024;
        else if (test->unit == 'M')
                divisor = 1024 * 1024;
        else if (test->unit == 'G')
                divisor = 1024 * 1024 * 1024;

        // find rounds up: a file of one byte is one block, and "-size 1" is
        // meant to find it.
        p64 units = (facts->size + divisor - 1) / divisor;

        if (test->comparison == '+')
                return units > (p64)test->number;

        if (test->comparison == '-')
                return units < (p64)test->number;

        return units == (p64)test->number;
}

static bool find_type_holds(p8 wanted, positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (wanted == 'f')
                return kind == MODE_FILE;

        if (wanted == 'd')
                return kind == MODE_DIRECTORY;

        if (wanted == 'l')
                return kind == MODE_LINK;

        if (wanted == 'b')
                return kind == MODE_BLOCK;

        if (wanted == 'c')
                return kind == MODE_CHARACTER;

        if (wanted == 'p')
                return kind == MODE_PIPE;

        if (wanted == 's')
                return kind == MODE_SOCKET;

        return false;
}

static bool find_empty(string_address path, file_facts address_to facts)
{
        if ((facts->mode & MODE_FORMAT) != MODE_DIRECTORY)
                return facts->size == 0;

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
                return false;

        struct linux_dirent64 address_to entry;
        bool empty = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                empty = false;
                break;
        }

        file_walk_close(address_of walk);

        return empty;
}

static bool find_holds(string_address path, string_address name, file_facts address_to facts,
                       positive depth)
{
        if (depth < find_minimum)
                return false;

        for (positive i = 0; i < find_have; i++)
        {
                find_test address_to test = address_of find_tests[i];

                if (test->kind == 'n' && !file_match(test->text, name))
                        return false;

                if (test->kind == 'p' && !file_match(test->text, path))
                        return false;

                if (test->kind == 't' && !find_type_holds((p8)test->number, facts->mode))
                        return false;

                if (test->kind == 's' && !find_size_holds(test, facts))
                        return false;

                if (test->kind == 'e' && !find_empty(path, facts))
                        return false;

                if (test->kind == 'm' && (facts->mode & 07777) != (positive)test->number)
                        return false;
        }

        return true;
}

static fn find_walk(string_address path, string_address name, positive depth)
{
        file_facts facts;

        if (!file_look_link(path, address_of facts))
        {
                string_format(file_fail, "find: '%s': No such file or directory\n", path);
                find_status = 1;
                return;
        }

        if (find_holds(path, name, address_of facts, depth))
                file_line(path);

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY || depth >= find_maximum)
                return;

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(file_fail, "find: '%s': Permission denied\n", path);
                find_status = 1;
                return;
        }

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];
                p8 held[FILE_NAME_MAX];

                string_copy_max(held, entry->d_name, FILE_NAME_MAX - 1);
                held[FILE_NAME_MAX - 1] = end;

                file_join(below, FILE_PATH_MAX, path, held);

                find_walk(below, held, depth + 1);
        }

        file_walk_close(address_of walk);
}

b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        positive roots_first = 1;
        positive roots_last = 1;

        while (index < count && !string_is(program_argument((b32)index), '-'))
                index++;

        roots_last = index;

        while (index < count)
        {
                string_address word = program_argument((b32)index);
                string_address value = index + 1 < count ? program_argument((b32)(index + 1)) : null;

                if (string_compare(word, (string_address) "-print") == 0 ||
                    string_compare(word, (string_address) "-print0") == 0)
                {
                        index++;
                        continue;
                }

                if (string_compare(word, (string_address) "-empty") == 0)
                {
                        if (find_have < FIND_TESTS_MAX)
                                find_tests[find_have++].kind = 'e';

                        index++;
                        continue;
                }

                if (!value)
                {
                        string_format(file_fail, "find: %s needs a value\n", word);
                        return 1;
                }

                if (string_compare(word, (string_address) "-maxdepth") == 0)
                {
                        find_maximum = file_count(value);
                        index += 2;
                        continue;
                }

                if (string_compare(word, (string_address) "-mindepth") == 0)
                {
                        find_minimum = file_count(value);
                        index += 2;
                        continue;
                }

                if (find_have >= FIND_TESTS_MAX)
                {
                        file_fail("find: too many tests\n", 0);
                        return 1;
                }

                find_test address_to test = address_of find_tests[find_have];

                if (string_compare(word, (string_address) "-name") == 0)
                {
                        test->kind = 'n';
                        test->text = value;
                }
                else if (string_compare(word, (string_address) "-path") == 0 ||
                         string_compare(word, (string_address) "-wholename") == 0)
                {
                        test->kind = 'p';
                        test->text = value;
                }
                else if (string_compare(word, (string_address) "-type") == 0)
                {
                        test->kind = 't';
                        test->number = string_get(value);
                }
                else if (string_compare(word, (string_address) "-perm") == 0)
                {
                        positive mode = 0;

                        if (!file_mode_of(value, 0, false, address_of mode))
                        {
                                string_format(file_fail, "find: invalid mode %s\n", value);
                                return 1;
                        }

                        test->kind = 'm';
                        test->number = (b64)mode;
                }
                else if (string_compare(word, (string_address) "-size") == 0)
                {
                        string_address step = value;

                        test->kind = 's';
                        test->comparison = ' ';

                        if (string_is(step, '+') || string_is(step, '-'))
                        {
                                test->comparison = string_get(step);
                                step++;
                        }

                        test->number = (b64)file_count(step);

                        while (string_get(step) >= '0' && string_get(step) <= '9')
                                step++;

                        test->unit = string_get(step) ? string_get(step) : 'b';
                }
                else
                {
                        string_format(file_fail, "find: unknown test: %s\n", word);
                        return 1;
                }

                find_have++;
                index += 2;
        }

        if (roots_last == roots_first)
        {
                find_walk((string_address) ".", (string_address) ".", 0);
                log_flush();
                return find_status;
        }

        for (positive i = roots_first; i < roots_last; i++)
        {
                string_address root = program_argument((b32)i);
                p8 name[FILE_PATH_MAX];

                file_tail(root, name);
                find_walk(root, name, 0);
        }

        log_flush();

        return find_status;
}
