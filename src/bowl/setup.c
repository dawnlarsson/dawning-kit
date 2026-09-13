/*
        Built-in first boots.

        Every named setup shares one pipeline: become root, install /bowl,
        download if the marker is missing, land (extract by magic, flatten
        by marker), configure whatever tree that archive actually contains,
        then expose the manager on PATH. Pacman, apt and apk stay guest
        binaries. Distros differ by URL, floor, marker, decoder and a
        small prime step.
*/

#define BOWL_ARCH_URL \
        "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
#define BOWL_ALPINE_URL \
        "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/alpine-minirootfs-3.24.1-x86_64.tar.gz"
#define BOWL_DEBIAN_URL \
        "https://github.com/debuerreotype/docker-debian-artifacts/raw/dist-amd64/stable/oci/blobs/rootfs.tar.gz"
#define BOWL_INTERPRETER "/bowl"
#define BOWL_BIN_DIRECTORY "/bin"

#define BOWL_PRIME_NONE 0
#define BOWL_PRIME_ARCH 1

struct bowl_distro
{
        string_address name;
        string_address label;
        string_address root;
        string_address store;
        string_address url;
        string_address marker;
        string_address decoder;
        string_address next;
        string_address refuse;
        p64 floor;
        p8 prime;
        string_address address_to expose;
};

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

static b32 bowl_setup_download(string_address dest, string_address url,
                               p64 floor)
{
        p8 self[BOWL_PATH_LIMIT];
        p8 part[BOWL_PATH_LIMIT];
        string_address argv[6];
        positive dest_length = string_length(dest);

        string_format(log, bowl_label "downloading %s\n", url);
        log_flush();

        if (dest_length + 5 >= sizeof(part))
                return bowl_refuse("bowl path is too long\n");

        memory_copy(part, dest, dest_length);
        memory_copy(part + dest_length, ".part", 5);

        if (bowl_setup_self(self, sizeof(self)))
                return 1;

        system_remove_at(AT_FDCWD, part, 0);

        argv[0] = "wget";
        argv[1] = "-q";
        argv[2] = "-O";
        argv[3] = part;
        argv[4] = url;
        argv[5] = null;

        if (bowl_setup_run(self, argv, "download failed\n"))
        {
                system_remove_at(AT_FDCWD, part, 0);
                return 1;
        }

        if (!bowl_archive_usable(part, floor))
        {
                system_remove_at(AT_FDCWD, part, 0);
                return bowl_refuse("download was not a bootstrap archive\n");
        }

        system_remove_at(AT_FDCWD, dest, 0);
        if (system_rename_at(AT_FDCWD, part, AT_FDCWD, dest, 0) < 0)
        {
                system_remove_at(AT_FDCWD, part, 0);
                return bowl_refuse("could not keep the bootstrap archive\n");
        }

        return 0;
}

static b32 bowl_setup_isolated(string_address root, string_address program,
                               string_address address_to arguments,
                               string_address what)
{
        bipolar child;

        log_flush();
        {
                b32 failed = bowl_configure(root);

                if (failed)
                        return failed;
        }
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

static b32 bowl_prime_arch(string_address root)
{
        string_address init_argv[] = {"/usr/bin/pacman-key", "--init", null};
        string_address populate_argv[] = {"/usr/bin/pacman-key", "--populate",
                                          "archlinux", null};
        b32 failed;

        if (bowl_has(root, "/etc/pacman.d/gnupg/pubring.gpg") ||
            bowl_has(root, "/etc/pacman.d/gnupg/pubring.kbx"))
                return 0;

        string_format(log, bowl_label "initialising the keyring\n");
        log_flush();
        failed = bowl_setup_isolated(root, "/usr/bin/pacman-key", init_argv,
                                     "pacman-key --init failed\n");
        if (!failed)
                failed = bowl_setup_isolated(root, "/usr/bin/pacman-key",
                                             populate_argv,
                                             "pacman-key --populate failed\n");
        return failed;
}

static b32 bowl_setup_publish(string_address root, string_address program)
{
        p8 name[256];
        b32 failed;

        path_tail_copy(name, sizeof(name), program);
        failed = bowl_expose_program(root, program, null, false);
        if (!failed)
                failed = bowl_publish_bin(name);
        return failed;
}

static string_address bowl_arch_expose[] = {
    "/usr/bin/pacman", "/usr/bin/pacman-key", null};
static string_address bowl_alpine_expose[] = {"/sbin/apk", null};
static string_address bowl_debian_expose[] = {
    "/usr/bin/apt-get", "/usr/bin/apt", null};

static const struct bowl_distro bowl_distros[] = {
    {"arch", "Arch", "/bowls/arch",
     "/bowls/archlinux-bootstrap-x86_64.tar.zst", BOWL_ARCH_URL,
     "/usr/bin/pacman", null, "pacman -Syu", null, (p64)32 * 1024 * 1024,
     BOWL_PRIME_ARCH, bowl_arch_expose},
    {"alpine", "Alpine", "/bowls/alpine",
     "/bowls/alpine-minirootfs-x86_64.tar.gz", BOWL_ALPINE_URL, "/sbin/apk",
     "gzip", "apk update", null, (p64)1024 * 1024, BOWL_PRIME_NONE,
     bowl_alpine_expose},
    {"debian", "Debian", "/bowls/debian", "/bowls/debian-rootfs-amd64.tar.gz",
     BOWL_DEBIAN_URL, "/usr/bin/apt-get", "gzip", "apt-get update", null,
     (p64)8 * 1024 * 1024, BOWL_PRIME_NONE, bowl_debian_expose},
    {"fedora", "Fedora", "/bowls/fedora", null, null, "/usr/bin/dnf", "xz",
     null, "fedora is an OCI image, not a rootfs tarball\n", 0,
     BOWL_PRIME_NONE, null},
    {"nix", "Nix", "/bowls/nix", null, null, "/bin/nix", "xz", null,
     "nix is a /nix store, not a distro root\n", 0, BOWL_PRIME_NONE, null},
};

static const struct bowl_distro address_to bowl_find_distro(string_address name)
{
        positive at;

        for (at = 0; at < array_count(bowl_distros); at++)
                if (string_equals(bowl_distros[at].name, name))
                        return address_of bowl_distros[at];

        return null;
}

static b32 bowl_setup_distro(const struct bowl_distro address_to distro)
{
        b32 failed = 0;
        string_address address_to program;

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (failed < 0)
                return bowl_fail(BOWL_ROOT_DIRECTORY, failed);

        if (!bowl_has(distro->root, distro->marker))
        {
                if (!bowl_archive_usable(distro->store, distro->floor))
                {
                        system_remove_at(AT_FDCWD, distro->store, 0);
                        failed = bowl_setup_download(distro->store, distro->url,
                                                     distro->floor);
                        if (failed)
                                return failed;
                }

                string_format(log, bowl_label "landing %s at %s\n",
                              distro->label, distro->root);
                log_flush();
                failed = bowl_land(distro->store, distro->root, distro->marker);
                if (failed)
                {
                        if (!bowl_has(distro->root, distro->marker))
                                system_remove_at(AT_FDCWD, distro->store, 0);
                        return failed;
                }

                system_remove_at(AT_FDCWD, distro->store, 0);
        }

        if (distro->prime == BOWL_PRIME_ARCH)
        {
                failed = bowl_prime_arch(distro->root);
                if (failed)
                        return failed;
        }

        program = distro->expose;
        while (program && *program)
        {
                failed = bowl_setup_publish(distro->root, *program);
                if (failed)
                        return failed;
                program++;
        }

        string_format(log, bowl_label "%s is ready. %s\n", distro->label,
                      distro->next);
        log_flush();
        return 0;
}

static b32 bowl_setup(positive count, string_address address_to arguments)
{
        const struct bowl_distro address_to distro;

        if (count != 3)
                return bowl_usage();

        distro = bowl_find_distro(arguments[2]);
        if (!distro)
                return bowl_refuse("known setups: arch alpine debian fedora nix\n");

        if (distro->refuse)
                return bowl_refuse(distro->refuse);

#ifndef X64
        string_format(log, bowl_label "%s setup is x86_64 for now\n",
                      distro->label);
        log_flush();
        return 1;
#endif

        if (!bowl_has(distro->root, distro->marker) &&
            !bowl_can_decode(distro->decoder))
        {
                string_format(log, bowl_label "%s setup needs %s\n",
                              distro->label, distro->decoder);
                log_flush();
                return 1;
        }

        if (bowl_setup_become_root(distro->name))
                return 1;

        return bowl_setup_distro(distro);
}
