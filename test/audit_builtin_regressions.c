/* Pure-state regressions for the verified enable/formatter/signal repairs.
   No machine-control builtin or utility is invoked. */
#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"
#include "counted.inc"

_Static_assert(SIGSTOP == 19, "Linux SIGSTOP must match the syscall ABI");
static p8 captured[8192];
static positive captured_used;
static fn capture(address_any data, positive length)
{
        if (!length) length = string_length(data);
        if (length < sizeof(captured) - captured_used)
        {
                memory_copy(captured + captured_used, data, length);
                captured_used += length;
                captured[captured_used] = 0;
        }
}
static fn enable_name(string_address name, bool off)
{
        shell_argc = off ? 3 : 2;
        shell_argv[0] = "enable";
        shell_argv[1] = off ? (string_address)"-n" : name;
        shell_argv[2] = name;
        shell_enable(capture, null);
}
b32 main(void)
{
        string_address arguments[8] = {0};
        shell_argv = arguments;
        for (positive cycle = 0; cycle < 1024; cycle++)
        {
                enable_name("echo", true);
                check("unbounded repeat disable", !shell_status && shell_builtin_disabled("echo"));
                enable_name("echo", false);
                check("unbounded repeat enable", !shell_status && !shell_disabled_count);
        }
        for (positive i = 0; i < SHELL_COMMAND_COUNT; i++)
        {
                enable_name(shell_commands[i].name, true);
                enable_name(shell_commands[i].name, true);
                check("all registry identities disable idempotently",
                      !shell_status && shell_disabled_count == i + 1 &&
                      !shell_command_named_hashed(shell_commands[i].name,
                          string_hash_33_length(shell_commands[i].name)));
        }
        for (positive i = 0; i < SHELL_COMMAND_COUNT; i++)
        {
                enable_name(shell_commands[i].name, false);
                enable_name(shell_commands[i].name, false);
                check("all registry identities enable idempotently",
                      !shell_status && shell_disabled_count == SHELL_COMMAND_COUNT - i - 1 &&
                      shell_command_named_hashed(shell_commands[i].name,
                          string_hash_33_length(shell_commands[i].name)) == shell_commands + i);
        }
        enable_name("complete", true);
        check("same-handler aliases retain independent state",
              shell_builtin_disabled("complete") && !shell_builtin_disabled("bind") &&
              !shell_builtin_disabled("true") && !shell_builtin_disabled(":"));
        for (positive mode = 0; mode < 4; mode++)
        {
                shell_argc = mode ? 2 : 1;
                shell_argv[0] = "enable";
                shell_argv[1] = mode == 1 ? "-n" : mode == 2 ? "-p" : "-a";
                captured_used = 0; captured[0] = 0;
                shell_enable(capture, null);
                check("listing selects disabled state", !!string_search(captured, "enable -n complete\n") == (mode == 1 || mode == 3));
                check("listing selects enabled state", !!string_search(captured, "enable bind\n") == (mode != 1));
        }
        enable_name("complete", false);
        const string_address names[] = {"bind", "complete", "compopt"};
        for (positive i = 0; i < array_count(names); i++)
        {
                shell_argc = 1; shell_argv[0] = names[i];
                shell_command address_to command = shell_command_named_hashed(
                    names[i], string_hash_33_length(names[i]));
                command->function(capture, null);
                check("constant-status handlers", shell_status == (i == 2));
        }
        static const struct {positive bytes; string_address text;} sizes[] = {
            {0, "0 B"}, {1, "1 B"}, {1024, "1 KiB"}, {8192, "8 KiB"},
            {1536, "1.5 KiB"}, {1048576, "1 MiB"}, {1073741824, "1 GiB"},
        };
        for (positive i = 0; i < array_count(sizes); i++)
        {
                p8 guarded[50]; memory_fill(guarded, 0xa5, sizeof(guarded));
                ul_human_size(guarded + 1, sizes[i].bytes);
                check("shared human trim bytes and guard", string_equals(guarded + 1, sizes[i].text) &&
                      guarded[0] == 0xa5 && guarded[49] == 0xa5);
        }
        return test_report(null);
}
