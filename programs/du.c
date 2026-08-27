#include "../src/sh/file.c"

/*
        du [-a] [-s] [-h] [-k] [-b] [-c] [PATH...]

        What a file costs on the disk, not how long it is: the kernel's block
        count, which is what makes a sparse file cheap and a tiny file cost a
        whole block. -b is the other question, and asks for the length.
*/
static bool du_all;
static bool du_summary;
static bool du_human;
static bool du_apparent;
static bool du_total;
static b32 du_status;
static p64 du_grand;

static fn du_report(p64 blocks, string_address path)
{
        if (du_human)
                file_human(log, du_apparent ? blocks : blocks * 512);
        else if (du_apparent)
                file_number(log, blocks);
        else
                file_number(log, blocks / 2);

        log("\t", 1);
        log(path, 0);
        log("\n", 1);
}

// Returns what the tree costs, and prints the parts of it that were asked for
// on the way back up, which is the order du has always reported in.
static p64 du_walk(string_address path, positive depth, bool named)
{
        file_facts facts;

        if (!file_look_link(path, address_of facts))
        {
                string_format(file_fail, "du: cannot access '%s': No such file or directory\n",
                              path);
                du_status = 1;
                return 0;
        }

        p64 mine = du_apparent ? facts.size : facts.blocks;

        // --apparent-size is asking how much was written, and nothing was
        // written into the directory itself; only what is under it counts.
        if (du_apparent && (facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                mine = 0;

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if (du_all || named)
                        du_report(mine, path);

                return mine;
        }

        p64 total = mine;

        if (depth > 0)
        {
                file_walk walk;

                if (file_walk_open(address_of walk, AT_FDCWD, path))
                {
                        struct linux_dirent64 address_to entry;

                        while ((entry = file_walk_next(address_of walk)))
                        {
                                if (file_is_dot(entry->d_name))
                                        continue;

                                p8 below[FILE_PATH_MAX];

                                file_join(below, FILE_PATH_MAX, path, entry->d_name);
                                total += du_walk(below, depth - 1, false);
                        }

                        file_walk_close(address_of walk);
                }
                else
                {
                        string_format(file_fail, "du: cannot read directory '%s'\n", path);
                        du_status = 1;
                }
        }

        if (named || !du_summary)
                du_report(total, path);

        return total;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "ashkbcSx", address_of first);

        du_all = (flags & FILE_FLAG('a')) != 0;
        du_summary = (flags & FILE_FLAG('s')) != 0;
        du_human = (flags & FILE_FLAG('h')) != 0;
        du_apparent = (flags & FILE_FLAG('b')) != 0;
        du_total = (flags & FILE_FLAG('c')) != 0;

        if (first >= count)
        {
                du_grand += du_walk((string_address) ".", FILE_MAX_DEPTH, true);
        }
        else
        {
                while (first < count)
                        du_grand += du_walk(program_argument((b32)first++),
                                            FILE_MAX_DEPTH, true);
        }

        if (du_total)
                du_report(du_grand, (string_address) "total");

        log_flush();

        return du_status;
}
