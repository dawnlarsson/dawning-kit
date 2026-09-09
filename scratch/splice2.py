p = 'src/build/build.c'
s = open(p).read()

helpers = open('scratch/part_helpers.c').read()
local = open('scratch/part_local.c').read()

anchor = """/*
        Every path in this tool is relative to the repository root, so running"""
assert anchor in s
s = s.replace(anchor, helpers + "\n" + local + "\n" + anchor, 1)

# --- settings the local build reads -------------------------------------
old = '''static build_setting build_settings[BUILD_SETTING_ROOM] = {
        /*      Where a build puts things. */
        {"artifacts", "artifacts"},'''
new = '''static build_setting build_settings[BUILD_SETTING_ROOM] = {
        /*      What the built system calls itself. */
        {"name", "moonwater"},
        {"version", "25"},
        {"full_name", "moonwater-25"},

        /*      Where a build puts things. */
        {"artifacts", "artifacts"},'''
assert old in s
s = s.replace(old, new)

old = '''        /*      What the build needs before it starts. */
        {"required", "bison flex bc gpg make gcc clang rustc"},'''
new = '''        /*      What the build needs before it starts. */
        {"required", "bison flex bc gpg make gcc clang rustc"},

        /*      The sources and scripts a build reads. */
        {"tool_registry", "src/sh/tools.inc"},
        {"shell_source", "programs/shell"},
        {"utilities_source", "programs/utilities"},
        {"monitor_source", "programs/monitor.sh"},
        {"patch_script", "kernel/patch/apply"},
        {"replace_script", "kernel/replace/apply"},

        /*      The image's own layout: the directories every build makes and
                the device nodes it boots with, as name, type, major, minor.
                The spark minor has to match SPARK_DEVICE_MINOR in src/spark.c. */
        {"image_directories",
         "sys proc dev tmp etc root bin sbin usr lib lib64 var opt bowls/bin"},
        {"image_nodes",
         "dev/tty c 5 0"
         " dev/console c 5 1"
         " dev/null c 1 3"
         " dev/zero c 1 5"
         " dev/random c 1 8"
         " dev/urandom c 1 9"
         " dev/kmsg c 1 11"
         " dev/spark c 10 250"},'''
assert old in s
s = s.replace(old, new)

# --- the pinned signature ------------------------------------------------
signature = '''
-----BEGIN PGP SIGNATURE-----
Comment: This signature is for the .tar version of the archive
Comment: git archive --format tar --prefix=linux-7.2/ v7.2
Comment: git version 2.55.0

iQIzBAABCgAdFiEEZH8oZUiU471FcZm+ONu9yGCSaT4FAmqCjM4ACgkQONu9yGCS
aT6jEBAAi+dDv3sQNuZPoSOjnv3be79xilhgbYRjXjYGyYr/axHwyCfRxYkV/sL0
SHOXT9ZGKp/GPjc8i21Pgca4c4UhckX48RTH7xNO3dR9X8n3g+8OLqP8FF2iFqdv
TWnagMo6CFyMmWj75WRwcZGKw2fOjCr9tSTSklAkLc8gytgUyHJKxcDHrYDpcdRF
GbhXn9GauSYu0ablmf6pSInjicXDMzPj9QVSt9NkO6FcrSoAfUfmU4c9EEsKW9T6
K5LsiyhRgcQfE0zrw1hYQBr2gFSXt8pa2u2XPVVukIBB9XSPdSG2x228b+yHmp/Y
zPRUzPDVkkK1BkU1D7XJdVmt2C3kfeBUJEcAlVKcDWf9rY80SU6FVyc45TwRfw8h
kq86+ERAmWOCwYsZjMK4i3PK4Zs60Q0rQZgmMY/mfqSxzMoCV2O9FGea8ZZQIlGH
m3qZw79igreY852bLihddRDgXAz47VFAwRnqzKaSJVMtUdigEPb34idC2ZE0yp07
PnHgCqFYktDu3+Enpm7RItsK0b0oQHdmeB8eOPgGSJ3gcJVmGKVaS4zd46gGDgJC
yt0LTonkwQO8q3jTN/2ffkVjzdrvk4IeYX5k3SQ6rinfebi0OMCQ9xDyR7MYvdwu
wgcVXSeiHcXa9SSFDvKn0L1q5nSLQGHp38qUi1ZPf/1uQSuB3ME=
=D53G
-----END PGP SIGNATURE-----
'''
lines = signature.split('\n')
literal = '\n'.join('         "%s\\n"' % line for line in lines if line != '')
# keep the blank line inside the armour: it separates the headers from the body
rebuilt = []
for line in signature.split('\n')[1:-1]:
    rebuilt.append('         "%s\\n"' % line)
literal = '\n'.join(rebuilt)

old = '''        /*      The kernel this tree builds on, and where it comes from. */
        {"kernel_version", "7.2"},
        {"kernel_mirror", "https://cdn.kernel.org/pub/linux/kernel"},
        {"kernel_keys", "torvalds@kernel.org gregkh@kernel.org"},'''
new = '''        /*      The kernel this tree builds on, and where it comes from.

                The signature is pinned here rather than downloaded next to
                the tarball. Fetching both would still verify, but only that
                the archive is signed by a trusted key -- pinning ties the
                build to this exact release, so a validly signed but different
                kernel cannot be substituted.

                To move to a new release: take the .sign file from the
                mirror's linux-VERSION.tar.sign and paste it here along with
                the version. */
        {"kernel_version", "7.2"},
        {"kernel_mirror", "https://cdn.kernel.org/pub/linux/kernel"},
        {"kernel_keys", "torvalds@kernel.org gregkh@kernel.org"},
        {"kernel_signature",
''' + literal + '''},'''
assert old in s
s = s.replace(old, new)
open(p, 'w').write(s)
print("ok")
