p = 'src/build/build.c'
s = open(p).read()

old = '''        if (command && (word_is(command, "--help") || word_is(command, "-h")))
        {
                build_usage();
                return 0;
        }

        build_usage();

        return 0;
}'''

new = '''        if (command && word_is(command, "freestanding"))
        {
                build_config_load();

                return build_freestanding((string_address address_to)(arguments + 2),
                                          count - 2);
        }

        if (command && word_is(command, "floor"))
        {
                build_is_safe();

                return build_floor(count > 2 ? arguments[2] : null);
        }

        if (command && (word_is(command, "--help") || word_is(command, "-h")))
        {
                build_usage();
                return 0;
        }

        /*
                The build.

                Anything that is not an option is a profile name, so the two
                can be mixed in any order: `build --run desktop`.
        */
        {
                string_address profiles[BUILD_ARGUMENT_ROOM];
                string_address host = string_get_environment(environ,
                                                             "MOONWATER_BUILD_HOST");
                string_address remote = string_get_environment(environ,
                                                               "MOONWATER_BUILD_DIR");
                string_address image = null;
                positive chosen = 0;
                bool run = false;
                bool make = true;
                bool clean = false;
                bool usb = false;
                bool console = false;

                build_is_safe();

                for (positive at = 1; at < count; at++)
                {
                        string_address word = arguments[at];

                        if (word_is(word, "--clean"))
                                clean = true;
                        else if (word_is(word, "--run"))
                                run = true;
                        else if (word_is(word, "--boot"))
                        {
                                run = true;
                                make = false;
                        }
                        else if (word_is(word, "--shell"))
                                console = true;
                        else if (word_is(word, "--usb"))
                                usb = true;
                        else if (word_is(word, "--host"))
                        {
                                if (at + 1 >= count)
                                        return build_die(
                                                "--host wants a machine to build on");

                                host = arguments[++at];
                        }
                        else if (!memory_compare(word, "--host=", 7))
                                host = word + 7;
                        else if (!memory_compare(word, "--set", 5) &&
                                 (word[5] == end || word[5] == '='))
                        {
                                //      --set name=value overrides one setting
                                //      for this run, which is how a tree that
                                //      is not this one points the tool at its
                                //      own paths without editing anything.
                                string_address pair = word[5] == '='
                                                              ? word + 6
                                                              : (at + 1 < count
                                                                         ? arguments[++at]
                                                                         : null);
                                p8 address_to cut;

                                if (!pair)
                                        return build_die("--set wants name=value");

                                pair = build_join(pair, null);
                                cut = (p8 address_to)string_first_of(pair, '=');

                                if (!cut)
                                        return build_die("--set wants name=value");

                                address_to cut = end;
                                build_setting_set(pair, (string_address)(cut + 1));
                        }
                        else if (word[0] == '-' && word[1] == '-')
                                return build_die(build_join("unknown option ",
                                                            word, null));
                        else if (chosen + 1 < BUILD_ARGUMENT_ROOM)
                                profiles[chosen++] = word;
                }

                profiles[chosen] = null;

                if (!remote || !*remote)
                        remote = build_join("/tmp/", build_setting_get("name"),
                                            "-",
                                            build_name_of(build_working_directory()),
                                            "-",
                                            build_number(build_path_mark(
                                                    build_working_directory())),
                                            null);

                if (clean)
                        return build_clean();

                build_config_load();

                if (make)
                {
                        if (host && *host)
                        {
                                if (build_remote(host, remote,
                                                 (string_address address_to)profiles,
                                                 chosen))
                                        return 1;

                                image = build_remote_image;
                        }
                        else if (build_local((string_address address_to)profiles,
                                             chosen))
                                return 1;
                }

                if (!usb && !run)
                        return 0;

                /*
                        Where the image ended up. A remote build set this from
                        its own generated configuration. A local build, or
                        --boot without a build, asks the local configuration
                        and finally falls back to the default export.
                */
                if (!image)
                {
                        build_config_load();
                        image = build_key_one("kernel_export", null);
                }

                if (!image || !*image)
                        image = build_setting_get("default_image");

                if (!build_is_file(image))
                        return build_die(build_join("no image at ", image,
                                                    " -- build one first, or drop --boot",
                                                    null));

                if (usb)
                        return build_usb(image);

                return build_boot(image, console);
        }
}'''

assert old in s
s = s.replace(old, new)

s = s.replace('        {"kernel_cmdline", "console=ttyS0 drm_client_lib.active="},',
              '        {"kernel_cmdline", "console=ttyS0 drm_client_lib.active="},\n'
              '        {"default_image", "dist/bootx64.efi"},')
open(p, 'w').write(s)
print("ok")
