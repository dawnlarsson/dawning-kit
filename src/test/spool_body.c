/*
        Experimental C standard library

        the rest of <stdio.h>, written once and compiled twice

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        One body, two implementations under it, which is the shape
        src/test/stream_body.c established and the reason it exists is the
        same here: glibc is on the machine, glibc defines what these functions
        answer, and a popen that is merely self-consistent is worth nothing.

        src/test/spool.c puts this tree's entries underneath;
        src/test/spool_reference.c puts the machine's glibc underneath. Both
        print the same trace on a correct implementation and src/test/spool.sh
        diffs them, on x86_64, arm64 and riscv64.

        WHAT IS TRACED AND WHAT IS NOT

        Names are not traced. A temporary name is random by design, so the two
        runs cannot agree on one and a diff of them would fail every time. So
        for everything in the temporary family the trace carries properties:
        how long the name is, what it starts with, whether the file exists,
        whether two draws differ, what mode the file was created with, what
        the errno is when the template is malformed. Those are the things a
        caller depends on and they are identical between any two correct
        implementations.

        Everything else is traced literally, including errno after each
        failure, because an implementation that fails with the wrong errno has
        failed differently from glibc and that is worth catching.

        Two things the wrapper supplies, because they cannot be written once:
        trace_number, which prints through whichever output layer that side
        has; and body_file_mode, which needs a stat structure whose fields are
        spelled differently on the two sides.

        WORK is the directory the driver was told to work in, as a string
        literal, so both runs write into their own tree.
*/

static long body_length(const char *text)
{
        long n = 0;

        while (text[n])
                n++;

        return n;
}

static void body_join(char *into, const char *leaf)
{
        const char *base = WORK;
        long at = 0;
        long i = 0;

        while (base[at])
        {
                into[at] = base[at];
                at++;
        }

        into[at] = '/';
        at++;

        while (leaf[i])
        {
                into[at] = leaf[i];
                at++;
                i++;
        }

        into[at] = 0;
}

static void body_make(const char *path, const char *content)
{
        FILE *f = fopen(path, "w");

        if (!f)
        {
                trace_number("make: fopen failed", 1);
                return;
        }

        fwrite(content, 1, (unsigned long)body_length(content), f);
        fclose(f);
}

static long body_exists(const char *path)
{
        return access(path, F_OK) == 0;
}

/*
        remove, which is the smallest of these and has four distinct answers.

        A regular file goes through unlink. A directory goes through rmdir,
        which is the half C leaves implementation-defined and POSIX requires.
        A missing path is ENOENT from unlink. A non-empty directory is the
        interesting one: unlink answers EISDIR, the fallback runs, and rmdir
        answers ENOTEMPTY -- so the errno a caller sees comes from the second
        call and not the first, and that is what is being pinned here.
*/
static void body_remove(void)
{
        char file[512];
        char folder[512];
        char inside[512];
        char missing[512];

        body_join(file, "removable");
        body_join(folder, "removable_folder");
        body_join(missing, "no_such_thing");

        body_make(file, "bytes");
        trace_number("remove file", remove(file));
        trace_number("remove file: exists after", body_exists(file));

        errno = 0;
        trace_number("remove missing", remove(missing));
        trace_number("remove missing: errno", errno);

        mkdir(folder, 0700);
        trace_number("remove empty folder", remove(folder));
        trace_number("remove empty folder: exists after", body_exists(folder));

        mkdir(folder, 0700);
        body_join(inside, "removable_folder/occupant");
        body_make(inside, "bytes");

        errno = 0;
        trace_number("remove full folder", remove(folder));
        trace_number("remove full folder: errno", errno);
        trace_number("remove full folder: exists after", body_exists(folder));

        remove(inside);
        remove(folder);
}

/*
        mkstemp and its relatives, traced by property.

        The five things a caller actually relies on: a descriptor comes back,
        the template was written into and no X survives, the file is there,
        it is private to the owner, and two calls do not collide. Then the two
        refusals -- a template with no marks and a template too short -- with
        their errno, because a caller that checks for EINVAL is entitled to
        get it.
*/
static long body_marks_left(const char *form)
{
        long at = body_length(form);
        long marks = 0;

        //      Only the trailing run, because the directory this test works
        //      in was itself made by mktemp -d and its name may legitimately
        //      contain an X. Counting every X in the whole path would make
        //      the answer depend on the working directory's random name and
        //      the two sides would disagree for a reason that is not a bug.
        while (at > 0 && form[at - 1] == 'X')
        {
                marks++;
                at--;
        }

        return marks;
}

static long body_same_text(const char *first, const char *second)
{
        long at = 0;

        while (first[at] && first[at] == second[at])
                at++;

        return first[at] == second[at];
}

static void body_temporary(void)
{
        char first[512];
        char second[512];
        char broken[512];
        int one;
        int two;

        body_join(first, "stemXXXXXX");
        body_join(second, "stemXXXXXX");

        one = mkstemp(first);
        two = mkstemp(second);

        trace_number("mkstemp: first is a descriptor", one >= 0);
        trace_number("mkstemp: second is a descriptor", two >= 0);
        trace_number("mkstemp: marks left in first", body_marks_left(first));
        trace_number("mkstemp: length unchanged",
                     body_length(first) == body_length(second));
        trace_number("mkstemp: names differ", !body_same_text(first, second));
        trace_number("mkstemp: first exists", body_exists(first));
        trace_number("mkstemp: mode", body_file_mode(first));

        if (one >= 0)
                close(one);

        if (two >= 0)
                close(two);

        remove(first);
        remove(second);

        body_join(broken, "nomarks");
        errno = 0;
        trace_number("mkstemp: no marks", mkstemp(broken));
        trace_number("mkstemp: no marks errno", errno);

        {
                char tiny[4];

                tiny[0] = 'X';
                tiny[1] = 'X';
                tiny[2] = 'X';
                tiny[3] = 0;

                errno = 0;
                trace_number("mkstemp: too short", mkstemp(tiny));
                trace_number("mkstemp: too short errno", errno);
        }
}

/*
        tmpnam, whose only checkable promises are that the name is shorter
        than L_tmpnam, that it does not exist at the moment it is handed over,
        and that two calls into caller buffers do not collide.
*/
static void body_temporary_name(void)
{
        char first[L_tmpnam + 1];
        char second[L_tmpnam + 1];
        const char *shared = tmpnam(0);

        tmpnam(first);
        tmpnam(second);

        trace_number("tmpnam: static answer given", shared != 0);
        trace_number("tmpnam: fits L_tmpnam", body_length(first) < L_tmpnam);
        trace_number("tmpnam: does not exist", !body_exists(first));
        trace_number("tmpnam: two draws differ",
                     !body_same_text(first, second));
        trace_number("tmpnam: absolute", first[0] == '/');
}

/*
        tmpfile, which has a name nowhere and must still behave like a file
        opened "w+": write, rewind, read the same bytes back, and be gone when
        it closes.
*/
static void body_temporary_stream(void)
{
        FILE *f = tmpfile();
        char back[8];
        long got;

        if (!f)
        {
                trace_number("tmpfile: opened", 0);
                return;
        }

        trace_number("tmpfile: opened", 1);
        trace_number("tmpfile: written",
                     (long)fwrite("abcdef", 1, 6, f));
        trace_number("tmpfile: tell after write", (long)ftell(f));

        rewind(f);

        got = (long)fread(back, 1, 6, f);
        back[got < 0 ? 0 : got] = 0;

        trace_number("tmpfile: read back", got);
        trace_number("tmpfile: bytes agree", body_same_text(back, "abcdef"));
        trace_number("tmpfile: closed", fclose(f) == 0);
}

/*
        fgetpos and fsetpos, which are ftell and fseek wearing an opaque type.

        Three positions and a round trip through each, because the failure
        this catches is an fpos_t that records nothing and an fsetpos that
        silently seeks to zero -- which passes any test that only ever marks
        the start.
*/
static void body_position(void)
{
        char path[512];
        FILE *f;
        fpos_t mark;
        fpos_t start;

        body_join(path, "positions");
        body_make(path, "0123456789");

        f = fopen(path, "r");

        if (!f)
        {
                trace_number("fgetpos: opened", 0);
                return;
        }

        trace_number("fgetpos: at start", fgetpos(f, &start) == 0);

        fseek(f, 4, SEEK_SET);
        trace_number("fgetpos: at four", fgetpos(f, &mark) == 0);
        trace_number("fgetpos: tell agrees", (long)ftell(f));

        fseek(f, 0, SEEK_END);
        trace_number("fgetpos: at end tell", (long)ftell(f));

        trace_number("fsetpos: back to four", fsetpos(f, &mark) == 0);
        trace_number("fsetpos: tell after", (long)ftell(f));
        trace_number("fsetpos: byte there", fgetc(f));

        trace_number("fsetpos: back to start", fsetpos(f, &start) == 0);
        trace_number("fsetpos: byte there", fgetc(f));

        fclose(f);
        remove(path);
}

/*
        popen and pclose, which is what this family is for.

        Six questions. Does a reading pipeline deliver the child's bytes. Does
        pclose hand back a raw wait status rather than an exit code -- the
        trace prints the raw number, so an implementation that returns 3 where
        glibc returns 768 fails here and nowhere else. Does a writing pipeline
        reach the child's standard input, which is checked by having the child
        write a file this process then reads. Does a command the shell cannot
        find come back 127, which is also the answer when there is no shell at
        all, since both are an execve that failed. Does an invalid mode
        refuse. And does a second pclose on a closed stream refuse rather than
        wait on a pid that is no longer ours.
*/
static void body_pipeline(void)
{
        char path[512];
        char line[128];
        FILE *f;
        int status;

        f = popen("printf 'alpha\\nbeta\\n'", "r");

        if (!f)
        {
                trace_number("popen r: opened", 0);
                return;
        }

        trace_number("popen r: opened", 1);
        trace_number("popen r: first line got",
                     fgets(line, (int)sizeof(line), f) != 0);
        trace_number("popen r: first line is alpha",
                     body_same_text(line, "alpha\n"));
        trace_number("popen r: second line got",
                     fgets(line, (int)sizeof(line), f) != 0);
        trace_number("popen r: second line is beta",
                     body_same_text(line, "beta\n"));
        trace_number("popen r: third line absent",
                     fgets(line, (int)sizeof(line), f) == 0);
        trace_number("popen r: eof", feof(f) != 0);
        trace_number("popen r: pclose raw", (long)pclose(f));

        f = popen("exit 3", "r");
        status = pclose(f);
        trace_number("popen: exit 3 raw", (long)status);

        f = popen("exit 0", "r");
        trace_number("popen: exit 0 raw", (long)pclose(f));

        f = popen("kill -TERM $$", "r");
        trace_number("popen: killed raw", (long)pclose(f));

        f = popen("no_such_command_anywhere_xyz 2>/dev/null", "r");
        trace_number("popen: missing command raw", (long)pclose(f));

        body_join(path, "written_by_child");
        {
                char command[600];
                long at = 0;
                const char *lead = "cat > ";

                while (lead[at])
                {
                        command[at] = lead[at];
                        at++;
                }

                {
                        long i = 0;

                        while (path[i])
                        {
                                command[at] = path[i];
                                at++;
                                i++;
                        }

                        command[at] = 0;
                }

                f = popen(command, "w");
        }

        if (!f)
        {
                trace_number("popen w: opened", 0);
                return;
        }

        trace_number("popen w: opened", 1);
        trace_number("popen w: written", (long)fwrite("gamma\n", 1, 6, f));
        trace_number("popen w: pclose raw", (long)pclose(f));
        trace_number("popen w: child wrote the file", body_exists(path));

        f = fopen(path, "r");

        if (f)
        {
                line[0] = 0;
                fgets(line, (int)sizeof(line), f);
                trace_number("popen w: bytes arrived",
                             body_same_text(line, "gamma\n"));
                fclose(f);
        }

        remove(path);

        errno = 0;
        trace_number("popen: bad mode", popen("true", "q") != 0);
        trace_number("popen: bad mode errno", errno);

        //      A null command is deliberately not asked here. glibc does
        //      not check for it -- the pointer goes straight into the argv it
        //      hands execve -- so the reference would not answer a question,
        //      it would misbehave. This tree refuses it with EINVAL, which is
        //      checked in the freestanding half of the lane where there is no
        //      reference to disagree with.
}

/*
        The small entries, which are traced for effect rather than for a
        return value.

        setlinebuf and setbuffer are policy changes with nothing to report, so
        what is checked is that output written after them still arrives and in
        order. __fpurge is checked by the only thing that distinguishes it
        from fflush: bytes written before it must NOT arrive.
*/
static void body_buffering(void)
{
        char path[512];
        FILE *f;
        char back[64];
        long got;

        body_join(path, "buffered");

        f = fopen(path, "w");
        setlinebuf(f);
        fwrite("line\n", 1, 5, f);
        fclose(f);

        f = fopen(path, "r");
        got = (long)fread(back, 1, 63, f);
        back[got < 0 ? 0 : got] = 0;
        fclose(f);

        trace_number("setlinebuf: bytes arrived", body_same_text(back, "line\n"));

        f = fopen(path, "w");
        {
                static char own[64];

                setbuffer(f, own, 64);
                fwrite("held\n", 1, 5, f);
                fclose(f);
        }

        f = fopen(path, "r");
        got = (long)fread(back, 1, 63, f);
        back[got < 0 ? 0 : got] = 0;
        fclose(f);

        trace_number("setbuffer: bytes arrived", body_same_text(back, "held\n"));

        f = fopen(path, "w");
        fwrite("dropped", 1, 7, f);
        __fpurge(f);
        fwrite("kept", 1, 4, f);
        fclose(f);

        f = fopen(path, "r");
        got = (long)fread(back, 1, 63, f);
        back[got < 0 ? 0 : got] = 0;
        fclose(f);

        trace_number("__fpurge: dropped the first write",
                     body_same_text(back, "kept"));

        remove(path);
}

/*
        fmemopen, read mode only, which is the only mode this tree defines.

        The reference has all of glibc's modes; the trace only ever asks for
        "r", so both sides answer the same thing and the divergence stays
        where it is written down rather than showing up as a failure here.
*/
static void body_memory_stream(void)
{
        static char source[] = "one\ntwo\nthree\n";
        FILE *f = fmemopen(source, sizeof(source) - 1, "r");
        char line[64];

        if (!f)
        {
                trace_number("fmemopen: opened", 0);
                return;
        }

        trace_number("fmemopen: opened", 1);

        fgets(line, (int)sizeof(line), f);
        trace_number("fmemopen: first line", body_same_text(line, "one\n"));

        fgets(line, (int)sizeof(line), f);
        trace_number("fmemopen: second line", body_same_text(line, "two\n"));

        trace_number("fmemopen: tell", (long)ftell(f));

        fseek(f, 0, SEEK_SET);
        fgets(line, (int)sizeof(line), f);
        trace_number("fmemopen: rewound", body_same_text(line, "one\n"));

        trace_number("fmemopen: closed", fclose(f) == 0);
}

/*
        The unlocked spellings, which must answer exactly what the locked ones
        do. On glibc they are a different code path with the lock removed; here
        they are the same function. Either way the answers agree or one of the
        two is wrong.
*/
static void body_unlocked(void)
{
        char path[512];
        FILE *f;

        body_join(path, "unlocked");

        f = fopen(path, "w");
        putc_unlocked('a', f);
        fputc_unlocked('b', f);
        fputs_unlocked("cd", f);
        fwrite_unlocked("ef", 1, 2, f);
        trace_number("unlocked: fileno is a descriptor", fileno_unlocked(f) >= 0);
        trace_number("unlocked: flush", fflush_unlocked(f) == 0);
        fclose(f);

        f = fopen(path, "r");
        trace_number("unlocked: getc", getc_unlocked(f));
        trace_number("unlocked: fgetc", fgetc_unlocked(f));

        {
                char rest[8];

                trace_number("unlocked: fread",
                             (long)fread_unlocked(rest, 1, 4, f));
                rest[4] = 0;
                trace_number("unlocked: bytes agree",
                             body_same_text(rest, "cdef"));
        }

        trace_number("unlocked: at end", getc_unlocked(f) == EOF);
        trace_number("unlocked: eof", feof_unlocked(f) != 0);
        trace_number("unlocked: error", ferror_unlocked(f) != 0);

        clearerr_unlocked(f);
        trace_number("unlocked: eof cleared", feof_unlocked(f) != 0);

        fclose(f);
        remove(path);

        flockfile(stdout);
        trace_number("flockfile: try while held", ftrylockfile(stdout) == 0);
        funlockfile(stdout);
}

static void trace_body(void)
{
        body_remove();
        body_temporary();
        body_temporary_name();
        body_temporary_stream();
        body_position();
        body_pipeline();
        body_buffering();
        body_memory_stream();
        body_unlocked();
}
