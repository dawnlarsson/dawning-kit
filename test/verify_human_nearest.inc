/*
        dd's nearest, ties-to-even byte formatter against the exact C body it
        replaced.  Eight output bytes are possible ("1023 KiB"), and the NUL
        is the ninth destination byte.

        Every architecture's normal verify lane includes this file.  Small
        values are complete, every pointer residue is crossed repeatedly,
        and each scale boundary, promotion boundary, exact half, and u64 edge
        is approached from both sides.
*/
#define HUMAN_NEAREST_DATA_MAX 8
#define HUMAN_NEAREST_CAPACITY 9
#define HUMAN_NEAREST_ROOM 64
#define HUMAN_NEAREST_HEAD 16

static positive reference_human_nearest(p8 address_to into, positive n,
                                        bool binary)
{
        positive base = binary ? 1024 : 1000;
        positive amount = n, tenths = 0, rounding = 0, exponent = 0;
        static p8 letters[] = {0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y', 'R', 'Q'};
        positive used = 0, fraction = 0;
        bool point = false;

        if (base <= amount)
        {
                do
                {
                        positive ten = (amount % base) * 10 + tenths;
                        positive two = (ten % base) * 2 + (rounding >> 1);

                        amount /= base;
                        tenths = ten / base;
                        rounding = two < base ? ((two + rounding) != 0)
                                              : 2 + (base < two + rounding);
                        exponent++;
                }
                while (base <= amount && exponent < 10);

                if (amount < 10)
                {
                        if (2 < rounding + (tenths & 1))
                        {
                                tenths++;
                                rounding = 0;

                                if (tenths == 10)
                                {
                                        amount++;
                                        tenths = 0;
                                }
                        }

                        if (amount < 10)
                        {
                                point = true;
                                fraction = tenths;
                                tenths = 0;
                                rounding = 0;
                        }
                }
        }

        if (5 < tenths + (0 < rounding + (amount & 1)))
        {
                amount++;

                if (amount == base && exponent < 10)
                {
                        exponent++;
                        point = true;
                        fraction = 0;
                        amount = 1;
                }
        }

        used += reference_into(into + used, amount);

        if (point)
        {
                into[used++] = '.';
                into[used++] = (p8)('0' + fraction);
        }

        into[used++] = ' ';

        if (exponent)
                into[used++] = !binary && exponent == 1 ? 'k' : letters[exponent];

        if (binary && exponent)
                into[used++] = 'i';

        into[used++] = 'B';
        into[used] = end;
        return used;
}

static fn check_human_nearest_one(positive value, bool binary,
                                  positive offset, p8 guard)
{
        p8 got_room[HUMAN_NEAREST_ROOM];
        p8 want_room[HUMAN_NEAREST_ROOM];

        reference_fill(got_room, guard, sizeof(got_room));
        reference_fill(want_room, guard, sizeof(want_room));

        p8 address_to got = got_room + HUMAN_NEAREST_HEAD + (offset & 15);
        p8 address_to want = want_room + HUMAN_NEAREST_HEAD + (offset & 15);
        positive want_length = reference_human_nearest(want, value, binary);
        positive got_length = positive_into_human_nearest_string(got, value, binary);

        same("positive_into_human_nearest_string", "length", got_length,
             want_length);
        same("positive_into_human_nearest_string", "eight data bytes at most",
             want_length <= HUMAN_NEAREST_DATA_MAX, true);
        same("positive_into_human_nearest_string", "data plus NUL fit capacity",
             want_length + 1 <= HUMAN_NEAREST_CAPACITY, true);
        same_bytes("positive_into_human_nearest_string",
                   "bytes, NUL, and every guard", got_room, want_room,
                   sizeof(got_room));
}

fn check_human_nearest()
{
        static p8 guards[] = {0, 0x55, 0xff};
        static positive fixed[] = {
            0, 1, 9, 10, 99, 999, 1000, 1023, 1024, 9999,
            999499, 999500, 999501, 999999, 1000000,
            1023 * 1024, 1024 * 1024 - 1, 1024 * 1024,
            (positive)-2, (positive)-1,
        };

        // Complete low domain.  Its index rotates all sixteen destination
        // residues and every poison without multiplying identical cases.
        for (positive binary = 0; binary < 2; binary++)
                for (positive value = 0; value <= 65535; value++)
                        check_human_nearest_one(value, (bool)binary,
                                                value & 15,
                                                guards[(value >> 4) %
                                                       sizeof(guards)]);

        // Every named edge at every residue and poison.
        for (positive binary = 0; binary < 2; binary++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive g = 0; g < sizeof(guards); g++)
                                for (positive i = 0;
                                     i < sizeof(fixed) / sizeof(fixed[0]); i++)
                                        check_human_nearest_one(fixed[i],
                                                                (bool)binary,
                                                                offset,
                                                                guards[g]);

        for (positive binary = 0; binary < 2; binary++)
        {
                positive base = binary ? 1024 : 1000;
                positive power = 1;
                positive serial = 0;

                for (positive unit = 1; unit <= 6; unit++)
                {
                        if (power > (positive)-1 / base)
                                break;
                        power *= base;

                        // Four thousand either side crosses the unit edge,
                        // its nearest promotion, and many tenths/integer
                        // rounding transitions at a rotating residue.
                        for (positive delta = 0; delta <= 4096; delta++)
                        {
                                if (power >= delta)
                                        check_human_nearest_one(power - delta,
                                                                (bool)binary,
                                                                serial++ & 15,
                                                                guards[serial %
                                                                       sizeof(guards)]);
                                if (power <= (positive)-1 - delta)
                                        check_human_nearest_one(power + delta,
                                                                (bool)binary,
                                                                serial++ & 15,
                                                                guards[serial %
                                                                       sizeof(guards)]);
                        }

                        // Exact halves and both neighbours prove ties-to-even
                        // at both odd and even retained digits.
                        positive half = power / 2;
                        for (positive q = 1; q <= 20; q++)
                                if (q <= ((positive)-1 - half) / power)
                                {
                                        positive tie = q * power + half;
                                        check_human_nearest_one(tie - 1,
                                                                (bool)binary,
                                                                serial++ & 15,
                                                                guards[serial %
                                                                       sizeof(guards)]);
                                        check_human_nearest_one(tie,
                                                                (bool)binary,
                                                                serial++ & 15,
                                                                guards[serial %
                                                                       sizeof(guards)]);
                                        check_human_nearest_one(tie + 1,
                                                                (bool)binary,
                                                                serial++ & 15,
                                                                guards[serial %
                                                                       sizeof(guards)]);
                                }
                }
        }

        for (positive binary = 0; binary < 2; binary++)
                for (positive i = 0; i < 65536; i++)
                {
                        positive value = next();
                        check_human_nearest_one(value, (bool)binary, i & 15,
                                                guards[(i >> 4) % sizeof(guards)]);
                }
}

fn check_wait_status_code()
{
        // The kernel-owned portion is sixteen bits.  Exhausting it includes
        // every exit byte, every signal value, and every otherwise-irrelevant
        // combination of the two.
        for (positive raw = 0; raw <= 65535; raw++)
        {
                positive signal = raw & 0x7f;
                b32 want = signal ? (b32)(128 + signal)
                                  : (b32)((raw >> 8) & 0xff);

                same("wait_status_code", "every raw 16-bit status",
                     (positive)wait_status_code(raw), (positive)want);
        }

        // High garbage is not part of wait status and must not leak into the
        // public answer.
        for (positive i = 0; i < 4096; i++)
        {
                positive raw = next();
                positive signal = raw & 0x7f;
                b32 want = signal ? (b32)(128 + signal)
                                  : (b32)((raw >> 8) & 0xff);

                same("wait_status_code", "ignores bits above wait4 status",
                     (positive)wait_status_code(raw), (positive)want);
        }
}
