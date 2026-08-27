#include "../src/library.c"
#include "../src/spark.c"
#include "../src/canvas/window.c"
#include "../src/sh/term.c"

b32 main()
{
        claim_standard_descriptors();

        window = window_open_text(COLUMNS_WANTED, ROWS_WANTED);

        if (!window)
        {
                log_direct(str("term: no window\n"));
                return 1;
        }

        cells = window_cells(window);
        COLUMNS = window->columns;
        ROWS = window->rows;
        erase(0, 0, ROWS - 1, COLUMNS - 1);

        /*
                Waiting for the other end to exist.

                The compositor starts this the moment it has a screen, which
                is before init has mounted devpts -- there is no ordering
                between the two and there does not need to be, so long as this
                is willing to wait a moment for it.
        */
        timespec wait = {0, 20000000};
        b32 master = -1;

        for (int tries = 0; tries < 200 && master < 0; tries++)
        {
                master = system_call_4(syscall(openat), AT_FDCWD, (positive)"/dev/ptmx",
                                       FILE_READ_WRITE | O_NONBLOCK, 0);

                if (master < 0)
                        system_call_2(syscall(nanosleep), (positive)address_of wait, 0);
        }

        if (master < 0)
        {
                log_direct(str("term: no /dev/ptmx\n"));
                return 1;
        }

        int unlock = 0;
        unsigned int number = 0;

        system_call_3(syscall(ioctl), master, TIOCSPTLCK, (positive)address_of unlock);
        system_call_3(syscall(ioctl), master, TIOCGPTN, (positive)address_of number);

        p8 name[16] = "/dev/pts/";
        positive at = 9;

        if (number >= 10)
                name[at++] = (p8)('0' + number / 10);

        name[at++] = (p8)('0' + number % 10);
        name[at] = 0;

        b32 slave = system_call_4(syscall(openat), AT_FDCWD, (positive)name,
                                  FILE_READ_WRITE, 0);

        if (slave < 0)
        {
                log_direct(str("term: no slave\n"));
                return 1;
        }

        winsize size = {(unsigned short)ROWS, (unsigned short)COLUMNS,
                        (unsigned short)(COLUMNS * WINDOW_CELL_W),
                        (unsigned short)(ROWS * WINDOW_CELL_H)};

        system_call_3(syscall(ioctl), master, TIOCSWINSZ, (positive)address_of size);

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                string_address argv[] = {SHELL, null};
                string_address envp[] = {"TERM=ansi", null};

                system_call(syscall(setsid));
                system_call_3(syscall(ioctl), slave, TIOCSCTTY, 0);
                system_call_3(syscall(dup3), slave, 0, 0);
                system_call_3(syscall(dup3), slave, 1, 0);
                system_call_3(syscall(dup3), slave, 2, 0);
                system_call_1(syscall(close), master);

                system_call_3(syscall(execve), (positive)SHELL,
                              (positive)argv, (positive)envp);
                system_call_1(syscall(exit), 127);
        }

        system_call_1(syscall(close), slave);

        window->region = WINDOW_CENTRED;
        window_damage(window, 0, ROWS);
        window_commit(window);

        p8 from_shell[1024];
        timespec nap = {0, 4000000};

        cursor_show();
        window_damage(window, 0, ROWS);
        window_commit(window);

        for (;;)
        {
                b32 changed = false;
                b32 gone = false;
                struct window_key key;

                touched_top = ROWS;
                touched_bottom = 0;

                if (window->columns != COLUMNS || window->rows != ROWS)
                {
                        regrid(master);
                        changed = true;
                }

                // Keys go out; they change nothing here until they come back.
                while (window_key(window, &key))
                {
                        if (!(key.flags & WINDOW_KEY_DOWN))
                                continue;

                        if (key.character)
                        {
                                p8 byte = (p8)key.character;

                                system_call_3(syscall(write), master,
                                              (positive)address_of byte, 1);
                                continue;
                        }

                        string_address sequence = key_sequence(key.code);

                        if (sequence)
                                system_call_3(syscall(write), master,
                                              (positive)sequence,
                                              string_length(sequence));
                }

                for (;;)
                {
                        bipolar got = system_call_3(syscall(read), master,
                                                    (positive)from_shell,
                                                    sizeof(from_shell));

                        if (got > 0)
                        {
                                if (!changed)
                                        cursor_hide();

                                for (bipolar i = 0; i < got; i++)
                                        consume(from_shell[i]);

                                changed = true;
                                continue;
                        }

                        /*
                                Nothing waiting is EAGAIN and only EAGAIN.

                                Everything else is the shell gone -- zero, or
                                EIO once the session went with it -- and
                                reading that as nothing waiting left this
                                spinning on a dead pty forever, with a zombie
                                behind it and a window nothing could dismiss.
                        */
                        if (got != -EAGAIN && got != -EINTR)
                                gone = true;

                        break;
                }

                /*
                        Only when something actually arrived.

                        Taking the cursor off and putting it back marks a row
                        changed whether or not anything did, and asking for a
                        redraw every four milliseconds because of that is a
                        whole screen recomposed two hundred and fifty times a
                        second for nothing.
                */
                if (changed)
                {
                        cursor_show();
                        window_damage(window, touched_top, touched_bottom - touched_top);
                        window_flush(window);
                }

                if (gone)
                        break;

                system_call_2(syscall(nanosleep), (positive)address_of nap, 0);
        }

        positive status = 0;

        system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        system_call_1(syscall(close), master);
        window_close(window);

        return 0;
}
