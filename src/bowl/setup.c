/*
        Built-in first boots.

        `bowl setup arch` is the whole new-install command: it becomes root,
        installs /bowl so shebangs resolve, lands the bootstrap, initialises
        the keyring inside isolated, and puts pacman on PATH. The next line a
        person types is pacman -Syu. Pacman itself stays the guest binary;
        this file only arranges the mounts and the names.
*/

#define BOWL_ARCH_ROOT "/bowls/arch"
#define BOWL_ARCH_STORE "/bowls/archlinux-bootstrap-x86_64.tar.zst"
#define BOWL_ARCH_URL \
        "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
#define BOWL_INTERPRETER "/bowl"
#define BOWL_BIN_DIRECTORY "/bin"

static string_address bowl_sudo_places[] = {
    "/usr/bin/sudo", "/bin/sudo", "/usr/local/bin/sudo", null};
static string_address bowl_ln_places[] = {
    "/usr/bin/ln", "/bin/ln", null};

static string_address bowl_find_executable(string_address address_to places)
{
        for (; *places; places++)
                if (system_access_at(AT_FDCWD, *places, BOWL_ACCESS_EXECUTE) >=
                    0)
                        return *places;

        return null;
}

static bool bowl_is_root(void)
{
        return system_call_1(syscall(geteuid), 0) == 0;
}

static b32 bowl_setup_self(p8 address_to into, positive room);

static b32 bowl_setup_run(string_address path, string_address address_to argv,
                          string_address what)
{
        bipolar child;

        child = system_fork();
        if (child == 0)
        {
                system_execute(path, argv, file_environment_all());
                exit(127);
        }

        return bowl_wait_applet(child, what);
}

static b32 bowl_setup_download(string_address dest, string_address url)
{
        p8 self[BOWL_PATH_LIMIT];
        string_address argv[5];

        string_format(log, bowl_label "downloading %s\n", url);
        log_flush();

        if (bowl_setup_self(self, sizeof(self)))
                return 1;

        argv[0] = "wget";
        argv[1] = "-O";
        argv[2] = dest;
        argv[3] = url;
        argv[4] = null;

        if (bowl_setup_run(self, argv, "download failed\n"))
        {
                system_remove_at(AT_FDCWD, dest, 0);
                return 1;
        }

        return 0;
}

static b32 bowl_setup_isolated(string_address root, string_address program,
                               string_address address_to arguments,
                               string_address what)
{
        bipolar child;

        log_flush();
        child = system_fork();
        if (child == 0)
                exit(bowl_launch(root, program, arguments, true));

        return bowl_wait_applet(child, what);
}

static b32 bowl_setup_self(p8 address_to into, positive room)
{
        bipolar got = file_link_text("/proc/self/exe", into, room);

        if (got < 0)
                return bowl_fail("/proc/self/exe", got);

        return 0;
}

static b32 bowl_setup_bind_via_sudo(string_address self)
{
        string_address sudo = bowl_find_executable(bowl_sudo_places);
        string_address ln = bowl_find_executable(bowl_ln_places);
        string_address argv[8];

        if (!sudo || !ln)
                return bowl_refuse("setup needs to write /bowls\n");

        argv[0] = sudo;
        argv[1] = "-n";
        argv[2] = ln;
        argv[3] = "-sfn";
        argv[4] = self;
        argv[5] = BOWL_INTERPRETER;
        argv[6] = null;

        return bowl_setup_run(sudo, argv, "cannot install /bowl\n");
}

static b32 bowl_setup_bind_interpreter(void)
{
        p8 self[BOWL_PATH_LIMIT];
        p8 current[BOWL_PATH_LIMIT];
        bipolar got;
        bipolar failed;

        if (bowl_setup_self(self, sizeof(self)))
                return 1;

        got = file_link_text(BOWL_INTERPRETER, current, sizeof(current));
        if (got >= 0 && string_equals(current, self))
                return 0;

        /*
                A regular file at /bowl already satisfies #!/bowl. Leave it.
                A dangling or differently-aimed symlink is replaced so this
                binary is the one later shebangs invoke.
        */
        if (got < 0 &&
            system_access_at(AT_FDCWD, BOWL_INTERPRETER, BOWL_ACCESS_EXECUTE) >=
                0)
                return 0;

        system_remove_at(AT_FDCWD, BOWL_INTERPRETER, 0);
        failed = system_symbolic_link_at(self, AT_FDCWD, BOWL_INTERPRETER);
        if (failed >= 0 || failed == -EEXIST)
                return 0;

        if (failed != -ERROR_NOT_PERMITTED && failed != -ERROR_ACCESS)
                return bowl_fail(BOWL_INTERPRETER, failed);

        return bowl_setup_bind_via_sudo(self);
}

static DEAD_END fn bowl_setup_reexec_root(string_address name)
{
        string_address sudo = bowl_find_executable(bowl_sudo_places);
        string_address argv[6];

        if (!sudo)
        {
                bowl_refuse("setup needs to write /bowls\n");
                exit(1);
        }

        argv[0] = sudo;
        argv[1] = "-n";
        argv[2] = BOWL_INTERPRETER;
        argv[3] = "setup";
        argv[4] = name;
        argv[5] = null;

        system_execute(sudo, argv, file_environment_all());
        bowl_refuse("cannot obtain root\n");
        exit(1);
}

static b32 bowl_setup_become_root(string_address name)
{
        if (bowl_setup_bind_interpreter())
                return 1;

        if (bowl_is_root())
                return 0;

        bowl_setup_reexec_root(name);
        return 1;
}

static b32 bowl_publish_bin(string_address name)
{
        p8 from[BOWL_PATH_LIMIT];
        p8 to[BOWL_PATH_LIMIT];
        bipolar failed;

        if (sizeof(BOWL_EXPOSE_DIRECTORY) + string_length(name) >=
                sizeof(from) ||
            sizeof(BOWL_BIN_DIRECTORY) + string_length(name) >= sizeof(to))
                return bowl_refuse("exposed path is too long\n");

        path_join(from, sizeof(from), BOWL_EXPOSE_DIRECTORY, name);
        path_join(to, sizeof(to), BOWL_BIN_DIRECTORY, name);

        if (system_access_at(AT_FDCWD, to, 0) >= 0)
                return 0;

        failed = system_symbolic_link_at(from, AT_FDCWD, to);
        if (failed < 0 && failed != -EEXIST)
                return bowl_fail(to, failed);

        return 0;
}

static b32 bowl_setup_arch(void)
{
        string_address init_argv[] = {"/usr/bin/pacman-key", "--init", null};
        string_address populate_argv[] = {"/usr/bin/pacman-key", "--populate",
                                          "archlinux", null};
        b32 failed;

#ifndef X64
        return bowl_refuse("arch setup is x86_64 for now\n");
#else
        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (failed < 0)
                return bowl_fail(BOWL_ROOT_DIRECTORY, failed);

        if (!bowl_has(BOWL_ARCH_ROOT, "/usr/bin/pacman"))
        {
                if (system_access_at(AT_FDCWD, BOWL_ARCH_STORE, 0) < 0)
                {
                        failed = bowl_setup_download(BOWL_ARCH_STORE,
                                                     BOWL_ARCH_URL);
                        if (failed)
                                return failed;
                }

                string_format(log, bowl_label "landing Arch at %s\n",
                              BOWL_ARCH_ROOT);
                log_flush();
                failed = bowl_land(BOWL_ARCH_STORE, BOWL_ARCH_ROOT);
                if (failed)
                        return failed;

                system_remove_at(AT_FDCWD, BOWL_ARCH_STORE, 0);
        }

        string_format(log, bowl_label "initialising the keyring\n");
        log_flush();
        failed = bowl_setup_isolated(BOWL_ARCH_ROOT, "/usr/bin/pacman-key",
                                     init_argv, "pacman-key --init failed\n");
        if (!failed)
                failed = bowl_setup_isolated(
                    BOWL_ARCH_ROOT, "/usr/bin/pacman-key", populate_argv,
                    "pacman-key --populate failed\n");
        if (!failed)
                failed = bowl_expose_program(BOWL_ARCH_ROOT, "/usr/bin/pacman",
                                             null, false);
        if (!failed)
                failed = bowl_expose_program(BOWL_ARCH_ROOT,
                                             "/usr/bin/pacman-key", null,
                                             false);
        if (!failed)
                failed = bowl_publish_bin("pacman");
        if (!failed)
                failed = bowl_publish_bin("pacman-key");
        if (failed)
                return failed;

        string_format(log, bowl_label "arch is ready. pacman -Syu\n");
        log_flush();
        return 0;
#endif
}

static b32 bowl_setup(positive count, string_address address_to arguments)
{
        string_address name;

        if (count != 3)
                return bowl_usage();

        name = arguments[2];
        if (bowl_setup_become_root(name))
                return 1;

        if (string_equals(name, "arch"))
                return bowl_setup_arch();

        if (string_equals(name, "debian") || string_equals(name, "alpine"))
                return bowl_refuse("that setup is not built in yet\n");

        return bowl_refuse("known setups: arch\n");
}
