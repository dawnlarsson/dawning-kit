#!/bin/sh
#
#       The file utilities against the system's own, diffed.
#
#           sh src/test/files.sh [directory holding our binaries]
#
#       Three kinds of case, because three kinds of thing are being checked
#       and pretending they are one kind is how a comparison gets rigged:
#
#         same    the two tools' output has to match byte for byte.
#         effect  the two tools are run on twin trees and what is left behind
#                 has to match -- names, permissions, link targets, contents.
#                 chmod and mv and rm print nothing worth diffing; what they
#                 did is the whole of what they are.
#         near    the output is compared after a named normalisation, and the
#                 normalisation is printed so it can be argued with. ls -l
#                 chooses its date format on a six month recency rule, stat's
#                 default block was a layout rather than an answer. All
#                 three turned out to be matchable in the end and are
#                 compared whole; what is left under near is find and du,
#                 whose output is in readdir order and is sorted first, and
#                 the two cases named where the tools genuinely disturb what
#                 they are measuring.
#
#       Everything of ours prints times in UTC, so the system tool is run
#       with TZ set to UTC as well; a comparison against Europe/Stockholm
#       would be measuring the timezone database and not the utility.
#
set -e

binaries=${1:-/tmp/multi}
export TZ=UTC0

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

started=$(date +%s)

pass=0
fail=0
current=""

group() { current=$1; }

report() {
        if [ "$1" = ok ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-10s %-30s %s\n' "$current" "$2" "$3"
}

# Byte for byte, both tools given the same words.
same() {
        name=$1
        tool=$2
        shift 2

        "$tool" "$@" > "$work/want" 2> "$work/want.err" || true
        "$binaries/$tool" "$@" > "$work/got" 2> "$work/got.err" || true

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 60 "$work/want" | tr '\n' '|')] got [$(head -c 60 "$work/got" | tr '\n' '|')]"
}

# Byte for byte, but the output of both is passed through a filter first.
near() {
        name=$1
        filter=$2
        tool=$3
        shift 3

        "$tool" "$@" 2>/dev/null | eval "$filter" > "$work/want" || true
        "$binaries/$tool" "$@" 2>/dev/null | eval "$filter" > "$work/got" || true

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 60 "$work/want" | tr '\n' '|')] got [$(head -c 60 "$work/got" | tr '\n' '|')]"
}

# When a file was last written, or the word now.
#
# The two trees are built one after the other and the two tools run one after
# the other, so anything either of them creates carries the second it happened
# to be created in. Comparing those fails whenever the pair falls either side
# of a tick -- a coin flip with nothing to do with the tools. A time from
# before the suite started is compared exactly, which is what touch -d and
# cp -p are for.
modified() {
        stamp=$(stat -c '%Y' "$1" 2>/dev/null)

        if [ -n "$stamp" ] && [ "$stamp" -ge "$started" ]; then
                echo now
        else
                echo "$stamp"
        fi
}

# A dump of a tree that captures everything the mutating tools are for.
dump() {
        (
                cd "$1" || exit 1
                find . | LC_ALL=C sort | while read -r entry; do
                        # %Y and not %X: find and cksum read the tree to
                        # dump it, so the access times differ between the two
                        # dumps by construction. The modify time is the one
                        # touch and cp -p are for.
                        printf '%s %s %s %s' "$entry" \
                                "$(stat -c '%a %F %s' "$entry" 2>/dev/null)" \
                                "$(modified "$entry")" \
                                "$(readlink "$entry" 2>/dev/null)"
                        if [ -f "$entry" ] && [ ! -L "$entry" ]; then
                                printf ' %s' "$(cksum < "$entry")"
                        fi
                        printf '\n'
                done
        )
}

# Twin trees, the system tool on one and ours on the other, and afterwards
# the two trees have to be the same tree.
effect() {
        name=$1
        tool=$2
        recipe=$3

        rm -rf "$work/a" "$work/b"
        mkdir -p "$work/a" "$work/b"

        seed "$work/a"
        seed "$work/b"

        ( cd "$work/a" && eval "TOOL=$tool; $recipe" ) > "$work/want.out" 2>&1 || true
        ( cd "$work/b" && eval "TOOL=$binaries/$tool; $recipe" ) > "$work/got.out" 2>&1 || true

        dump "$work/a" > "$work/want"
        dump "$work/b" > "$work/got"

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "$(diff "$work/want" "$work/got" | head -4 | tr '\n' '|')"
}

# What every effect case starts from: a small tree with a bit of everything
# in it that the mutating tools have to deal with.
seed() {
        root=$1
        mkdir -p "$root/tree/deep/deeper"
        printf 'hello\n' > "$root/tree/one"
        printf 'two two two\n' > "$root/tree/two"
        printf 'buried\n' > "$root/tree/deep/three"
        printf 'deepest\n' > "$root/tree/deep/deeper/four"
        printf 'top\n' > "$root/plain"
        ln -s tree/one "$root/link"
        chmod 0640 "$root/tree/two"
        chmod 0700 "$root/tree/deep"
        touch -d @1000000000 "$root/tree/one"
}

# How long a sleep takes, rounded to a tenth of a second. Nothing is printed
# by either tool, so the elapsed time is the whole of what there is to compare.
tenths() {
        started=$(date +%s%N)
        "$@" >/dev/null 2>&1 || true
        stopped=$(date +%s%N)
        echo $(( (stopped - started) / 100000000 ))
}

timing() {
        name=$1
        shift

        want=$(tenths sleep "$@")
        got=$(tenths "$binaries/sleep" "$@")

        if [ "$want" = "$got" ]; then
                report ok
                return 0
        fi

        report bad "$name" "want ${want} tenths, got ${got} tenths"
}

# ------------------------------------------------------------------

fixture=$work/fixture
mkdir -p "$fixture/sub/inner" "$fixture/empty"
printf 'a\n' > "$fixture/alpha"
printf 'bbbbbbbbbbbbbbbb\n' > "$fixture/beta.txt"
printf 'c\n' > "$fixture/sub/gamma.txt"
printf 'd\n' > "$fixture/sub/inner/delta"
ln -s ../alpha "$fixture/sub/back"
chmod 0751 "$fixture/sub"
touch -d @1500000000 "$fixture/alpha"
touch -d @1400000000 "$fixture/beta.txt"
printf 'old\n' > "$fixture/ancient"
touch -d @1000000000 "$fixture/ancient"

group basename
same 'path'             basename /usr/bin/ls
same 'trailing slash'   basename /usr/bin/
same 'no slash'         basename usr
same 'root'             basename /
same 'double root'      basename //
same 'dot'              basename .
same 'dotdot'           basename ..
same 'suffix'           basename file.txt .txt
same 'suffix absent'    basename file.txt .c
same 'suffix is all'    basename .txt .txt
same 'multiple'         basename -a /a/b /c/d
same 'suffix flag'      basename -s .txt one.txt two.txt

group dirname
same 'path'             dirname /usr/bin/ls
same 'trailing slash'   dirname /usr/bin/
same 'no slash'         dirname usr
same 'root'             dirname /
same 'one level'        dirname /a
same 'many'             dirname /a/b /c ./d

group seq
same 'last'             seq 5
same 'first last'       seq 3 9
same 'increment'        seq 2 3 20
same 'negative'         seq -4 4
same 'descending'       seq 10 -2 1
same 'empty'            seq 9 2
same 'padded'           seq -w 8 11
same 'separator'        seq -s , 1 5
same 'one'              seq 1 1

group readlink
same 'link'             readlink "$fixture/sub/back"
same 'not a link'       readlink "$fixture/alpha"
same 'resolve'          readlink -f "$fixture/sub/back"
same 'resolve plain'    readlink -f "$fixture/alpha"
same 'resolve missing'  readlink -f "$fixture/nothing"
same 'exists missing'   readlink -e "$fixture/nothing"

group realpath
same 'plain'            realpath "$fixture/alpha"
same 'through link'     realpath "$fixture/sub/back"
same 'dots'             realpath "$fixture/sub/../alpha"
same 'directory'        realpath "$fixture/sub/inner"
same 'root'             realpath /
same 'missing'          realpath "$fixture/nothing"
same 'missing allowed'  realpath -m "$fixture/nothing/at/all"

group id
same 'default'          id
same 'user'             id -u
same 'group'            id -g
same 'user name'        id -un
same 'group name'       id -gn
same 'groups'           id -G

group uname
same 'system'           uname
same 'node'             uname -n
same 'release'          uname -r
same 'machine'          uname -m
near 'all'              'cat' uname -snrvm

group find
near 'plain'            'LC_ALL=C sort' find "$fixture"
near 'name'             'LC_ALL=C sort' find "$fixture" -name '*.txt'
near 'type file'        'LC_ALL=C sort' find "$fixture" -type f
near 'type directory'   'LC_ALL=C sort' find "$fixture" -type d
near 'type link'        'LC_ALL=C sort' find "$fixture" -type l
near 'maxdepth 1'       'LC_ALL=C sort' find "$fixture" -maxdepth 1
near 'maxdepth 2'       'LC_ALL=C sort' find "$fixture" -maxdepth 2
near 'mindepth'         'LC_ALL=C sort' find "$fixture" -mindepth 2
near 'size bytes'       'LC_ALL=C sort' find "$fixture" -type f -size -10c
near 'size blocks'      'LC_ALL=C sort' find "$fixture" -type f -size 1
near 'empty'            'LC_ALL=C sort' find "$fixture" -empty
near 'name and type'    'LC_ALL=C sort' find "$fixture" -type f -name '*a*'
near 'name question'    'LC_ALL=C sort' find "$fixture" -name 'sub?'
near 'name class'       'LC_ALL=C sort' find "$fixture" -name '[ab]*'
near 'name negated'     'LC_ALL=C sort' find "$fixture" -name '[!ab]*'
near 'name exact'       'LC_ALL=C sort' find "$fixture" -name alpha
near 'path'             'LC_ALL=C sort' find "$fixture" -path '*sub*'
near 'perm'             'LC_ALL=C sort' find "$fixture" -perm 751
near 'size plus'        'LC_ALL=C sort' find "$fixture" -type f -size +10c
near 'depth and name'   'LC_ALL=C sort' find "$fixture" -maxdepth 1 -type f
near 'two roots'        'LC_ALL=C sort' find "$fixture/sub" "$fixture/empty"

group stat
same 'name'             stat -c '%n' "$fixture/alpha"
same 'size'             stat -c '%s' "$fixture/beta.txt"
same 'octal mode'       stat -c '%a' "$fixture/sub"
same 'mode letters'     stat -c '%A' "$fixture/sub"
same 'kind'             stat -c '%F' "$fixture/sub"
same 'kind of link'     stat -c '%F' "$fixture/sub/back"
same 'links'            stat -c '%h' "$fixture/alpha"
same 'owner'            stat -c '%u %U' "$fixture/alpha"
same 'group'            stat -c '%g %G' "$fixture/alpha"
same 'blocks'           stat -c '%b %B' "$fixture/alpha"
same 'inode'            stat -c '%i' "$fixture/alpha"
same 'modify epoch'     stat -c '%Y' "$fixture/alpha"
same 'access epoch'     stat -c '%X' "$fixture/beta.txt"
same 'modify stamp'     stat -c '%y' "$fixture/alpha"
same 'raw mode'         stat -c '%f' "$fixture/alpha"
same 'many'             stat -c '%n %s %a %F' "$fixture/alpha" "$fixture/sub"
same 'escapes'          stat -c '%n:%s' "$fixture/alpha"
same 'follow'           stat -L -c '%F' "$fixture/sub/back"
same 'follow size'      stat -L -c '%s' "$fixture/sub/back"
same 'io block'         stat -c '%o' "$fixture/alpha"
same 'device'           stat -c '%d' "$fixture/alpha"
same 'change epoch'     stat -c '%Z' "$fixture/alpha"
same 'access stamp'     stat -c '%x' "$fixture/beta.txt"
same 'change stamp'     stat -c '%z' "$fixture/beta.txt"
same 'literal percent'  stat -c '100%%' "$fixture/alpha"
same 'tab escape'       stat -c '%n\t%s' "$fixture/alpha"
same 'directory octal'  stat -c '%a %A %F' "$fixture/empty"
same 'default block'    stat "$fixture/alpha"
same 'default directory' stat "$fixture/sub"
# The Access line is dropped for the link only: reading a symlink is what
# sets its access time, so whichever of the two tools runs second sees the
# time the first one caused. Every other line is compared.
near 'default link'     "grep -v '^Access: 2'" stat "$fixture/sub/back"
same 'quoted name'      stat -c '%N' "$fixture/alpha"
same 'quoted link'      stat -c '%N' "$fixture/sub/back"

group ls
near 'plain'            'cat' ls "$fixture"
near 'all'              'cat' ls -a "$fixture"
near 'almost all'       'cat' ls -A "$fixture"
near 'one per line'     'cat' ls -1 "$fixture"
near 'time sorted'      'cat' ls -t "$fixture"
near 'size sorted'      'cat' ls -S "$fixture"
near 'reversed'         'cat' ls -r "$fixture"
near 'directory itself' 'cat' ls -d "$fixture"
near 'recursive'        'cat' ls -R "$fixture"
near 'inode'            "awk '{print \$NF}'" ls -i "$fixture"
same 'long'             ls -l "$fixture"
same 'long human'       ls -lh "$fixture"
same 'long links'       ls -l "$fixture/sub"
same 'long time sorted' ls -lt "$fixture"
same 'long size sorted' ls -lS "$fixture"
same 'long reversed'    ls -lr "$fixture"
same 'long all'         ls -la "$fixture"
same 'long numeric'     ls -n "$fixture"
same 'long inode'       ls -li "$fixture"
same 'long recursive'   ls -lR "$fixture"
same 'long empty dir'   ls -l "$fixture/empty"
near 'all time reverse' 'cat' ls -atr "$fixture"
near 'a file'           'cat' ls "$fixture/alpha"
near 'two operands'     'cat' ls "$fixture/sub" "$fixture/empty"
near 'file and dir'     'cat' ls "$fixture/alpha" "$fixture/empty"
near 'recursive deep'   'cat' ls -R "$fixture/sub"
near 'recursive all'    'cat' ls -aR "$fixture/empty"
near 'missing'          'cat' ls "$fixture/nothing"

group du
near 'summary'          "awk '{print \$1}'" du -s "$fixture"
near 'all'              "LC_ALL=C sort" du "$fixture"
near 'every file'       "LC_ALL=C sort" du -a "$fixture"
near 'apparent'         "awk '{print \$1}'" du -sb "$fixture"
near 'human summary'    "awk '{print \$1}'" du -sh "$fixture"
near 'with total'       "LC_ALL=C sort" du -c "$fixture/sub"
near 'summary of two'   "LC_ALL=C sort" du -s "$fixture/sub" "$fixture/empty"
near 'apparent all'     "LC_ALL=C sort" du -ab "$fixture"

group df
# The digits are masked and their count is not: how full a filesystem is
# changes while the two tools are being run -- this suite writes into /tmp
# between them -- and the column layout is the part that is being compared.
near 'whole table'      "sed 's/[0-9]/X/g'" df
near 'human'            "sed 's/[0-9]/X/g'" df -h
near 'root'             "sed 's/[0-9]/X/g'" df /
near 'a path'           "sed 's/[0-9]/X/g'" df /tmp
near 'mount points'     "tail -n +2 | awk '{print \$NF}' | LC_ALL=C sort" df

group env
# The shell sets _ to the path of the command it is about to run, so the two
# tools disagree on that one line by construction and it is dropped.
near 'listing'          "grep -v '^_='" env
same 'empty'            env -i
same 'set and run'      env NEW=here /bin/sh -c 'echo $NEW'
same 'empty and run'    env -i /bin/sh -c 'echo [$PATH]'
same 'unset and run'    env -u HOME /bin/sh -c 'echo [$HOME]'
same 'plain command'    env /bin/echo through

group chown
effect 'user and group' chown '$TOOL '"$(id -un):$(id -gn)"' tree/one'
effect 'numeric'        chown '$TOOL '"$(id -u):$(id -g)"' tree/one'
effect 'user only'      chown '$TOOL '"$(id -un)"' tree/one'
effect 'group only'     chown '$TOOL :'"$(id -gn)"' tree/two'
effect 'recursive'      chown '$TOOL -R '"$(id -un):$(id -gn)"' tree'
effect 'unknown user'   chown '$TOOL nosuchuser tree/one'
effect 'through link'   chown '$TOOL '"$(id -un)"' link'
effect 'not the link'   chown '$TOOL -h '"$(id -un)"' link'

group sleep
timing 'fraction'       0.3
timing 'seconds'        1
timing 'with suffix'    2s
timing 'and a half'     1.5
same 'bad interval'     sleep nonsense

group mkdir
effect 'plain'          mkdir '$TOOL made'
effect 'parents'        mkdir '$TOOL -p a/b/c/d'
effect 'parents exist'  mkdir '$TOOL -p tree/deep/deeper'
effect 'mode'           mkdir '$TOOL -m 0700 walled'
effect 'parents mode'   mkdir '$TOOL -p -m 0705 x/y'

group rmdir
effect 'empty'          rmdir 'mkdir gone; $TOOL gone'
effect 'not empty'      rmdir '$TOOL tree'
effect 'parents'        rmdir 'mkdir -p a/b/c; $TOOL -p a/b/c'

group chmod
effect 'octal'          chmod '$TOOL 0600 tree/one'
effect 'octal directory' chmod '$TOOL 0711 tree/deep'
effect 'symbolic add'   chmod '$TOOL u+x tree/one'
effect 'symbolic remove' chmod '$TOOL go-rwx tree/two'
effect 'symbolic set'   chmod '$TOOL a=rw tree/one'
effect 'symbolic many'  chmod '$TOOL u+rw,g=r,o-rwx tree/two'
effect 'capital x'      chmod '$TOOL -R a+X tree'
effect 'recursive'      chmod '$TOOL -R 0755 tree'
effect 'setuid'         chmod '$TOOL 4755 tree/one'
effect 'sticky'         chmod '$TOOL 1777 tree/deep'
effect 'through link'   chmod '$TOOL 0600 link'

group ln
effect 'symbolic'       ln '$TOOL -s tree/one pointer'
effect 'symbolic force' ln '$TOOL -s tree/two link'
effect 'hard'           ln '$TOOL tree/one hard'
effect 'into directory' ln '$TOOL -s ../plain tree/'

group touch
effect 'create'         touch '$TOOL fresh'
effect 'no create'      touch '$TOOL -c absent'
effect 'reference'      touch '$TOOL -r tree/one tree/two'
effect 'modify only'    touch '$TOOL -m -r tree/one tree/two'
effect 'access only'    touch '$TOOL -a -r tree/one tree/two'
effect 'epoch date'     touch '$TOOL -d @1234567890 tree/two'
effect 'epoch on new'   touch '$TOOL -d @1000000000 born'
effect 'modify epoch'   touch '$TOOL -m -d @1234567890 tree/one'
effect 'bad date'       touch '$TOOL -d nonsense tree/one'
effect 'many'           touch '$TOOL -d @900000000 tree/one tree/two plain'

group cp
effect 'file'           cp '$TOOL tree/one copy'
effect 'over file'      cp '$TOOL tree/one plain'
effect 'into directory' cp '$TOOL plain tree/'
effect 'recursive'      cp '$TOOL -r tree copied'
effect 'preserving'     cp '$TOOL -rp tree kept'
effect 'many into dir'  cp '$TOOL tree/one tree/two tree/deep/'

group mv
effect 'rename'         mv '$TOOL plain renamed'
effect 'into directory' mv '$TOOL plain tree/'
effect 'directory'      mv '$TOOL tree moved'
effect 'over file'      mv '$TOOL tree/one tree/two'
effect 'many'           mv '$TOOL tree/one tree/two tree/deep/'

group rm
effect 'file'           rm '$TOOL plain'
effect 'directory'      rm '$TOOL tree'
effect 'recursive'      rm '$TOOL -r tree'
effect 'forced missing' rm '$TOOL -f nothing'
effect 'recursive force' rm '$TOOL -rf tree plain nothing'
effect 'link'           rm '$TOOL link'

#
#       Names that are awkward to hold.
#
#       A space, a leading dash, a newline: every one of these has been a
#       whole class of bug in a tool that built its output by pasting words
#       together, and nothing above uses a name harder than "beta.txt".
#

odd=$work/odd
mkdir -p "$odd/a dir"
printf 'x\n' > "$odd/with space"
printf 'x\n' > "$odd/-dash"
printf 'xx\n' > "$odd/two  spaces"
printf 'x\n' > "$odd/a dir/inner file"
printf 'x\n' > "$odd/dot.in.the.middle"
ln -s "with space" "$odd/link to space"
ln -s nowhere "$odd/broken"
ln -s loop "$odd/loop"

group awkward
same 'basename space'   basename "$odd/with space"
same 'basename dash'    basename "$odd/-dash"
same 'dirname space'    dirname "$odd/a dir/inner file"
same 'realpath space'   realpath "$odd/with space"
same 'readlink space'   readlink "$odd/link to space"
same 'readlink broken'  readlink "$odd/broken"
same 'stat space'       stat "$odd/with space"
near 'stat link'        "grep -v '^Access: 2'" stat "$odd/link to space"
near 'stat broken'      "grep -v '^Access: 2'" stat "$odd/broken"
near 'stat loop'        "grep -v '^Access: 2'" stat "$odd/loop"
same 'long listing'     ls -l "$odd"
same 'long inner'       ls -l "$odd/a dir"
near 'listed'           'cat' ls "$odd"
near 'found'            "LC_ALL=C sort" find "$odd"
near 'measured'         "LC_ALL=C sort" du -a "$odd"
same 'realpath of link' realpath "$odd/link to space"
same 'realpath broken'  realpath "$odd/broken"
same 'realpath loop'    realpath "$odd/loop"
effect 'copy a space'   cp '$TOOL tree/one "a name with spaces"'
effect 'move a space'   mv '$TOOL tree/one "a name with spaces"'
effect 'remove a space' rm '$TOOL "nothing here" 2>/dev/null'
effect 'make a space'   mkdir '$TOOL "a made dir"'
effect 'touch a space'  touch '$TOOL "a touched file"'
effect 'link a space'   ln '$TOOL -s "tree/one" "a linked name"'

#       mktemp, whose whole point is a name nobody can predict -- so what the
#       two tools print cannot be compared with each other directly. What is
#       compared is the contract both of them are held to: the name is the
#       template with every X replaced by something, in the directory the
#       options asked for, and what each tool left on disk is the same tree.

# The printed name, with the template's X positions blanked out. A name of a
# different length cannot be shaped and says so, which is the case where a
# tool replaced too few characters or none at all.
shaped() {
        given=$1
        pattern=$2
        made=""
        at=1

        if [ "${#given}" != "${#pattern}" ]; then
                printf 'wrong length'
                return 0
        fi

        while [ "$at" -le "${#pattern}" ]; do
                letter=$(printf '%s' "$pattern" | cut -c "$at")

                if [ "$letter" = X ]; then
                        made="$made#"
                else
                        made="$made$(printf '%s' "$given" | cut -c "$at")"
                fi

                at=$((at + 1))
        done

        printf '%s' "$made"
}

temporary() {
        name=$1
        pattern=$2
        shift 2

        rm -rf "$work/a" "$work/b"
        mkdir -p "$work/a" "$work/b"

        if want=$(cd "$work/a" && TMPDIR=$work/a mktemp "$@" 2>/dev/null); then
                want_status=0
        else
                want_status=$?
        fi

        if got=$(cd "$work/b" && TMPDIR=$work/b "$binaries/mktemp" "$@" 2>/dev/null); then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$want_status" != "$got_status" ]; then
                report bad "$name" "want status $want_status, got status $got_status"
                return 0
        fi

        if [ "$want_status" != 0 ]; then
                want=nothing-was-made
                got=nothing-was-made
        else
                wanted=$(printf '%s' "$pattern" | tr X '#')
                want_shape=$(shaped "${want##*/}" "$pattern")
                got_shape=$(shaped "${got##*/}" "$pattern")
                want_where=.
                got_where=.

                case $want in */*) want_where=$(printf '%s' "${want%/*}" | sed "s|$work/a|DIR|") ;; esac
                case $got in */*) got_where=$(printf '%s' "${got%/*}" | sed "s|$work/b|DIR|") ;; esac

                if [ "$want_shape" != "$wanted" ] || [ "$got_shape" != "$wanted" ]; then
                        report bad "$name" "wanted $wanted, want [$want_shape] got [$got_shape]"
                        return 0
                fi

                # Blanking the X positions cannot tell an answer from the
                # question, so the question being given back is caught here.
                if [ "${want##*/}" = "$pattern" ] || [ "${got##*/}" = "$pattern" ]; then
                        report bad "$name" "the template came back unchanged"
                        return 0
                fi

                if [ "$want_where" != "$got_where" ]; then
                        report bad "$name" "want in $want_where, got in $got_where"
                        return 0
                fi
        fi

        # A run that made nothing has no name to mask out.
        dump "$work/a" | sed "s|${want##*/}|NAME|g" > "$work/want"
        dump "$work/b" | sed "s|${got##*/}|NAME|g" > "$work/got"

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "$(diff "$work/want" "$work/got" | head -4 | tr '\n' '|')"
}

group mktemp
temporary 'default'            tmp.XXXXXXXXXX
temporary 'default directory'  tmp.XXXXXXXXXX -d
temporary 'default unmade'     tmp.XXXXXXXXXX -u
temporary 'template'           run.XXXXXX     run.XXXXXX
temporary 'template directory' run.XXXXXX     -d run.XXXXXX
temporary 'template unmade'    run.XXXXXX     -u run.XXXXXX
temporary 'long field'         run.XXXXXXXXXXXX run.XXXXXXXXXXXX
temporary 'least field'        run.XXX        run.XXX
temporary 'suffix kept'        run.XXXXXX.log run.XXXXXX.log
temporary 'too few'            run.XX         run.XX
temporary 'no field'           run            run
temporary 'in tmpdir'          run.XXXXXX     -t run.XXXXXX
temporary 'quiet failure'      run.XX         -q run.XX
temporary 'unmade directory'   run.XXXXXX     -u -d run.XXXXXX
temporary 'missing directory'  run.XXXXXX     sub/run.XXXXXX

printf '  %-12s %s of %s\n' files "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'files %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
