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

def shot(name, settle=2):
    hf.write(("screendump %s/%s.ppm\n" % (work, name)).encode())
    hf.flush()
    time.sleep(settle)

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

processor = ["-cpu", "Nehalem"]
if os.access("/dev/kvm", os.R_OK | os.W_OK):
    # The floor is covered by boot.sh.  Here the widest host path matters:
    # compositor copies to a trapped device mapping have failed only when a
    # real processor selected the AVX-512 body that KVM then had to emulate.
    processor = ["-cpu", "host", "-enable-kvm"]

guest = subprocess.Popen([
    "qemu-system-x86_64", "-m", "2G", "-smp", "2"] + processor + [
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

def key(code):
    # A real keyboard event, not bytes slipped into the serial console.  The
    # window server, terminal, pty and interactive shell all have to carry it.
    send([{"type": "key", "data": {"down": True,
                                      "key": {"type": "qcode", "data": code}}},
          {"type": "key", "data": {"down": False,
                                      "key": {"type": "qcode", "data": code}}}])
    time.sleep(0.02)

def chord(*codes):
    events = []
    for code in codes:
        events.append({"type": "key", "data": {
            "down": True, "key": {"type": "qcode", "data": code}}})
    for code in reversed(codes):
        events.append({"type": "key", "data": {
            "down": False, "key": {"type": "qcode", "data": code}}})
    send(events)
    time.sleep(0.2)

def type_text(value):
    names = {" ": "spc", ".": "dot", "/": "slash", "-": "minus",
             "\n": "ret"}
    for character in value:
        key(names.get(character, character))

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

    # Three distinct wheel detents should move three conventional lines each,
    # with no speed multiplier merely because they arrived close together.
    # Compare the static log before and after: its old pixels move down by
    # nine 16-pixel rows. Events are separated enough for the compositor to
    # consume each one, but remain inside the old acceleration window.
    bottom = next((y for y in range(h - 1, top, -1)
                   if at((left + right) // 2, y) != bg), h - 1)
    moveto((left + right) // 2, min(top + 80, bottom - 4))
    for _ in range(3):
        send([{"type": "btn", "data": {"down": True,
                                        "button": "wheel-up"}},
              {"type": "btn", "data": {"down": False,
                                        "button": "wheel-up"}}])
        time.sleep(0.08)

    shot("wheel")
    wheel_w, wheel_h, wheel_px = load("wheel")

    def pixel(data, width, x, y):
        offset = (y * width + x) * 3
        return data[offset:offset + 3]

    content_top = min(top + 20, bottom)
    content_right = max(left + 1, right - 18)
    scores = []
    for shift in range(16, min(320, bottom - content_top), 16):
        same_pixels = 0
        evidence = 0
        for y in range(content_top + shift, bottom - 2):
            for x in range(left + 4, content_right):
                old = pixel(px, w, x, y - shift)
                new = pixel(wheel_px, wheel_w, x, y)
                if sum(old) > 180 or sum(new) > 180:
                    evidence += 1
                    if old == new:
                        same_pixels += 1
        scores.append((same_pixels / max(evidence, 1), shift))

    wheel_shift = max(scores, key=lambda item: item[0])[1] if scores else 0
    out.append(("wheel is three lines", str(wheel_shift), "144"))

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

    # The log window is anchored near the left edge.  The graphical terminal
    # asks to be centred and is therefore the second run.  Exercise the route
    # a person on the machine actually uses: USB keyboard -> compositor ->
    # terminal -> pty -> shell.  The monitor redraws sixteen times, long enough
    # to catch faults tied to a refresh rather than merely to startup.
    b_top = next((y for y in range(h2)
                  if at((b_left + b_right) // 2, y) != bg), 0)
    b_bottom = next((y for y in range(h2 - 1, b_top, -1)
                     if at((b_left + b_right) // 2, y) != bg), h2 - 1)

    moveto((b_left + b_right) // 2, b_top + 6)
    send([{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.2)
    send([{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.5)

    shot("shell-before-monitor")
    before_w, before_h, before_monitor = load("shell-before-monitor")
    type_text("/mointor.sh 0.5 16\n")

    time.sleep(2.5)
    shot("monitor-first")
    first_w, first_h, first_monitor = load("monitor-first")
    time.sleep(2.0)
    shot("monitor-second")
    second_w, second_h, second_monitor = load("monitor-second")

    def changed(a, b):
        different = 0
        top = min(b_top + 20, second_h)
        bottom = min(b_bottom, second_h - 1)
        for y in range(top, bottom + 1):
            for x in range(b_left, min(b_right + 1, second_w)):
                offset = (y * second_w + x) * 3
                if a[offset:offset + 3] != b[offset:offset + 3]:
                    different += 1
        return different

    appeared = changed(before_monitor, first_monitor)
    redrawn = changed(first_monitor, second_monitor)
    out.append(("monitor reached screen", "yes" if appeared > 200 else
                "%d pixels" % appeared, "yes"))
    out.append(("monitor redraws", "yes" if redrawn > 32 else
                "%d pixels" % redrawn, "yes"))

    # Let all sixteen updates complete before looking for the fault report.
    time.sleep(5)

    # A framed pixel window cannot grow beyond the mapping its client owns.
    # This used to maximize to desktop dimensions anyway, then compose read
    # those invented pixels past the end of the mapping. The window demo's
    # blue pane is exactly 200x120; double-click its titlebar and prove those
    # are still the only blue pixels.
    type_text("/window\n")
    time.sleep(0.5)
    shot("pixel-window", 0.3)
    pixel_w, pixel_h, pixel_before = load("pixel-window")

    def colour_bounds(data, width, height, colour):
        xs, ys = [], []
        for y in range(height):
            row = data[y * width * 3:(y + 1) * width * 3]
            for x in range(width):
                if row[x * 3:x * 3 + 3] == colour:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return min(xs), min(ys), max(xs), max(ys)

    blue = colour_bounds(pixel_before, pixel_w, pixel_h, b"\x00\x66\xcc")
    if blue:
        blue_left, blue_top, blue_right, blue_bottom = blue
        moveto((blue_left + blue_right) // 2, max(blue_top - 10, 0))
        for _ in range(2):
            send([{"type": "btn", "data": {"down": True, "button": "left"}}])
            time.sleep(0.08)
            send([{"type": "btn", "data": {"down": False, "button": "left"}}])
            time.sleep(0.12)

        shot("pixel-maximized", 0.3)
        max_w, max_h, pixel_after = load("pixel-maximized")
        blue_after = colour_bounds(pixel_after, max_w, max_h,
                                   b"\x00\x66\xcc")
    else:
        blue_after = None

    if blue_after:
        blue_width = blue_after[2] - blue_after[0] + 1
        blue_height = blue_after[3] - blue_after[1] + 1
        pixel_extent = "%dx%d" % (blue_width, blue_height)
    else:
        pixel_extent = "absent"
    out.append(("pixel maximize is bounded", pixel_extent, "200x120"))

    if blue_after:
        max_left, max_top, max_right, max_bottom = blue_after
        title_x = (max_left + max_right) // 2
        title_y = max(max_top - 10, 0)
        moveto(title_x, title_y)
        send([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.08)
        moveto(min(title_x + 120, W - 1), min(title_y + 80, H - 1))
        send([{"type": "btn", "data": {"down": False, "button": "left"}}])
        shot("pixel-restored", 0.3)
        drag_w, drag_h, pixel_dragged = load("pixel-restored")
        blue_dragged = colour_bounds(pixel_dragged, drag_w, drag_h,
                                     b"\x00\x66\xcc")
    else:
        blue_dragged = None

    restored = (blue_dragged is not None and
                blue_dragged[0] != blue_after[0] and
                blue_dragged[2] - blue_dragged[0] + 1 == 200 and
                blue_dragged[3] - blue_dragged[1] + 1 == 120)
    out.append(("maximized drag restores", "yes" if restored else "no", "yes"))

    # One held Alt with two Tab presses must walk past the next window to the
    # third, rather than bouncing between the top two. The orange 400x260
    # window is third in this demo; raising it changes the pixel where its
    # titlebar was previously obscured.
    orange = (colour_bounds(pixel_dragged, drag_w, drag_h, b"\xff\x99\x00")
              if blue_after else None)
    if orange:
        orange_x = (orange[0] + orange[2]) // 2
        orange_y = max(orange[1] - 10, 0)
        orange_before = pixel(pixel_dragged, drag_w, orange_x, orange_y)
    else:
        orange_before = None

    send([{"type": "key", "data": {"down": True,
                                     "key": {"type": "qcode", "data": "alt"}}},
          {"type": "key", "data": {"down": True,
                                     "key": {"type": "qcode", "data": "tab"}}},
          {"type": "key", "data": {"down": False,
                                     "key": {"type": "qcode", "data": "tab"}}},
          {"type": "key", "data": {"down": True,
                                     "key": {"type": "qcode", "data": "tab"}}},
          {"type": "key", "data": {"down": False,
                                     "key": {"type": "qcode", "data": "tab"}}},
          {"type": "key", "data": {"down": False,
                                     "key": {"type": "qcode", "data": "alt"}}}])
    time.sleep(0.2)
    shot("third-focused", 0.3)
    third_w, third_h, third_px = load("third-focused")
    third_changed = (orange_before is not None and
                     pixel(third_px, third_w, orange_x, orange_y) !=
                     orange_before)
    out.append(("Alt-Tab reaches third", "yes" if third_changed else "no",
                "yes"))

    # From the newly raised orange window, one step selects the blue window
    # again so the minimize/restore checks below have an unambiguous target.
    chord("alt", "tab")

    # Alt-F9 is a compositor shortcut, not a byte sent to the focused
    # program. Alt-Tab includes minimized clients and restores the selected
    # one when Alt is released.
    chord("alt", "f9")
    shot("pixel-minimized", 0.3)
    min_w, min_h, pixel_minimized = load("pixel-minimized")
    blue_minimized = colour_bounds(pixel_minimized, min_w, min_h,
                                   b"\x00\x66\xcc")
    out.append(("Alt-F9 minimizes", "yes" if blue_minimized is None else "no",
                "yes"))

    chord("alt", "tab")
    shot("pixel-restored-by-key", 0.3)
    key_w, key_h, pixel_key = load("pixel-restored-by-key")
    blue_key = colour_bounds(pixel_key, key_w, key_h, b"\x00\x66\xcc")
    key_extent = ("absent" if blue_key is None else
                  "%dx%d" % (blue_key[2] - blue_key[0] + 1,
                             blue_key[3] - blue_key[1] + 1))
    out.append(("Alt-Tab restores", key_extent, "200x120"))

try:
    serial = open(work + "/serial", "rb").read().lower()
except OSError:
    serial = b""

for name, marker in (("no protection fault", b"general protection fault"),
                     ("no oops", b"oops:"),
                     ("no kernel panic", b"kernel panic")):
    out.append((name, "present" if marker in serial else "absent", "absent"))

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
