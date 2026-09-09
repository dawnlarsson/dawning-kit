/*
        The same checksum the shell took of this tree's path.

        One build directory per source tree, not one per machine. This used to
        be one path for everybody: two people, or two sessions, or a person and
        an agent building at the same time wrote their objects and their image
        into the same place and neither was told. An incremental build then
        reuses whatever is there -- the userspace half from one tree and the
        kernel module from another, linked into one image that matches no
        checkout anybody has, and every measurement taken off it is about a
        tree that does not exist.

        The suffix is a checksum of this tree's own path, so the same checkout
        always gets the same directory and two checkouts never share one. It is
        our cksum, called as a function on the bytes rather than run on a file
        written only to be checksummed, and it is the POSIX one, so a directory
        made by the shell build is the directory this finds.
*/
static positive build_path_mark(string_address text)
{
        positive length = string_length(text);
        p32 crc;
        p64 count = length;

        cksum_crc_prepare();
        crc = cksum_crc_block((p8 address_to)text, length, 0);

        while (count)
        {
                p8 byte = (p8)count;

                crc = (crc << 8) ^ cksum_crc_table[0][(crc >> 24) ^ byte];
                count >>= 8;
        }

        return (positive)(p32)~crc;
}

//      ssh takes shell source, not an argument vector. Keep empty words,
//      quotes and newlines intact.
static string_address build_quote(string_address address_to words, positive count)
{
        positive total = 0;
        p8 address_to into;
        p8 address_to at;

        for (positive which = 0; which < count; which++)
                total += string_length(words[which]) * 4 + 4;

        into = build_text_take(total + 1);
        at = into;

        for (positive which = 0; which < count; which++)
        {
                string_address word = words[which];

                if (which)
                        *at++ = ' ';

                *at++ = '\'';

                for (positive step = 0; word[step]; step++)
                {
                        if (word[step] != '\'')
                        {
                                *at++ = word[step];
                                continue;
                        }

                        *at++ = '\'';
                        *at++ = '\\';
                        *at++ = '\'';
                        *at++ = '\'';
                }

                *at++ = '\'';
        }

        *at = end;

        return (string_address)into;
}

/*
        Building somewhere else.

        Only reached when a host was named. The remote command carries no host
        of its own and ssh does not forward the environment, so the build over
        there is an ordinary local one and this cannot recurse.
*/
static string_address build_remote_image;

static b32 build_remote(string_address host, string_address remote,
                        string_address address_to profiles, positive count)
{
        string_address quoted = build_quote(address_of remote, 1);
        string_address arguments = build_quote(profiles, count);
        string_address stock = string_get_environment(environ, "MOONWATER_STOCK");

        build_say(build_join("Checking ", host, null));

        if (build_run("ssh", "-n", "-o", "BatchMode=yes", "-o",
                      "ConnectTimeout=20", host, "true", null))
                return build_die(build_join("cannot reach ", host,
                                            " over ssh", null));

        build_say(build_join("Copying the tree to ", host, ":", remote, null));

        /*
                The kernel source, its artifacts and the built filesystem stay
                on the build host: they are large, and none of them belong to
                this checkout. The upstream tree is not part of this
                repository.
        */
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                string_address excluded = build_setting_get("remote_excludes");
                string_address names[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(excluded, string_length(excluded),
                                               (string_address address_to)names,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);
                positive at = 0;

                words[at++] = "rsync";
                words[at++] = "-az";
                words[at++] = "--delete";
                words[at++] = build_join("--rsync-path=mkdir -p -- ", quoted,
                                         " && cd -- ", quoted, " && rsync", null);

                for (positive which = 0; which < many; which++)
                {
                        words[at++] = "--exclude";
                        words[at++] = names[which];
                }

                words[at++] = "./";
                words[at++] = build_join(host, ":./", null);
                words[at] = null;

                if (build_run_words((string_address address_to)words, null))
                        return build_die("copying the tree failed");
        }

        build_say(build_join("Building on ", host, ": ", arguments, null));

        /*
                -n so the build does not swallow this program's stdin. Without
                it the USB prompts read nothing, because ssh forwards whatever
                is on stdin to the remote command.

                sudo drops the environment, so anything the remote build has to
                know is named here. env rather than a VAR=value prefix, which
                sudo only passes when it has been configured to.
        */
        if (build_run("ssh", "-n", host,
                      build_join("cd -- ", quoted, " && sudo env ",
                                 stock && *stock ? "MOONWATER_STOCK=1" : "",
                                 " sh build.sh ", arguments, null),
                      null))
                return build_die(build_join("the build failed on ", host, null));

        /*
                The host which built the configured profile is authoritative
                about its export. A stale local configuration may describe
                another architecture entirely -- an ARM Mac commonly names
                kernel8.img.
        */
        {
                string_address words[8];
                positive at = 0;

                words[at++] = "ssh";
                words[at++] = "-n";
                words[at++] = host;
                words[at++] = build_join("cd -- ", quoted,
                                         " && ./build key-one kernel_export", null);
                words[at] = null;

                if (build_capture_words((string_address address_to)words,
                                        build_file_two, BUILD_FILE_ROOM) < 0)
                        return build_die("could not identify the built image");

                {
                        positive length = string_length((string_address)build_file_two);

                        while (length && (build_file_two[length - 1] == '\n' ||
                                          build_file_two[length - 1] == '\r'))
                                build_file_two[--length] = end;

                        build_remote_image = build_join((string_address)build_file_two,
                                                        null);
                }
        }

        {
                string_address expected = build_join(build_setting_get("output"),
                                                     "/", null);
                positive length = string_length(expected);

                if (string_length(build_remote_image) <= length ||
                    memory_compare(build_remote_image, expected, length))
                        return build_die(build_join(
                                "remote build reported an invalid image path: ",
                                build_remote_image, null));
        }

        build_say(build_join("Fetching ", build_remote_image, null));
        build_tool("mkdir", "-p", build_directory_of(build_remote_image), null);

        {
                string_address words[8];
                positive at = 0;
                b32 handle;
                b32 child;

                words[at++] = "ssh";
                words[at++] = "-n";
                words[at++] = host;
                words[at++] = build_join("cd -- ", quoted, " && cat -- ",
                                         build_quote(address_of build_remote_image, 1),
                                         null);
                words[at] = null;

                handle = open(build_remote_image, O_WRONLY | O_CREAT | O_TRUNC,
                              0644);

                if (handle < 0)
                        return build_die("could not fetch the built image");

                log_flush();
                child = fork();

                if (child == 0)
                {
                        string_address found = build_resolve(words[0]);

                        dup2(handle, 1);
                        close(handle);

                        if (found)
                                execve(found, (string_address address_to)words,
                                       environ);

                        exit(127);
                }

                close(handle);

                if (build_wait(child))
                        return build_die("could not fetch the built image");
        }

        return 0;
}

/*
        Removing what a build produced.

        The artifacts directory keeps the downloaded kernel tarball and is left
        alone on purpose: throwing it away means fetching a hundred and fifty
        megabytes again to get back where you were.
*/
static b32 build_clean()
{
        string_address artifacts = build_setting_get("artifacts");
        string_address leftovers = build_setting_get("clean_patterns");
        string_address names[BUILD_ARGUMENT_ROOM];
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        positive many;

        build_say("Removing build output");

        build_tool("rm", "-rf", build_setting_get("output"),
                   build_setting_get("image_root"),
                   build_setting_get("kernel_tree"),
                   build_join(artifacts, "/merge.config", null),
                   build_join(artifacts, "/.config", null),
                   build_join(artifacts, "/info", null),
                   build_join(artifacts, "/asm.applied", null),
                   build_join(artifacts, "/asm.arch", null),
                   build_join(artifacts, "/asm.requested", null), null);

        /*
                The build products beside the module's source, including the
                .S each .asm becomes -- which kbuild writes there because that
                directory is the kernel tree's own module directory.

                The patterns begin [!.] because a shell glob does not match a
                leading dot and find's -name does. kbuild's own .o.cmd files
                are dotfiles and the shell never removed them; neither does
                this.
        */
        many = build_words_of(leftovers, string_length(leftovers),
                              (string_address address_to)names,
                              BUILD_ARGUMENT_ROOM, store, BUILD_WORD_ROOM);

        for (positive at = 0; at < many; at++)
                build_tool("find", build_setting_get("module_root"), "-maxdepth",
                           "1", "-name", names[at], "-delete", null);

        return 0;
}

/*
        Writing to a USB stick.

        The image is already an EFI application -- the kernel is built with the
        EFI stub, which is why it is called bootx64.efi -- so firmware can load
        it directly and there is no bootloader to install. It goes at the path
        the UEFI spec reserves for removable media, \\EFI\\BOOT\\BOOTX64.EFI,
        which is what a machine looks for when told to boot from USB.

        This lists the candidates and prints the command rather than running
        it. Writing to a raw block device with the wrong name destroys the
        wrong disk, and there is no honest way to claim care against an
        untested lsblk and dd, so the last step stays in your hands.
*/
static b32 build_usb(string_address image)
{
        build_say("Removable disks");

        if (build_have("lsblk"))
        {
                string_address words[8];
                positive at = 0;
                build_lines walk;
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);

                words[at++] = "lsblk";
                words[at++] = "-dno";
                words[at++] = "NAME,SIZE,RM,MODEL";
                words[at] = null;

                if (build_capture_words((string_address address_to)words,
                                        build_file_one, BUILD_FILE_ROOM) >= 0)
                {
                        build_lines_open(address_of walk,
                                         (string_address)build_file_one);

                        while (build_lines_next(address_of walk))
                        {
                                string_address found[BUILD_ARGUMENT_ROOM];
                                positive parts = build_words_of(
                                        walk.line, walk.length,
                                        (string_address address_to)found,
                                        BUILD_ARGUMENT_ROOM, store,
                                        BUILD_WORD_ROOM);

                                if (parts < 3 || !word_is(found[2], "1"))
                                        continue;

                                string_format(log, "  /dev/%s  %s  %s\n",
                                              found[0], found[1],
                                              parts > 3 ? found[3]
                                                        : (string_address)"");
                        }
                }
        }
        else
                string_format(log, "  (lsblk is missing; find the device yourself)\n");

        string_format(log, "\n");
        string_format(log, "Write it with, replacing sdX with the stick:\n");
        string_format(log, "\n");
        string_format(log,
                      "  sudo mkfs.vfat -F32 /dev/sdX1        # after partitioning it GPT/ESP\n");
        string_format(log, "  sudo mount /dev/sdX1 /mnt\n");
        string_format(log, "  sudo mkdir -p /mnt/EFI/BOOT\n");
        string_format(log, "  sudo cp %s /mnt/EFI/BOOT/BOOTX64.EFI\n", image);
        string_format(log, "  sudo umount /mnt\n");
        string_format(log, "\n");
        string_format(log,
                      "Check the device name twice. This erases whatever it names.\n");
        log_flush();

        return 0;
}

/*
        Booting it here.

        Every one of these flags is a requirement rather than a preference, and
        they are settings so another tree can boot its own image with its own:

        virtio-gpu rather than the default VGA, because it is the only device
        here that offers a hardware cursor plane, which is what lets the
        compositor move the pointer without repainting anything. usb-tablet
        reports absolute positions, so the pointer inside the guest follows the
        one on the host instead of drifting. -vga none matters: without it QEMU
        also creates a standard VGA device, the window shows that one because
        it is the boot VGA, and the compositor ends up drawing on the other
        card where nobody can see it.

        -cpu Nehalem, not the default. The kernel is compiled -march=x86-64-v2,
        whose floor is Nehalem, and QEMU's default model is qemu64 -- SSE3-era,
        no POPCNT. There are 334 popcnt instructions in vmlinux, so on the
        default model the image takes an invalid opcode before the console
        exists and prints nothing whatsoever. This line is what stands between
        that and here.

        drm_client_lib.active= on the command line stops the fbdev client
        claiming the display. It has to be built, but it must not take the
        screen, or the compositor is drawing underneath something else.
*/
static b32 build_boot(string_address image, bool console)
{
        string_address emulator = build_setting_get("emulator");
        string_address words[BUILD_ARGUMENT_ROOM];
        string_address accelerators = "";
        string_address display = null;
        positive count = 0;

        if (!build_have(emulator))
                return build_die(build_join(emulator, " is not installed", null));

        build_say(build_join("Booting ", image, null));
        build_size(image);

        words[count++] = emulator;
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_setting_get("emulator_flags"));
        words[count++] = "-kernel";
        words[count++] = image;

        //      Hardware acceleration where this QEMU has it. -cpu host
        //      replaces the model above, which is what you want when the guest
        //      is running on the real one.
        {
                string_address ask[4];

                ask[0] = emulator;
                ask[1] = "-accel";
                ask[2] = "help";
                ask[3] = null;

                if (build_capture_words((string_address address_to)ask,
                                        build_file_one, BUILD_FILE_ROOM) >= 0)
                        accelerators = (string_address)build_file_one;
        }

        if (build_word_listed(accelerators, "hvf"))
        {
                words[count++] = "-accel";
                words[count++] = "hvf";
                words[count++] = "-cpu";
                words[count++] = "host";
        }
        else if (build_word_listed(accelerators, "kvm") &&
                 access("/dev/kvm", W_OK) >= 0)
        {
                words[count++] = "-accel";
                words[count++] = "kvm";
                words[count++] = "-cpu";
                words[count++] = "host";
        }

        words[count++] = "-append";
        words[count++] = build_setting_get("kernel_cmdline");

        if (console)
        {
                build_say("Console on this terminal, ctrl-a x to quit");
                words[count++] = "-display";
                words[count++] = "none";
                words[count++] = "-serial";
                words[count++] = "mon:stdio";
                words[count] = null;

                return build_run_words((string_address address_to)words, null);
        }

        {
                string_address ask[4];
                string_address available = "";

                ask[0] = emulator;
                ask[1] = "-display";
                ask[2] = "help";
                ask[3] = null;

                if (build_capture_words((string_address address_to)ask,
                                        build_file_two, BUILD_FILE_ROOM) >= 0)
                        available = (string_address)build_file_two;

                if (build_word_listed(available, "gtk"))
                        display = "gtk";
                else if (build_word_listed(available, "sdl"))
                        display = "sdl";
                else
                        return build_die(
                                "this QEMU has no graphical display backend -- use --shell");
        }

        build_say("Window opening, ctrl-alt-g releases the mouse");
        words[count++] = "-display";
        words[count++] = display;
        words[count++] = "-serial";
        words[count++] = "mon:stdio";
        words[count] = null;

        return build_run_words((string_address address_to)words, null);
}
