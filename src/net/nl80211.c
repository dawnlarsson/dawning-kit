/*
        Experimental C standard library

        nl80211: join a station, leave it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_NL80211
#define STANDARD_MODERN_C_NET_NL80211

#define GENL_ID_CTRL 16
#define GENL_HEADER 4
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_MCAST_GROUPS 7
#define CTRL_ATTR_MCAST_GRP_NAME 1
#define CTRL_ATTR_MCAST_GRP_ID 2

#define NL80211_CMD_GET_WIPHY 1
#define NL80211_CMD_NEW_WIPHY 3
#define NL80211_CMD_GET_INTERFACE 5
#define NL80211_CMD_NEW_INTERFACE 7
#define NL80211_CMD_NEW_KEY 11
#define NL80211_CMD_GET_STATION 17
#define NL80211_CMD_SET_STATION 18
#define NL80211_CMD_NEW_STATION 19
#define NL80211_CMD_CONNECT 46
#define NL80211_CMD_DISCONNECT 48

#define NL80211_ATTR_WIPHY 1
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_IFNAME 4
#define NL80211_ATTR_IFTYPE 5
#define NL80211_ATTR_MAC 6
#define NL80211_ATTR_KEY_DATA 7
#define NL80211_ATTR_KEY_IDX 8
#define NL80211_ATTR_KEY_CIPHER 9
#define NL80211_ATTR_KEY_SEQ 10
#define NL80211_ATTR_KEY_DEFAULT 11
#define NL80211_ATTR_SSID 52
#define NL80211_ATTR_AUTH_TYPE 53
#define NL80211_ATTR_KEY_TYPE 55
#define NL80211_ATTR_TIMED_OUT 65
#define NL80211_ATTR_STA_FLAGS2 67
#define NL80211_ATTR_CONTROL_PORT 68
#define NL80211_ATTR_PRIVACY 70
#define NL80211_ATTR_STATUS_CODE 72
#define NL80211_ATTR_CIPHER_SUITES_PAIRWISE 73
#define NL80211_ATTR_CIPHER_SUITE_GROUP 74
#define NL80211_ATTR_WPA_VERSIONS 75
#define NL80211_ATTR_AKM_SUITES 76
#define NL80211_ATTR_REQ_IE 77
#define NL80211_ATTR_EXT_FEATURES 217
#define NL80211_ATTR_PMK 254

#define NL80211_IFTYPE_STATION 2
#define NL80211_AUTHTYPE_OPEN 0
#define NL80211_WPA_VERSION_2 2
#define NL80211_KEYTYPE_GROUP 0
#define NL80211_KEYTYPE_PAIRWISE 1
#define NL80211_STA_FLAG_AUTHORIZED 1
#define NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK 15
#define WLAN_CIPHER_CCMP 0x000fac04u
#define WLAN_AKM_PSK 0x000fac02u
#define NL80211_CONNECT_SECONDS 20
#define WIFI_EAPOL_SECONDS 8
#define WIFI_EAPOL_HDR 99
#define WIFI_GTK_WRAP 24
#define WIFI_WRAP_MOST 408

typedef struct
{
        b32 handle;
        p16 family;
        p32 mlme;
} nl80211;

typedef struct
{
        p32 index;
        p32 wiphy;
        bool found;
        bool has_mac;
        p8 mac[6];
        p8 name[IFNAME_SIZE];
} nl80211_iface;

typedef struct
{
        p32 wiphy;
        p32 seen;
        bool offload;
} nl80211_wiphy_query;

static bipolar nl80211_disconnect(nl80211 address_to session, p32 index);

typedef struct
{
        p16 family;
        p32 mlme;
} nl80211_family_info;

static bool nl80211_begin(netlink_buffer address_to buffer, p16 family, p8 command,
                          p16 flags, p32 sequence)
{
        p8 address_to body;

        if (!netlink_begin(buffer, family, flags, sequence, GENL_HEADER))
                return false;

        body = (p8 address_to)netlink_body(buffer);
        body[0] = command;
        body[1] = 1;
        body[2] = 0;
        body[3] = 0;
        return true;
}

static bool nl80211_attribute_u32(netlink_buffer address_to buffer, p16 type,
                                  p32 value)
{
        return netlink_attribute_add(buffer, type, address_of value, 4);
}

static p32 nl80211_find_u32(netlink_header address_to header, p16 type, p32 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 4)
                return missing;
        return memory_load_unaligned(p32, at);
}

static p16 nl80211_find_u16(netlink_header address_to header, p16 type,
                            p16 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 2)
                return missing;
        return (p16)memory_load_unaligned(p16, at);
}

static bool nl80211_attr(netlink_header address_to header, p16 type)
{
        return netlink_find(header, GENL_HEADER, type, null) != null;
}

static bool nl80211_family_seen(netlink_header address_to header,
                                address_any context)
{
        nl80211_family_info address_to info = (nl80211_family_info address_to)context;
        positive size = 0;
        positive groups_length = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER,
                                                       CTRL_ATTR_FAMILY_ID,
                                                       address_of size);
        p8 address_to groups;
        positive cursor = 0;

        if (at && size >= 2)
                info->family = memory_load_unaligned(p16, at);

        groups = (p8 address_to)netlink_find(header, GENL_HEADER,
                                             CTRL_ATTR_MCAST_GROUPS,
                                             address_of groups_length);
        while (groups && cursor + sizeof(netlink_attribute) <= groups_length)
        {
                netlink_attribute address_to attribute =
                    (netlink_attribute address_to)(groups + cursor);
                positive payload;
                p8 address_to name;
                p8 address_to id;
                positive name_length = 0;
                positive id_length = 0;

                if (attribute->length < sizeof(netlink_attribute) ||
                    cursor + attribute->length > groups_length)
                        break;
                payload = attribute->length - sizeof(netlink_attribute);
                name = (p8 address_to)netlink_find_span(
                    groups + cursor + sizeof(netlink_attribute), payload,
                    CTRL_ATTR_MCAST_GRP_NAME, address_of name_length);
                id = (p8 address_to)netlink_find_span(
                    groups + cursor + sizeof(netlink_attribute), payload,
                    CTRL_ATTR_MCAST_GRP_ID, address_of id_length);
                if (name && name_length >= 4 && id && id_length >= 4 &&
                    !memory_compare(name, "mlme", 4))
                        info->mlme = memory_load_unaligned(p32, id);
                cursor += netlink_align(attribute->length);
        }

        return false;
}

static bipolar nl80211_open(nl80211 address_to session)
{
        netlink_buffer request = {0};
        p32 sequence;
        p8 name[] = "nl80211";
        bipolar handle;
        nl80211_family_info info = {0};

        memory_fill(session, 0, sizeof(*session));
        session->handle = -1;

        handle = netlink_open_protocol(NETLINK_GENERIC, 0);
        if (handle < 0)
                return handle;

        sequence = netlink_sequence_take();
        if (!nl80211_begin(address_of request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
                           NLM_REQUEST | NLM_ACK, sequence))
        {
                socket_close((b32)handle);
                return -1;
        }

        netlink_attribute_add(address_of request, CTRL_ATTR_FAMILY_NAME, name,
                              sizeof(name));

        if (netlink_transact((b32)handle, address_of request, sequence,
                             nl80211_family_seen, address_of info) < 0 ||
            !info.family)
        {
                socket_close((b32)handle);
                return -19;
        }

        if (info.mlme &&
            socket_option_set((b32)handle, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                              address_of info.mlme, sizeof(info.mlme)) >= 0)
                session->mlme = info.mlme;

        session->handle = (b32)handle;
        session->family = info.family;
        return 0;
}

static fn nl80211_close(nl80211 address_to session)
{
        if (session->handle >= 0)
                socket_close(session->handle);
        session->handle = -1;
}

static bool nl80211_iface_seen(netlink_header address_to header,
                               address_any context)
{
        nl80211_iface address_to found = (nl80211_iface address_to)context;
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        p32 type;
        p32 index;
        positive length = 0;
        string_address name;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_INTERFACE &&
            body[0] != NL80211_CMD_GET_INTERFACE)
                return true;

        type = nl80211_find_u32(header, NL80211_ATTR_IFTYPE, 0);
        if (found->found && type != NL80211_IFTYPE_STATION)
                return true;

        index = nl80211_find_u32(header, NL80211_ATTR_IFINDEX, 0);
        if (!index)
                return true;

        name = (string_address)netlink_find(header, GENL_HEADER,
                                            NL80211_ATTR_IFNAME,
                                            address_of length);
        found->index = index;
        found->found = true;
        found->wiphy = nl80211_find_u32(header, NL80211_ATTR_WIPHY, 0);
        found->name[0] = end;
        if (name && length)
        {
                if (length >= IFNAME_SIZE)
                        length = IFNAME_SIZE - 1;
                memory_copy(found->name, name, length);
                found->name[length] = end;
        }

        {
                positive mac_length = 0;
                p8 address_to mac = (p8 address_to)netlink_find(
                    header, GENL_HEADER, NL80211_ATTR_MAC, address_of mac_length);

                if (mac && mac_length >= 6)
                {
                        memory_copy(found->mac, mac, 6);
                        found->has_mac = true;
                }
        }

        return type != NL80211_IFTYPE_STATION;
}

static bipolar nl80211_interface(nl80211 address_to session,
                                 nl80211_iface address_to found)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        memory_fill(found, 0, sizeof(*found));
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_INTERFACE,
                           NLM_REQUEST | NLM_DUMP, sequence))
                return -1;

        return netlink_transact(session->handle, address_of request, sequence,
                                nl80211_iface_seen, found) < 0
                   ? -1
                   : (found->found ? 0 : -19);
}

static fn wifi_hmac_sha1(p8 address_to key, positive key_length,
                         p8 address_to data, positive length, p8 address_to out)
{
        digest_state inner;
        digest_state outer;
        p8 pad[64];
        p8 inner_sum[20];
        p8 key_block[64];
        p8 hashed[20];
        positive i;

        memory_fill(key_block, 0, 64);
        if (key_length > 64)
        {
                digest_open(address_of inner, DIGEST_SHA1, 20);
                digest_write(address_of inner, key, key_length);
                digest_close(address_of inner, hashed);
                memory_copy(key_block, hashed, 20);
        }
        else
                memory_copy(key_block, key, key_length);

        for (i = 0; i < 64; i++)
                pad[i] = key_block[i] ^ 0x36;

        digest_open(address_of inner, DIGEST_SHA1, 20);
        digest_write(address_of inner, pad, 64);
        digest_write(address_of inner, data, length);
        digest_close(address_of inner, inner_sum);

        for (i = 0; i < 64; i++)
                pad[i] = key_block[i] ^ 0x5c;

        digest_open(address_of outer, DIGEST_SHA1, 20);
        digest_write(address_of outer, pad, 64);
        digest_write(address_of outer, inner_sum, 20);
        digest_close(address_of outer, out);

        crypto_forget(key_block, sizeof(key_block));
        crypto_forget(pad, sizeof(pad));
        crypto_forget(inner_sum, sizeof(inner_sum));
        crypto_forget(hashed, sizeof(hashed));
}

static p8 wifi_nibble(p8 byte)
{
        if (byte_is_digit(byte))
                return (p8)(byte - '0');
        byte = byte_to_lower(byte);
        return (p8)(byte - 'a' + 10);
}

static bool wifi_psk(p8 address_to ssid, positive ssid_length,
                     p8 address_to pass, positive pass_length, p8 address_to pmk)
{
        p8 block[36];
        p8 last[20];
        p8 mix[20];
        positive round;
        positive which;
        positive i;

        if (pass_length == 64)
        {
                for (i = 0; i < 64; i++)
                        if (!byte_is_hexadecimal(pass[i]))
                                return false;
                for (i = 0; i < 32; i++)
                        pmk[i] = (p8)((wifi_nibble(pass[i * 2]) << 4) |
                                      wifi_nibble(pass[i * 2 + 1]));
                return true;
        }

        if (pass_length < 8 || pass_length > 63 || !ssid_length ||
            ssid_length > 32)
                return false;

        memory_copy(block, ssid, ssid_length);
        for (which = 1; which <= 2; which++)
        {
                block[ssid_length] = 0;
                block[ssid_length + 1] = 0;
                block[ssid_length + 2] = 0;
                block[ssid_length + 3] = (p8)which;
                wifi_hmac_sha1(pass, pass_length, block, ssid_length + 4, last);
                memory_copy(mix, last, 20);
                for (round = 1; round < 4096; round++)
                {
                        wifi_hmac_sha1(pass, pass_length, last, 20, last);
                        for (i = 0; i < 20; i++)
                                mix[i] ^= last[i];
                }
                memory_copy(pmk + (which - 1) * 20, mix,
                            which == 1 ? 20 : 12);
        }

        crypto_forget(block, sizeof(block));
        crypto_forget(last, sizeof(last));
        crypto_forget(mix, sizeof(mix));
        return true;
}

static fn wifi_put_be16(p8 address_to bytes, p16 value)
{
        bytes[0] = (p8)(value >> 8);
        bytes[1] = (p8)value;
}

static p16 wifi_be16(p8 address_to bytes)
{
        return (p16)(((p16)bytes[0] << 8) | bytes[1]);
}

static b32 wifi_compare(p8 address_to left, p8 address_to right, positive length)
{
        positive at;

        for (at = 0; at < length; at++)
                if (left[at] != right[at])
                        return (b32)left[at] - (b32)right[at];
        return 0;
}

static p8 wifi_xtime(p8 value)
{
        return (p8)((value << 1) ^ ((value & 0x80) ? 0x1b : 0));
}

static p8 wifi_gmul(p8 left, p8 right)
{
        p8 product = 0;
        positive bit;

        for (bit = 0; bit < 8; bit++)
        {
                if (right & 1)
                        product ^= left;
                left = wifi_xtime(left);
                right >>= 1;
        }

        return product;
}

static fn wifi_aes_decrypt(p8 address_to key, p8 address_to in, p8 address_to out)
{
        static const p8 inverse[256] = {
            0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3,
            0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f,
            0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54,
            0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b,
            0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24,
            0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8,
            0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d,
            0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
            0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84, 0x90, 0xd8, 0xab,
            0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3,
            0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1,
            0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
            0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6,
            0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9,
            0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d,
            0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
            0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0,
            0xfe, 0x78, 0xcd, 0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07,
            0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60,
            0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f,
            0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5,
            0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b,
            0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55,
            0x21, 0x0c, 0x7d};
        p8 round[176];
        p8 state[16];
        p8 hold[16];
        positive step;
        positive row;
        p8 temp;

        crypto_aes128_expand(key, round);
        memory_copy(state, in, 16);
        for (step = 0; step < 16; step++)
                state[step] ^= round[160 + step];

        for (step = 9; step >= 1; step--)
        {
                temp = state[1];
                state[1] = state[13];
                state[13] = state[9];
                state[9] = state[5];
                state[5] = temp;
                temp = state[2];
                state[2] = state[10];
                state[10] = temp;
                temp = state[6];
                state[6] = state[14];
                state[14] = temp;
                temp = state[3];
                state[3] = state[7];
                state[7] = state[11];
                state[11] = state[15];
                state[15] = temp;
                for (row = 0; row < 16; row++)
                        state[row] = inverse[state[row]];
                for (row = 0; row < 16; row++)
                        state[row] ^= round[step * 16 + row];
                memory_copy(hold, state, 16);
                for (row = 0; row < 4; row++)
                {
                        p8 address_to column = hold + row * 4;

                        state[row * 4] = (p8)(wifi_gmul(column[0], 0x0e) ^
                                              wifi_gmul(column[1], 0x0b) ^
                                              wifi_gmul(column[2], 0x0d) ^
                                              wifi_gmul(column[3], 0x09));
                        state[row * 4 + 1] = (p8)(wifi_gmul(column[0], 0x09) ^
                                                  wifi_gmul(column[1], 0x0e) ^
                                                  wifi_gmul(column[2], 0x0b) ^
                                                  wifi_gmul(column[3], 0x0d));
                        state[row * 4 + 2] = (p8)(wifi_gmul(column[0], 0x0d) ^
                                                  wifi_gmul(column[1], 0x09) ^
                                                  wifi_gmul(column[2], 0x0e) ^
                                                  wifi_gmul(column[3], 0x0b));
                        state[row * 4 + 3] = (p8)(wifi_gmul(column[0], 0x0b) ^
                                                  wifi_gmul(column[1], 0x0d) ^
                                                  wifi_gmul(column[2], 0x09) ^
                                                  wifi_gmul(column[3], 0x0e));
                }
        }

        temp = state[1];
        state[1] = state[13];
        state[13] = state[9];
        state[9] = state[5];
        state[5] = temp;
        temp = state[2];
        state[2] = state[10];
        state[10] = temp;
        temp = state[6];
        state[6] = state[14];
        state[14] = temp;
        temp = state[3];
        state[3] = state[7];
        state[7] = state[11];
        state[11] = state[15];
        state[15] = temp;
        for (row = 0; row < 16; row++)
                state[row] = inverse[state[row]];
        for (row = 0; row < 16; row++)
                state[row] ^= round[row];
        memory_copy(out, state, 16);
        crypto_forget(round, sizeof(round));
        crypto_forget(state, sizeof(state));
        crypto_forget(hold, sizeof(hold));
}

static bool wifi_kw_unwrap(p8 address_to kek, p8 address_to wrap, positive length,
                           p8 address_to plain, positive address_to plain_length)
{
        p8 block[16];
        p8 a[8];
        p8 r[WIFI_WRAP_MOST - 8];
        positive words;
        positive round;
        positive i;
        p64 t;

        if (length < 24 || (length & 7) || length > WIFI_WRAP_MOST)
                return false;
        words = (length / 8) - 1;
        memory_copy(a, wrap, 8);
        memory_copy(r, wrap + 8, words * 8);
        for (round = 6; round > 0; round--)
                for (i = words; i > 0; i--)
                {
                        t = (p64)words * (round - 1) + i;
                        crypto_put_be64(a, crypto_be64(a) ^ t);
                        memory_copy(block, a, 8);
                        memory_copy(block + 8, r + (i - 1) * 8, 8);
                        wifi_aes_decrypt(kek, block, block);
                        memory_copy(a, block, 8);
                        memory_copy(r + (i - 1) * 8, block + 8, 8);
                }

        if (crypto_be64(a) != 0xa6a6a6a6a6a6a6a6ull)
                return false;
        memory_copy(plain, r, words * 8);
        address_to plain_length = words * 8;
        crypto_forget(block, sizeof(block));
        crypto_forget(a, sizeof(a));
        crypto_forget(r, sizeof(r));
        return true;
}

static fn wifi_ptk(p8 address_to pmk, p8 address_to ap, p8 address_to sta,
                   p8 address_to anonce, p8 address_to snonce, p8 address_to ptk)
{
        p8 label[] = "Pairwise key expansion";
        p8 data[6 + 6 + 32 + 32];
        p8 input[22 + 1 + 76 + 1];
        p8 hash[20];
        p8 address_to min_mac;
        p8 address_to max_mac;
        p8 address_to min_nonce;
        p8 address_to max_nonce;
        positive used = 0;
        positive which;

        if (wifi_compare(ap, sta, 6) < 0)
        {
                min_mac = ap;
                max_mac = sta;
        }
        else
        {
                min_mac = sta;
                max_mac = ap;
        }
        if (wifi_compare(anonce, snonce, 32) < 0)
        {
                min_nonce = anonce;
                max_nonce = snonce;
        }
        else
        {
                min_nonce = snonce;
                max_nonce = anonce;
        }

        memory_copy(data, min_mac, 6);
        memory_copy(data + 6, max_mac, 6);
        memory_copy(data + 12, min_nonce, 32);
        memory_copy(data + 44, max_nonce, 32);
        memory_copy(input, label, 22);
        input[22] = 0;
        memory_copy(input + 23, data, 76);
        for (which = 0; used < 64; which++)
        {
                input[99] = (p8)which;
                wifi_hmac_sha1(pmk, 32, input, 100, hash);
                memory_copy(ptk + used, hash, used + 20 > 64 ? 64 - used : 20);
                used += 20;
        }
        crypto_forget(data, sizeof(data));
        crypto_forget(input, sizeof(input));
        crypto_forget(hash, sizeof(hash));
}

static const p8 wifi_rsn_ie[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
                                 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};

static bool nl80211_ext_bit(p8 address_to bits, positive length, positive which)
{
        return which / 8 < length && (bits[which / 8] & (1u << (which % 8)));
}

static bool nl80211_wiphy_seen(netlink_header address_to header, address_any context)
{
        nl80211_wiphy_query address_to query = (nl80211_wiphy_query address_to)context;
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        positive size = 0;
        p8 address_to bits;
        p32 wiphy;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_WIPHY && body[0] != NL80211_CMD_GET_WIPHY)
                return true;

        wiphy = nl80211_find_u32(header, NL80211_ATTR_WIPHY, query->seen);
        if (nl80211_attr(header, NL80211_ATTR_WIPHY))
                query->seen = wiphy;
        if (wiphy != query->wiphy)
                return true;

        bits = (p8 address_to)netlink_find(
            header, GENL_HEADER, NL80211_ATTR_EXT_FEATURES, address_of size);
        if (bits && nl80211_ext_bit(bits, size,
                                    NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK))
                query->offload = true;
        return true;
}

static bool nl80211_psk_offload(nl80211 address_to session, p32 wiphy)
{
        netlink_buffer request = {0};
        p32 sequence;
        nl80211_wiphy_query query = {.wiphy = wiphy, .seen = ~0u};

        sequence = netlink_sequence_take();
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_WIPHY, NLM_REQUEST | NLM_DUMP,
                           sequence))
                return false;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_WIPHY, wiphy);
        if (netlink_transact(session->handle, address_of request, sequence,
                             nl80211_wiphy_seen, address_of query) < 0)
                return false;
        return query.offload;
}

static bool nl80211_station_seen(netlink_header address_to header,
                                 address_any context)
{
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        p8 address_to into = (p8 address_to)context;
        positive length = 0;
        p8 address_to mac;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_STATION &&
            body[0] != NL80211_CMD_GET_STATION)
                return true;
        mac = (p8 address_to)netlink_find(header, GENL_HEADER, NL80211_ATTR_MAC,
                                          address_of length);
        if (mac && length >= 6)
        {
                memory_copy(into, mac, 6);
                return false;
        }
        return true;
}

static bool nl80211_station(nl80211 address_to session, p32 index, p8 address_to mac)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        memory_fill(mac, 0, 6);
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_STATION, NLM_REQUEST | NLM_DUMP,
                           sequence))
                return false;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        if (netlink_transact(session->handle, address_of request, sequence,
                             nl80211_station_seen, mac) < 0)
                return false;
        return mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5];
}

static bipolar nl80211_new_key(nl80211 address_to session, p32 index, p8 idx,
                               p32 type, p8 address_to mac, p8 address_to key,
                               positive key_length, p8 address_to seq,
                               positive seq_length, bool group_default)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 ccmp = WLAN_CIPHER_CCMP;

        if (!nl80211_begin(address_of request, session->family, NL80211_CMD_NEW_KEY,
                           NLM_REQUEST | NLM_ACK, sequence))
                return -1;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_KEY_DATA, key,
                              key_length);
        netlink_attribute_add(address_of request, NL80211_ATTR_KEY_IDX, address_of idx,
                              1);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_KEY_CIPHER, ccmp);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_KEY_TYPE, type);
        if (mac)
                netlink_attribute_add(address_of request, NL80211_ATTR_MAC, mac, 6);
        if (seq && seq_length)
                netlink_attribute_add(address_of request, NL80211_ATTR_KEY_SEQ, seq,
                                      seq_length);
        if (group_default)
                netlink_attribute_add(address_of request, NL80211_ATTR_KEY_DEFAULT,
                                      null, 0);
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

static bipolar nl80211_authorize(nl80211 address_to session, p32 index,
                                 p8 address_to mac)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 flags[2];

        flags[0] = (p32)1 << NL80211_STA_FLAG_AUTHORIZED;
        flags[1] = flags[0];
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_SET_STATION, NLM_REQUEST | NLM_ACK,
                           sequence))
                return -1;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_MAC, mac, 6);
        netlink_attribute_add(address_of request, NL80211_ATTR_STA_FLAGS2, flags,
                              sizeof(flags));
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

static bipolar nl80211_eapol_open(p32 index)
{
        socket_address_packet self = {
            .family = AF_PACKET,
            .protocol = network_order_16(ETH_P_PAE),
            .index = index,
        };
        bipolar handle = socket_new(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC,
                                    (b32)network_order_16(ETH_P_PAE));

        if (handle < 0)
                return handle;
        if (socket_bind((b32)handle, address_of self, sizeof(self)) < 0)
        {
                socket_close((b32)handle);
                return -1;
        }
        return handle;
}

static fn wifi_eapol_mic(p8 address_to kck, p8 address_to frame, positive length)
{
        p8 hash[20];
        p8 saved[16];

        memory_copy(saved, frame + 81, 16);
        memory_fill(frame + 81, 0, 16);
        wifi_hmac_sha1(kck, 16, frame, length, hash);
        memory_copy(frame + 81, hash, 16);
        crypto_forget(hash, sizeof(hash));
        crypto_forget(saved, sizeof(saved));
}

static bipolar wifi_eapol_send(b32 handle, p32 index, p8 address_to bssid,
                               p8 address_to frame, positive length)
{
        socket_address_packet to = {.family = AF_PACKET,
                                    .protocol = network_order_16(ETH_P_PAE),
                                    .index = index,
                                    .halen = 6};

        memory_copy(to.addr, bssid, 6);
        return socket_send(handle, frame, length, 0, address_of to, sizeof(to)) ==
                       (bipolar)length
                   ? 0
                   : -1;
}

static bipolar wifi_handshake(nl80211 address_to session, p32 index, b32 eapol,
                              p8 address_to sta, p8 address_to bssid,
                              p8 address_to pmk)
{
        p8 snonce[32];
        p8 anonce[32];
        p8 ptk[64];
        p8 gtk[32];
        p8 rsc[8];
        p8 frame[512];
        p8 replay[8];
        socket_address_packet from;
        network_deadline deadline;
        positive gtk_length = 0;
        p8 gtk_idx = 1;
        bool have_anonce = false;
        bipolar got;

        if (system_random_fill(snonce, sizeof(snonce), 0) < 0)
                return -1;
        if (!network_deadline_begin(address_of deadline, WIFI_EAPOL_SECONDS, 0))
        {
                crypto_forget(snonce, sizeof(snonce));
                return -1;
        }

        while (network_wait_readable_until(eapol, address_of deadline) > 0)
        {
                p32 from_length = sizeof(from);
                p16 info;
                positive length;
                positive data_length;
                p8 address_to data;

                memory_fill(address_of from, 0, sizeof(from));
                got = socket_receive(eapol, frame, sizeof(frame), 0, address_of from,
                                     address_of from_length);
                if (got < WIFI_EAPOL_HDR)
                        continue;
                length = (positive)got;
                if (frame[1] != 3 || frame[4] != 2)
                        continue;
                info = wifi_be16(frame + 5);
                if ((info & 7) != 2 || !(info & 8))
                        continue;
                if (!(info & 0x80))
                        continue;

                if (!(info & 0x100))
                {
                        memory_copy(anonce, frame + 17, 32);
                        memory_copy(replay, frame + 9, 8);
                        have_anonce = true;
                        wifi_ptk(pmk, bssid, sta, anonce, snonce, ptk);
                        memory_fill(frame, 0, WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        frame[0] = 1;
                        frame[1] = 3;
                        wifi_put_be16(frame + 2, (p16)(95 + sizeof(wifi_rsn_ie)));
                        frame[4] = 2;
                        wifi_put_be16(frame + 5, 0x010a);
                        wifi_put_be16(frame + 7, 16);
                        memory_copy(frame + 9, replay, 8);
                        memory_copy(frame + 17, snonce, 32);
                        wifi_put_be16(frame + 97, (p16)sizeof(wifi_rsn_ie));
                        memory_copy(frame + 99, wifi_rsn_ie, sizeof(wifi_rsn_ie));
                        wifi_eapol_mic(ptk, frame, WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        wifi_eapol_send(eapol, index, bssid, frame,
                                        WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        continue;
                }

                if (!have_anonce || !(info & 0x1000))
                        continue;

                data_length = wifi_be16(frame + 97);
                if (WIFI_EAPOL_HDR + data_length > length)
                        continue;
                {
                        p8 mic[16];
                        p8 check[20];

                        memory_copy(mic, frame + 81, 16);
                        memory_fill(frame + 81, 0, 16);
                        wifi_hmac_sha1(ptk, 16, frame, WIFI_EAPOL_HDR + data_length,
                                       check);
                        if (memory_compare(mic, check, 16))
                        {
                                crypto_forget(check, sizeof(check));
                                continue;
                        }
                        crypto_forget(check, sizeof(check));
                }

                data = frame + 99;
                if (data_length >= WIFI_GTK_WRAP)
                {
                        p8 unwrapped[WIFI_WRAP_MOST];
                        positive plain = 0;
                        positive at = 0;

                        if (!wifi_kw_unwrap(ptk + 16, data, data_length, unwrapped,
                                            address_of plain) &&
                            !(data_length >= 8 + WIFI_GTK_WRAP &&
                              wifi_kw_unwrap(ptk + 16, data + 8, data_length - 8,
                                             unwrapped, address_of plain)))
                                plain = 0;

                        while (at + 2 <= plain)
                        {
                                p8 tag = unwrapped[at];
                                p8 room = unwrapped[at + 1];

                                if (at + 2 + room > plain)
                                        break;
                                if (tag == 0xdd && room >= 8 &&
                                    unwrapped[at + 2] == 0 &&
                                    unwrapped[at + 3] == 0x0f &&
                                    unwrapped[at + 4] == 0xac &&
                                    unwrapped[at + 5] == 1)
                                {
                                        gtk_idx = (p8)(unwrapped[at + 6] & 3);
                                        gtk_length = room - 6;
                                        if (gtk_length > sizeof(gtk))
                                                gtk_length = sizeof(gtk);
                                        memory_copy(gtk, unwrapped + at + 8, gtk_length);
                                        break;
                                }
                                at += 2 + room;
                        }
                        crypto_forget(unwrapped, sizeof(unwrapped));
                }

                memory_copy(replay, frame + 9, 8);
                memory_copy(rsc, frame + 65, 8);
                memory_fill(frame, 0, WIFI_EAPOL_HDR);
                frame[0] = 1;
                frame[1] = 3;
                wifi_put_be16(frame + 2, 95);
                frame[4] = 2;
                wifi_put_be16(frame + 5, 0x030a);
                wifi_put_be16(frame + 7, 16);
                memory_copy(frame + 9, replay, 8);
                wifi_eapol_mic(ptk, frame, WIFI_EAPOL_HDR);
                if (wifi_eapol_send(eapol, index, bssid, frame, WIFI_EAPOL_HDR) < 0)
                        break;
                got = nl80211_new_key(session, index, 0, NL80211_KEYTYPE_PAIRWISE,
                                      bssid, ptk + 32, 16, null, 0, false);
                if (!got && gtk_length >= 16 && gtk_idx)
                        got = nl80211_new_key(session, index, gtk_idx,
                                              NL80211_KEYTYPE_GROUP, null, gtk, 16,
                                              rsc, 6, true);
                if (!got)
                        nl80211_authorize(session, index, bssid);
                crypto_forget(snonce, sizeof(snonce));
                crypto_forget(anonce, sizeof(anonce));
                crypto_forget(ptk, sizeof(ptk));
                crypto_forget(gtk, sizeof(gtk));
                crypto_forget(rsc, sizeof(rsc));
                crypto_forget(frame, sizeof(frame));
                return got;
        }

        crypto_forget(snonce, sizeof(snonce));
        crypto_forget(anonce, sizeof(anonce));
        crypto_forget(ptk, sizeof(ptk));
        crypto_forget(gtk, sizeof(gtk));
        crypto_forget(rsc, sizeof(rsc));
        crypto_forget(frame, sizeof(frame));
        return -110;
}

static bool wifi_link_mac(string_address name, p8 address_to mac)
{
        netlink_search search;
        bipolar handle = netlink_open_groups(0);

        memory_fill(address_of search, 0, sizeof(search));
        if (handle < 0)
                return false;
        search.wanted = name;
        if (netlink_link_find((b32)handle, address_of search) < 0 ||
            !search.has_hardware)
        {
                socket_close((b32)handle);
                return false;
        }
        memory_copy(mac, search.hardware, 6);
        socket_close((b32)handle);
        return true;
}

static bipolar nl80211_wait_associated(nl80211 address_to session, p32 sequence,
                                       p32 index, p8 address_to bssid)
{
        netlink_buffer reply = {0};
        network_deadline deadline;
        bool got_ack = false;
        bool associated = false;
        bipolar ack = 0;
        p64 last_poll = 0;

        if (!network_deadline_begin(address_of deadline, NL80211_CONNECT_SECONDS, 0))
                return -1;

        for (;;)
        {
                p32 local_port = 0;
                bipolar got;
                positive at = 0;

                if (got_ack && !associated && !session->mlme)
                {
                        p64 now = clock_monotonic_nanoseconds();

                        if (!last_poll || now - last_poll >= 200000000)
                        {
                                last_poll = now;
                                if (nl80211_station(session, index, bssid))
                                {
                                        netlink_forget(address_of reply);
                                        return 0;
                                }
                        }
                }

                got = network_wait_readable_until(session->handle, address_of deadline);
                if (got <= 0)
                {
                        netlink_forget(address_of reply);
                        if (got_ack && !associated &&
                            nl80211_station(session, index, bssid))
                                return 0;
                        return got < 0 ? got : -110;
                }

                got = netlink_receive(session->handle, address_of reply,
                                      address_of local_port);
                if (got == NETWORK_INTERRUPTED)
                        continue;
                if (got < 0)
                {
                        netlink_forget(address_of reply);
                        return got;
                }

                while (at + NETLINK_HEADER <= reply.used)
                {
                        netlink_header address_to header =
                            (netlink_header address_to)(reply.bytes + at);
                        p8 address_to body;

                        if (header->length < NETLINK_HEADER ||
                            at + header->length > reply.used)
                                break;
                        if (header->type == NLMSG_IS_ERROR &&
                            header->sequence == sequence &&
                            header->port == local_port)
                        {
                                ack = netlink_status(header, false);
                                got_ack = true;
                                if (ack < 0)
                                {
                                        netlink_forget(address_of reply);
                                        return ack;
                                }
                        }
                        else if (header->length >= NETLINK_HEADER + GENL_HEADER &&
                                 header->type == session->family)
                        {
                                body = (p8 address_to)header + NETLINK_HEADER;
                                if (nl80211_find_u32(header, NL80211_ATTR_IFINDEX, 0) ==
                                    index)
                                {
                                        if (body[0] == NL80211_CMD_CONNECT)
                                        {
                                                p16 status = nl80211_find_u16(
                                                    header, NL80211_ATTR_STATUS_CODE,
                                                    0);
                                                positive mac_length = 0;
                                                p8 address_to mac;

                                                if (nl80211_attr(header,
                                                                 NL80211_ATTR_TIMED_OUT))
                                                {
                                                        netlink_forget(address_of reply);
                                                        return -110;
                                                }
                                                if (status)
                                                {
                                                        netlink_forget(address_of reply);
                                                        return -111;
                                                }
                                                mac = (p8 address_to)netlink_find(
                                                    header, GENL_HEADER, NL80211_ATTR_MAC,
                                                    address_of mac_length);
                                                if (mac && mac_length >= 6)
                                                        memory_copy(bssid, mac, 6);
                                                associated = true;
                                        }
                                        else if (body[0] == NL80211_CMD_DISCONNECT &&
                                                 got_ack)
                                        {
                                                netlink_forget(address_of reply);
                                                return -111;
                                        }
                                }
                        }
                        at += netlink_align(header->length);
                }

                if (got_ack && associated)
                {
                        netlink_forget(address_of reply);
                        return 0;
                }
        }
}

static bipolar nl80211_connect(nl80211 address_to session, p32 index,
                               p8 address_to ssid, positive ssid_length,
                               p8 address_to pmk, bool offload)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 open = NL80211_AUTHTYPE_OPEN;
        p32 version = NL80211_WPA_VERSION_2;
        p32 ccmp = WLAN_CIPHER_CCMP;
        p32 psk = WLAN_AKM_PSK;
        bipolar sent;

        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_CONNECT, NLM_REQUEST | NLM_ACK, sequence))
                return -1;

        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_SSID, ssid,
                              ssid_length);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_AUTH_TYPE, open);

        if (pmk)
        {
                netlink_attribute_add(address_of request, NL80211_ATTR_PRIVACY, null,
                                      0);
                nl80211_attribute_u32(address_of request, NL80211_ATTR_WPA_VERSIONS,
                                      version);
                nl80211_attribute_u32(address_of request,
                                      NL80211_ATTR_CIPHER_SUITES_PAIRWISE, ccmp);
                nl80211_attribute_u32(address_of request,
                                      NL80211_ATTR_CIPHER_SUITE_GROUP, ccmp);
                nl80211_attribute_u32(address_of request, NL80211_ATTR_AKM_SUITES,
                                      psk);
                if (offload)
                        netlink_attribute_add(address_of request, NL80211_ATTR_PMK, pmk,
                                              32);
                else
                        netlink_attribute_add(address_of request,
                                              NL80211_ATTR_CONTROL_PORT, null, 0);
        }

        if (request.failed)
        {
                netlink_forget(address_of request);
                return -1;
        }

        sent = socket_send(session->handle, request.bytes, request.used, 0, 0, 0);
        netlink_forget(address_of request);
        if (sent < 0)
                return sent;
        return sequence;
}

static bool nl80211_associated(void)
{
        nl80211 session;
        nl80211_iface iface;
        p8 mac[6];
        bool up = false;

        if (nl80211_open(address_of session) < 0)
                return false;
        if (!nl80211_interface(address_of session, address_of iface))
                up = nl80211_station(address_of session, iface.index, mac);
        nl80211_close(address_of session);
        return up;
}

static bipolar nl80211_join(p8 address_to ssid, positive ssid_length, p8 address_to pmk)
{
        nl80211 session;
        nl80211_iface iface;
        bipolar route;
        bipolar failed;
        bipolar eapol = -1;
        p8 bssid[6];
        p8 sta[6];
        bool offload = false;
        bipolar sequence;

        memory_fill(bssid, 0, 6);
        memory_fill(sta, 0, 6);
        failed = nl80211_open(address_of session);
        if (failed < 0)
                return failed;

        failed = nl80211_interface(address_of session, address_of iface);
        if (failed < 0)
        {
                nl80211_close(address_of session);
                return failed;
        }

        route = netlink_open_groups(0);
        if (route >= 0)
        {
                netlink_link_up((b32)route, iface.index);
                socket_close((b32)route);
        }

        if (iface.has_mac)
                memory_copy(sta, iface.mac, 6);
        else
                wifi_link_mac((string_address)iface.name, sta);

        if (pmk)
                offload = nl80211_psk_offload(address_of session, iface.wiphy);

        if (pmk && !offload)
        {
                eapol = nl80211_eapol_open(iface.index);
                if (eapol < 0)
                {
                        nl80211_close(address_of session);
                        return eapol;
                }
        }

        nl80211_disconnect(address_of session, iface.index);

        sequence = nl80211_connect(address_of session, iface.index, ssid,
                                   ssid_length, pmk, offload);
        if (sequence < 0)
        {
                if (eapol >= 0)
                        socket_close((b32)eapol);
                nl80211_close(address_of session);
                return sequence;
        }

        failed = nl80211_wait_associated(address_of session, (p32)sequence, iface.index,
                                         bssid);
        if (!failed && pmk && !offload)
        {
                if (!(bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] |
                      bssid[5]))
                        nl80211_station(address_of session, iface.index, bssid);
                if (!(sta[0] | sta[1] | sta[2] | sta[3] | sta[4] | sta[5]) ||
                    !(bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] |
                      bssid[5]))
                        failed = -1;
                else
                        failed = wifi_handshake(address_of session, iface.index,
                                                (b32)eapol, sta, bssid, pmk);
        }
        if (failed && pmk && offload)
        {
                nl80211_disconnect(address_of session, iface.index);
                eapol = nl80211_eapol_open(iface.index);
                if (eapol >= 0)
                {
                        memory_fill(bssid, 0, 6);
                        sequence = nl80211_connect(address_of session, iface.index,
                                                   ssid, ssid_length, pmk, false);
                        if (sequence >= 0)
                        {
                                failed = nl80211_wait_associated(
                                    address_of session, (p32)sequence, iface.index,
                                    bssid);
                                if (!failed)
                                {
                                        if (!(bssid[0] | bssid[1] | bssid[2] |
                                              bssid[3] | bssid[4] | bssid[5]))
                                                nl80211_station(
                                                    address_of session,
                                                    iface.index, bssid);
                                        failed = wifi_handshake(
                                            address_of session, iface.index,
                                            (b32)eapol, sta, bssid, pmk);
                                }
                        }
                }
        }

        if (failed)
                nl80211_disconnect(address_of session, iface.index);

        if (eapol >= 0)
                socket_close((b32)eapol);
        nl80211_close(address_of session);
        return failed;
}

static bipolar nl80211_disconnect(nl80211 address_to session, p32 index)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_DISCONNECT, NLM_REQUEST | NLM_ACK,
                           sequence))
                return -1;

        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

#endif
