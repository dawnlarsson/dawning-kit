#include "../src/compiler_memory.c"
#include "counted.inc"

static positive fixed_oracle(p8 address_to out, positive magnitude,
                              positive scale, bool minus, positive width,
                              positive precision, positive flags)
{
        positive divisor = 1;
        for (positive i = precision; i < scale; i++) divisor *= 10;
        if (scale > precision)
        {
                positive remainder = magnitude % divisor;
                magnitude /= divisor;
                if (remainder > divisor / 2 ||
                    (remainder == divisor / 2 && magnitude % 2)) magnitude++;
                scale = precision;
        }
        p8 reverse[40], body[128];
        positive digits = 0, used = 0;
        do { reverse[digits++] = '0' + magnitude % 10; magnitude /= 10; }
        while (magnitude);
        while (digits <= scale) reverse[digits++] = '0';
        p8 sign = minus ? '-' : flags & 2 ? '+' : flags & 4 ? ' ' : 0;
        if (sign) body[used++] = sign;
        for (positive i = digits; i;)
        {
                if (i == scale) body[used++] = '.';
                body[used++] = reverse[--i];
        }
        if (!scale && (precision || (flags & 8))) body[used++] = '.';
        for (positive i = scale; i < precision; i++) body[used++] = '0';
        positive padding = width > used ? width - used : 0, at = 0;
        if (!(flags & 1))
        {
                if ((flags & 16) && sign) out[at++] = sign;
                for (positive i = 0; i < padding; i++) out[at++] = flags & 16 ? '0' : ' ';
        }
        positive skip = !(flags & 1) && (flags & 16) && sign ? 1 : 0;
        for (positive i = skip; i < used; i++) out[at++] = body[i];
        if (flags & 1)
                for (positive i = 0; i < padding; i++) out[at++] = ' ';
        return at;
}

static positive series_oracle(p8 address_to into, positive room,
                               positive length, positive first,
                               positive finish, bipolar step)
{
        if (!length || room < length || first >= finish || finish > length)
                return 0;
        positive used = length;
        while (room - used >= length)
        {
                for (positive i = 0; i < length; i++) into[used + i] = into[used - length + i];
                positive carry = step < 0 ? (positive)0 - (positive)step : (positive)step;
                for (positive i = finish; carry && i > first;)
                {
                        p8 address_to byte = into + used + --i;
                        if (*byte == '.') continue;
                        b32 digit = *byte - '0';
                        b32 part = carry % 10;
                        carry /= 10;
                        digit += step < 0 ? -part : part;
                        if (digit < 0) { digit += 10; carry++; }
                        if (digit >= 10) { digit -= 10; carry++; }
                        *byte = '0' + digit;
                }
                if (carry) break;
                used += length;
        }
        return used;
}

b32 main(void)
{
        const positive values[] = {0, 1, 5, 9, 10, 15, 25, 95, 99, 999,
            99995, 1000000000000000000ULL, 9223372036854775808ULL, positive_max};
        p8 actual[4096], expected[4096];
        for (positive v = 0; v < array_count(values); v++)
                for (positive scale = 0; scale <= 18; scale++)
                        for (positive precision = 0; precision <= 20; precision++)
                                for (positive flags = 0; flags < 32; flags++)
                                {
                                        positive width = (v * 13 + scale * 7 + flags) % 80;
                                        bool minus = (v + scale + precision) & 1;
                                        fixed_decimal field = fixed_decimal_prepare(values[v], scale,
                                            minus, width, precision, flags);
                                        positive wanted = fixed_oracle(expected, values[v], scale,
                                            minus, width, precision, flags);
                                        memory_fill(actual, 0xa5, sizeof(actual));
                                        check("fixed short buffer is untouched",
                                            fixed_decimal_into(actual, wanted - 1, address_of field) == 0 &&
                                            actual[0] == 0xa5 && actual[wanted - 1] == 0xa5);
                                        positive got = fixed_decimal_into(actual, wanted, address_of field);
                                        check("fixed exact decimal field", got == wanted &&
                                            !memory_compare(actual, expected, wanted) && actual[wanted] == 0xa5);
                                }
        const bipolar steps[] = {0, 1, -1, 3, -3, 25, -25, 999, -999,
                                 bipolar_max, bipolar_min};
        for (positive length = 1; length <= 64; length++)
                for (positive room = 0; room <= length * 9 + 1; room++)
                        for (positive s = 0; s < array_count(steps); s++)
                        {
                                positive first = length / 4, finish = length - length / 4;
                                memory_fill(actual, 0xa5, sizeof(actual));
                                for (positive at = 0; at < length; at++)
                                        actual[at] = at >= first && at < finish
                                            ? '0' + (at * 7 + room) % 10 : '#';
                                if (finish - first > 3) actual[finish - 3] = '.';
                                memory_copy_apart(expected, actual, sizeof(actual));
                                positive wanted = series_oracle(expected, room, length, first, finish, steps[s]);
                                positive got = memory_decimal_series(actual, room, length, first, finish, steps[s]);
                                check("decimal series exact carry and boundary", got == wanted &&
                                      !memory_compare(actual, expected, got));
                                bool intact = true;
                                for (positive at = max(room, length); at < sizeof(actual); at++)
                                        if (actual[at] != 0xa5) intact = false;
                                check("decimal series bounded writes", intact);
                        }
        check("decimal empty inaccessible span",
            memory_decimal_series(address_bad, 0, 0, 0, 0, 1) == 0 &&
            memory_decimal_series(address_bad, 0, 1, 0, 1, 1) == 0 &&
            memory_decimal_series(address_bad, 8, 8, 8, 8, 1) == 0 &&
            memory_decimal_series(address_bad, 8, 8, 0, 9, 1) == 0);
        // Cached short records must carry across every digit position without
        // touching literal prefixes, suffixes or a decimal point. All-nines
        // fields exercise rollover, including a final partial reservation.
        for (positive length = 4; length <= 8; length++)
                for (positive first = 0; first < length; first++)
                        for (positive finish = first + 1; finish <= length; finish++)
                                for (positive seed = 7; seed <= 9; seed++)
                                        for (positive point = first; point < finish; point++)
                                                for (positive extra = 0; extra <= length * 3 + 7; extra++)
                                                {
                                                        positive room = length + extra;
                                                        memory_fill(actual, 0xa5, sizeof(actual));
                                                        for (positive at = 0; at < length; at++)
                                                                actual[at] = at < first || at >= finish
                                                                    ? '#' : '9';
                                                        actual[finish - 1] = '0' + seed;
                                                        if (point > first && point + 1 < finish)
                                                                actual[point] = '.';
                                                        memory_copy_apart(expected, actual, sizeof(actual));
                                                        positive wanted = series_oracle(expected, room,
                                                            length, first, finish, 1);
                                                        positive got = memory_decimal_series(actual, room,
                                                            length, first, finish, 1);
                                                        check("decimal cached carry positions", got == wanted &&
                                                              !memory_compare(actual, expected, got));
                                                        bool intact = true;
                                                        for (positive at = room; at < sizeof(actual); at++)
                                                                if (actual[at] != 0xa5) intact = false;
                                                        check("decimal cached partial reservation", intact);
                                                }
        const positive quantum = 65536;
        p8 address_to guarded = memory(quantum * 3);
        bool mapped = guarded && (positive)guarded < positive_max - 4095;
        check("decimal guard mapping", mapped);
        if (mapped)
        {
                check("decimal left guard", system_call_3(syscall(mprotect),
                    (positive)guarded, quantum, 0) == 0);
                check("decimal right guard", system_call_3(syscall(mprotect),
                    (positive)(guarded + quantum * 2), quantum, 0) == 0);
                for (positive length = 1; length <= 128; length++)
                        for (positive extra = 0; extra <= length * 2; extra++)
                                for (positive edge = 0; edge < 2; edge++)
                                {
                                        positive room = length + extra;
                                        p8 address_to into = guarded +
                                            (edge ? quantum * 2 - room : quantum);
                                        for (positive i = 0; i < length; i++)
                                                into[i] = '0' + (i * 3 + extra) % 10;
                                        memory_copy_apart(expected, into, length);
                                        bipolar step = extra & 1 ? -1 : 1;
                                        positive wanted = series_oracle(expected, room,
                                            length, 0, length, step);
                                        positive got = memory_decimal_series(into, room,
                                            length, 0, length, step);
                                        check("decimal exact guard edge", got == wanted &&
                                            !memory_compare(into, expected, got));
                                        fixed_decimal field = fixed_decimal_prepare(positive_max,
                                            extra % 19, edge, extra, extra % 21, extra % 32);
                                        positive size = field.length + field.zeroes + field.padding;
                                        p8 address_to field_at = guarded +
                                            (edge ? quantum * 2 - size : quantum);
                                        check("fixed exact guard edge",
                                            fixed_decimal_into(field_at, size, address_of field) == size);
                                }
                memory_free(guarded, quantum * 3);
        }
        return test_report(null);
}
