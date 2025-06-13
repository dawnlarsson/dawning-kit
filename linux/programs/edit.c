#include "../../standard/library.c"
#include "../../standard/platform/shell.c"

positive2 res;
positive2 last_res;

positive2 cursor;
positive time;

bool dirty_ui = true;

fn set_cursor(positive x, positive y)
{
        cursor.x = x;
        cursor.y = y;
        dawn_shell_set_cursor(log, cursor);
}

fn interface()
{
        res = term_size();

        if (res.x != last_res.x || res.y != last_res.y)
                dirty_ui = true;

        if (!dirty_ui)
                return;

        dirty_ui = false;
        last_res = res;

        log_flush();

        dawn_shell_ls(log, ".");
}

fn frame()
{
        // only re-rended when absolutly needed
        interface();

        // any live editing
}

b32 main()
{
        dawn_shell_styles = false;
        log(str(TERM_CLEAR_SCREEN TERM_HIDE_CURSOR TERM_ALT_BUFFER TERM_RESET));

        while (1)
        {
                frame();
                log_flush();
        }

        log(str(TERM_CLEAR_SCREEN TERM_SHOW_CURSOR TERM_MAIN_BUFFER TERM_RESET));
        log_flush();
}