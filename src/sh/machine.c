/*
        The machine script.

        One file, /root/main.moonwater.sh, is the auditable place a live USB
        or an install writes what the machine should do. Moonwater never
        sources it to print or refuse a bind: a scan of the text is enough
        to name the hooks and the case arms, with the line each was written
        on. The process that sources it is started by init, attaches to
        Spark, and is what a bound event reaches while it is alive.

        Three optional functions are the contract:

          moonwater_init    once, after boot has written a verdict
          moonwater_event   every bound event; $1 is the name, $2 is extra
                            (canvas events are $1 canvas and $2 on|off)
          moonwater_end     when the machine stops

        moonwater_event is the event table. A literal arm owns that event at
        that line; canvas) owns both canvas on and canvas off; *) owns the
        rest. The CLI prints those paths in place of the image binds, and
        SET is refused there. recover is not a bind row: it is fired once
        at start when the last event did not finish.
*/

#define HOST_MACHINE_OK 0
#define HOST_MACHINE_ABSENT 1
#define HOST_MACHINE_REFUSED 2
#define HOST_MACHINE_BYTES ((positive)64 << 10)
#define HOST_MACHINE_WORD 64

#define HOST_MACHINE_TOK_END 0
#define HOST_MACHINE_TOK_WORD 1
#define HOST_MACHINE_TOK_PUNCT 2
#define HOST_MACHINE_TOK_DSEMI 3

#define HOST_REBOOT_RESTART 0x01234567u
#define HOST_REBOOT_POWER_OFF 0x4321fedcu

typedef struct
{
        p16 line;
} host_machine_bind;

typedef struct
{
        p8 hooks;
        bool star;
        p16 init_line;
        p16 event_line;
        p16 end_line;
        p16 star_line;
        host_machine_bind bind[SPARK_BIND_EVENTS];
} host_machine_script;

typedef struct
{
        const p8 address_to text;
        positive length;
        positive at;
        p16 line;
} host_machine_read;

typedef struct
{
        p8 kind;
        p8 punct;
        p16 line;
        p8 word[HOST_MACHINE_WORD];
} host_machine_token;

extern string_address address_to shell_argv;
extern positive shell_argc;

COLD fn shell_dot(writer write, string_address input);
bool exec_function_readonly_set(string_address name);
b32 shell_call_function(string_address name, string_address address_to arguments,
                        positive count);
fn shell_stop(writer write, positive command);
bool env_assign(const_string name, const_string value);

static host_machine_script host_machine_cache;
static p8 host_machine_cache_state;
static bool host_machine_self;

static bool host_machine_is_letter(p8 value)
{
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static bool host_machine_is_digit(p8 value)
{
        return value >= '0' && value <= '9';
}

static bool host_machine_word_start(p8 value)
{
        return host_machine_is_letter(value) || value == '_' || value == '$' ||
               value == '*' || value == '?' || value == '[' || value == '@';
}

static bool host_machine_word_next(p8 value)
{
        return host_machine_word_start(value) || host_machine_is_digit(value) ||
               value == '-' || value == '/' || value == ']' || value == '.';
}

static fn host_machine_skip(host_machine_read address_to scan)
{
        while (scan->at < scan->length)
        {
                p8 value = scan->text[scan->at];

                if (value == '\n')
                {
                        scan->line++;
                        scan->at++;
                        continue;
                }

                if (value == ' ' || value == '\t' || value == '\r')
                {
                        scan->at++;
                        continue;
                }

                if (value == '\\' && scan->at + 1 < scan->length &&
                    scan->text[scan->at + 1] == '\n')
                {
                        scan->at += 2;
                        scan->line++;
                        continue;
                }

                if (value == '#')
                {
                        while (scan->at < scan->length &&
                               scan->text[scan->at] != '\n')
                                scan->at++;
                        continue;
                }

                break;
        }
}

static fn host_machine_next(host_machine_read address_to scan,
                            host_machine_token address_to token)
{
        p8 value;
        p8 quote;
        positive used = 0;

        memory_zero(token, sizeof(address_to token));
        host_machine_skip(scan);
        token->line = scan->line;

        if (scan->at >= scan->length)
        {
                token->kind = HOST_MACHINE_TOK_END;
                return;
        }

        value = scan->text[scan->at];

        if (value == ';' && scan->at + 1 < scan->length &&
            scan->text[scan->at + 1] == ';')
        {
                scan->at += 2;
                token->kind = HOST_MACHINE_TOK_DSEMI;
                return;
        }

        if (value == '(' || value == ')' || value == '{' || value == '}' ||
            value == '|' || value == ';')
        {
                scan->at++;
                token->kind = HOST_MACHINE_TOK_PUNCT;
                token->punct = value;
                token->word[0] = value;
                token->word[1] = end;
                return;
        }

        token->kind = HOST_MACHINE_TOK_WORD;

        if (value == '"' || value == '\'' || value == '`')
        {
                quote = value;
                scan->at++;
                while (scan->at < scan->length && scan->text[scan->at] != quote)
                {
                        value = scan->text[scan->at];
                        if (quote != '\'' && value == '\\' &&
                            scan->at + 1 < scan->length)
                        {
                                scan->at++;
                                value = scan->text[scan->at];
                        }
                        if (value == '\n')
                                scan->line++;
                        if (used + 1 < HOST_MACHINE_WORD)
                                token->word[used++] = value;
                        scan->at++;
                }
                if (scan->at < scan->length)
                        scan->at++;
                token->word[used] = end;
                return;
        }

        while (scan->at < scan->length &&
               host_machine_word_next(scan->text[scan->at]))
        {
                if (used + 1 < HOST_MACHINE_WORD)
                        token->word[used++] = scan->text[scan->at];
                scan->at++;
        }

        token->word[used] = end;
        if (!used)
        {
                scan->at++;
                token->kind = HOST_MACHINE_TOK_PUNCT;
                token->punct = value;
                token->word[0] = value;
                token->word[1] = end;
        }
}

static host_machine_token host_machine_peek(host_machine_read address_to scan)
{
        host_machine_read held = address_to scan;
        host_machine_token token;

        host_machine_next(scan, address_of token);
        address_to scan = held;
        return token;
}

static bool host_machine_word_is(host_machine_token address_to token,
                                 string_address text)
{
        return token->kind == HOST_MACHINE_TOK_WORD &&
               string_equals((string_address)token->word, text);
}

static bool host_machine_punct_is(host_machine_token address_to token, p8 value)
{
        return token->kind == HOST_MACHINE_TOK_PUNCT && token->punct == value;
}

static p8 host_machine_hook_of(string_address name)
{
        if (string_equals(name, "moonwater_init"))
                return HOST_MACHINE_HOOK_INIT;
        if (string_equals(name, "moonwater_event"))
                return HOST_MACHINE_HOOK_EVENT;
        if (string_equals(name, "moonwater_end"))
                return HOST_MACHINE_HOOK_END;
        return 0;
}

static fn host_machine_hook_set(host_machine_script address_to into, p8 hook,
                                p16 line)
{
        into->hooks |= hook;
        if (hook == HOST_MACHINE_HOOK_INIT && !into->init_line)
                into->init_line = line;
        if (hook == HOST_MACHINE_HOOK_EVENT && !into->event_line)
                into->event_line = line;
        if (hook == HOST_MACHINE_HOOK_END && !into->end_line)
                into->end_line = line;
}

static fn host_machine_arm(host_machine_script address_to into,
                           string_address pattern, p16 line)
{
        unsigned int event;

        if (string_equals(pattern, "*"))
        {
                if (!into->star)
                {
                        into->star = true;
                        into->star_line = line;
                }
                return;
        }

        if (string_equals(pattern, "canvas"))
        {
                if (!into->bind[SPARK_BIND_CANVAS_ON - 1].line)
                        into->bind[SPARK_BIND_CANVAS_ON - 1].line = line;
                if (!into->bind[SPARK_BIND_CANVAS_OFF - 1].line)
                        into->bind[SPARK_BIND_CANVAS_OFF - 1].line = line;
                return;
        }

        for (event = 0; event < SPARK_BIND_EVENTS; event++)
                if (string_equals((string_address)spark_bind_event_name[event],
                                  pattern))
                {
                        if (!into->bind[event].line)
                                into->bind[event].line = line;
                        return;
                }
}

static fn host_machine_skip_body(host_machine_read address_to scan, p16 depth);

static fn host_machine_parse_case(host_machine_read address_to scan,
                                  host_machine_script address_to into)
{
        host_machine_token token;
        p16 cases = 1;

        for (;;)
        {
                host_machine_next(scan, address_of token);
                if (token.kind == HOST_MACHINE_TOK_END)
                        return;
                if (host_machine_word_is(address_of token, "esac") ||
                    host_machine_word_is(address_of token, "in"))
                        break;
        }

        if (host_machine_word_is(address_of token, "esac"))
                return;

        while (cases)
        {
                p8 patterns[SPARK_BIND_EVENTS + 2][HOST_MACHINE_WORD];
                p16 pattern_line[SPARK_BIND_EVENTS + 2];
                positive count = 0;
                positive at;

                host_machine_next(scan, address_of token);
                if (token.kind == HOST_MACHINE_TOK_END)
                        return;
                if (host_machine_word_is(address_of token, "esac"))
                {
                        cases--;
                        continue;
                }
                if (host_machine_punct_is(address_of token, '{'))
                {
                        host_machine_skip_body(scan, 1);
                        continue;
                }
                if (host_machine_punct_is(address_of token, '}'))
                        return;
                if (host_machine_word_is(address_of token, "case"))
                {
                        cases++;
                        continue;
                }

                if (host_machine_punct_is(address_of token, '('))
                        host_machine_next(scan, address_of token);

                while (token.kind == HOST_MACHINE_TOK_WORD)
                {
                        if (count < array_count(patterns))
                        {
                                string_copy_bounded(patterns[count],
                                                    (string_address)token.word,
                                                    HOST_MACHINE_WORD);
                                pattern_line[count] = token.line;
                                count++;
                        }
                        host_machine_next(scan, address_of token);
                        if (host_machine_punct_is(address_of token, '|'))
                        {
                                host_machine_next(scan, address_of token);
                                continue;
                        }
                        break;
                }

                while (!host_machine_punct_is(address_of token, ')') &&
                       token.kind != HOST_MACHINE_TOK_END &&
                       token.kind != HOST_MACHINE_TOK_DSEMI &&
                       !host_machine_word_is(address_of token, "esac"))
                        host_machine_next(scan, address_of token);

                if (host_machine_punct_is(address_of token, ')'))
                {
                        for (at = 0; at < count; at++)
                                host_machine_arm(into, patterns[at],
                                                 pattern_line[at]);
                }
                else if (host_machine_word_is(address_of token, "esac"))
                {
                        cases--;
                        continue;
                }

                for (;;)
                {
                        host_machine_next(scan, address_of token);
                        if (token.kind == HOST_MACHINE_TOK_END)
                                return;
                        if (token.kind == HOST_MACHINE_TOK_DSEMI)
                                break;
                        if (host_machine_word_is(address_of token, "case"))
                                cases++;
                        else if (host_machine_word_is(address_of token, "esac"))
                        {
                                cases--;
                                break;
                        }
                        else if (host_machine_punct_is(address_of token, '{'))
                                host_machine_skip_body(scan, 1);
                        else if (host_machine_punct_is(address_of token, '}'))
                                return;
                }
        }
}

static fn host_machine_skip_body(host_machine_read address_to scan, p16 depth)
{
        host_machine_token token;

        while (depth)
        {
                host_machine_next(scan, address_of token);
                if (token.kind == HOST_MACHINE_TOK_END)
                        return;
                if (host_machine_punct_is(address_of token, '{'))
                        depth++;
                else if (host_machine_punct_is(address_of token, '}'))
                        depth--;
        }
}

static fn host_machine_parse_event(host_machine_read address_to scan,
                                   host_machine_script address_to into)
{
        host_machine_token token;
        p16 depth = 1;

        while (depth)
        {
                host_machine_next(scan, address_of token);
                if (token.kind == HOST_MACHINE_TOK_END)
                        return;
                if (host_machine_punct_is(address_of token, '{'))
                        depth++;
                else if (host_machine_punct_is(address_of token, '}'))
                        depth--;
                else if (host_machine_word_is(address_of token, "case"))
                        host_machine_parse_case(scan, into);
        }
}

static bool host_machine_take_function(host_machine_read address_to scan,
                                       string_address name, p16 line,
                                       host_machine_script address_to into)
{
        host_machine_token token;
        p8 hook = host_machine_hook_of(name);

        token = host_machine_peek(scan);
        if (host_machine_punct_is(address_of token, '('))
        {
                host_machine_next(scan, address_of token);
                token = host_machine_peek(scan);
                if (host_machine_punct_is(address_of token, ')'))
                        host_machine_next(scan, address_of token);
                token = host_machine_peek(scan);
        }

        if (!host_machine_punct_is(address_of token, '{'))
                return false;

        host_machine_next(scan, address_of token);
        if (hook)
                host_machine_hook_set(into, hook, line);
        if (hook == HOST_MACHINE_HOOK_EVENT)
                host_machine_parse_event(scan, into);
        else
                host_machine_skip_body(scan, 1);
        return true;
}

static fn host_machine_scan_text(string_address text, positive length,
                                 host_machine_script address_to into)
{
        host_machine_read scan;
        host_machine_token token;

        memory_zero(into, sizeof(address_to into));
        scan.text = (const p8 address_to)text;
        scan.length = length;
        scan.at = 0;
        scan.line = 1;

        while (scan.at < scan.length)
        {
                host_machine_next(address_of scan, address_of token);
                if (token.kind == HOST_MACHINE_TOK_END)
                        return;

                if (host_machine_word_is(address_of token, "function"))
                {
                        host_machine_next(address_of scan, address_of token);
                        if (token.kind == HOST_MACHINE_TOK_WORD)
                                host_machine_take_function(
                                    address_of scan, (string_address)token.word,
                                    token.line, into);
                        continue;
                }

                if (token.kind == HOST_MACHINE_TOK_WORD)
                {
                        host_machine_token next = host_machine_peek(address_of scan);

                        if (host_machine_punct_is(address_of next, '('))
                                host_machine_take_function(
                                    address_of scan, (string_address)token.word,
                                    token.line, into);
                }
        }
}

static bool host_machine_file_allowed(p16 mode, p32 owner)
{
        return (mode & MODE_FORMAT) == MODE_FILE && owner == 0 &&
               (mode & 0022) == 0;
}

static p16 host_machine_event_line_of(host_machine_script address_to script,
                                      unsigned int event)
{
        p16 line;

        if (!(script->hooks & HOST_MACHINE_HOOK_EVENT) || !event ||
            event > SPARK_BIND_EVENTS)
                return 0;

        line = script->bind[event - 1].line;
        if (line)
                return line;
        if (script->star)
                return script->star_line;
        return script->event_line;
}

static p8 host_machine_load(string_address path, host_machine_script address_to into)
{
        file_facts facts;
        file_facts opened;
        bipolar handle;
        bipolar got;
        static p8 text[HOST_MACHINE_BYTES];
        positive used = 0;

        memory_zero(into, sizeof(address_to into));

        if (!file_look(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, address_of facts))
                return HOST_MACHINE_ABSENT;

        if ((facts.mode & MODE_FORMAT) == MODE_LINK ||
            !host_machine_file_allowed(facts.mode, facts.owner))
                return HOST_MACHINE_REFUSED;

        handle = system_open_at(AT_FDCWD, path,
                                FILE_READ | O_CLOEXEC | O_NOFOLLOW);
        if (handle < 0)
                return HOST_MACHINE_REFUSED;

        if (!file_look(handle, (string_address)"", AT_EMPTY_PATH,
                       address_of opened) ||
            !host_machine_file_allowed(opened.mode, opened.owner))
        {
                system_close(handle);
                return HOST_MACHINE_REFUSED;
        }

        for (;;)
        {
                positive room = HOST_MACHINE_BYTES - 1 - used;

                if (!room)
                {
                        p8 extra;

                        got = system_read_once(handle, address_of extra, 1);
                        if (got == -4)
                                continue;
                        if (got < 0)
                        {
                                system_close(handle);
                                return HOST_MACHINE_REFUSED;
                        }
                        if (got)
                        {
                                system_close(handle);
                                return HOST_MACHINE_REFUSED;
                        }
                        break;
                }

                got = system_read_once(handle, text + used, room);
                if (got == -4)
                        continue;
                if (got < 0)
                {
                        system_close(handle);
                        return HOST_MACHINE_REFUSED;
                }
                if (!got)
                        break;
                used += (positive)got;
        }

        system_close(handle);
        text[used] = end;
        host_machine_scan_text((string_address)text, used, into);
        return HOST_MACHINE_OK;
}

static host_machine_script address_to host_machine_peek_script(void)
{
        if (!host_machine_cache_state)
        {
                host_machine_load(HOST_MACHINE_SCRIPT, address_of host_machine_cache);
                host_machine_cache_state = 1;
        }

        return address_of host_machine_cache;
}

static bool host_machine_has_hook(p8 hook)
{
        return (host_machine_peek_script()->hooks & hook) != 0;
}

static p16 host_machine_hook_line(p8 hook)
{
        host_machine_script address_to script = host_machine_peek_script();

        if (hook == HOST_MACHINE_HOOK_INIT)
                return script->init_line;
        if (hook == HOST_MACHINE_HOOK_EVENT)
                return script->event_line;
        if (hook == HOST_MACHINE_HOOK_END)
                return script->end_line;
        return 0;
}

static p16 host_machine_event_line(unsigned int event)
{
        return host_machine_event_line_of(host_machine_peek_script(), event);
}

static p16 host_machine_named_line(string_address name)
{
        unsigned int event;

        for (event = 0; event < SPARK_BIND_EVENTS; event++)
                if (string_equals((string_address)spark_bind_event_name[event],
                                  name))
                        return host_machine_event_line(event + 1);

        return 0;
}

static fn host_machine_refused(string_address name, p16 line)
{
        string_format(log_error, host_label "%s is %s:%p; change it there\n", name,
                      HOST_MACHINE_SCRIPT, (positive)line);
        log_flush();
}

static fn host_machine_wait_verdict(p8 address_to into, positive room)
{
        p64 started = system_clock_ns(HOST_CLOCK_BOOTTIME);

        into[0] = end;
        for (;;)
        {
                if (host_read_text(HOST_VERDICT, into, room) >= 0)
                        return;
                if (system_clock_ns(HOST_CLOCK_BOOTTIME) - started >=
                    HOST_VERDICT_WAIT_NS)
                        return;
                host_pause(HOST_POLL_NS * 2);
        }
}

static b32 host_machine_call(string_address name, string_address first,
                             string_address second)
{
        string_address arguments[2];
        positive count = 0;

        if (first)
                arguments[count++] = first;
        if (second)
                arguments[count++] = second;

        return shell_call_function(name, arguments, count);
}

static fn host_machine_dirty(bool on)
{
        if (on)
                host_write_text(HOST_MACHINE_DIRTY, "1\n");
        else
                system_remove_at(AT_FDCWD, HOST_MACHINE_DIRTY, 0);
}

static bool host_machine_stop_event(unsigned int event)
{
        return event == SPARK_BIND_POWEROFF || event == SPARK_BIND_RESET ||
               event == SPARK_BIND_CTRL_ALT_DELETE;
}

static bool host_machine_stop(void)
{
        struct machine_control control;
        bipolar device;
        p64 started;

        if (host_machine_self)
                return true;

        device = system_open_at(AT_FDCWD, SPARK_DEVICE, FILE_READ | O_CLOEXEC);
        if (device < 0)
                return false;

        memory_zero(address_of control, sizeof(control));
        control.op = SPARK_MACHINE_STATUS;
        if (system_control(device, SPARK_IOCTL_MACHINE, address_of control) < 0 ||
            !(control.flags & SPARK_MACHINE_ATTACHED))
        {
                system_close(device);
                return false;
        }

        memory_zero(address_of control, sizeof(control));
        control.op = SPARK_MACHINE_END;
        (void)system_control(device, SPARK_IOCTL_MACHINE, address_of control);

        started = system_clock_ns(HOST_CLOCK_BOOTTIME);
        while (system_clock_ns(HOST_CLOCK_BOOTTIME) - started < HOST_EXIT_EACH_NS)
        {
                memory_zero(address_of control, sizeof(control));
                control.op = SPARK_MACHINE_STATUS;
                if (system_control(device, SPARK_IOCTL_MACHINE,
                                   address_of control) < 0 ||
                    !(control.flags & SPARK_MACHINE_ATTACHED))
                        break;
                host_pause(HOST_EVENT_POLL_NS);
        }

        system_close(device);
        return true;
}

static b32 host_machine_run(void)
{
        host_machine_script script;
        struct machine_control control;
        p8 verdict[HOST_NAME_ROOM + 16];
        string_address argv[3];
        string_address address_to saved_argv;
        positive saved_argc;
        bipolar device = -1;
        bipolar failed;
        p8 loaded;
        p8 dirty[8];

        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater machine");

        host_state_ready();
        system_call_1(syscall(chdir), (positive)(string_address) "/root");
        bowl_session_prepare("/root", null);
        env_assign("HOME", "/root");
        env_assign("TERM", "dumb");
        env_assign("PATH", BOWL_DEFAULT_PATH);
        env_assign("LANG", "C.UTF-8");

        loaded = host_machine_load(HOST_MACHINE_SCRIPT, address_of script);
        if (loaded == HOST_MACHINE_ABSENT)
                return 0;

        if (loaded == HOST_MACHINE_REFUSED)
        {
                string_address line[] = {"machine script refused: ",
                                         HOST_MACHINE_SCRIPT, null};

                host_kmsg(line);
                return 1;
        }

        device = system_open_at(AT_FDCWD, SPARK_DEVICE, FILE_READ | O_CLOEXEC);
        if (device < 0)
                return host_fail(SPARK_DEVICE, device);

        memory_zero(address_of control, sizeof(control));
        control.op = SPARK_MACHINE_ATTACH;
        failed = system_control(device, SPARK_IOCTL_MACHINE, address_of control);
        if (failed < 0)
        {
                system_close(device);
                return host_fail("machine attach", failed);
        }

        host_machine_wait_verdict(verdict, sizeof(verdict));

        saved_argv = shell_argv;
        saved_argc = shell_argc;
        argv[0] = ".";
        argv[1] = HOST_MACHINE_SCRIPT;
        argv[2] = null;
        shell_argv = argv;
        shell_argc = 2;
        shell_dot(log, HOST_MACHINE_SCRIPT);
        shell_argv = saved_argv;
        shell_argc = saved_argc;

        exec_function_readonly_set("moonwater_init");
        exec_function_readonly_set("moonwater_event");
        exec_function_readonly_set("moonwater_end");

        if (script.hooks & HOST_MACHINE_HOOK_INIT)
                host_machine_call("moonwater_init", (string_address)verdict, null);

        if (host_read_text(HOST_MACHINE_DIRTY, dirty, sizeof(dirty)) >= 0)
        {
                host_machine_dirty(false);
                if (script.hooks & HOST_MACHINE_HOOK_EVENT)
                        host_machine_call("moonwater_event", "recover", null);
        }

        for (;;)
        {
                string_address extra = null;

                memory_zero(address_of control, sizeof(control));
                control.op = SPARK_MACHINE_WAIT;
                failed = system_control(device, SPARK_IOCTL_MACHINE,
                                        address_of control);
                if (failed == -4)
                        continue;
                if (failed < 0)
                        break;

                if (!control.event)
                {
                        host_machine_self = true;
                        if (script.hooks & HOST_MACHINE_HOOK_END)
                                host_machine_call("moonwater_end", null, null);
                        break;
                }

                control.name[SPARK_BIND_NAME_MAX - 1] = end;
                if (control.event == SPARK_BIND_CANVAS_ON ||
                    control.event == SPARK_BIND_CANVAS_OFF)
                {
                        extra = control.extra ? (string_address) "on"
                                              : (string_address) "off";
                        host_machine_dirty(true);
                        if (script.hooks & HOST_MACHINE_HOOK_EVENT)
                                host_machine_call("moonwater_event",
                                                  (string_address) "canvas", extra);
                }
                else
                {
                        host_machine_dirty(true);
                        if (script.hooks & HOST_MACHINE_HOOK_EVENT)
                                host_machine_call("moonwater_event",
                                                  (string_address)control.name, extra);
                }
                host_machine_dirty(false);

                if (host_machine_stop_event(control.event))
                {
                        unsigned int stopping = control.event;

                        host_machine_self = true;
                        if (script.hooks & HOST_MACHINE_HOOK_END)
                                host_machine_call("moonwater_end", null, null);
                        memory_zero(address_of control, sizeof(control));
                        control.op = SPARK_MACHINE_DETACH;
                        (void)system_control(device, SPARK_IOCTL_MACHINE,
                                             address_of control);
                        system_close(device);
                        device = -1;
                        shell_stop(log, stopping == SPARK_BIND_POWEROFF
                                            ? HOST_REBOOT_POWER_OFF
                                            : HOST_REBOOT_RESTART);
                        return 1;
                }
        }

        memory_zero(address_of control, sizeof(control));
        control.op = SPARK_MACHINE_DETACH;
        if (device >= 0)
        {
                (void)system_control(device, SPARK_IOCTL_MACHINE, address_of control);
                system_close(device);
        }

        return 0;
}
