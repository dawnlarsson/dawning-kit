/*
        Land a bootstrap at /bowls/NAME.

        Arch's tarball is zstd with a root.x86_64 prefix. Setup downloads it;
        this file does not talk to pacman. It
        extracts, hoists that prefix, and writes the three files a first
        isolated install actually needs -- resolv.conf without a stub
        resolver, one mirror, and pacman.conf without the alpm download
        sandbox Moonwater's kernel does not provide.
*/

#define BOWL_TEXT 131072
#define BOWL_GEO_MIRROR \
        "Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n"

static bool bowl_has(string_address root, string_address path)
{
        p8 installed[BOWL_PATH_LIMIT];

        return bowl_root_path(installed, sizeof(installed), root, path) &&
               system_access_at(AT_FDCWD, installed, 0) >= 0;
}

static bool bowl_root_busy(string_address root)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        bool busy = false;

        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return false;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (!file_is_dot(entry->d_name))
                {
                        busy = true;
                        break;
                }
        }

        file_walk_close(address_of walk);
        return busy;
}

static b32 bowl_write_bytes(string_address path, string_address text,
                            positive length)
{
        bipolar handle = system_open_at_mode(AT_FDCWD, path,
                                             FILE_WRITE | O_CLOEXEC, 0644);

        if (handle < 0)
                return bowl_fail(path, handle);

        if (system_write_all((positive)handle, text, length) != length)
        {
                system_close(handle);
                return bowl_refuse("could not write a bowl file\n");
        }

        system_close(handle);
        return 0;
}

static bool bowl_put(p8 address_to into, positive room,
                     positive address_to used, const_string text,
                     positive length)
{
        if (address_to used + length >= room)
                return false;

        memory_copy(into + address_to used, text, length);
        address_to used += length;
        return true;
}

static string_address bowl_line_word(string_address line,
                                     bool address_to commented)
{
        while (*line == ' ' || *line == '\t')
                line++;

        address_to commented = *line == '#';
        if (address_to commented)
        {
                line++;
                while (*line == ' ' || *line == '\t')
                        line++;
        }

        return line;
}

static bool bowl_keyword(string_address line, string_address word)
{
        positive length = string_length(word);

        if (string_compare_max(line, word, length))
                return false;

        return !line[length] || line[length] == ' ' || line[length] == '\t' ||
               line[length] == '=' || line[length] == '\n';
}

static bool bowl_nameserver_ok(string_address line, positive length)
{
        positive at = 0;

        while (at < length && (line[at] == ' ' || line[at] == '\t'))
                at++;

        if (length - at < 11 || string_compare_max(line + at, "nameserver ", 11))
                return false;

        at += 11;
        while (at < length && (line[at] == ' ' || line[at] == '\t'))
                at++;

        if (at >= length || line[at] == '\n' || line[at] == '#')
                return false;

        return string_compare_max(line + at, "127.", 4) != 0;
}

static b32 bowl_wait_applet(bipolar child, string_address what)
{
        positive status = 0;
        bipolar failed;

        if (child < 0)
                return bowl_fail(what, child);

        failed = system_wait4_retry(child, address_of status, 0, null);
        if (failed < 0)
                return bowl_fail(what, failed);

        if (wait_status_code(status))
                return bowl_refuse(what);

        return 0;
}

static bool bowl_archive_zstd(string_address archive)
{
        p8 head[4];
        p32 magic = 0;
        bipolar handle = system_open_at(AT_FDCWD, archive, FILE_READ | O_CLOEXEC);
        bipolar got;

        if (handle < 0)
                return false;

        got = system_read_retry((positive)handle, head, 4);
        system_close(handle);
        if (got != 4)
                return false;

        memory_copy(address_of magic, head, 4);
        return magic == ZSTD_MAGIC;
}

static b32 bowl_extract(string_address archive, string_address root)
{
        string_address tar_words[] = {
            "tar", "-x", "-f", "-", "-C", root, null};
        string_address tar_file[] = {
            "tar", "-x", "-f", archive, "-C", root, null};
        string_address zstd_words[] = {
            "zstd", "-d", "-c", archive, null};
        b32 channel[2];
        bipolar decoder = -1;
        bipolar extract = -1;
        b32 failed;

        log_flush();

        if (!bowl_archive_zstd(archive))
        {
                extract = system_fork();
                if (extract == 0)
                {
                        program_arguments_use(tar_file, 6);
                        exit(file_tar());
                }

                return bowl_wait_applet(extract,
                                        "tar could not extract the archive\n");
        }

        if (system_pipe(address_of channel, 0) < 0)
                return bowl_refuse("cannot pipe zstd into tar\n");

        decoder = system_fork();
        if (decoder == 0)
        {
                system_close(channel[0]);
                system_duplicate(channel[1], 1, 0);
                system_close(channel[1]);
                program_arguments_use(zstd_words, 4);
                exit(file_zstd());
        }

        extract = system_fork();
        if (extract == 0)
        {
                system_close(channel[1]);
                system_duplicate(channel[0], 0, 0);
                system_close(channel[0]);
                program_arguments_use(tar_words, 6);
                exit(file_tar());
        }

        system_close(channel[0]);
        system_close(channel[1]);

        failed = bowl_wait_applet(decoder, "zstd could not decode the archive\n");
        if (!failed)
                failed = bowl_wait_applet(extract,
                                          "tar could not extract the archive\n");
        else
                bowl_wait_applet(extract, "tar could not extract the archive\n");

        return failed;
}

static b32 bowl_flatten(string_address root)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        p8 inner[256];
        p8 sibling[BOWL_PATH_LIMIT];
        p8 from[BOWL_PATH_LIMIT];
        p8 rel[258];
        positive root_length;
        bipolar failed;
        bool found = false;

        if (bowl_has(root, "/usr/bin/pacman"))
                return 0;

        inner[0] = end;
        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return bowl_fail(root, -ERROR_NOT_DIRECTORY);

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                rel[0] = '/';
                string_copy_max_end(rel + 1, entry->d_name, sizeof(rel) - 2);
                if (!bowl_root_path(from, sizeof(from), root, rel) ||
                    !bowl_has(from, "/usr/bin/pacman"))
                        continue;

                if (found)
                {
                        file_walk_close(address_of walk);
                        return bowl_refuse("archive has more than one root\n");
                }

                string_copy_max_end(inner, entry->d_name, sizeof(inner) - 1);
                found = true;
        }

        file_walk_close(address_of walk);

        if (!found)
                return bowl_refuse("archive is not an Arch bootstrap\n");

        root_length = string_length(root);
        if (root_length + 11 >= sizeof(sibling))
                return bowl_refuse("bowl path is too long\n");

        memory_copy(sibling, root, root_length);
        memory_copy(sibling + root_length, ".bowl-from", 11);

        failed = system_rename_at(AT_FDCWD, root, AT_FDCWD, sibling, 0);
        if (failed < 0)
                return bowl_fail(sibling, failed);

        rel[0] = '/';
        string_copy_max_end(rel + 1, inner, sizeof(rel) - 2);
        if (!bowl_root_path(from, sizeof(from), sibling, rel))
                return bowl_refuse("bowl path is too long\n");

        failed = system_rename_at(AT_FDCWD, from, AT_FDCWD, root, 0);
        if (failed < 0)
                return bowl_fail(from, failed);

        system_remove_at(AT_FDCWD, sibling, AT_REMOVEDIR);
        return 0;
}

static b32 bowl_write_resolv(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 host[4096];
        p8 out[4096];
        positive used = 0;
        bipolar got;
        positive at = 0;
        const_string fallback = "nameserver 1.1.1.1\n";

        if (!bowl_root_path(path, sizeof(path), root, "/etc/resolv.conf"))
                return bowl_refuse("bowl path is too long\n");

        system_remove_at(AT_FDCWD, path, 0);

        if (!bowl_put(out, sizeof(out), address_of used, fallback,
                      string_length(fallback)))
                return bowl_refuse("resolv.conf is too long\n");

        got = file_slurp("/etc/resolv.conf", host, sizeof(host));
        if (got > 0 && (positive)got >= sizeof(host))
                got = (bipolar)(sizeof(host) - 1);
        if (got > 0)
                host[got] = end;
        while (got > 0 && at < (positive)got)
        {
                positive start = at;
                positive stop = start;

                while (stop < (positive)got && host[stop] != '\n')
                        stop++;

                if (bowl_nameserver_ok(host + start, stop - start) &&
                    string_compare_max(host + start, fallback,
                                       string_length(fallback) - 1))
                {
                        if (!bowl_put(out, sizeof(out), address_of used,
                                      host + start, stop - start) ||
                            !bowl_put(out, sizeof(out), address_of used, "\n",
                                      1))
                                return bowl_refuse("resolv.conf is too long\n");
                }

                at = stop + (stop < (positive)got);
        }

        return bowl_write_bytes(path, out, used);
}

static b32 bowl_write_mirror(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 out[BOWL_TEXT];
        bipolar got;
        positive used = 0;
        positive at = 0;
        bool live = false;

        if (!bowl_root_path(path, sizeof(path), root, "/etc/pacman.d/mirrorlist"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, text, sizeof(text));
        if (got <= 0)
                return bowl_fail(path, got < 0 ? got : -ERROR_NO_ENTRY);

        if ((positive)got >= sizeof(text) - 1)
                return bowl_refuse("mirrorlist is too long\n");
        text[got] = end;

        while (at < (positive)got)
        {
                bool commented = false;
                positive start = at;

                while (at < (positive)got && text[at] != '\n')
                        at++;

                if (bowl_keyword(bowl_line_word(text + start, address_of commented),
                                 "Server") &&
                    !commented)
                        live = true;

                at = at + (at < (positive)got);
        }

        if (!live &&
            !bowl_put(out, sizeof(out), address_of used, BOWL_GEO_MIRROR,
                      string_length(BOWL_GEO_MIRROR)))
                return bowl_refuse("mirrorlist is too long\n");

        if (!bowl_put(out, sizeof(out), address_of used, text, (positive)got))
                return bowl_refuse("mirrorlist is too long\n");

        return bowl_write_bytes(path, out, used);
}

static b32 bowl_write_pacman(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 out[BOWL_TEXT];
        bipolar got;
        positive used = 0;
        positive at = 0;
        bool sandbox = false;

        if (!bowl_root_path(path, sizeof(path), root, "/etc/pacman.conf"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, text, sizeof(text));
        if (got <= 0)
                return bowl_fail(path, got < 0 ? got : -ERROR_NO_ENTRY);

        if ((positive)got >= sizeof(text) - 1)
                return bowl_refuse("pacman.conf is too long\n");
        text[got] = end;

        if (string_find(text, "DisableHook = *systemd*"))
                return 0;

        while (at < (positive)got)
        {
                bool commented = false;
                positive start = at;
                positive stop = start;
                string_address word;

                while (stop < (positive)got && text[stop] != '\n')
                        stop++;

                word = bowl_line_word(text + start, address_of commented);

                if (bowl_keyword(word, "DownloadUser"))
                {
                        if (!commented &&
                            !bowl_put(out, sizeof(out), address_of used, "#", 1))
                                return bowl_refuse("pacman.conf is too long\n");
                }
                else if (bowl_keyword(word, "DisableSandbox") ||
                         bowl_keyword(word, "DisableSandboxFilesystem") ||
                         bowl_keyword(word, "DisableSandboxSyscalls"))
                {
                        positive take = (positive)((text + stop) - word);

                        sandbox = true;
                        if (!bowl_put(out, sizeof(out), address_of used, word,
                                      take) ||
                            !bowl_put(out, sizeof(out), address_of used, "\n",
                                      1))
                                return bowl_refuse("pacman.conf is too long\n");

                        at = stop + (stop < (positive)got);
                        continue;
                }

                if (!bowl_put(out, sizeof(out), address_of used, text + start,
                              stop - start + (stop < (positive)got)))
                        return bowl_refuse("pacman.conf is too long\n");

                at = stop + (stop < (positive)got);
        }

        if (!sandbox &&
            (!bowl_put(out, sizeof(out), address_of used,
                       "DisableSandboxFilesystem\nDisableSandboxSyscalls\n",
                       49)))
                return bowl_refuse("pacman.conf is too long\n");

        /*
                Isolated is not a booted systemd. Those hooks fail with
                "Current root is not booted" and tmpfiles cannot resolve
                specifiers against this kernel's /etc.
        */
        {
                const_string hooks =
                    "DisableHook = *systemd*\n"
                    "DisableHook = dbus*.hook\n"
                    "DisableHook = 90-mkinitcpio-*\n";

                if (!bowl_put(out, sizeof(out), address_of used, hooks,
                              string_length(hooks)))
                        return bowl_refuse("pacman.conf is too long\n");
        }

        return bowl_write_bytes(path, out, used);
}

static b32 bowl_land(string_address archive, string_address root)
{
        bipolar failed;

        if (!bowl_named_root(root) || !archive || archive[0] != '/')
                return bowl_refuse("setup needs an archive path and /bowls/NAME\n");

        failed = system_access_at(AT_FDCWD, archive, 0);
        if (failed < 0)
                return bowl_fail(archive, failed);

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (!failed)
                failed = bowl_mkdir(root);
        if (failed < 0)
                return bowl_fail(root, failed);

        if (bowl_has(root, "/usr/bin/pacman"))
                return 0;

        if (bowl_root_busy(root))
                return bowl_refuse("root is not empty\n");

        failed = bowl_extract(archive, root);
        if (!failed)
                failed = bowl_flatten(root);
        if (!failed && !bowl_has(root, "/usr/bin/pacman"))
                return bowl_refuse("archive is not an Arch bootstrap\n");
        if (!failed)
                failed = bowl_write_resolv(root);
        if (!failed)
                failed = bowl_write_mirror(root);
        if (!failed)
                failed = bowl_write_pacman(root);

        return failed;
}
