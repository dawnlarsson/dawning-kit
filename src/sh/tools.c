/*
        The utilities that are neither text nor files.

        dd copies with a block size and a count, diff says what changed between
        two files, ps says what is running. They share nothing with each other
        beyond not belonging anywhere else.
*/

static b32 tools_dd(void)
{
        string_format(log, "dd: not yet\n");
        log_flush();
        return 127;
}

static b32 tools_diff(void)
{
        string_format(log, "diff: not yet\n");
        log_flush();
        return 127;
}

static b32 tools_ps(void)
{
        string_format(log, "ps: not yet\n");
        log_flush();
        return 127;
}
