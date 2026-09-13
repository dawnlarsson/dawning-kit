#!/bin/sh
set -eu

# A sudo build creates artifacts and dist as root before this hook runs, while
# its private checkout remains owned by the invoking user. That checkout owner
# already controls this root-executed hook source. Accept those two owners and
# nobody else; a non-root process never trusts an environment-supplied UID.
firmware_uid=$(id -u) || {
        printf 'rpi: could not identify the current user\n' >&2
        exit 1
}
firmware_invoking_uid=
if [ "$firmware_uid" = 0 ]; then
        case ${SUDO_UID:-} in
        ''|*[!0-9]*|0) ;;
        *) firmware_invoking_uid=$SUDO_UID ;;
        esac
fi

. src/build/host.sh

# The immutable commit is the official raspberrypi/firmware master observed on
# 2026-09-13. Updating it requires updating every digest below from the bytes
# at that same commit.
firmware_commit=ae2a7dc5330b7ea2c7107e5c4cb6b2691355bb9c
firmware_source="https://raw.githubusercontent.com/raspberrypi/firmware/$firmware_commit/boot"

firmware_digest()
{
        case $1 in
        bootcode.bin) printf '%s\n' 4245ec23b58158dc3e55f3e065eb4ecc0c0705d76c8db506e9f824941feee32e ;;
        start.elf)    printf '%s\n' b743faeaf6084f6cce501c62242e67570a68c6c4b57e1c4b4e4f212552a320d5 ;;
        fixup.dat)    printf '%s\n' b473192739dcac1ef9b3996653f6afb34c0b3ae576a90391bfbc4a8ca5fd17bf ;;
        *) return 1 ;;
        esac
}

firmware_verify()
{
        expected=$(firmware_digest "$1") || return 1
        printf '%s  %s\n' "$expected" "$2" |
                sha256sum -c - >/dev/null 2>&1
}

firmware_fail()
{
        printf 'rpi: %s\n' "$*" >&2
        exit 1
}

firmware_owner_of()
{
        stat -c %u -- "$1" 2>/dev/null || stat -f %u -- "$1"
}

firmware_mode_of()
{
        stat -c %a -- "$1" 2>/dev/null || stat -f %Lp -- "$1"
}

firmware_same_file()
{
        if [ "$(uname -s)" = Darwin ]; then
                /bin/zsh -c '[[ $1 -ef $2 ]]' same-file "$1" "$2"
        else
                [ "$1" -ef "$2" ]
        fi
}

firmware_directory_safe()
{
        [ -d "$1" ] && [ ! -L "$1" ] || return 1
        directory_owner=$(firmware_owner_of "$1") || return 1
        if [ "$directory_owner" != "$firmware_uid" ]; then
                [ -n "$firmware_invoking_uid" ] &&
                        [ "$directory_owner" = "$firmware_invoking_uid" ] ||
                        return 1
        fi
        directory_mode=$(firmware_mode_of "$1") || return 1
        case $directory_mode in
        [0-7][0145][0145]|[0-7][0-7][0145][0145]) return 0 ;;
        *) return 1 ;;
        esac
}

firmware_prepare_directory()
{
        [ ! -L "$1" ] || firmware_fail "$1 is a symbolic link"
        if [ ! -e "$1" ]; then
                (umask 077; mkdir -- "$1") ||
                        firmware_fail "could not create $1"
        fi
        firmware_directory_safe "$1" ||
                firmware_fail "$1 is not private to this user"
}

firmware_stage=
firmware_payload=
firmware_cleanup()
{
        exec 8<&- 9>&- || :
        [ -z "$firmware_payload" ] || rm -f -- "$firmware_payload"
        [ -z "$firmware_stage" ] || rmdir -- "$firmware_stage" 2>/dev/null || :
}

firmware_stage_open()
{
        firmware_stage=$(mktemp -d "$1/.$2.XXXXXX") ||
                firmware_fail "could not create a private stage for $2"
        chmod 0700 "$firmware_stage" ||
                firmware_fail "could not secure the stage for $2"
        firmware_directory_safe "$firmware_stage" ||
                firmware_fail "the stage for $2 is not private"
        firmware_payload=$firmware_stage/payload
        (umask 077; set -C; : > "$firmware_payload") ||
                firmware_fail "could not create the staged $2"
        exec 9<> "$firmware_payload" ||
                firmware_fail "could not pin the staged $2"
        firmware_same_file "$firmware_payload" /dev/fd/9 ||
                firmware_fail "the staged $2 changed while it was opened"
}

firmware_stage_seal()
{
        [ -f "$firmware_payload" ] && [ ! -L "$firmware_payload" ] &&
                firmware_same_file "$firmware_payload" /dev/fd/9 ||
                firmware_fail "the staged $1 changed while it was written"
        chmod 0644 /dev/fd/9 ||
                firmware_fail "could not set permissions on $1"
        exec 9>&-
}

firmware_stage_publish()
{
        # `mv source symlink-to-directory` moves source inside the symlink
        # target instead of replacing the link. Accepted parents exclude
        # co-tenant writers, so a link can be removed safely before the rename;
        # a real directory is never a firmware destination.
        if [ -L "$1" ]; then
                rm -f -- "$1" || firmware_fail "could not replace $2 link"
        fi
        [ ! -d "$1" ] || firmware_fail "$2 destination is a directory"
        mv -f -- "$firmware_payload" "$1" ||
                firmware_fail "could not publish $2"
        firmware_payload=
        rmdir -- "$firmware_stage" ||
                firmware_fail "could not remove the stage for $2"
        firmware_stage=
}

for utility in curl sha256sum mktemp stat id; do
        command -v "$utility" >/dev/null 2>&1 ||
                firmware_fail "$utility is required to install Raspberry Pi firmware"
done

label "$GREEN"'Raspberry Pi Post build setup'

firmware_directory_safe . ||
        firmware_fail 'the build directory is writable by another user'
for directory in artifacts artifacts/pi dist dist/boot; do
        firmware_prepare_directory "$directory"
done
trap firmware_cleanup 0 HUP INT TERM

for firmware in bootcode.bin start.elf fixup.dat; do
        cached=artifacts/pi/$firmware

        if [ -L "$cached" ] || [ ! -f "$cached" ] ||
                ! firmware_verify "$firmware" "$cached"; then
                firmware_stage_open artifacts/pi "$firmware.download"
                if ! curl --fail --location --silent --show-error \
                        --proto '=https' --proto-redir '=https' --tlsv1.2 \
                        --output - "$firmware_source/$firmware" >&9; then
                        firmware_fail "could not download $firmware"
                fi
                firmware_stage_seal "$firmware"

                firmware_verify "$firmware" "$firmware_payload" ||
                        firmware_fail "$firmware did not match its repository digest"
                firmware_stage_publish "$cached" "$firmware into the cache"
        fi

        # Pin the verified cache entry before copying it.  The output is also
        # written through an already-open descriptor in a private stage.
        firmware_verify "$firmware" "$cached" ||
                firmware_fail "$firmware cache changed before publication"
        firmware_stage_open dist/boot "$firmware.copy"
        exec 8< "$cached" || firmware_fail "could not open cached $firmware"
        [ ! -L "$cached" ] && [ -f "$cached" ] &&
                firmware_same_file "$cached" /dev/fd/8 ||
                firmware_fail "$firmware cache changed while it was opened"
        cat <&8 >&9 || firmware_fail "could not copy $firmware"
        exec 8<&-
        firmware_stage_seal "$firmware"
        firmware_verify "$firmware" "$firmware_payload" ||
                firmware_fail "$firmware copy did not match its repository digest"
        firmware_stage_publish "dist/boot/$firmware" "$firmware"
done

trap - 0 HUP INT TERM
