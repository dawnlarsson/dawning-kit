/*
        awk.

        Its own file because it is its own language: a lexer, a parser, an
        expression evaluator, fields, associative arrays and output formatting.
        Nothing else here needs any of that, and everything else here would
        have to be read around it.
*/

static b32 text_awk()
{
        string_format(log, "awk: not yet\n");
        log_flush();
        return 127;
}
