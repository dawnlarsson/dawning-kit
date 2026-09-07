#include "../compiler_memory.c"
#include "../spark.c"

/* Launch code is deliberately unreachable: exercise the actual pure Bowl
   validation without mounting filesystems or invoking another process. */
string_address address_to file_environment_all(void);
#include "../bowl/runtime.c"
#include "counted.inc"

static fn names(void)
{
        static const struct { string_address name; bool valid; } roots[] = {
            {null, false}, {"", false}, {"/", false}, {"/bowl", false},
            {"/bowls", false}, {"/bowls/", false}, {"/bowls/a", true},
            {"/bowls/bin", false}, {"/bowls/a.b_-", true}, {"/bowls/.", false},
            {"/bowls/..", false}, {"/bowls/a/b", false}, {"/bowls/a+", false},
            {"/bowls/a ", false}, {"/bowls/a\xff", false}};

        for (positive at = 0; at < array_count(roots); at++)
                check("Bowl root prefixes and reserved names",
                      bowl_named_root(roots[at].name) == roots[at].valid);

        for (positive byte = 0; byte <= 255; byte++)
        for (positive plus = 0; plus < 2; plus++)
        {
                p8 name[] = {'x', byte, 'y', 0};
                bool valid = !byte || (byte >= 'a' && byte <= 'z') ||
                    (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                    byte == '-' || byte == '_' || byte == '.' || (plus && byte == '+');
                check("Bowl root and command byte policies",
                      bowl_name(name, plus) == valid);
        }
}

static fn launchers(void)
{
        static string_address encoded[] = {
            "@/bowls/debian/usr/bin/jq", "@/bowls/arch/bin/sh", "@/bowls/./bin/sh",
            "@/bowls/../bin/sh", "@/bowls/bin/echo", "@/bowls/a/", "@/bowls/a",
            "@/bowls/", "@/bowls", "@/", "@", "", "!/bowls/a/bin/sh"};
        static string_address roots[] = {"/bowls/debian", "/bowls/arch"};
        static string_address programs[] = {"/usr/bin/jq", "/bin/sh"};

        for (positive item = 0; item < array_count(encoded); item++)
        for (positive room = 0; room <= 48; room++)
        {
                p8 kept[56];
                string_address program = null;
                memory_fill(kept, 0xa5, sizeof kept);
                bool got = bowl_launcher(encoded[item], kept + 4, room, &program);
                bool valid = item < 2 && room > string_length(roots[item]);
                bool intact = true;
                for (positive at = 0; at < sizeof kept; at++)
                        if (at < 4 || at >= 4 + room)
                                intact &= kept[at] == 0xa5;
                check("Bowl launcher root boundaries and destination canaries",
                      got == valid && intact &&
                          (valid ? string_equals(kept + 4, roots[item]) &&
                                       string_equals(program, programs[item])
                                 : program == null));
        }
}

b32 main(void)
{
        names();
        launchers();
        return test_report(null);
}
