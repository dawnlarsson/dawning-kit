#include "../src/library.c"

// Proves the three spark regions are mapped with the right permissions:
// a writable global lives in .data, a zeroed one in .bss, and the code
// itself must be executable to be running at all.
p8 in_data[] = "data:starts-here";
p8 in_bss[64];

b32 main()
{
        // .bss must arrive zeroed
        positive nonzero = 0;
        for (positive i = 0; i < sizeof(in_bss); i++)
                if (in_bss[i])
                        nonzero++;

        string_format(log, "bss zeroed:   %s\n", nonzero ? "NO" : "yes");

        // .data must be readable with its initialiser intact
        string_format(log, "data initial: %s\n", in_data);

        // and writable
        in_data[0] = 'D';
        in_data[5] = '!';
        string_format(log, "data written: %s\n", in_data);

        in_bss[0] = 'B';
        in_bss[1] = 'S';
        in_bss[2] = 'S';
        in_bss[3] = 0;
        string_format(log, "bss written:  %s\n", in_bss);

        log_flush();
        return 0;
}
