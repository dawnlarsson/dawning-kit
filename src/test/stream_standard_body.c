/*
        Experimental C standard library

        the three standard streams, a pipe, and a terminal

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        Compiled twice like stream_body.c, and run two ways by
        src/test/stream.sh: with a regular file on standard input and a
        regular file on standard output, and then with a pipe on both. The
        pipe run is the one that matters -- a pipe cannot seek, so ftell has
        to answer minus one rather than a number it made up, and ungetc has to
        work with nowhere to put the byte but its own pushback.

        The trace goes to descriptor three, which the runner opens, so that
        what the program writes to standard output and standard error stays
        payload and is compared on its own.
*/

static long body_length(const char *text)
{
        long n = 0;
        while (text[n]) n++;
        return n;
}

static void trace_body(void)
{
        char *line = 0;
        unsigned long capacity = 0;
        long got;
        long lines = 0;

        trace_number("isatty 0", isatty(0));
        trace_number("isatty 1", isatty(1));
        trace_number("isatty 2", isatty(2));
        trace_number("fileno stdin", fileno(stdin));
        trace_number("fileno stdout", fileno(stdout));
        trace_number("fileno stderr", fileno(stderr));
        trace_number("ftell stdin before", (long)ftell(stdin));
        trace_number("feof stdin before", (long)feof(stdin));

        trace_number("ungetc onto stdin", (long)ungetc('>', stdin));
        trace_number("getc it back", (long)fgetc(stdin));

        while ((got = getline(&line, &capacity, stdin)) != -1)
        {
                lines++;
                trace_number("line length", got);
                fputs("out: ", stdout);
                fwrite(line, 1, (unsigned long)got, stdout);

                if (got > 0 && line[got - 1] != '\n')
                        fputc('\n', stdout);
        }

        trace_number("lines", lines);
        trace_number("feof stdin after", (long)feof(stdin));
        trace_number("ferror stdin after", (long)ferror(stdin));
        trace_number("getc past the end", (long)fgetc(stdin));
        trace_number("feof still", (long)feof(stdin));
        clearerr(stdin);
        trace_number("feof cleared", (long)feof(stdin));

        fputs("err: diagnostic\n", stderr);
        trace_number("stderr ferror", (long)ferror(stderr));

        trace_number("fflush all", (long)fflush(0));
        trace_number("ftell stdout", (long)ftell(stdout));
        trace_number("stdout ferror", (long)ferror(stdout));

        fputs("out: done\n", stdout);
        trace_number("fflush stdout", (long)fflush(stdout));
        trace_number("length of the last line seen", line ? body_length(line) : -2);
}
