//      grep -qw, for the one-word-per-line answers QEMU gives to -accel help
//      and -display help.
static bool build_word_listed(string_address text, string_address word)
{
        build_lines walk;
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);

        if (!text)
                return false;

        build_lines_open(address_of walk, text);

        while (build_lines_next(address_of walk))
        {
                string_address found[BUILD_ARGUMENT_ROOM];
                positive parts = build_words_of(walk.line, walk.length,
                                                (string_address address_to)found,
                                                BUILD_ARGUMENT_ROOM, store,
                                                BUILD_WORD_ROOM);

                for (positive at = 0; at < parts; at++)
                        if (word_is(found[at], word))
                                return true;
        }

        return false;
}
