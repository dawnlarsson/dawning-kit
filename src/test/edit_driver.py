#!/usr/bin/env python3
"""Exercise the editor's real pty, resize, and atomic-save driver."""

import fcntl
import os
import pty
import select
import shlex
import struct
import subprocess
import sys
import tempfile
import termios
import time


def fail(message):
    print(f"  driver       {message}")
    raise SystemExit(1)


def read_until(master, process, marker, timeout=3.0):
    seen = bytearray()
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        ready, _, _ = select.select([master], [], [], 0.05)
        if ready:
            try:
                seen.extend(os.read(master, 65536))
            except OSError:
                break
            if marker in seen:
                return bytes(seen)
        if process.poll() is not None:
            break
    return bytes(seen)


def start(shell, path, initial_flags=False, umask=0o022):
    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 60, 0, 0))
    original = termios.tcgetattr(slave)
    if initial_flags:
        unusual = termios.tcgetattr(slave)
        unusual[0] |= termios.ISTRIP | termios.INLCR
        unusual[2] &= ~termios.CSIZE
        unusual[2] |= termios.CS7
        termios.tcsetattr(slave, termios.TCSANOW, unusual)
        # Linux ptys may normalize unsupported character sizes immediately.
        original = termios.tcgetattr(slave)

    def child_setup():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        os.umask(umask)

    command = "edit " + shlex.quote(path)
    process = subprocess.Popen(
        [shell, "-c", command], stdin=slave, stdout=slave, stderr=slave,
        close_fds=True, preexec_fn=child_setup,
    )
    shown = read_until(master, process, b"\x1b[?2004h")
    if b"\x1b[?2004h" not in shown:
        process.kill()
        process.wait()
        os.close(master)
        os.close(slave)
        fail("editor did not enter its terminal screen")
    return master, slave, process, original


def finish(master, slave, process, keys, original=None):
    os.write(master, keys)
    read_until(master, process, b"\x1b[?1049l")
    try:
        result = process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        fail("editor did not leave after Ctrl+Q")
    if original is not None:
        restored = termios.tcgetattr(slave)
        if restored != original:
            changed = [str(at) for at, pair in enumerate(zip(original, restored))
                       if pair[0] != pair[1]]
            fail("terminal modes were not restored exactly (fields " +
                 ",".join(changed) + f", cflag {original[2]:x}->{restored[2]:x})")
    os.close(master)
    os.close(slave)
    if result:
        fail(f"editor exited with status {result}")


def replace(shell, path, text, original=None):
    master, slave, process, before = start(shell, path,
                                            initial_flags=original is not None)
    if original is not None:
        raw = termios.tcgetattr(slave)
        if raw[0] & (termios.ISTRIP | termios.INLCR):
            fail("raw mode still transforms input bytes")
        if (raw[2] & termios.CSIZE) != termios.CS8:
            fail("raw mode is not eight-bit clean")
    finish(master, slave, process, b"\x01" + text + b"\x13\x11", before)


def main():
    if len(sys.argv) != 2:
        fail("usage: edit_driver.py SHELL")
    shell = os.path.abspath(sys.argv[1])
    passed = 0

    with tempfile.TemporaryDirectory(prefix="moonwater-edit-") as directory:
        target = os.path.join(directory, "target")
        link = os.path.join(directory, "link")
        with open(target, "wb") as handle:
            handle.write(b"old\n")
        os.chmod(target, 0o755)
        os.symlink("target", link)
        replace(shell, link, b"new")
        if not os.path.islink(link) or open(target, "rb").read() != b"new\n":
            fail("saving through a symlink replaced the link or missed its target")
        if os.stat(target).st_mode & 0o7777 != 0o755:
            fail("saving changed the existing file mode")
        passed += 3

        created = os.path.join(directory, "created")
        replace(shell, created, b"fresh")
        if open(created, "rb").read() != b"fresh\n":
            fail("new file bytes differ")
        if os.stat(created).st_mode & 0o777 != 0o644:
            fail("new file ignored umask 022")
        passed += 2

        unusual = os.path.join(directory, "unusual")
        with open(unusual, "wb") as handle:
            handle.write(b"x\n")
        replace(shell, unusual, b"eight", original=True)
        passed += 2

        master, slave, process, before = start(shell, target)
        # No input follows this resize. SIGWINCH alone must wake ppoll and
        # cause a redraw, including the new 50-column status line.
        fcntl.ioctl(master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 10, 50, 0, 0))
        redrawn = read_until(master, process, b"\x1b[10;1H", 2.0)
        if b"\x1b[10;1H" not in redrawn:
            process.kill()
            process.wait()
            fail("SIGWINCH did not redraw without a keypress")
        finish(master, slave, process, b"\x11", before)
        passed += 1

        leftovers = [name for name in os.listdir(directory)
                     if name.startswith(".moonwater-edit-")]
        if leftovers:
            fail("atomic save left temporary files behind")
        passed += 1

    print(f"  driver       {passed} of {passed}")


if __name__ == "__main__":
    main()
