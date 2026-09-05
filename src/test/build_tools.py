#!/usr/bin/env python3
"""Exercise build argument boundaries and watch-process ownership without GCC."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


class BuildTools(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="moonwater-build-")
        self.work = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        BUILD_FIXTURE=str(self.work), TERM="dumb")
        self.executable("cc", '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
if sys.argv[1:] == ["--version"]:
    print("gcc (GCC) fixture")
    raise SystemExit
with (root / "compiled").open("a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
out = pathlib.Path(sys.argv[sys.argv.index("-o") + 1])
out.write_text("#!/usr/bin/env python3\\n"
    "import os, pathlib, signal, time\\n"
    "root = pathlib.Path(os.environ['BUILD_FIXTURE'])\\n"
    "if 'BUILD_FIXTURE_EXIT' in os.environ: raise SystemExit(int(os.environ['BUILD_FIXTURE_EXIT']))\\n"
    "with (root / 'started').open('a') as f: f.write(str(os.getpid()) + '\\\\n')\\n"
    "signal.signal(signal.SIGTERM, lambda *_: exit(0))\\n"
    "while True: time.sleep(0.05)\\n")
''')
        self.env["CC"] = str(self.bin / "cc")

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def invoke(self, *args):
        return subprocess.run(["sh", str(ROOT / "kit/build"), *map(str, args)],
                              cwd=self.work, env=self.env, capture_output=True,
                              text=True, timeout=10)

    def test_paths_preserve_spaces_globs_and_option_terminator(self):
        source = self.work / "source [literal] space.c"
        source.touch()
        output = self.work / "output [literal] space"
        result = self.invoke("--", source, output)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads((self.work / "compiled").read_text())
        self.assertEqual(args[0], str(source))
        self.assertEqual(args[args.index("-o") + 1], str(output))
        self.assertTrue(output.exists())

    def test_extra_operand_is_rejected_before_compiling(self):
        source = self.work / "source.c"
        source.touch()
        result = self.invoke(source, self.work / "out", "unexpected")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "compiled").exists())

    def test_run_uses_local_output_and_preserves_status(self):
        source = self.work / "source.c"
        source.touch()
        self.env["BUILD_FIXTURE_EXIT"] = "7"
        result = self.invoke("--run", source, "out")
        self.assertEqual(result.returncode, 7, result.stderr)

    def test_watch_reaps_latest_app_and_watcher_on_termination(self):
        source = self.work / "source.c"
        source.touch()
        self.executable("clear", "#!/bin/sh\nexit 0\n")
        self.executable("inotifywait", '''#!/usr/bin/env python3
import os, pathlib, time
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
(root / "watcher").write_text(str(os.getpid()))
time.sleep(0.1)
print("changed", flush=True)
while True: time.sleep(0.05)
''')
        process = subprocess.Popen(["sh", str(ROOT / "kit/build"), "--watch",
                                    str(source), str(self.work / "out")],
                                   cwd=self.work, env=self.env, start_new_session=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pids = []
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                started = self.work / "started"
                pids = list(map(int, started.read_text().split())) if started.exists() else []
                if len(pids) >= 2:
                    break
                time.sleep(0.02)
            self.assertGreaterEqual(len(pids), 2, "watch did not launch a replacement")
            process.terminate()
            process.wait(timeout=5)
            watcher = int((self.work / "watcher").read_text())
            for pid in [*pids, watcher]:
                with self.assertRaises(ProcessLookupError, msg=f"process {pid} survived build"):
                    os.kill(pid, 0)
        finally:
            # This disposable process group is ours, including the unfixed
            # pipeline watcher that would otherwise survive the test.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)

    def test_onbox_rejects_remote_path_code_before_contact(self):
        for name in ("rsync", "ssh"):
            self.executable(name, '#!/bin/sh\nprintf contacted >>"$BUILD_FIXTURE/contacted"\nexit 0\n')
        result = subprocess.run(["sh", str(ROOT / "kit/onbox"), "run; exit 0 #", "kit"],
                                cwd=ROOT, env=self.env, capture_output=True, text=True,
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "contacted").exists())

    def test_onbox_quotes_remote_arguments_and_omits_agent_worktrees(self):
        for name in ("rsync", "ssh"):
            self.executable(name, '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
(root / pathlib.Path(sys.argv[0]).name).write_text(json.dumps(sys.argv[1:]))
''')
        hostile = "kit; printf injected 'bad'"
        result = subprocess.run(["sh", str(ROOT / "kit/onbox"), "audit-fixture", hostile],
                                cwd=ROOT, env=self.env, capture_output=True, text=True,
                                timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        copy = json.loads((self.work / "rsync").read_text())
        self.assertIn(".claude", copy)
        remote = json.loads((self.work / "ssh").read_text())[-1]
        # Execute only the final command with a stub sh: the literal hostile
        # argument must arrive intact and must never become shell syntax.
        command = remote.split("&&")[-1].strip()
        self.executable("sh", '''#!/usr/bin/env python3
import json, os, pathlib, sys
(pathlib.Path(os.environ["BUILD_FIXTURE"]) / "remote-args").write_text(json.dumps(sys.argv[1:]))
''')
        ran = subprocess.run(["/bin/sh", "-c", command], env=self.env,
                             capture_output=True, text=True, timeout=5)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "")
        self.assertEqual(json.loads((self.work / "remote-args").read_text()),
                         ["src/test/run", hostile])


if __name__ == "__main__":
    unittest.main()
