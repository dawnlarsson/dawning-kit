/*
        Experimental C standard library

        what a FILE does, written once and compiled twice

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        One body, two implementations under it.

        src/test/stream.c puts this tree's streams underneath;
        src/test/stream_reference.c puts the machine's glibc underneath. Both
        print the same trace on a correct implementation and src/test/stream.sh
        diffs them, on x86_64, arm64 and riscv64.

        The trace is the test, not the bytes. Every classic hand-written stdio
        bug leaves the byte stream correct: feof set one call early still hands
        back every byte, fread returning bytes rather than items still copies
        the right memory, an ftell that models the kernel offset instead of
        asking it still answers correctly until somebody uses fileno. So after
        every single call this prints the return value, ftell, feof and ferror,
        and then the files the run produced are compared byte for byte as well.

        WORK is the directory the driver was told to work in, as a string
        literal, so both runs write to their own and the two trees are diffed
        afterwards.
*/

static long body_length(const char *text)
{
        long n = 0;
        while (text[n]) n++;
        return n;
}

static void trace_run(const char *label, const unsigned char *data, long length)
{
        long i;
        trace_number(label, length);
        for (i = 0; i < length; i++)
                trace_number("    byte", (long)data[i]);
}

static void trace_state(const char *label, FILE *f)
{
        trace_number(label, 0);
        trace_number("    ftell", (long)ftell(f));
        trace_number("    feof", (long)feof(f));
        trace_number("    ferror", (long)ferror(f));
}

static long body_file_size(const char *path)
{
        FILE *f = fopen(path, "r");
        long size;

        if (!f)
                return -1;

        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
        return size;
}

static void body_make(const char *path, const char *content)
{
        FILE *f = fopen(path, "w");
        long length = body_length(content);
        trace_number("make: fwrite items", (long)fwrite(content, 1, (unsigned long)length, f));
        trace_number("make: fclose", (long)fclose(f));
        trace_number("make: size", body_file_size(path));
}

/*      Reading three bytes out of a three byte file, one at a time.

        The point of this block is the pair of feof readings around each
        getc. A stream sitting on the last byte has not seen the end; the
        end arrives on the call after the last byte comes out, and an
        implementation that sets the indicator when its refill came back
        short instead of empty says so one call early. */
static void body_getc_to_the_end(void)
{
        FILE *f = fopen(WORK "/three.txt", "r");
        int i;

        trace_number("getc-to-end: opened", f != 0);

        for (i = 0; i < 5; i++)
        {
                trace_number("getc-to-end: feof before", (long)feof(f));
                trace_number("getc-to-end: getc", (long)fgetc(f));
                trace_number("getc-to-end: feof after", (long)feof(f));
                trace_number("getc-to-end: ferror", (long)ferror(f));
                trace_number("getc-to-end: ftell", (long)ftell(f));
        }

        trace_number("getc-to-end: fclose", (long)fclose(f));
}

/*      fread returns items, and a partial item at the end is not one. */
static void body_fread_items(void)
{
        unsigned char buffer[64];
        FILE *f = fopen(WORK "/three.txt", "r");
        unsigned long got;

        got = fread(buffer, 4, 2, f);
        trace_number("fread-items: eight wanted from three", (long)got);
        trace_state("fread-items: after", f);
        trace_run("fread-items: buffer", buffer, 3);

        got = fread(buffer, 1, 10, f);
        trace_number("fread-items: again at the end", (long)got);
        trace_state("fread-items: after again", f);
        fclose(f);

        f = fopen(WORK "/three.txt", "r");
        trace_number("fread-items: size zero", (long)fread(buffer, 0, 5, f));
        trace_state("fread-items: after size zero", f);
        trace_number("fread-items: count zero", (long)fread(buffer, 5, 0, f));
        trace_state("fread-items: after count zero", f);
        trace_number("fread-items: then getc", (long)fgetc(f));

        got = fread(buffer, 1, 1, f);
        trace_number("fread-items: one byte", (long)got);
        trace_run("fread-items: one byte value", buffer, 1);
        trace_state("fread-items: after one byte", f);
        fclose(f);
}

/*      A large fread, which takes the path that reads straight into the
        caller's memory rather than through the buffer. */
static void body_fread_large(void)
{
        static unsigned char buffer[20000];
        FILE *f = fopen(WORK "/large.txt", "r");
        unsigned long got = fread(buffer, 1, sizeof(buffer), f);
        long i;
        long sum = 0;

        trace_number("fread-large: got", (long)got);
        trace_state("fread-large: after", f);

        for (i = 0; i < (long)got; i++)
                sum += buffer[i];

        trace_number("fread-large: checksum", sum);
        trace_number("fread-large: at the end now", (long)fgetc(f));
        trace_state("fread-large: after the end", f);
        fclose(f);
}

/*      ungetc before anything has been read, which is where a design that
        pushes back into the read buffer has nowhere to put the byte. */
static void body_ungetc_at_the_start(void)
{
        FILE *f = fopen(WORK "/three.txt", "r");

        trace_state("ungetc-start: fresh", f);
        trace_number("ungetc-start: ungetc", (long)ungetc('X', f));
        trace_state("ungetc-start: after ungetc", f);
        trace_number("ungetc-start: getc", (long)fgetc(f));
        trace_state("ungetc-start: after getc", f);
        trace_number("ungetc-start: getc again", (long)fgetc(f));
        trace_state("ungetc-start: after getc again", f);
        trace_number("ungetc-start: refused EOF", (long)ungetc(EOF, f));
        fclose(f);
}

/*      ungetc after the end has been seen has to clear the indicator, or the
        byte the stream is holding can never come back out. */
static void body_ungetc_at_the_end(void)
{
        FILE *f = fopen(WORK "/three.txt", "r");

        while (fgetc(f) != EOF)
                ;

        trace_state("ungetc-end: at the end", f);
        trace_number("ungetc-end: ungetc", (long)ungetc('Z', f));
        trace_state("ungetc-end: after ungetc", f);
        trace_number("ungetc-end: getc", (long)fgetc(f));
        trace_state("ungetc-end: after getc", f);
        trace_number("ungetc-end: getc again", (long)fgetc(f));
        trace_state("ungetc-end: after getc again", f);
        fclose(f);
}

/*      Seeking with a partly consumed buffer behind us: a relative seek has
        to be measured from where the caller is, not from where the kernel
        was left after a refill. */
static void body_seeking(void)
{
        FILE *f = fopen(WORK "/lines.txt", "r");
        long size = body_file_size(WORK "/lines.txt");

        trace_number("seek: file size", size);
        fgetc(f);
        fgetc(f);
        fgetc(f);
        trace_state("seek: three bytes in", f);

        trace_number("seek: cur +2", (long)fseek(f, 2, SEEK_CUR));
        trace_state("seek: after cur +2", f);
        trace_number("seek: getc", (long)fgetc(f));

        trace_number("seek: cur -1", (long)fseek(f, -1, SEEK_CUR));
        trace_state("seek: after cur -1", f);
        trace_number("seek: getc", (long)fgetc(f));

        trace_number("seek: end", (long)fseek(f, 0, SEEK_END));
        trace_state("seek: after end", f);
        trace_number("seek: getc at end", (long)fgetc(f));
        trace_state("seek: end seen", f);

        trace_number("seek: set 1 clears eof", (long)fseek(f, 1, SEEK_SET));
        trace_state("seek: after set 1", f);
        trace_number("seek: getc", (long)fgetc(f));

        trace_number("seek: end -3", (long)fseek(f, -3, SEEK_END));
        trace_state("seek: after end -3", f);
        trace_number("seek: getc", (long)fgetc(f));

        trace_number("seek: after ungetc, seek", (long)ungetc('Q', f));
        trace_number("seek: set 0", (long)fseek(f, 0, SEEK_SET));
        trace_state("seek: after set 0", f);
        trace_number("seek: getc", (long)fgetc(f));
        fclose(f);
}

/*      rewind clears both indicators and fseek clears only one. */
static void body_rewind(void)
{
        FILE *f = fopen(WORK "/three.txt", "r");

        while (fgetc(f) != EOF)
                ;

        trace_state("rewind: at the end", f);
        rewind(f);
        trace_state("rewind: after rewind", f);
        trace_number("rewind: getc", (long)fgetc(f));
        fclose(f);
}

/*      An update stream: bytes written and then read back through the same
        FILE, with the seek in between doing the flushing. */
static void body_update_stream(void)
{
        unsigned char buffer[64];
        FILE *f = fopen(WORK "/update.txt", "w+");
        unsigned long got;

        trace_number("update: fwrite", (long)fwrite("0123456789", 1, 10, f));
        trace_state("update: after write", f);
        trace_number("update: size before seek", body_file_size(WORK "/update.txt"));

        trace_number("update: seek 0", (long)fseek(f, 0, SEEK_SET));
        trace_number("update: size after seek", body_file_size(WORK "/update.txt"));
        trace_state("update: at the start", f);

        got = fread(buffer, 1, 10, f);
        trace_number("update: fread", (long)got);
        trace_run("update: bytes", buffer, (long)got);
        trace_state("update: after read", f);
        trace_number("update: getc past the end", (long)fgetc(f));
        trace_state("update: end seen", f);

        trace_number("update: seek 3", (long)fseek(f, 3, SEEK_SET));
        trace_number("update: putc", (long)fputc('X', f));
        trace_state("update: after putc", f);
        trace_number("update: seek 0 again", (long)fseek(f, 0, SEEK_SET));
        got = fread(buffer, 1, 10, f);
        trace_run("update: bytes again", buffer, (long)got);
        trace_number("update: fclose", (long)fclose(f));
        trace_number("update: final size", body_file_size(WORK "/update.txt"));
}

/*      fgets: the terminator is always written, null means nothing at all was
        read, and a line longer than the buffer comes out in pieces. */
static void body_fgets(void)
{
        char buffer[8];
        FILE *f = fopen(WORK "/lines.txt", "r");
        char *result;

        while ((result = fgets(buffer, 8, f)) != 0)
        {
                trace_run("fgets 8: line", (const unsigned char *)buffer,
                          body_length(buffer));
                trace_state("fgets 8: after", f);
        }

        trace_number("fgets 8: null returned", 1);
        trace_state("fgets 8: at the end", f);
        fclose(f);

        f = fopen(WORK "/lines.txt", "r");

        while ((result = fgets(buffer, 3, f)) != 0)
                trace_run("fgets 3: piece", (const unsigned char *)buffer,
                          body_length(buffer));

        trace_state("fgets 3: at the end", f);
        fclose(f);

        f = fopen(WORK "/lines.txt", "r");
        buffer[0] = '!';
        result = fgets(buffer, 1, f);
        trace_number("fgets 1: returned something", result != 0);
        trace_run("fgets 1: content", (const unsigned char *)buffer,
                  body_length(buffer));
        trace_state("fgets 1: after", f);
        trace_number("fgets 1: getc", (long)fgetc(f));
        fclose(f);
}

/*      getline and getdelim, including the file whose last line has no
        newline on it -- the one every line reader gets wrong. */
static void body_getline(void)
{
        char *line = 0;
        unsigned long capacity = 0;
        long got;
        FILE *f = fopen(WORK "/lines.txt", "r");

        while ((got = getline(&line, &capacity, f)) != -1)
        {
                trace_number("getline: length", got);
                trace_run("getline: bytes", (const unsigned char *)line, got);
                trace_state("getline: after", f);
        }

        trace_number("getline: minus one", got);
        trace_state("getline: at the end", f);
        fclose(f);

        f = fopen(WORK "/colons.txt", "r");

        while ((got = getdelim(&line, &capacity, ':', f)) != -1)
        {
                trace_number("getdelim: length", got);
                trace_run("getdelim: bytes", (const unsigned char *)line, got);
        }

        trace_state("getdelim: at the end", f);
        fclose(f);
        free(line);

        line = 0;
        capacity = 0;
        f = fopen(WORK "/large.txt", "r");
        got = getline(&line, &capacity, f);
        trace_number("getline: one long line", got);
        trace_number("getline: first byte", line ? (long)(unsigned char)line[0] : -2);
        trace_number("getline: last byte",
                     (got > 0) ? (long)(unsigned char)line[got - 1] : -2);
        trace_state("getline: after the long line", f);
        fclose(f);
        free(line);
}

/*      Buffering policy, observed rather than asserted: the size of the file
        on disk while the stream still has bytes in hand is the only way to
        see which of the three modes is in force. Under an emulator with a
        pipe on the far end nothing is a terminal, so the line buffered case
        is reached through setvbuf rather than by asking isatty. */
static void body_buffering(void)
{
        FILE *f;

        f = fopen(WORK "/buffered.txt", "w");
        setvbuf(f, 0, _IOFBF, 4096);
        fwrite("hello", 1, 5, f);
        trace_number("full: size while held", body_file_size(WORK "/buffered.txt"));
        fwrite("\n", 1, 1, f);
        trace_number("full: size after newline", body_file_size(WORK "/buffered.txt"));
        fflush(f);
        trace_number("full: size after flush", body_file_size(WORK "/buffered.txt"));
        fclose(f);

        f = fopen(WORK "/line.txt", "w");
        setvbuf(f, 0, _IOLBF, 4096);
        fwrite("hello", 1, 5, f);
        trace_number("line: size while held", body_file_size(WORK "/line.txt"));
        fwrite("\n", 1, 1, f);
        trace_number("line: size after newline", body_file_size(WORK "/line.txt"));
        fwrite("tail", 1, 4, f);
        trace_number("line: size after tail", body_file_size(WORK "/line.txt"));
        trace_number("line: fclose", (long)fclose(f));
        trace_number("line: final size", body_file_size(WORK "/line.txt"));

        f = fopen(WORK "/none.txt", "w");
        setvbuf(f, 0, _IONBF, 0);
        fwrite("hello", 1, 5, f);
        trace_number("none: size while held", body_file_size(WORK "/none.txt"));
        fputc('!', f);
        trace_number("none: size after putc", body_file_size(WORK "/none.txt"));
        trace_state("none: state", f);
        fclose(f);

        f = fopen(WORK "/reader.txt", "r");
        setvbuf(f, 0, _IONBF, 0);
        trace_number("none: getc", (long)fgetc(f));
        trace_state("none: after getc", f);
        trace_number("none: getc", (long)fgetc(f));
        trace_state("none: after second getc", f);
        fclose(f);
}

/*      Append, which is the one place ftell is allowed to be surprising:
        where an O_APPEND write lands is decided by the kernel at write time
        and is not knowable before it. */
static void body_append(void)
{
        FILE *f = fopen(WORK "/append.txt", "w");

        fwrite("one\n", 1, 4, f);
        fclose(f);

        f = fopen(WORK "/append.txt", "a");
        trace_state("append: freshly opened", f);
        fwrite("two\n", 1, 4, f);
        trace_state("append: after write", f);
        fflush(f);
        trace_state("append: after flush", f);
        fclose(f);
        trace_number("append: size", body_file_size(WORK "/append.txt"));

        f = fopen(WORK "/append.txt", "r");
        {
                unsigned char buffer[32];
                unsigned long got = fread(buffer, 1, sizeof(buffer), f);
                trace_run("append: content", buffer, (long)got);
        }
        fclose(f);
}

/*      fputs, fputc, fwrite item counts, fileno, freopen and fflush(null). */
static void body_odds_and_ends(void)
{
        FILE *f = fopen(WORK "/odds.txt", "w");
        int descriptor;

        trace_number("odds: fputs non negative", fputs("first\n", f) >= 0);
        trace_number("odds: fputc", (long)fputc('!', f));
        trace_number("odds: fwrite items", (long)fwrite("abcdefghijkl", 3, 4, f));
        trace_number("odds: fwrite zero size", (long)fwrite("x", 0, 4, f));
        trace_number("odds: fwrite zero count", (long)fwrite("x", 4, 0, f));
        descriptor = fileno(f);
        trace_number("odds: fileno is a descriptor", descriptor >= 0);
        trace_number("odds: flush all", (long)fflush(0));
        trace_number("odds: size after flush all", body_file_size(WORK "/odds.txt"));

        f = freopen(WORK "/odds2.txt", "w", f);
        trace_number("odds: freopen gave a stream", f != 0);
        trace_number("odds: fputs after freopen", fputs("second\n", f) >= 0);
        trace_number("odds: fclose", (long)fclose(f));
        trace_number("odds: first file size", body_file_size(WORK "/odds.txt"));
        trace_number("odds: second file size", body_file_size(WORK "/odds2.txt"));

        f = fopen(WORK "/odds.txt", "r");
        {
                unsigned char buffer[64];
                unsigned long got = fread(buffer, 1, sizeof(buffer), f);
                trace_run("odds: first content", buffer, (long)got);
        }
        fclose(f);
}

/*      Everything a program is allowed to hand these routines that is not
        the ordinary case: a path that does not exist, a mode that is not a
        mode, a write to a stream opened for reading. Both sides run all of
        it, and both sides agree, which is worth more than the guess that
        glibc would be undefined here -- it is not, and the trace says so. */
static void body_refusals(void)
{
        FILE *f = fopen(WORK "/nowhere/at/all.txt", "r");
        trace_number("refusals: missing file", f == 0);

        f = fopen(WORK "/three.txt", "q");
        trace_number("refusals: bad mode", f == 0);

        f = fopen(WORK "/three.txt", "r");
        trace_number("refusals: write to a read stream",
                     (long)fwrite("x", 1, 1, f));
        trace_number("refusals: ferror set", (long)ferror(f));
        clearerr(f);
        trace_number("refusals: cleared", (long)ferror(f));
        trace_number("refusals: setvbuf bad mode", (long)setvbuf(f, 0, 77, 0));
        fclose(f);

        {
                unsigned char scratch[4];
                f = fopen(WORK "/writeonly.txt", "w");
                trace_number("refusals: read from a write stream",
                             (long)fread(scratch, 1, 1, f));
                fclose(f);
        }
}

/*      /dev/full accepts an open and refuses every write, which is the only
        way to reach the failure paths without a full disk. What has to come
        out of it: a short item count, the error indicator set, EOF from
        fflush and EOF from fclose. */
static void body_write_failure(void)
{
        static char block[9000];
        FILE *f = fopen("/dev/full", "w");
        long i;

        for (i = 0; i < (long)sizeof(block); i++)
                block[i] = 'z';

        trace_number("full-device: opened", f != 0);

        if (!f)
                return;

        trace_number("full-device: fwrite items", (long)fwrite(block, 1, 9000, f));
        trace_number("full-device: ferror", (long)ferror(f));
        trace_number("full-device: fflush", (long)fflush(f));
        trace_number("full-device: ferror after flush", (long)ferror(f));
        trace_number("full-device: feof", (long)feof(f));
        clearerr(f);
        trace_number("full-device: cleared", (long)ferror(f));
        trace_number("full-device: fputc", (long)fputc('q', f));
        trace_number("full-device: fclose", (long)fclose(f));
}

static void trace_body(void)
{
        body_make(WORK "/three.txt", "abc");
        body_make(WORK "/lines.txt", "alpha\nbeta\n\ngamma");
        body_make(WORK "/colons.txt", "a:bb:ccc:");
        body_make(WORK "/reader.txt", "xy");

        {
                FILE *f = fopen(WORK "/large.txt", "w");
                long i;

                for (i = 0; i < 12000; i++)
                        fputc((int)('A' + (i % 26)), f);

                trace_number("large: fclose", (long)fclose(f));
                trace_number("large: size", body_file_size(WORK "/large.txt"));
        }

        body_getc_to_the_end();
        body_fread_items();
        body_fread_large();
        body_ungetc_at_the_start();
        body_ungetc_at_the_end();
        body_seeking();
        body_rewind();
        body_update_stream();
        body_fgets();
        body_getline();
        body_buffering();
        body_append();
        body_odds_and_ends();
        body_refusals();
        body_write_failure();
}
