/*
        Land a bootstrap at /bowls/NAME.

        Setup downloads the archive; this file does not talk to pacman, apt
        or apk. It probes the compression magic, extracts, hoists a prefix
        directory when the marker is under one child (Arch root.x86_64, a
        Nix version dir), and writes the files a first isolated install
        actually needs -- resolv.conf without a stub resolver, plus the
        manager conf that exists in that tree.

        gzip and xz applets are compiled in. Extract looks at the archive
        magic and `tar` unpacks gzip, xz, zstd and uncompressed streams
        in-process.
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

#define BOWL_RESET_DEPTH 48

static bipolar bowl_reset_walk_at(bipolar directory, string_address name,
                                   positive depth)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        bipolar failed = 0;

        if (depth >= BOWL_RESET_DEPTH)
                return -ERROR_LOOP;

        if (!file_walk_open_found(address_of walk, directory, name))
                return walk.error == -ERROR_NO_ENTRY ? 0 : walk.error;

        while (!failed && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                if (file_is_directory(walk.handle, entry->d_name))
                {
                        failed = bowl_reset_walk_at(walk.handle, entry->d_name,
                                                    depth + 1);
                        if (!failed)
                                failed = system_remove_at(walk.handle,
                                    entry->d_name, AT_REMOVEDIR);
                }
                else
                        failed = system_remove_at(walk.handle, entry->d_name, 0);

                if (failed == -ERROR_NO_ENTRY)
                        failed = 0;
        }

        if (!failed)
                failed = walk.error;
        file_walk_close(address_of walk);
        return failed;
}

static bipolar bowl_reset_walk(string_address path, positive depth)
{
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        if (parent < 0)
                return parent == -ERROR_NO_ENTRY ? 0 : parent;
        bipolar failed = bowl_reset_walk_at(parent, leaf, depth);
        system_close(parent);
        return failed;
}

static b32 bowl_reset_root(string_address root)
{
        bipolar failed = bowl_reset_walk(root, 0);

        if (failed < 0)
                return bowl_fail(root, failed);
        return 0;
}

static b32 bowl_forget_path(string_address path)
{
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        bipolar failed;

        if (parent < 0)
                return parent == -ERROR_NO_ENTRY ? 0 : bowl_fail(path, parent);
        if (file_is_directory(parent, leaf))
        {
                failed = bowl_reset_walk_at(parent, leaf, 0);
                if (!failed)
                        failed = system_remove_at(parent, leaf, AT_REMOVEDIR);
        }
        else
                failed = system_remove_at(parent, leaf, 0);
        system_close(parent);

        if (failed < 0 && failed != -ERROR_NO_ENTRY)
                return bowl_fail(path, failed);
        return 0;
}

static b32 bowl_recover_from(string_address root, string_address marker)
{
        p8 from[BOWL_PATH_LIMIT];

        if (!bowl_root_path(from, sizeof(from), root, ".bowl-from"))
                return bowl_refuse("bowl path is too long\n");

        if (system_access_at(AT_FDCWD, from, 0) < 0)
                return 0;

        if (bowl_has(root, marker) || bowl_root_busy(root))
                return bowl_forget_path(from);

        system_remove_at(AT_FDCWD, root, AT_REMOVEDIR);
        {
                bipolar failed = system_rename_at(AT_FDCWD, from, AT_FDCWD,
                                                  root, 0);

                if (failed < 0)
                        return bowl_fail(from, failed);
        }
        return 0;
}

static b32 bowl_write_bytes(string_address path, string_address text,
                            positive length)
{
        p8 temp[BOWL_PATH_LIMIT];
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        bipolar handle;
        bipolar failed = 0;

        if (parent < 0)
                return bowl_fail(path, parent);

        handle = file_temporary_open_at(parent, leaf, temp, sizeof(temp),
            ".bowl-", 6, (positive)system_call_1(syscall(getpid), 0), 128, 0644);
        if (handle < 0)
        {
                system_close(parent);
                return bowl_fail(path, handle);
        }

        if (system_write_all((positive)handle, text, length) != length)
                failed = -ERROR_INPUT_OUTPUT;
        if (!failed)
                failed = system_call_1(syscall(fsync), (positive)handle);
        system_close(handle);

        if (!failed)
                failed = system_rename_at(parent, temp, parent, leaf, 0);
        if (failed < 0)
                system_remove_at(parent, temp, 0);
        system_close(parent);

        return failed < 0 ? bowl_fail(path, failed) : 0;
}

static string_address bowl_line_word(string_address line,
                                     bool address_to commented)
{
        line += string_span(line, string_set_blanks);
        address_to commented = *line == '#';
        if (address_to commented)
                line += 1 + string_span(line + 1, string_set_blanks);
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

static bool bowl_section_is(string_address line, string_address name)
{
        positive length = string_length(name);

        if (line[0] != '[')
                return false;
        if (string_compare_max(line + 1, name, length))
                return false;
        return line[1 + length] == ']';
}

static bool bowl_isolation_keyword(string_address line)
{
        return bowl_keyword(line, "DisableSandbox") ||
               bowl_keyword(line, "DisableSandboxFilesystem") ||
               bowl_keyword(line, "DisableSandboxSyscalls");
}

static bool bowl_put_isolation(byte_store address_to out)
{
        string_address text =
            "DisableSandboxFilesystem\n"
            "DisableSandboxSyscalls\n";

        return byte_store_append_exact(out, text, string_length(text));
}

static bool bowl_options_keyword(string_address text, string_address word)
{
        positive at = 0;
        positive length = string_length(text);
        bool in_options = false;

        while (at < length)
        {
                bool commented = false;
                positive stop = at + memory_span_without_byte(text + at, '\n',
                                                              length - at);
                string_address token;

                token = bowl_line_word(text + at, address_of commented);
                if (!commented && token[0] == '[')
                        in_options = bowl_section_is(token, "options");

                if (in_options && !commented && bowl_keyword(token, word))
                        return true;

                at = stop + (stop < length);
        }

        return false;
}

static bool bowl_nameserver_ok(string_address line, positive length)
{
        /* Blanks stop at the line's newline or terminator, so they stay
           inside length. */
        positive at = string_span(line, string_set_blanks);

        if (length - at < 11 || string_compare_max(line + at, "nameserver ", 11))
                return false;

        at += 11;
        at += string_span(line + at, string_set_blanks);

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

/* A bootstrap is a ustar archive or one packed by a codec tar reads. */
static bool bowl_archive_known(string_address archive)
{
        p8 head[512];
        bipolar handle = system_open_at(AT_FDCWD, archive, FILE_READ | O_CLOEXEC);
        bipolar got;

        if (handle < 0)
                return false;

        got = system_read_retry((positive)handle, head, sizeof(head));
        system_close(handle);
        if (got < 6)
                return false;

        p8 pack = tar_pack_from_magic(head, (positive)got);
        return pack == TAR_PACK_GZIP || pack == TAR_PACK_XZ ||
               pack == TAR_PACK_ZSTD ||
               (got >= 262 && !string_compare_max(head + 257, "ustar", 5));
}

static b32 bowl_extract(string_address archive, string_address root)
{
        string_address tar_file[] = {
            "tar", "-x", "-f", archive, "-C", root, null};
        bipolar extract;

        log_flush();
        if (bowl_archive_known(archive))
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

        return bowl_refuse("archive is not a bootstrap\n");
}

static bool bowl_prefix_ready(string_address root, string_address marker)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        p8 from[BOWL_PATH_LIMIT];
        p8 rel[258];
        bool found = false;

        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return false;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                rel[0] = '/';
                string_copy_max_end(rel + 1, entry->d_name, sizeof(rel) - 2);
                if (bowl_root_path(from, sizeof(from), root, rel) &&
                    bowl_has(from, marker))
                {
                        found = true;
                        break;
                }
        }

        file_walk_close(address_of walk);
        return found;
}

static b32 bowl_flatten(string_address root, string_address marker)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        p8 inner[256];
        p8 sibling[BOWL_PATH_LIMIT];
        p8 from[BOWL_PATH_LIMIT];
        p8 rel[258];
        bipolar failed;
        bool found = false;

        if (bowl_has(root, marker))
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
                    !bowl_has(from, marker))
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
                return bowl_refuse("archive is not a bowl bootstrap\n");

        if (!bowl_root_path(sibling, sizeof(sibling), root, ".bowl-from"))
                return bowl_refuse("bowl path is too long\n");

        if (system_access_at(AT_FDCWD, sibling, 0) >= 0)
        {
                failed = bowl_forget_path(sibling);
                if (failed)
                        return failed;
        }

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

        if (system_remove_at(AT_FDCWD, sibling, AT_REMOVEDIR) < 0)
                bowl_forget_path(sibling);
        return 0;
}

static b32 bowl_write_resolv(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 host[4096];
        p8 buffer[4096];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
        bipolar failed;
        positive at = 0;
        string_address fallback = "nameserver 1.1.1.1\n";

        if (!bowl_root_path(path, sizeof(path), root, "/etc/resolv.conf"))
                return bowl_refuse("bowl path is too long\n");

        {
                p8 etc[BOWL_PATH_LIMIT];

                if (!bowl_root_path(etc, sizeof(etc), root, "/etc"))
                        return bowl_refuse("bowl path is too long\n");
                failed = bowl_mkdir(etc);
                if (failed < 0)
                        return bowl_fail(etc, failed);
        }

        if (!byte_store_append_exact(address_of out, fallback,
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
                positive stop = start + memory_span_without_byte(
                                            host + start, '\n', (positive)got - start);

                if (bowl_nameserver_ok(host + start, stop - start) &&
                    string_compare_max(host + start, fallback,
                                       string_length(fallback) - 1))
                {
                        if (!byte_store_append_exact(address_of out, host + start,
                                                     stop - start) ||
                            !byte_store_append_exact(address_of out, "\n", 1))
                                return bowl_refuse("resolv.conf is too long\n");
                }

                at = stop + (stop < (positive)got);
        }

        return bowl_write_bytes(path, out.bytes, out.used);
}

static b32 bowl_write_mirror(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 buffer[BOWL_TEXT];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
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

                at += memory_span_without_byte(text + at, '\n', (positive)got - at);

                if (bowl_keyword(bowl_line_word(text + start, address_of commented),
                                 "Server") &&
                    !commented)
                        live = true;

                at = at + (at < (positive)got);
        }

        if ((!live &&
             !byte_store_append_exact(address_of out, BOWL_GEO_MIRROR,
                                      string_length(BOWL_GEO_MIRROR))) ||
            !byte_store_append_exact(address_of out, text, (positive)got))
                return bowl_refuse("mirrorlist is too long\n");

        return bowl_write_bytes(path, out.bytes, out.used);
}

static fn bowl_clear_lock(string_address root, string_address rel)
{
        p8 path[BOWL_PATH_LIMIT];

        if (bowl_root_path(path, sizeof(path), root, rel))
        {
                p8 leaf[BOWL_PATH_LIMIT];
                bipolar parent = system_open_parent_nofollow(AT_FDCWD, path,
                    false, 0, leaf, sizeof(leaf));
                if (parent >= 0)
                {
                        system_remove_at(parent, leaf, 0);
                        system_close(parent);
                }
        }
}

static bool bowl_archive_usable(string_address path, p64 floor)
{
        file_facts facts;

        return file_look_at(path, address_of facts) && facts.size >= floor &&
               bowl_archive_known(path);
}

static b32 bowl_write_pacman(string_address root);
static b32 bowl_write_apk(string_address root);
static b32 bowl_write_apt(string_address root);

static b32 bowl_configure(string_address root)
{
        b32 failed = bowl_write_resolv(root);

        if (!failed && bowl_has(root, "/etc/pacman.conf"))
        {
                if (bowl_has(root, "/etc/pacman.d/mirrorlist"))
                        failed = bowl_write_mirror(root);
                if (!failed)
                        failed = bowl_write_pacman(root);
                bowl_clear_lock(root, "/var/lib/pacman/db.lck");
        }

        if (!failed && bowl_has(root, "/etc/apk/repositories"))
                failed = bowl_write_apk(root);
        bowl_clear_lock(root, "/lib/apk/db/lock");

        if (!failed && bowl_has(root, "/etc/apt"))
                failed = bowl_write_apt(root);
        bowl_clear_lock(root, "/var/lib/dpkg/lock");
        bowl_clear_lock(root, "/var/lib/dpkg/lock-frontend");
        bowl_clear_lock(root, "/var/lib/apt/lists/lock");
        bowl_clear_lock(root, "/var/cache/apt/archives/lock");
        bowl_clear_lock(root, "/run/dnf/dnf.conf.lock");
        bowl_clear_lock(root, "/var/lib/rpm/.rpm.lock");

        return failed;
}

static b32 bowl_write_apk(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 existing[4096];
        bipolar got;
        positive at = 0;
        string_address text =
            "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main\n"
            "https://dl-cdn.alpinelinux.org/alpine/latest-stable/community\n";

        if (!bowl_root_path(path, sizeof(path), root, "/etc/apk/repositories"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, existing, sizeof(existing));
        if (got > 0 && (positive)got >= sizeof(existing))
                got = (bipolar)(sizeof(existing) - 1);
        if (got > 0)
                existing[got] = end;
        while (got > 0 && at < (positive)got)
        {
                bool commented = false;
                string_address word =
                    bowl_line_word(existing + at, address_of commented);

                if (!commented && !string_compare_max(word, "https://", 8))
                        return 0;

                at += memory_span_without_byte(existing + at, '\n',
                                               (positive)got - at);
                at = at + (at < (positive)got);
        }

        return bowl_write_bytes(path, text, string_length(text));
}

static b32 bowl_write_apt(string_address root)
{
        p8 dir[BOWL_PATH_LIMIT];
        p8 path[BOWL_PATH_LIMIT];
        bipolar failed;
        string_address text =
            "APT::Sandbox::User \"root\";\n"
            "DPkg::Use-Pty \"false\";\n";

        if (!bowl_root_path(dir, sizeof(dir), root, "/etc/apt/apt.conf.d") ||
            !bowl_root_path(path, sizeof(path), root,
                            "/etc/apt/apt.conf.d/99bowl"))
                return bowl_refuse("bowl path is too long\n");

        failed = bowl_mkdir(dir);
        if (failed < 0)
                return bowl_fail(dir, failed);

        return bowl_write_bytes(path, text, string_length(text));
}

static b32 bowl_write_pacman(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 buffer[BOWL_TEXT];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
        positive at = 0;
        bool seen_options = false;
        bool injected = false;

        if (!bowl_root_path(path, sizeof(path), root, "/etc/pacman.conf"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, text, sizeof(text));
        if (got <= 0)
                return bowl_fail(path, got < 0 ? got : -ERROR_NO_ENTRY);

        if ((positive)got >= sizeof(text) - 1)
                return bowl_refuse("pacman.conf is too long\n");
        text[got] = end;

        /*
                Pacman only reads these from [options]. Appending them after
                [extra] is a no-op and prints "directive not recognized".
        */
        if (bowl_options_keyword(text, "DisableSandboxFilesystem") &&
            bowl_options_keyword(text, "DisableSandboxSyscalls"))
                return 0;

        while (at < (positive)got)
        {
                bool commented = false;
                positive start = at;
                positive stop = start + memory_span_without_byte(
                                            text + start, '\n', (positive)got - start);
                string_address word;

                word = bowl_line_word(text + start, address_of commented);

                if (!commented && word[0] == '[')
                {
                        if (bowl_section_is(word, "options"))
                                seen_options = true;
                        else if (!injected)
                        {
                                if ((!seen_options &&
                                     !byte_store_append_exact(address_of out,
                                                              "[options]\n", 10)) ||
                                    !bowl_put_isolation(address_of out))
                                        return bowl_refuse(
                                            "pacman.conf is too long\n");
                                injected = true;
                        }
                }

                if (!commented && bowl_isolation_keyword(word))
                {
                        at = stop + (stop < (positive)got);
                        continue;
                }

                if (bowl_keyword(word, "DownloadUser") ||
                    bowl_keyword(word, "CheckSpace"))
                {
                        if (!commented &&
                            !byte_store_append_exact(address_of out, "#", 1))
                                return bowl_refuse("pacman.conf is too long\n");
                }

                if (!byte_store_append_exact(address_of out, text + start,
                                             stop - start + (stop < (positive)got)))
                        return bowl_refuse("pacman.conf is too long\n");

                at = stop + (stop < (positive)got);
        }

        if (!injected)
        {
                if ((!seen_options &&
                     !byte_store_append_exact(address_of out, "[options]\n", 10)) ||
                    !bowl_put_isolation(address_of out))
                        return bowl_refuse("pacman.conf is too long\n");
        }

        return bowl_write_bytes(path, out.bytes, out.used);
}

static b32 bowl_land(string_address archive, string_address root,
                     string_address marker)
{
        bipolar failed;

        if (!bowl_named_root(root) || !archive || archive[0] != '/' ||
            !marker || marker[0] != '/')
                return bowl_refuse("setup needs an archive path and /bowls/NAME\n");

        failed = system_access_at(AT_FDCWD, archive, 0);
        if (failed < 0)
                return bowl_fail(archive, failed);

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (!failed)
                failed = bowl_mkdir(root);
        if (failed < 0)
                return bowl_fail(root, failed);

        failed = bowl_recover_from(root, marker);
        if (failed)
                return failed;

        if (!bowl_has(root, marker))
        {
                if (bowl_root_busy(root))
                {
                        if (bowl_prefix_ready(root, marker))
                        {
                                failed = bowl_flatten(root, marker);
                                if (failed)
                                        return failed;
                        }
                        else
                        {
                                failed = bowl_reset_root(root);
                                if (failed)
                                        return failed;
                        }
                }

                if (!bowl_has(root, marker))
                {
                        failed = bowl_extract(archive, root);
                        if (!failed)
                                failed = bowl_flatten(root, marker);
                        if (failed)
                        {
                                bowl_reset_root(root);
                                return failed;
                        }
                }

                if (!bowl_has(root, marker))
                        return bowl_refuse("archive is not a bowl bootstrap\n");
        }

        return bowl_configure(root);
}
