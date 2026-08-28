#!/bin/sh
#
#       The compositor, looked at.
#
#           sh src/test/canvas.sh [image]
#
#       Every other lane reads bytes. This one reads pixels: it boots the
#       image under qemu with no display, asks the monitor for the framebuffer
#       and answers questions about what is on it.
#
#       It exists because the kernel log window shipped broken and nothing
#       here could tell. The boot lane reads the serial transcript, which said
#       everything was fine while the window on the screen was writing every
#       message where the last one ended -- printk says \n and means the start
#       of the next line, and the emulator behind that window is where a pty's
#       line discipline would have put the \r. A picture would have caught it
#       in a second; there was no picture.
#
#       Input goes in through QMP rather than the human monitor, because
#       mouse_move there is relative whatever device is selected and a tablet
#       has no relative axes to move along.

image=${1:-dist/bootx64.efi}

pass=0
fail=0

check()
{
        if [ "$2" = "$3" ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-24s want %s   got %s\n' "$1" "$3" "$2"
}

if ! command -v qemu-system-x86_64 > /dev/null 2>&1; then
        echo "  canvas       no qemu-system-x86_64, skipped"
        exit 2
fi

if ! command -v python3 > /dev/null 2>&1; then
        echo "  canvas       no python3, skipped"
        exit 2
fi

if [ ! -f "$image" ]; then
        echo "  canvas       no image at $image, skipped"
        exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/dawning-canvas.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

MOONWATER_CANVAS_WORK=$work
export MOONWATER_CANVAS_WORK

python3 - "$image" > "$work/answers" 2>"$work/why" <<'PY'
import json, os, socket, subprocess, sys, time

image = sys.argv[1]
work = os.environ["MOONWATER_CANVAS_WORK"]
qmp = work + "/qmp"
mon = work + "/mon"
W, H = 2560, 1080

def shot(name):
    hf.write(("screendump %s/%s.ppm\n" % (work, name)).encode())
    hf.flush()
    time.sleep(2)

def load(name):
    d = open("%s/%s.ppm" % (work, name), "rb").read()
    i, vals = 2, []
    while len(vals) < 3:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        vals.append(int(d[i:j]))
        i = j
    return vals[0], vals[1], d[i + 1:]

guest = subprocess.Popen([
    "qemu-system-x86_64", "-m", "2G", "-smp", "2", "-cpu", "Nehalem",
    "-kernel", image, "-vga", "none", "-device", "virtio-gpu-pci",
    "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd",
    "-no-reboot", "-display", "none", "-serial", "file:" + work + "/serial",
    "-qmp", "unix:" + qmp + ",server,nowait",
    "-monitor", "unix:" + mon + ",server,nowait",
    "-append", "console=ttyS0 drm_client_lib.active="],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for _ in range(200):
    if os.path.exists(qmp) and os.path.exists(mon):
        break
    time.sleep(0.2)

qs = socket.socket(socket.AF_UNIX)
qs.connect(qmp)
qs.settimeout(20)
qf = qs.makefile("rwb")
qf.readline()

def qcmd(obj):
    qf.write((json.dumps(obj) + "\n").encode())
    qf.flush()
    while True:
        line = qf.readline()
        if not line:
            return {}
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "event" not in msg:
            return msg

qcmd({"execute": "qmp_capabilities"})

hs = socket.socket(socket.AF_UNIX)
hs.connect(mon)
hf = hs.makefile("rwb")
time.sleep(0.5)

def send(events):
    qcmd({"execute": "input-send-event", "arguments": {"events": events}})

def moveto(x, y):
    send([{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
          {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}])
    time.sleep(0.1)

#       Long enough for the desktop, the shell and the boot log replay. The
#       serial transcript is not what is being read, but a machine that never
#       got that far has nothing on its screen either.
time.sleep(25)

out = []

shot("first")
w, h, px = load("first")

def at(x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]

bg = at(4, 4)

#       Where the windows are, found rather than assumed: a run of non
#       background across the middle of the screen is a window.
runs, start = [], None
for x in range(w):
    lit = at(x, h // 2) != bg
    if lit and start is None:
        start = x
    elif not lit and start is not None:
        if x - start > 40:
            runs.append((start, x - 1))
        start = None

out.append(("windows", str(len(runs)), "2"))

if len(runs) >= 1:
    left, right = runs[0]
    top = next((y for y in range(h) if at((left + right) // 2, y) != bg), 0)

    #       Every row of the log starts at the left.
    #
    #       This is the one the missing carriage return broke. A message that
    #       begins where the last one ended leaves its row blank on the left
    #       and its text out in the middle, so counting rows whose first lit
    #       pixel is near the margin says whether the log is a log or a mess.
    def lit(x, y):
        r, g, b = at(x, y)
        return r + g + b > 180

    margin = 0
    rows = 0
    for row in range(3, 26):
        band = range(top + 24 + row * 16, min(top + 24 + row * 16 + 16, h))
        first = None
        for x in range(left + 2, right - 2):
            if any(lit(x, y) for y in band):
                first = x
                break
        if first is None:
            continue
        rows += 1
        if first - left < 28:
            margin += 1

    out.append(("log rows at the margin", "yes" if margin >= rows - 4 else
                "%d of %d" % (margin, rows), "yes"))

#       A click on a titlebar takes the focus, which the titlebar says.
if len(runs) >= 2:
    a_left, a_right = runs[0]
    b_left, b_right = runs[1]
    a_top = next((y for y in range(h) if at((a_left + a_right) // 2, y) != bg), 0)
    before = at((a_left + a_right) // 2, a_top + 4)

    moveto((a_left + a_right) // 2, a_top + 6)
    send([{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.3)
    send([{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(1.5)

    shot("focused")
    w2, h2, px = load("focused")
    after = at((a_left + a_right) // 2, a_top + 4)

    out.append(("focus follows a click", "moved" if after != before else "same",
                "moved"))

hf.write(b"quit\n")
hf.flush()
time.sleep(1)
guest.terminate()
try:
    guest.wait(10)
except Exception:
    guest.kill()

for name, got, want in out:
    print("%s|%s|%s" % (name, got, want))
PY

if [ ! -s "$work/answers" ]; then
        echo "  canvas       the guest answered nothing"
        sed 's/^/    /' "$work/why" | tail -5
        exit 1
fi

while IFS='|' read -r name got want; do
        check "$name" "$got" "$want"
done < "$work/answers"

printf '  %-12s %s of %s\n' canvas "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'canvas %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
