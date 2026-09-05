#!/usr/bin/env python3
"""Check the differential oracle itself, including deliberately wrong subjects."""

import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

import shell_generated
from shell_generated import Runner, differences, shrink


class OracleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="moonwater-oracle-")
        self.root = Path(self.temporary.name)
        self.runner = Runner("/bin/bash", self.root, 2)
        self.true = shutil.which("true")
        self.assertIsNotNone(self.true)

    def tearDown(self):
        self.temporary.cleanup()

    def run_main(self, generated, *arguments, subject=None):
        module = types.ModuleType("shell_cases_oracle_fixture")
        module.cases = generated
        argv = ["shell_generated.py", subject or self.true,
                "--seed", "0x1234", "--cases", "2", *arguments]
        output = io.StringIO()
        with mock.patch.object(shell_generated, "MODULES", (module.__name__,)), \
             mock.patch.dict(sys.modules, {module.__name__: module}), \
             mock.patch.object(sys, "argv", argv), \
             contextlib.redirect_stdout(output):
            status = shell_generated.main()
        return status, output.getvalue()

    def test_same_shell_same_directory_and_environment(self):
        want, got = self.runner.pair("bash", 'printf "%s:%s\\n" "$PWD" "$HOME"; printf x > made')
        self.assertFalse(differences(want, got))
        self.assertIn("made", got["effects"])

    def test_clean_directory_between_executions(self):
        self.runner.run("bash", "printf first > made", False)
        result = self.runner.run("bash", 'test ! -e made; printf "%s\\n" "$?"', False)
        self.assertEqual(result["stdout"], b"0\n")
        self.assertNotIn("made", result["effects"])

    def test_wrong_subject_cannot_pass_status(self):
        wrong = Runner(self.true, self.root, 2)
        want, got = wrong.pair("bash", "false")
        self.assertEqual(differences(want, got), ("status",))

    def test_wrong_subject_cannot_pass_stdout(self):
        wrong = Runner(self.true, self.root, 2)
        want, got = wrong.pair("bash", "printf correct")
        self.assertEqual(differences(want, got), ("stdout",))

    def test_wrong_subject_cannot_pass_filesystem_effects(self):
        wrong = Runner(self.true, self.root, 2)
        want, got = wrong.pair("bash", "printf correct > made")
        self.assertEqual(differences(want, got), ("effects",))

    def test_binary_output_is_not_text_normalized(self):
        result = self.runner.run("bash", "printf '\\000\\377\\n'", False)
        self.assertEqual(result["stdout"], b"\x00\xff\n")

    def test_directory_mode_is_an_effect(self):
        result = self.runner.run("bash", "chmod 700 dir", False)
        self.assertEqual(result["effects"]["dir"], ["directory", 0o700])

    def test_file_and_stdin_paths_are_executed_and_compared(self):
        for input_kind in ("command", "file", "stdin"):
            with self.subTest(input=input_kind):
                runner = Runner("/bin/bash", self.root, 2, input_kind=input_kind)
                script = "printf 'first\\n'\nprintf '\\000last\\n'\n"
                want, got = runner.pair("bash", script)
                self.assertFalse(differences(want, got))
                self.assertEqual(got["stdout"], b"first\n\x00last\n")
                wrong = Runner(self.true, self.root, 2, input_kind=input_kind)
                want, got = wrong.pair("bash", script)
                self.assertEqual(differences(want, got), ("stdout",))

    def test_file_and_stdin_error_status_is_not_command_status(self):
        for input_kind in ("file", "stdin"):
            runner = Runner("/bin/bash", self.root, 2, input_kind=input_kind)
            want, got = runner.pair("bash", 'unset missing; : "${missing:?required}"\n')
            self.assertFalse(differences(want, got))
            self.assertEqual(got["status"], 1)

    def test_timeout_is_explicit(self):
        runner = Runner("/bin/bash", self.root, 0.1)
        result = runner.run("bash", "while :; do :; done", False)
        self.assertTrue(result["timeout"])
        self.assertLess(result["status"], 0)

    def test_descendant_is_stopped_after_parent_exits(self):
        # A descendant retaining output descriptors must not hang collection
        # or write into the next case's recreated directory.
        result = self.runner.run("bash", "/bin/sleep 5 & printf done", False)
        self.assertFalse(result["timeout"])
        self.assertEqual(result["stdout"], b"done")

    def test_shrink_preserves_failure_category_and_statuses(self):
        wrong = Runner(self.true, self.root, 2)
        script = ":\nprintf correct\n:\n"
        want, got = wrong.pair("bash", script)
        minimal, left, right = shrink(wrong, "bash", script, want, got)
        self.assertEqual(minimal, "printf correct\n")
        self.assertEqual(differences(left, right), ("stdout",))
        self.assertEqual((left["status"], right["status"]), (0, 0))

    def test_grouped_failures_name_and_count_every_case(self):
        def generated(rng, budget):
            del rng, budget
            yield "same-class", ("bash",), "printf one"
            yield "same-class", ("bash",), "printf two"

        status, output = self.run_main(generated)
        self.assertEqual(status, 1)
        self.assertIn("divergences=2", output)
        self.assertIn(": 2 cases", output)
        self.assertEqual(output.count("case="), 2)
        self.assertIn("ALSO FAIL", output)

    def test_reference_timeout_is_invalid_and_never_passes(self):
        def generated(rng, budget):
            del rng, budget
            yield "invalid-reference", ("bash",), "while :; do :; done"

        status, output = self.run_main(generated, "--timeout", "0.05")
        self.assertEqual(status, 1)
        self.assertIn("INVALID ORACLE", output)
        self.assertIn("invalid_oracles=1", output)

    def test_candidate_timeout_is_a_divergence(self):
        subject = self.root / "loop"
        subject.write_text("#!/bin/sh\nwhile :; do :; done\n")
        subject.chmod(0o755)

        def generated(rng, budget):
            del rng, budget
            yield "candidate-timeout", ("bash",), "printf done"

        status, output = self.run_main(
            generated, "--timeout", "0.05", subject=str(subject))
        self.assertEqual(status, 1)
        self.assertIn("differences=('status', 'stdout', 'timeout')", output)
        self.assertIn("invalid_oracles=0", output)

    def test_case_and_saved_artifact_replay_without_generator(self):
        artifacts = self.root / "artifacts"

        def generated(rng, budget):
            del budget
            yield "replay", ("bash",), f"printf replayed-{rng.randrange(1 << 32):x}"

        status, output = self.run_main(
            generated, "--artifacts", str(artifacts))
        self.assertEqual(status, 1)
        case = re.search(r"case=([0-9a-f]{16})", output).group(1)

        status, repeated = self.run_main(generated, "--case", case)
        self.assertEqual(status, 1)
        self.assertIn("case=" + case, repeated)
        self.assertIn("generated 0 of 1", repeated)

        artifact = next(artifacts.glob("*.json"))
        saved = json.loads(artifact.read_text())
        self.assertTrue(saved["script"].startswith("printf replayed-"))
        replayed = io.StringIO()
        with mock.patch.object(shell_generated, "MODULES", ("missing_after_save",)), \
             mock.patch.object(sys, "argv", ["shell_generated.py", self.true,
                                               "--replay", str(artifact)]), \
             contextlib.redirect_stdout(replayed):
            replay_status = shell_generated.main()
        self.assertEqual(replay_status, 1)
        self.assertIn("script=" + repr(saved["script"]), replayed.getvalue())

    def test_saved_artifact_retains_stdin_transport(self):
        artifacts = self.root / "stdin-artifacts"

        def generated(rng, budget):
            del rng, budget
            yield "stdin-replay", ("bash",), "printf from-stdin\n"

        status, output = self.run_main(generated, "--input", "stdin",
                                       "--artifacts", str(artifacts))
        self.assertEqual(status, 1)
        case = re.search(r"case=([0-9a-f]{16})", output).group(1)
        artifact = next(artifacts.glob("*.json"))
        self.assertEqual(json.loads(artifact.read_text())["input"], "stdin")
        replayed = io.StringIO()
        with mock.patch.object(sys, "argv", ["shell_generated.py", self.true,
                                               "--replay", str(artifact)]), \
             contextlib.redirect_stdout(replayed):
            self.assertEqual(shell_generated.main(), 1)
        self.assertIn("input=stdin", replayed.getvalue())
        self.assertIn("case=" + case, replayed.getvalue())

    def test_emulator_argv0_and_case_replay(self):
        log = self.root / "emulator.json"
        emulator = self.root / "qemu-fake"
        emulator.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, sys\n"
            "open(" + repr(str(log)) + ", 'w').write(json.dumps(sys.argv[1:]))\n"
            "os.execv(sys.argv[3], [sys.argv[2], *sys.argv[4:]])\n")
        emulator.chmod(0o755)

        def generated(rng, budget):
            del rng, budget
            yield "emulated", ("bash",), "printf emulated"

        status, output = self.run_main(
            generated, "--emulator", str(emulator))
        self.assertEqual(status, 1)
        arguments = json.loads(log.read_text())
        self.assertEqual(arguments[:3], ["-0", "bash", self.true])
        case = re.search(r"case=([0-9a-f]{16})", output).group(1)

        status, repeated = self.run_main(
            generated, "--emulator", str(emulator), "--case", case)
        self.assertEqual(status, 1)
        self.assertIn("case=" + case, repeated)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OracleTest)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if os.environ.get("TEST_TALLY"):
        with open(os.environ["TEST_TALLY"], "a") as tally:
            passed = result.testsRun - len(result.failures) - len(result.errors)
            tally.write(f"shell_oracle {passed} {result.testsRun}\n")
    raise SystemExit(0 if result.wasSuccessful() else 1)
