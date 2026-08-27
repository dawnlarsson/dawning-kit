#include "../src/sh/file.c"

/*
        env [-i] [-u NAME] [NAME=VALUE]... [COMMAND [ARGUMENT]...]

        With no command it prints the environment it would have used, which is
        also the only way anything here can look at its own environment.
*/
#define ENV_MAX 512
#define ENV_ARGUMENTS_MAX 64

static string_address env_list[ENV_MAX + 1];
static positive env_have;

static string_address env_key_end(string_address entry)
{
        return string_first_of(entry, '=');
}

static bool env_same_key(string_address entry, string_address name, positive length)
{
        string_address mark = env_key_end(entry);

        if (!mark || (positive)(mark - entry) != length)
                return false;

        for (positive i = 0; i < length; i++)
                if (string_get(entry + i) != string_get(name + i))
                        return false;

        return true;
}

static fn env_drop(string_address name)
{
        positive length = string_length(name);
        positive keep = 0;

        for (positive i = 0; i < env_have; i++)
                if (!env_same_key(env_list[i], name, length))
                        env_list[keep++] = env_list[i];

        env_have = keep;
}

static fn env_put(string_address entry)
{
        string_address mark = env_key_end(entry);

        if (mark)
        {
                positive length = (positive)(mark - entry);

                for (positive i = 0; i < env_have; i++)
                {
                        if (env_same_key(env_list[i], entry, length))
                        {
                                env_list[i] = entry;
                                return;
                        }
                }
        }

        if (env_have < ENV_MAX)
                env_list[env_have++] = entry;
}

b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool empty = false;

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, '-') &&
                    string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                if (string_is(argument, '-') && string_is(argument + 1, 'i') &&
                    string_is(argument + 2, end))
                {
                        empty = true;
                        index++;
                        continue;
                }

                if (string_is(argument, '-') && string_is(argument + 1, end))
                {
                        empty = true;
                        index++;
                        continue;
                }

                break;
        }

        if (!empty)
        {
                for (b32 i = 0; program_environment(i) && env_have < ENV_MAX; i++)
                        env_list[env_have++] = program_environment(i);
        }

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, 'u') &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        env_drop(program_argument((b32)(index + 1)));
                        index += 2;
                        continue;
                }

                if (!env_key_end(argument))
                        break;

                env_put(argument);
                index++;
        }

        env_list[env_have] = null;

        if (index >= count)
        {
                for (positive i = 0; i < env_have; i++)
                        file_line(env_list[i]);

                log_flush();
                return 0;
        }

        string_address arguments[ENV_ARGUMENTS_MAX + 1];
        positive have = 0;

        while (index < count && have < ENV_ARGUMENTS_MAX)
                arguments[have++] = program_argument((b32)index++);

        arguments[have] = null;

        log_flush();

        p8 candidate[FILE_PATH_MAX];
        string_address name = arguments[0];

        if (string_first_of(name, '/'))
        {
                system_call_3(syscall(execve), (positive)name, (positive)arguments,
                              (positive)env_list);
        }
        else
        {
                // PATH from the environment being handed on, not from the one
                // this program was started with: env -i changes both.
                string_address path = null;

                for (positive i = 0; i < env_have; i++)
                {
                        if (env_same_key(env_list[i], (string_address) "PATH", 4))
                        {
                                path = env_list[i] + 5;
                                break;
                        }
                }

                if (!path)
                        path = (string_address) "/bin:/usr/bin:/";

                while (string_get(path))
                {
                        positive length = 0;

                        while (string_get(path + length) && !string_is(path + length, ':'))
                                length++;

                        positive filled = 0;

                        for (positive i = 0; i < length && filled + 1 < FILE_PATH_MAX; i++)
                                candidate[filled++] = string_get(path + i);

                        if (filled == 0)
                                candidate[filled++] = '.';

                        if (candidate[filled - 1] != '/')
                                candidate[filled++] = '/';

                        for (positive i = 0; string_get(name + i) && filled + 1 < FILE_PATH_MAX; i++)
                                candidate[filled++] = string_get(name + i);

                        candidate[filled] = end;

                        system_call_3(syscall(execve), (positive)candidate,
                                      (positive)arguments, (positive)env_list);

                        path += length;

                        if (string_is(path, ':'))
                                path++;
                }
        }

        string_format(file_fail, "env: '%s': No such file or directory\n", name);

        return 127;
}
