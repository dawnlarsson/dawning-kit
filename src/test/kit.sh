#!/bin/sh
#
#       The kits the build scripts are made of.
#
#           sh src/test/kit.sh
#
#       kit/bit.sh writes bytes and kit/doc.sh writes HTML, and both were
#       tested only by heredocs pasted into the CI workflow -- which meant
#       they could not be run without pushing, and were not run at all by
#       anybody working on them.
#
#       bit_N has to emit the same bytes under bash and under dash, and the
#       right NUMBER of bytes for input that does not fit. doc has to escape
#       what a document can contain, because a generator that does not is a
#       way of writing somebody else's script into a page.
#
set -u

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0
current=""

group() { current=$1; }

won() { pass=$((pass + 1)); }

lost()
{
        fail=$((fail + 1))
        printf '  %-10s %-24s %s\n' "$current" "$1" "$2"
}

same()
{
        name=$1
        want=$2
        got=$3

        if [ "$want" = "$got" ]; then
                won
                return 0
        fi

        lost "$name" "want [$want] got [$got]"
}

#
#       Bytes.
#

bytes()
{
        printf '. ./kit/bit.sh\nBIT_QUIET=1\n%s | od -An -tx1 | tr -d " \\n"\n' \
                "$1" > "$work/bit.sh"

        "$2" "$work/bit.sh"
}

group bit

for shell in bash dash; do
        command -v "$shell" > /dev/null 2>&1 || continue

        same "hex $shell"       41       "$(bytes 'bit_8 0x41' "$shell")"
        same "decimal $shell"   41       "$(bytes 'bit_8 65' "$shell")"
        same "string $shell"    454c46   "$(bytes 'bit_8 ELF' "$shell")"
        same "overflow $shell"  2c       "$(bytes 'bit_8 300' "$shell")"
        same "two bytes $shell" 3412     "$(bytes 'bit_16 0x1234' "$shell")"
        same "too wide $shell"  5634     "$(bytes 'bit_16 0x123456' "$shell")"
        same "negative $shell"  ffff     "$(bytes 'bit_16 -1' "$shell")"
        same "four $shell"      44332211 "$(bytes 'bit_32 0x11223344' "$shell")"
        same "eight $shell"     0100000000000000 "$(bytes 'bit_64 1' "$shell")"
        same "sixteen $shell"   01000000000000000000000000000000 \
                "$(bytes 'bit128 1' "$shell")"
        same "commas $shell"    48c7c0   "$(bytes 'bit_8 0x48,0xc7,0xc0' "$shell")"
        same "zero $shell"      00       "$(bytes 'bit_8 0' "$shell")"
        same "top byte $shell"  ff       "$(bytes 'bit_8 255' "$shell")"
        same "wraps $shell"     00       "$(bytes 'bit_8 256' "$shell")"

        # The variable length integers a wasm section is measured in. 128 is
        # the first that needs a second byte, and getting the continuation bit
        # wrong there produces a module that will not validate.
        same "leb small $shell" 00   "$(bytes 'wasm_var 0' "$shell")"
        same "leb 127 $shell"   7f   "$(bytes 'wasm_var 127' "$shell")"
        same "leb 128 $shell"   8001 "$(bytes 'wasm_var 128' "$shell")"
        same "leb 300 $shell"   ac02 "$(bytes 'wasm_var 300' "$shell")"
done

#
#       An ELF that a loader would accept.
#

group elf

cat > "$work/elf.sh" <<'SCRIPT'
. ./kit/bit.sh
x64() { bit_8 0x48,0xc7,0xc0,0x3c,0x00,0x00,0x00
        bit_8 0x48,0xc7,0xc7,0x00,0x00,0x00,0x00
        bit_8 0x0f,0x05; }
arm() { bit_32 0xd2800ba8; bit_32 0xd2800000; bit_32 0xd4000001; }
elf "$1/x64" x64
elf "$1/arm" arm aarch64
SCRIPT

sh "$work/elf.sh" "$work" > /dev/null 2>&1

if command -v python3 > /dev/null 2>&1; then
        for machine in x64:62 arm:183; do
                name=${machine%:*}
                number=${machine#*:}

                same "$name valid" ok "$(python3 - "$work/$name" "$number" <<'PYTHON'
import struct, sys

data = open(sys.argv[1], 'rb').read()
machine = int(sys.argv[2])

head = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
segment = struct.unpack_from('<IIQQQQQQ', data, 64)

why = []
if data[:4] != b'\x7fELF':
        why.append('not an elf')
if head[1] != machine:
        why.append('machine %s' % head[1])
if segment[1] != 5:
        why.append('segment flags %s' % segment[1])
if head[3] != segment[3] + 120:
        why.append('entry %s' % head[3])
if segment[5] != len(data):
        why.append('size %s of %s' % (segment[5], len(data)))

print(', '.join(why) if why else 'ok')
PYTHON
)"
        done
else
        printf '  %-10s %s\n' elf 'no python3, skipped'
fi

#
#       A wasm module a runtime would accept.
#

group wasm

cat > "$work/wasm.sh" <<'SCRIPT'
. ./kit/bit.sh
type_section()     { wasm_var 1; bit_8 0x60; bit_8 0x00,0x01,0x7F; }
function_section() { wasm_var 1; wasm_var 0; }
export_section()   { wasm_var 1; wasm_var 4; bit_8 "main"; bit_8 0x00,0x00; }
code_body()        { wasm_var 0; bit_8 0x41,0x00,0x0B; }
code_section()     { wasm_var 1; wasm_body code_body; }
module()           { wasm_section 1 type_section
                     wasm_section 3 function_section
                     wasm_section 7 export_section
                     wasm_section 10 code_section; }
wasm "$1/example.wasm" module
SCRIPT

sh "$work/wasm.sh" "$work" > /dev/null 2>&1

if command -v node > /dev/null 2>&1; then
        same 'validates and runs' 0 "$(node -e '
                const b = require("fs").readFileSync(process.argv[1]);
                if (!WebAssembly.validate(b)) { console.log("does not validate"); process.exit(0); }
                const i = new WebAssembly.Instance(new WebAssembly.Module(b));
                console.log(i.exports.main());
        ' "$work/example.wasm" 2>&1)"
else
        printf '  %-10s %s\n' wasm 'no node, skipped'
fi

#
#       HTML that carries no script.
#

group doc

doc_of()
{
        printf '%s' "$1" > "$work/in.md"
        bash -c ". ./kit/doc.sh; doc $work/in.md" > "$work/out.html" 2>/dev/null || true
}

carries()
{
        case $(cat "$work/out.html") in
        *"$2"*) won ;;
        *) lost "$1" "no $2 in the output" ;;
        esac
}

lacks()
{
        case $(cat "$work/out.html") in
        *"$2"*) lost "$1" "$2 survived" ;;
        *) won ;;
        esac
}

if command -v bash > /dev/null 2>&1; then
        doc_of 'a & b < c > d
'
        carries 'ampersand'     'a &amp; b &lt; c &gt; d'

        doc_of '[x](javascript:alert(1))
'
        lacks 'javascript url'  'javascript:'

        doc_of '![i](x" onerror="alert(1))
'
        lacks 'attribute out'   ' onerror='

        doc_of 'he said "hi"
'
        carries 'quote in text' '<p>he said "hi"</p>'

        doc_of 'a < b
'
        carries 'less than'     'a &lt; b'

        doc_of '&amp;
'
        carries 'entity twice'  '&amp;amp;'

        doc_of '[x](data:text/html,alert)
'
        lacks 'data url'        'data:text/html'

        doc_of '[x](data:image/png,abc)
'
        carries 'image data url' 'data:image/png,abc'

        doc_of '[x](https://example.com/a)
'
        carries 'plain link'    'href="https://example.com/a"'

        #       Anything that looks like a tag is written straight out. That
        #       is markdown's own rule and it is what makes a document able to
        #       carry a table, but it also means the escaping above is a
        #       promise about text and not about a document from a stranger.
        #       Recorded rather than left out: tightening it should fail here.
        doc_of '<script>alert(1)</script>
'
        carries 'raw tag kept'  '<script>alert(1)</script>'

        doc_of '<img src=x onerror=alert(1)>
'
        carries 'raw attribute' 'onerror=alert(1)'

        doc_of '# a heading

some text
'
        carries 'a heading'     '<h1'

        if bash -c '. ./kit/doc.sh; doc /nonexistent.md' > /dev/null 2>&1; then
                lost 'missing input' 'a missing file was not an error'
        else
                won
        fi
else
        printf '  %-10s %s\n' doc 'no bash, skipped'
fi

#
#       The library compaction passes are generators too. In particular, the
#       alias block must survive restructuring byte for byte: if its current
#       shape is compared with an old template, the second pass renames both
#       sides and quietly creates aliases from a symbol to itself.
#

group compact

if command -v python3 > /dev/null 2>&1; then
        cp src/library.c "$work/library.c"
        python3 kit/compact/restructure.py "$work/library.c" > /dev/null 2>&1
        python3 kit/compact/restructure.py "$work/library.c" > /dev/null 2>&1

        same 'aliases preserved' yes "$(python3 - src/library.c "$work/library.c" <<'PYTHON'
import re, sys

pattern = (r'/\*\n        The libc names, as second labels rather than as code\..*?'
           r'^__asm__\(\n.*?^\);\n\n')

blocks = []
for path in sys.argv[1:]:
    found = re.search(pattern, open(path).read(), flags=re.S | re.M)
    blocks.append(found.group(0) if found else None)

print('yes' if blocks[0] is not None and blocks[0] == blocks[1] else 'no')
PYTHON
)"

        mkdir -p "$work/inventory-tree/platform"
        cp src/library.c "$work/inventory-tree/library.c"
        cp src/platform/*.inc "$work/inventory-tree/platform/"
        python3 kit/compact/inventory.py "$work/inventory-tree/library.c" \
                > /dev/null 2>&1

        if cmp -s src/library.c "$work/inventory-tree/library.c"; then
                won
        else
                lost 'inventory current' 'src/library.c differs from its generator'
        fi

        if python3 kit/compact/inventory.py --check --target all src/library.c \
                > /dev/null 2> "$work/inventory-all.err"; then
                won
        else
                lost 'all-platform asm' "$(tail -8 "$work/inventory-all.err" | tr '\n' ' ')"
        fi

        if python3 src/test/inventory.py > "$work/inventory-test.out" 2>&1; then
                won
        else
                lost 'inventory audit' "$(tail -8 "$work/inventory-test.out" | tr '\n' ' ')"
        fi
else
        printf '  %-10s %s\n' compact 'no python3, skipped'
fi

printf '  %-12s %s of %s\n' kit "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'kit %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
