#include "../compiler_memory.c"
#include "counted.inc"

/* Exercise the real mapping owner on Linux, including movement and the
   allocate/copy fallback. 64 KiB boundaries also suit larger-page ARM hosts. */
#define RESERVE_QUANTUM 65536

static fn reserve_boundaries(void)
{
        address_any held = null;
        positive have = 0, used = 0;

        check("initial reserve", memory_reserve(address_of held, address_of have,
              0, 1, sizeof(positive), 64));
        if (!held)
                return;
        check("geometric initial capacity", have == 64);
        positive address_to words = held;
        for (used = 0; used < have; used++)
                words[used] = used ^ 0xa55a;
        address_any before = held;
        check("capacity hit", memory_reserve(address_of held, address_of have,
              used, have, sizeof(positive), 64) && held == before);
        check("used beyond capacity is refused", !memory_reserve(
              address_of held, address_of have, have + 1, have + 1,
              sizeof(positive), 64));
        check("invalid count leaves owner alone", held == before && have == 64);
        check("growth preserves elements", memory_reserve(address_of held,
              address_of have, used, 65, sizeof(positive), 64));
        words = held;
        for (positive i = 0; i < used; i++)
                check("element survived", words[i] == (i ^ 0xa55a));
        before = held;
        positive old_room = have;
        check("overflow is refused", !memory_reserve(address_of held,
              address_of have, used, positive_max, sizeof(positive), 64));
        check("overflow leaves owner alone", held == before && have == old_room);
        memory_release(address_of held, address_of have, address_of used,
                       sizeof(positive));
        check("release resets owner", !held && !have && !used);
}

static fn reserve_mapping(bool split)
{
        positive half = RESERVE_QUANTUM;
        address_any allocation = memory(half * 2);
        check("mapping fixture allocated", allocation &&
              (positive)allocation < (positive)-4095);
        if (!allocation || (positive)allocation >= (positive)-4095)
                return;
        p8 address_to bytes = allocation;
        bytes[0] = 0x5a;
        bytes[half - 1] = 0xa5;
        bytes[half] = 0x7c;
        bipolar protected = system_call_3(syscall(mprotect),
              (positive)(bytes + half), half, split ? FILE_PROTECT_READ : 0);
        check("mapping protection split", protected == 0);
        if (protected)
        {
                memory_free(allocation, half * 2);
                return;
        }

        /* A guard beyond a single VMA forces movement. A read-only second
           VMA inside the owned range refuses a growing mremap and exercises
           its copy fallback instead. Both must preserve the owned bytes. */
        positive have = split ? half * 2 : half;
        positive used = have;
        bool grew = memory_reserve(address_of allocation, address_of have,
                                   used, have + 1, 1, half);
        check("split mapping grows", grew);
        check("blocked mapping moved", !grew || allocation != bytes);
        if (grew)
        {
                p8 address_to moved = allocation;
                check("mapped bytes survive", moved[0] == 0x5a &&
                      moved[half - 1] == 0xa5);
                if (split)
                {
                        check("fallback copied second VMA", moved[half] == 0x7c);
                        moved[half] = 0x6d;
                        check("fallback is writable", moved[half] == 0x6d);
                }
        }
        memory_release(address_of allocation, address_of have, address_of used, 1);
        if (!split)
                memory_free(bytes + half, half);
}

static fn reserve_sparse(void)
{
        positive have = 2 * 1024 * 1024;
        positive used = have;
        address_any held = memory(have);
        if (!held || (positive)held >= (positive)-4095)
        {
                check("sparse fixture allocated", false);
                return;
        }
        /* This test needs base-page residency, not a transparent huge-page
           fault populating its whole 2 MiB span when the first byte is set. */
        system_call_3(syscall(madvise), (positive)held, have, 15);
        ((p8 address_to)held)[0] = 0x3e;
        p8 residency[16] = {0};
        bipolar queried = system_call_3(syscall(mincore),
              (positive)held + RESERVE_QUANTUM, RESERVE_QUANTUM,
              (positive)residency);
        check("initial sparse residency query", queried == 0);
        bool sparse = queried == 0 && !(residency[0] & 1);
        check("sparse reserve grows", memory_reserve(address_of held,
              address_of have, used, used + 1, 1, 64));
        queried = system_call_3(syscall(mincore),
              (positive)held + RESERVE_QUANTUM, RESERVE_QUANTUM,
              (positive)residency);
        check("sparse residency query", queried == 0);
        if (sparse)
                check("growth did not populate untouched pages", !queried &&
                      !(residency[0] & 1));
        else
                log_direct(str("reserve: sparse RAM assertion NOT RUN -- pages already resident (huge-page advice may be ignored under emulation)\n"));
        check("sparse live byte preserved", ((p8 address_to)held)[0] == 0x3e);
        memory_release(address_of held, address_of have, address_of used, 1);
}

static fn reserve_refused(void)
{
        positive have = RESERVE_QUANTUM, used = 1;
        address_any held = memory(have);
        if (!held || (positive)held >= (positive)-4095)
        {
                check("refusal fixture allocated", false);
                return;
        }
        ((p8 address_to)held)[0] = 0x29;
        positive limits[2], constrained[2];
        bipolar answer = system_call_4(syscall(prlimit64), 0, 9, 0,
                                       (positive)limits);
        check("query address-space limit", answer == 0);
        if (!answer)
        {
                constrained[0] = 0;
                constrained[1] = limits[1];
                answer = system_call_4(syscall(prlimit64), 0, 9,
                                        (positive)constrained, 0);
                check("constrain own address space", answer == 0);
                if (!answer)
                {
                        address_any before = held;
                        bool grew = memory_reserve(address_of held,
                                address_of have, used, 64 * 1024 * 1024, 1, 64);
                        answer = system_call_4(syscall(prlimit64), 0, 9,
                                                (positive)limits, 0);
                        check("restore own address-space limit", answer == 0);
                        check("both remap and allocation refused", !grew);
                        check("refusal preserves mapping and capacity",
                              held == before && have == RESERVE_QUANTUM &&
                              ((p8 address_to)held)[0] == 0x29);
                }
        }
        memory_release(address_of held, address_of have, address_of used, 1);
}

b32 main(void)
{
        reserve_boundaries();
        reserve_mapping(false);
        reserve_mapping(true);
        reserve_sparse();
        /* User-mode emulators can satisfy guest mappings inside a host VMA
           reserved before the limit was lowered. This cannot prove native
           address-space refusal, so the runner names that case explicitly. */
        if (program_argument_count() > 1 &&
            !string_compare(program_argument(1), (string_address)"--emulated"))
                log_direct(str("reserve: address-space limit assertion NOT RUN under emulation\n"));
        else
                reserve_refused();
        return test_report(null);
}
