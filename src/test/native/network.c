/* Exact lifted ARM64 unaligned wire accessors on native Apple ARM silicon. */
#include "network.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

u16 network_load_16(u8 *);
u32 network_load_32(u8 *);
void network_store_16(u8 *, u16);
void network_store_32(u8 *, u32);
int printf(const char *, ...);

static u64 seed = 0x9e3779b97f4a7c15ul;

static u32 next(void)
{
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return (u32)seed;
}

int main(void)
{
        u8 got[32], want[32];
        u64 checks = 0, bad = 0;

        for (u32 value = 0; value <= 0xffff; value++)
                for (u64 offset = 0; offset < 16; offset++)
                {
                        for (u64 i = 0; i < sizeof got; i++)
                                got[i] = want[i] = 0xa5;

                        want[offset] = (u8)(value >> 8);
                        want[offset + 1] = (u8)value;
                        network_store_16(got + offset, (u16)value);

                        checks++;
                        if (network_load_16(got + offset) != value)
                        {
                                bad++;
                                continue;
                        }

                        for (u64 i = 0; i < sizeof got; i++)
                                if (got[i] != want[i])
                                {
                                        bad++;
                                        break;
                                }
                }

        for (u64 round = 0; round < 1000000; round++)
        {
                u32 value = next();
                u64 offset = round & 15;

                for (u64 i = 0; i < sizeof got; i++)
                        got[i] = want[i] = 0x5a;

                want[offset] = (u8)(value >> 24);
                want[offset + 1] = (u8)(value >> 16);
                want[offset + 2] = (u8)(value >> 8);
                want[offset + 3] = (u8)value;
                network_store_32(got + offset, value);

                checks++;
                if (network_load_32(got + offset) != value)
                {
                        bad++;
                        continue;
                }

                for (u64 i = 0; i < sizeof got; i++)
                        if (got[i] != want[i])
                        {
                                bad++;
                                break;
                        }
        }

        printf("arm64 network fields: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}
