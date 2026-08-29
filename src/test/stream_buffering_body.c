/*
        Experimental C standard library

        which buffering policy is in force, seen from outside the process

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        A terminal makes a stream line buffered and a file makes it block
        buffered, and that decision is invisible from inside the program: both
        produce the same bytes in the end. It is visible from outside, in the
        middle -- after the newline and before the flush a line buffered
        stream has already written and a block buffered one has not -- so the
        program stops there for long enough to be looked at.

        src/test/stream.sh runs this with standard output on a file and
        expects nothing to have arrived at the pause, then runs it again under
        a pseudo terminal and expects the first line to have arrived. The same
        two runs are made against glibc, which is what says the expectation is
        the standard's and not this file's.

        body_pause is the driver's, because the way to sleep is the one thing
        here that has no name in common between a freestanding build and a
        hosted one.
*/
static void trace_body(void)
{
        fputs("one", stdout);
        fputs("two\n", stdout);
        body_pause();
        fputs("three", stdout);
        fflush(stdout);
        body_pause();
}
