import re

p = 'src/build/build.c'
s = open(p).read()

old = '''/*
        Text.

        A build assembles a great many short strings -- paths, command lines,
        flag lists -- and none of them outlives the step that made it. A ring
        of fixed buffers handed out in turn is the whole allocator this needs:
        no free, no growth, and a fixed ceiling that a build cannot quietly
        walk past. BUILD_TEXT_LIVE is how many are live at once; exceed it and
        an earlier one is overwritten, which is why nothing here keeps a
        borrowed string across a step.
*/
#define BUILD_TEXT_ROOM 8192
#define BUILD_TEXT_LIVE 32

static p8 build_text_ring[BUILD_TEXT_LIVE][BUILD_TEXT_ROOM];
static positive build_text_next;

static p8 address_to build_text_take()
{
        p8 address_to answer = build_text_ring[build_text_next];

        build_text_next = (build_text_next + 1) % BUILD_TEXT_LIVE;
        answer[0] = end;

        return answer;
}

/*
        Join, with the pieces named rather than counted.

        A null argument ends the list, so a caller can pass a value it knows
        may be absent and get the shorter string instead of a crash.
*/
static string_address build_join(string_address first, ...)
{
        p8 address_to into = build_text_take();
        p8 address_to at = into;
        positive left = BUILD_TEXT_ROOM - 1;
        string_address piece = first;
        var_args rest;

        var_list(rest, first);

        while (piece)
        {
                positive length = string_length(piece);

                if (length > left)
                        length = left;

                memory_copy(at, piece, length);
                at += length;
                left -= length;
                piece = var_list_get(rest, string_address);
        }

        var_list_end(rest);
        *at = end;

        return (string_address)into;
}

/*
        A string that must outlive the ring.

        The ring hands out BUILD_TEXT_LIVE buffers in turn, so anything kept
        across a loop that joins something has been overwritten by the time it
        is read. Whatever is held for longer than one step is copied into the
        caller's own storage first.

        This was found the honest way. `build config` printed "Configuration
        generated at kernel/profile/debug_none" -- the path it had written to
        was correct, but the name it had been holding was the last profile
        read. The assembly splitter had the same shape and was invisible: it
        wrote its temporary file under a recycled name and then renamed that
        to the right place, so the output was right and the debris was not.
*/
static string_address build_own(p8 address_to into, positive room,
                                string_address text)
{
        positive length = string_length(text);

        if (length + 1 > room)
                length = room - 1;

        memory_copy(into, text, length);
        into[length] = end;

        return (string_address)into;
}
'''

new = '''/*
        Text.

        A build assembles a great many short strings -- paths, command lines,
        flag lists -- and it is one command from start to finish, so an arena
        that only ever grows is the whole allocator this needs. Nothing is
        freed and nothing is reused, which is the point: every string this
        hands out stays valid until the command ends.

        The first version was a ring of thirty two buffers handed out in turn,
        on the reasoning that no string outlives the step that made it. Three
        separate bugs said otherwise and only one of them was visible. `build
        config` reported writing to the last profile it had read. The assembly
        splitter wrote its temporary file under a recycled name and renamed
        that to the right place, so the output was correct and the debris was
        not. And the key reader took a buffer per line of a two thousand line
        configuration, which recycled the very marker it was matching against,
        so every key came back empty and the compiler was invoked as its own
        directory. A ring is a bet that the author remembers its rule at every
        call site. This does not need the bet.

        The ceiling is a fixed array rather than a growing one, so a build that
        asks for too much stops and says so instead of failing later in a way
        that looks like something else.
*/
#define BUILD_TEXT_ROOM (16 << 20)

static p8 build_text_arena[BUILD_TEXT_ROOM];
static positive build_text_used;

static b32 build_die(string_address text);

static p8 address_to build_text_take(positive want)
{
        p8 address_to answer;

        if (build_text_used + want > BUILD_TEXT_ROOM)
                build_die("build: ran out of room for text");

        answer = build_text_arena + build_text_used;
        build_text_used += want;
        answer[0] = end;

        return answer;
}

//      Only --watch runs more than one build in one process, and it is the
//      one caller that has to give the arena back.
static fn build_text_reset()
{
        build_text_used = 0;
}

/*
        Join, with the pieces named rather than counted.

        A null argument ends the list, so a caller can pass a value it knows
        may be absent and get the shorter string instead of a crash. The list
        is walked twice -- once to measure, once to copy -- so the arena is
        asked for exactly what the answer needs.
*/
static string_address build_join(string_address first, ...)
{
        positive total = 0;
        string_address piece = first;
        p8 address_to into;
        p8 address_to at;
        var_args rest;
        var_args measure;

        var_list(rest, first);
        var_list_copy(rest, measure);

        while (piece)
        {
                total += string_length(piece);
                piece = var_list_get(measure, string_address);
        }

        var_list_end(measure);

        into = build_text_take(total + 1);
        at = into;
        piece = first;

        while (piece)
        {
                positive length = string_length(piece);

                memory_copy(at, piece, length);
                at += length;
                piece = var_list_get(rest, string_address);
        }

        var_list_end(rest);
        *at = end;

        return (string_address)into;
}
'''

assert old in s, "text block not found"
s = s.replace(old, new)

# build_text_take now needs a size everywhere it is called.
s = s.replace("p8 address_to store = build_text_take();",
              "p8 address_to store = build_text_take(BUILD_WORD_ROOM);")
s = s.replace("p8 address_to into = build_text_take();\n        p8 address_to store = build_text_take(BUILD_WORD_ROOM);",
              "p8 address_to into = build_text_take(BUILD_WORD_ROOM);\n        p8 address_to store = build_text_take(BUILD_WORD_ROOM);")
s = s.replace("p8 address_to into = build_text_take();", "p8 address_to into = build_text_take(BUILD_WORD_ROOM);")

# The word store is bounded by the line it splits, so a fixed chunk is right.
s = s.replace("#define BUILD_TEXT_ROOM (16 << 20)",
              "#define BUILD_TEXT_ROOM (16 << 20)\n\n//      One line's worth of words, which is what every splitter asks for.\n#define BUILD_WORD_ROOM 8192")

# build_own no longer has a reason to exist: nothing is recycled.
s = re.sub(r"        p8 target_store\[512\];\n        p8 information_store\[512\];\n", "", s)
s = s.replace('''        string_address target = build_own(target_store, 512,
                                          build_join(artifacts, "/.config", null));
        string_address information = build_own(information_store, 512,
                                               build_join(artifacts, "/info", null));''',
              '''        string_address target = build_join(artifacts, "/.config", null);
        string_address information = build_join(artifacts, "/info", null);''')
s = s.replace('''        p8 temporary_store[512];
        string_address target;
        string_address temporary = build_own(temporary_store, 512,
                                             build_join(output, ".asm_tmp", null));''',
              '''        string_address target;
        string_address temporary = build_join(output, ".asm_tmp", null);''')
s = s.replace('''        p8 marker_store[256];
        p8 address_to into = build_text_take(BUILD_WORD_ROOM);
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        p8 address_to write_at = into;
        positive left = BUILD_TEXT_ROOM - 1;
        string_address marker = build_own(marker_store, 256,
                                          build_join("#> ", name, " ", null));''',
              '''        p8 address_to into = build_text_take(BUILD_WORD_ROOM);
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        p8 address_to write_at = into;
        positive left = BUILD_WORD_ROOM - 1;
        string_address marker = build_join("#> ", name, " ", null);''')
s = s.replace('''        //      Both buffers are taken once, before the walk. Taking the word
        //      store inside it recycled the ring past the marker and the
        //      answer within thirty two lines, and a configuration is
        //      thousands: every key came back empty.
        build_lines_open''', '''        build_lines_open''')

s = s.replace('''        p8 output_store[512];
        p8 elf_store[512];
        p8 text_store[512];
        p8 data_store[512];
        string_address compiler''', '''        string_address compiler''')
s = s.replace('''        output = build_own(output_store, 512, output);

''', '')
s = s.replace('''        elf = build_own(elf_store, 512, build_join(work, "/image.elf", null));
        text_binary = build_own(text_store, 512,
                                build_join(work, "/text.bin", null));
        data_binary = build_own(data_store, 512,
                                build_join(work, "/data.bin", null));''',
              '''        elf = build_join(work, "/image.elf", null);
        text_binary = build_join(work, "/text.bin", null);
        data_binary = build_join(work, "/data.bin", null);''')
s = s.replace('''        p8 found_store[1024];
        string_address path = build_resolve(words[0]);
        b32 child;''', '''        string_address path = build_resolve(words[0]);
        b32 child;''')
s = s.replace('''        path = build_own(found_store, 1024, path);

        //      BUILD_TRACE''', '''        //      BUILD_TRACE''')
s = s.replace('''        p8 found_store[1024];
        string_address found;
        b32 pair[2];''', '''        string_address found;
        b32 pair[2];''')
s = s.replace('''                found = build_own(found_store, 1024, path);''',
              '''                found = path;''')
s = s.replace('''//      The spelling printf gives %#x, which is what the packer's line has
//      always shown. Zero would print bare there; nothing here is ever zero,
//      because the caller refuses an image whose base or entry is.
static string_address build_hex(positive value)
{
        p8 address_to into = build_text_take(BUILD_WORD_ROOM);''',
              '''//      The spelling printf gives %#x, which is what the packer's line has
//      always shown. Zero would print bare there; nothing here is ever zero,
//      because the caller refuses an image whose base or entry is.
static string_address build_hex(positive value)
{
        p8 address_to into = build_text_take(32);''')
s = s.replace('''static string_address build_number(positive value)
{
        p8 address_to into = build_text_take(BUILD_WORD_ROOM);''',
              '''static string_address build_number(positive value)
{
        p8 address_to into = build_text_take(32);''')

open(p, 'w').write(s)
print("ok")
