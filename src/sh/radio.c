/*
        Wireless and bluetooth, as moonwater verbs.

        Secrets stay on /root so an image update does not take the password
        with it. /ip watch still owns the address: this only joins the radio
        and says which link to prefer when both have carrier.
*/

/*      ----------------------------------------------------------------
        nl80211: how the wifi above talks to the kernel.
        ---------------------------------------------------------------- */

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

static COLD bipolar nl80211_disconnect(nl80211 address_to session, p32 index);

typedef struct
{
        p16 family;
        p32 mlme;
} nl80211_family_info;

static COLD bool nl80211_begin(netlink_buffer address_to buffer, p16 family, p8 command,
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

static COLD bool nl80211_attribute_u32(netlink_buffer address_to buffer, p16 type,
                                  p32 value)
{
        return netlink_attribute_add(buffer, type, address_of value, 4);
}

static COLD p32 nl80211_find_u32(netlink_header address_to header, p16 type, p32 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 4)
                return missing;
        return memory_load_unaligned(p32, at);
}

static COLD p16 nl80211_find_u16(netlink_header address_to header, p16 type,
                            p16 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 2)
                return missing;
        return (p16)memory_load_unaligned(p16, at);
}

static COLD bool nl80211_attr(netlink_header address_to header, p16 type)
{
        return netlink_find(header, GENL_HEADER, type, null) != null;
}

static COLD bool nl80211_family_seen(netlink_header address_to header,
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

static COLD bipolar nl80211_open(nl80211 address_to session)
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

static COLD fn nl80211_close(nl80211 address_to session)
{
        if (session->handle >= 0)
                socket_close(session->handle);
        session->handle = -1;
}

static COLD bool nl80211_iface_seen(netlink_header address_to header,
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

static COLD bipolar nl80211_interface(nl80211 address_to session,
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

/* WPA's PBKDF2 and PRF are HMAC-SHA-1, which is crypto.c's HMAC under a
   different digest and nothing else. */
static COLD fn wifi_hmac_sha1(p8 address_to key, positive key_length,
                         p8 address_to data, positive length, p8 address_to out)
{
        crypto_mac mac;

        crypto_hmac_open(address_of mac, DIGEST_SHA1, 20, key, key_length);
        crypto_hmac_write(address_of mac, data, length);
        crypto_hmac_close(address_of mac, out);
}

static COLD p8 wifi_nibble(p8 byte)
{
        if (byte_is_digit(byte))
                return (p8)(byte - '0');
        byte = byte_to_lower(byte);
        return (p8)(byte - 'a' + 10);
}

static COLD bool wifi_psk(p8 address_to ssid, positive ssid_length,
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

/* One byte of InvMixColumns.  The matrix's four rows are the same four
   coefficients rotated, so the row it is wanted for says where to start
   reading them, and the multiply is crypto.c's constant-time one -- the
   same GF(2^8) its S-box inversion runs in. */
static COLD p8 wifi_unmix(p8 address_to column, positive row)
{
        static const p8 factor[4] = {0x0e, 0x0b, 0x0d, 0x09};
        p8 mixed = 0;

        for (positive at = 0; at < 4; at++)
                mixed ^= crypto_aes_field_multiply(
                    column[at], factor[(4 + at - row) & 3]);

        return mixed;
}

/* The inverse S-box, built rather than written out: it is the forward box's
   inverse permutation, and crypto.c already computes that box in the field
   it lives in. Two hundred and fifty six hand-typed bytes are two hundred
   and fifty six chances to mistype one, and built this way the two boxes
   cannot disagree. The unwrap builds it once and lends it to every block. */
static COLD fn wifi_inverse_box(p8 address_to inverse)
{
        for (positive at = 0; at < 256; at++)
                inverse[crypto_aes_substitute((p8)at)] = (p8)at;
}

static COLD fn wifi_aes_decrypt(p8 address_to key, p8 address_to in,
                           p8 address_to out, const p8 address_to inverse)
{
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

        /*
                The last round is every other round without InvMixColumns, so
                the loop runs once more and leaves from the middle rather
                than repeating its first three steps underneath itself.
        */
        for (step = 9;; step--)
        {
                //      InvShiftRows: row r of the state -- the bytes r, r+4,
                //      r+8 and r+12 -- rotates right by r.
                for (row = 1; row < 4; row++)
                        for (positive turn = 0; turn < row; turn++)
                        {
                                temp = state[row + 12];
                                state[row + 12] = state[row + 8];
                                state[row + 8] = state[row + 4];
                                state[row + 4] = state[row];
                                state[row] = temp;
                        }

                for (row = 0; row < 16; row++)
                        state[row] = inverse[state[row]];
                for (row = 0; row < 16; row++)
                        state[row] ^= round[step * 16 + row];
                if (!step)
                        break;

                memory_copy(hold, state, 16);
                for (row = 0; row < 4; row++)
                        for (positive at = 0; at < 4; at++)
                                state[row * 4 + at] =
                                    wifi_unmix(hold + row * 4, at);
        }

        memory_copy(out, state, 16);
        crypto_forget(round, sizeof(round));
        crypto_forget(state, sizeof(state));
        crypto_forget(hold, sizeof(hold));
}

static COLD bool wifi_kw_unwrap(p8 address_to kek, p8 address_to wrap, positive length,
                           p8 address_to plain, positive address_to plain_length)
{
        p8 block[16];
        p8 a[8];
        p8 inverse[256];
        p8 r[WIFI_WRAP_MOST - 8];
        positive words;
        positive round;
        positive i;
        p64 t;

        if (length < 24 || (length & 7) || length > WIFI_WRAP_MOST)
                return false;
        words = (length / 8) - 1;
        wifi_inverse_box(inverse);
        memory_copy(a, wrap, 8);
        memory_copy(r, wrap + 8, words * 8);
        for (round = 6; round > 0; round--)
                for (i = words; i > 0; i--)
                {
                        t = (p64)words * (round - 1) + i;
                        crypto_put_be64(a, crypto_be64(a) ^ t);
                        memory_copy(block, a, 8);
                        memory_copy(block + 8, r + (i - 1) * 8, 8);
                        wifi_aes_decrypt(kek, block, block, inverse);
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

static COLD fn wifi_ptk(p8 address_to pmk, p8 address_to ap, p8 address_to sta,
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

        if (memory_compare(ap, sta, 6) < 0)
        {
                min_mac = ap;
                max_mac = sta;
        }
        else
        {
                min_mac = sta;
                max_mac = ap;
        }
        if (memory_compare(anonce, snonce, 32) < 0)
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

static COLD bool nl80211_ext_bit(p8 address_to bits, positive length, positive which)
{
        return which / 8 < length && (bits[which / 8] & (1u << (which % 8)));
}

static COLD bool nl80211_wiphy_seen(netlink_header address_to header, address_any context)
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

static COLD bool nl80211_psk_offload(nl80211 address_to session, p32 wiphy)
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

static COLD bool nl80211_station_seen(netlink_header address_to header,
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

static COLD bool nl80211_station(nl80211 address_to session, p32 index, p8 address_to mac)
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

static COLD bipolar nl80211_new_key(nl80211 address_to session, p32 index, p8 idx,
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

static COLD bipolar nl80211_authorize(nl80211 address_to session, p32 index,
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

static COLD bipolar nl80211_eapol_open(p32 index)
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

static COLD fn wifi_eapol_mic(p8 address_to kck, p8 address_to frame, positive length)
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

static COLD bipolar wifi_eapol_send(b32 handle, p32 index, p8 address_to bssid,
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

static COLD bipolar wifi_handshake(nl80211 address_to session, p32 index, b32 eapol,
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
                info = network_load_16(frame + 5);
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
                        network_store_16(frame + 2, (p16)(95 + sizeof(wifi_rsn_ie)));
                        frame[4] = 2;
                        network_store_16(frame + 5, 0x010a);
                        network_store_16(frame + 7, 16);
                        memory_copy(frame + 9, replay, 8);
                        memory_copy(frame + 17, snonce, 32);
                        network_store_16(frame + 97, (p16)sizeof(wifi_rsn_ie));
                        memory_copy(frame + 99, wifi_rsn_ie, sizeof(wifi_rsn_ie));
                        wifi_eapol_mic(ptk, frame, WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        wifi_eapol_send(eapol, index, bssid, frame,
                                        WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        continue;
                }

                if (!have_anonce || !(info & 0x1000))
                        continue;

                data_length = network_load_16(frame + 97);
                if (WIFI_EAPOL_HDR + data_length > length)
                        continue;
                {
                        p8 mic[16];
                        p8 check[20];

                        memory_copy(mic, frame + 81, 16);
                        memory_fill(frame + 81, 0, 16);
                        wifi_hmac_sha1(ptk, 16, frame, WIFI_EAPOL_HDR + data_length,
                                       check);
                        if (!crypto_same(mic, check, 16))
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
                network_store_16(frame + 2, 95);
                frame[4] = 2;
                network_store_16(frame + 5, 0x030a);
                network_store_16(frame + 7, 16);
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

static COLD bool wifi_link_mac(string_address name, p8 address_to mac)
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

static COLD bipolar nl80211_wait_associated(nl80211 address_to session, p32 sequence,
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

static COLD bipolar nl80211_connect(nl80211 address_to session, p32 index,
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

static COLD bool nl80211_associated(void)
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

static COLD bipolar nl80211_join(p8 address_to ssid, positive ssid_length, p8 address_to pmk)
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


#define RADIO_SSID_MOST 32
#define RADIO_PASS_MOST 63
#define RADIO_WIFI_MOST 16
#define RADIO_RFKILL_WLAN 1
#define RADIO_RFKILL_BLUETOOTH 2
#define RADIO_RFKILL_CHANGE_ALL 3
#define RADIO_LOCK_PATH NET_STATE_DIR "/radio.lock"
#define RADIO_LOCK_EX 2
#define RADIO_LOCK_NB 4
#define RADIO_LOCK_UN 8

typedef struct
{
        p8 ssid[RADIO_SSID_MOST + 1];
        p8 pass[RADIO_PASS_MOST + 1];
        p8 ssid_length;
        p8 pass_length;
} radio_network;

static fn radio_net_wake(void)
{
        bipolar handle;
        p8 one = '1';

        host_state_ready();
        system_call_4(syscall(mknodat), AT_FDCWD,
                      (positive)(string_address)NET_WAKE_PATH, S_IFIFO | 0600, 0);
        handle = system_open_at(AT_FDCWD, NET_WAKE_PATH,
                                FILE_READ_WRITE | O_NONBLOCK | O_CLOEXEC);
        if (handle < 0)
                return;

        system_write_all((positive)handle, address_of one, 1);
        system_close(handle);
}

/* One word and its newline over a state file. The room is a timezone name's
   room, because the clock's words come through here too. */
static bipolar radio_write_word(string_address path, string_address word)
{
        p8 line[96];

        string_copy_bounded(line, word, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        return host_write_file(path, line, string_length(line), 0644, true);
}

static bool radio_word_is(string_address path, string_address word)
{
        p8 text[16];

        if (host_read_text(path, text, sizeof(text)) < 0)
                return false;
        return string_equals(text, word);
}

static bipolar radio_lock(bool wait)
{
        bipolar handle;
        bipolar locked;

        host_state_ready();
        handle = system_open_at_mode(AT_FDCWD, RADIO_LOCK_PATH,
                                     FILE_READ_WRITE | FILE_CREATE | O_CLOEXEC,
                                     0600);
        if (handle < 0)
                return handle;

        locked = system_call_2(syscall(flock), (positive)handle,
                               wait ? RADIO_LOCK_EX
                                    : (RADIO_LOCK_EX | RADIO_LOCK_NB));
        if (locked < 0)
        {
                system_close(handle);
                return locked;
        }

        return handle;
}

static fn radio_unlock(bipolar handle)
{
        if (handle < 0)
                return;

        system_call_2(syscall(flock), (positive)handle, RADIO_LOCK_UN);
        system_close(handle);
}

static bipolar radio_rfkill(p8 type, bool block)
{
        ul_rfkill_event event = {
            .index = 0,
            .type = type,
            .operation = RADIO_RFKILL_CHANGE_ALL,
            .soft = block,
        };
        bipolar handle = system_open_at(AT_FDCWD, "/dev/rfkill",
                                        O_WRONLY | O_CLOEXEC | O_NONBLOCK);

        if (handle < 0)
                return handle;

        if (system_write_all((positive)handle, address_of event,
                             sizeof(event)) != sizeof(event))
        {
                system_close(handle);
                return -1;
        }

        system_close(handle);
        return 0;
}

static bool radio_text_plain(string_address text, positive length)
{
        positive at;

        for (at = 0; at < length; at++)
                if (text[at] < 32)
                        return false;
        return true;
}

static bool radio_line_has(p8 address_to text, positive got, string_address want)
{
        positive at = 0;
        positive want_length = string_length(want);

        while (at < got)
        {
                positive start = at;

                while (at < got && text[at] != '\n')
                        at++;
                if (at - start == want_length &&
                    !memory_compare(text + start, want, want_length))
                        return true;
                if (at < got)
                        at++;
        }

        return false;
}

static positive radio_wifi_load(radio_network address_to into, positive room)
{
        p8 text[8192];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_WIFI_LIST, text,
                                         sizeof(text));
        positive count = 0;
        positive at = 0;
        bool want_ssid = true;

        memory_fill(into, 0, sizeof(radio_network) * room);
        if (got <= 0)
                return 0;
        /* text holds the saved passphrases in the clear, exactly as the
           radio_network array below does, so it is scrubbed the same way
           before this frame is left to whatever runs in it next. */

        while (at < (positive)got && count < room)
        {
                positive start = at;
                positive length;

                while (at < (positive)got && text[at] != '\n')
                        at++;
                length = at - start;
                if (at < (positive)got)
                        at++;

                if (want_ssid)
                {
                        if (!length)
                                continue;
                        if (length > RADIO_SSID_MOST ||
                            !radio_text_plain((string_address)(text + start), length))
                        {
                                want_ssid = false;
                                continue;
                        }
                        memory_copy(into[count].ssid, text + start, length);
                        into[count].ssid[length] = end;
                        into[count].ssid_length = (p8)length;
                        into[count].pass[0] = end;
                        into[count].pass_length = 0;
                        want_ssid = false;
                        continue;
                }

                if (into[count].ssid_length)
                {
                        if (length &&
                            (length > RADIO_PASS_MOST ||
                             !radio_text_plain((string_address)(text + start),
                                               length)))
                        {
                                memory_fill(address_of into[count], 0,
                                            sizeof(into[count]));
                        }
                        else
                        {
                                memory_copy(into[count].pass, text + start, length);
                                into[count].pass[length] = end;
                                into[count].pass_length = (p8)length;
                                count++;
                        }
                }
                want_ssid = true;
        }

        if (!want_ssid && count < room && into[count].ssid_length)
                count++;

        crypto_forget(text, sizeof(text));
        return count;
}

static bipolar radio_wifi_save(radio_network address_to networks, positive count)
{
        p8 text[8192];
        positive used = 0;
        positive at;

        for (at = 0; at < count; at++)
        {
                if (used + networks[at].ssid_length + networks[at].pass_length +
                        2 >=
                    sizeof(text))
                {
                        crypto_forget(text, sizeof(text));
                        return -1;
                }
                memory_copy(text + used, networks[at].ssid,
                            networks[at].ssid_length);
                used += networks[at].ssid_length;
                text[used++] = '\n';
                memory_copy(text + used, networks[at].pass,
                            networks[at].pass_length);
                used += networks[at].pass_length;
                text[used++] = '\n';
        }

        {
                bipolar failed = host_write_file(NET_WIFI_LIST, text, used,
                                                 0600, true);

                crypto_forget(text, sizeof(text));
                return failed;
        }
}

static bipolar radio_wifi_join(string_address ssid, string_address pass)
{
        p8 pmk[32];
        bipolar failed = -19;
        bool secured = pass && pass[0];
        positive ssid_length = string_length(ssid);
        p64 started;

        if (!ssid_length || ssid_length > RADIO_SSID_MOST)
                return -22;

        if (secured && !wifi_psk((p8 address_to)ssid, ssid_length,
                                 (p8 address_to)pass, string_length(pass), pmk))
                return -22;

        started = system_clock_ns(HOST_CLOCK_BOOTTIME);
        for (;;)
        {
                failed = nl80211_join((p8 address_to)ssid, ssid_length,
                                      secured ? pmk : null);
                if (failed != -19)
                        break;
                if (system_clock_ns(HOST_CLOCK_BOOTTIME) - started >= 8000000000)
                        break;
                host_pause(200000000);
        }

        crypto_forget(pmk, sizeof(pmk));
        return failed;
}

static bipolar radio_wifi_leave(void)
{
        nl80211 session;
        nl80211_iface iface;
        bipolar failed;

        failed = nl80211_open(address_of session);
        if (failed < 0)
                return failed;

        failed = nl80211_interface(address_of session, address_of iface);
        if (!failed)
                failed = nl80211_disconnect(address_of session, iface.index);

        nl80211_close(address_of session);
        return failed;
}

static b32 radio_wifi_bring(bool say)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count = radio_wifi_load(networks, RADIO_WIFI_MOST);
        positive at;
        bipolar failed = 0;
        bool joined = false;

        radio_write_word(NET_WIFI_POWER, "on");
        radio_rfkill(RADIO_RFKILL_WLAN, false);

        if (!say && nl80211_associated())
        {
                radio_net_wake();
                crypto_forget(networks, sizeof(networks));
                return 0;
        }

        for (at = 0; at < count; at++)
        {
                failed = radio_wifi_join((string_address)networks[at].ssid,
                                         (string_address)networks[at].pass);
                if (!failed)
                {
                        joined = true;
                        if (say)
                        {
                                string_format(log, host_label "wifi joined %s\n",
                                              (string_address)networks[at].ssid);
                                log_flush();
                        }
                        break;
                }
                if (failed == -19)
                        break;
        }

        radio_net_wake();
        crypto_forget(networks, sizeof(networks));

        if (!count)
        {
                if (say)
                {
                        string_format(log, host_label "wifi on\n");
                        log_flush();
                }
                return 0;
        }

        if (joined)
                return 0;

        if (say)
                return failed == -19
                           ? host_refuse("no wireless interface%s\n", "")
                           : failed == -110
                                 ? host_refuse("the network did not associate%s\n",
                                               "")
                                 : failed == -111
                                       ? host_refuse("the network refused the join%s\n",
                                                     "")
                                       : host_fail("wifi", failed ? failed : -1);
        return 1;
}

static b32 radio_wifi_on(bool say)
{
        bipolar lock = radio_lock(true);
        b32 result;

        if (lock < 0)
                return say ? host_fail("wifi", lock) : 1;
        result = radio_wifi_bring(say);
        radio_unlock(lock);
        return result;
}

static b32 radio_wifi_off(bool say)
{
        bipolar lock = radio_lock(true);

        if (lock < 0)
                return say ? host_fail("wifi", lock) : 1;
        radio_write_word(NET_WIFI_POWER, "off");
        radio_wifi_leave();
        radio_rfkill(RADIO_RFKILL_WLAN, true);
        radio_net_wake();
        radio_unlock(lock);
        if (say)
        {
                string_format(log, host_label "wifi off\n");
                log_flush();
        }
        return 0;
}

static b32 radio_wifi_add(string_address ssid, string_address pass)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count;
        positive at;
        positive ssid_length = string_length(ssid);
        positive pass_length = pass ? string_length(pass) : 0;

        if (!ssid_length || ssid_length > RADIO_SSID_MOST)
                return host_refuse("that network name is empty or too long%s\n",
                                   "");
        if (pass_length > RADIO_PASS_MOST)
                return host_refuse("that password is too long%s\n", "");
        if (pass_length && pass_length < 8 && pass_length != 64)
                return host_refuse("a WPA password is 8 to 63 characters%s\n",
                                   "");
        if (!radio_text_plain(ssid, ssid_length) ||
            (pass_length && !radio_text_plain(pass, pass_length)))
                return host_refuse("that network name cannot be stored%s\n", "");

        count = radio_wifi_load(networks, RADIO_WIFI_MOST);

        for (at = 0; at < count; at++)
                if (string_equals((string_address)networks[at].ssid, ssid))
                        break;

        if (at == count)
        {
                if (count == RADIO_WIFI_MOST)
                {
                        crypto_forget(networks, sizeof(networks));
                        return host_refuse("too many saved networks%s\n", "");
                }
                count++;
        }

        memory_fill(networks[at].ssid, 0, sizeof(networks[at].ssid));
        memory_copy(networks[at].ssid, ssid, ssid_length);
        networks[at].ssid[ssid_length] = end;
        networks[at].ssid_length = (p8)ssid_length;
        memory_fill(networks[at].pass, 0, sizeof(networks[at].pass));
        if (pass_length)
                memory_copy(networks[at].pass, pass, pass_length);
        networks[at].pass[pass_length] = end;
        networks[at].pass_length = (p8)pass_length;

        if (radio_wifi_save(networks, count) < 0)
        {
                crypto_forget(networks, sizeof(networks));
                return host_fail("wifi", -1);
        }

        radio_write_word(NET_WIFI_POWER, "on");
        radio_rfkill(RADIO_RFKILL_WLAN, false);

        {
                bipolar lock = radio_lock(true);
                bipolar failed;

                if (lock < 0)
                {
                        crypto_forget(networks, sizeof(networks));
                        return host_fail("wifi", lock);
                }
                failed = radio_wifi_join(ssid, pass);
                radio_unlock(lock);

                radio_net_wake();
                crypto_forget(networks, sizeof(networks));
                if (failed < 0)
                        return failed == -19
                                   ? host_refuse("saved, but there is no "
                                                 "wireless interface%s\n",
                                                 "")
                                   : failed == -110
                                         ? host_refuse("saved, but the network "
                                                       "did not associate%s\n",
                                                       "")
                                         : failed == -111
                                               ? host_refuse("saved, but the "
                                                             "network refused "
                                                             "the join%s\n",
                                                             "")
                                               : host_fail("wifi", failed);
                if (pass && pass[0])
                        crypto_forget((address_any)pass, string_length(pass));
        }

        string_format(log, host_label "wifi joined %s\n", ssid);
        log_flush();
        return 0;
}

static b32 radio_wifi_status(void)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count = radio_wifi_load(networks, RADIO_WIFI_MOST);
        positive at;
        bool off = radio_word_is(NET_WIFI_POWER, "off");

        string_format(log, host_label "wifi %s\n",
                      off ? (string_address) "off" : (string_address) "on");
        for (at = 0; at < count; at++)
                string_format(log, host_label "  %s\n",
                              (string_address)networks[at].ssid);
        log_flush();
        crypto_forget(networks, sizeof(networks));
        return 0;
}

/* The remembered word and the rfkill switch, set together. say is for the
   person who asked; restoring the machine's own choice stays quiet. */
static b32 radio_bluetooth_power(bool on, bool say)
{
        string_address word = on ? (string_address)"on" : (string_address)"off";

        radio_write_word(NET_BLUETOOTH_POWER, word);
        radio_rfkill(RADIO_RFKILL_BLUETOOTH, !on);
        if (say)
        {
                string_format(log, host_label "bluetooth %s\n", word);
                log_flush();
        }
        return 0;
}

static b32 radio_bluetooth_add(string_address identity)
{
        p8 text[4096];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_BLUETOOTH_LIST, text,
                                         sizeof(text));
        p8 line[320];
        positive used;

        if (!identity[0] || string_length(identity) > 128)
                return host_refuse("that bluetooth name is empty or too long%s\n",
                                   "");
        if (!radio_text_plain(identity, string_length(identity)))
                return host_refuse("that bluetooth name cannot be stored%s\n", "");

        if (got < 0)
        {
                text[0] = end;
                got = 0;
        }

        if (!(got > 0 && radio_line_has(text, (positive)got, identity)))
        {
                string_copy_bounded(line, identity, sizeof(line));
                string_append_bounded(line, "\n", sizeof(line));
                used = (positive)got;
                if (used + string_length(line) >= sizeof(text))
                        return host_refuse("too many saved bluetooth devices%s\n",
                                           "");
                memory_copy(text + used, line, string_length(line));
                used += string_length(line);
                if (host_write_file(NET_BLUETOOTH_LIST, text, used, 0644,
                                    true) < 0)
                        return host_fail("bluetooth", -1);
        }

        radio_bluetooth_power(true, false);
        string_format(log, host_label "bluetooth remembered %s\n", identity);
        log_flush();
        return 0;
}

static b32 radio_bluetooth_status(void)
{
        p8 text[4096];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_BLUETOOTH_LIST, text,
                                         sizeof(text));
        bool off = radio_word_is(NET_BLUETOOTH_POWER, "off");
        positive at = 0;

        string_format(log, host_label "bluetooth %s\n",
                      off ? (string_address) "off" : (string_address) "on");
        if (got > 0)
                while (at < (positive)got)
                {
                        positive start = at;

                        while (at < (positive)got && text[at] != '\n')
                                at++;
                        if (at > start)
                        {
                                p8 name[129];
                                positive length = at - start;

                                if (length > 128)
                                        length = 128;
                                memory_copy(name, text + start, length);
                                name[length] = end;
                                string_format(log, host_label "  %s\n",
                                              (string_address)name);
                        }
                        if (at < (positive)got)
                                at++;
                }
        log_flush();
        return 0;
}

static string_address radio_internet_word(void)
{
        return net_internet_prefer() == NETLINK_PREFER_WIFI
                   ? (string_address) "wifi"
                   : (string_address) "wired";
}

static b32 radio_internet_set(string_address which)
{
        p8 line[8];

        if (!string_equals(which, "wired") && !string_equals(which, "wifi"))
                return host_usage();

        string_copy_bounded(line, which, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        host_state_ready();
        if (host_write_text(NET_INTERNET_RUN, line) < 0)
                return host_fail("internet", -1);
        if (host_write_file(NET_INTERNET_ROOT, line, string_length(line),
                            0644, true) < 0)
                return host_fail("internet", -1);
        radio_net_wake();

        string_format(log, host_label "internet prefers %s\n", which);
        log_flush();
        return 0;
}

static b32 radio_internet_status(void)
{
        string_format(log, host_label "internet prefers %s\n",
                      radio_internet_word());
        log_flush();
        return 0;
}

static fn radio_internet_copy(void)
{
        p8 text[16];
        p8 have[16];
        p8 line[8];

        if (host_read_text(NET_INTERNET_ROOT, text, sizeof(text)) < 0)
                return;
        if (host_read_text(NET_INTERNET_RUN, have, sizeof(have)) >= 0 &&
            string_equals(have, text))
                return;

        string_copy_bounded(line, text, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        host_state_ready();
        if (host_write_text(NET_INTERNET_RUN, line) >= 0)
                radio_net_wake();
}

static bool radio_wifi_wanted(void)
{
        radio_network networks[1];
        bool wanted;

        if (radio_word_is(NET_WIFI_POWER, "off"))
                return false;
        if (radio_word_is(NET_WIFI_POWER, "on"))
                return true;
        wanted = radio_wifi_load(networks, 1) != 0;
        crypto_forget(networks, sizeof(networks));
        return wanted;
}

static fn radio_reap(void)
{
        positive status = 0;

        while (system_call_4(syscall(wait4), (positive)-1,
                             (positive)address_of status, 1, 0) > 0)
                ;
}

static fn radio_wifi_keep(void)
{
        bipolar lock;
        bipolar child;

        radio_rfkill(RADIO_RFKILL_WLAN, false);
        if (nl80211_associated())
                return;

        lock = radio_lock(false);
        if (lock < 0)
                return;

        child = system_fork();
        if (child < 0)
        {
                radio_unlock(lock);
                return;
        }
        if (child)
        {
                system_close(lock);
                return;
        }

        radio_wifi_bring(false);
        radio_unlock(lock);
        system_call_1(syscall(exit), 0);
}

static fn radio_restore(void)
{
        radio_internet_copy();

        if (radio_word_is(NET_WIFI_POWER, "off"))
                radio_wifi_off(false);
        else if (radio_wifi_wanted())
                radio_wifi_on(false);

        if (radio_word_is(NET_BLUETOOTH_POWER, "off"))
                radio_bluetooth_power(false, false);
        else if (radio_word_is(NET_BLUETOOTH_POWER, "on"))
                radio_bluetooth_power(true, false);
}

static fn radio_recover(void)
{
        p8 verdict[HOST_NAME_ROOM + 16];

        radio_reap();
        if (host_read_text(HOST_VERDICT, verdict, sizeof(verdict)) >= 0 &&
            host_starts(verdict, "ask "))
                return;

        radio_internet_copy();

        if (radio_wifi_wanted())
                radio_wifi_keep();

        if (radio_word_is(NET_BLUETOOTH_POWER, "on"))
                radio_rfkill(RADIO_RFKILL_BLUETOOTH, false);
}

static b32 host_radio(string_address address_to arguments, positive count)
{
        string_address verb = arguments[1];
        string_address word = count > 2 ? arguments[2] : null;
        bool mutate;

        if (string_equals(verb, "priority"))
        {
                if (count == 2)
                        return radio_internet_status();
                if (!string_equals(word, "internet"))
                        return host_usage();
                if (count == 3)
                        return radio_internet_status();
                if (count != 4)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return radio_internet_set(arguments[3]);
        }

        mutate = count > 2;
        if (mutate && !bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");

        if (string_equals(verb, "wifi"))
        {
                if (count < 3)
                        return radio_wifi_status();
                if (string_equals(word, "on") && count == 3)
                        return radio_wifi_on(true);
                if (string_equals(word, "off") && count == 3)
                        return radio_wifi_off(true);
                if (string_equals(word, "add") && count >= 4 && count <= 5)
                {
                        p8 pass[RADIO_PASS_MOST + 1];
                        string_address secret = count == 5 ? arguments[4]
                                                           : (string_address)"";
                        positive length = string_length(secret);
                        b32 result;

                        if (length > RADIO_PASS_MOST)
                                return host_refuse("that password is too long%s\n",
                                                   "");
                        memory_copy(pass, secret, length);
                        pass[length] = end;
                        if (count == 5)
                                crypto_forget(arguments[4], length);
                        result = radio_wifi_add(arguments[3], pass);
                        crypto_forget(pass, sizeof(pass));
                        return result;
                }
                return host_usage();
        }

        if (count < 3)
                return radio_bluetooth_status();
        if (string_equals(word, "on") && count == 3)
                return radio_bluetooth_power(true, true);
        if (string_equals(word, "off") && count == 3)
                return radio_bluetooth_power(false, true);
        if (string_equals(word, "add") && count == 4)
                return radio_bluetooth_add(arguments[3]);
        return host_usage();
}
