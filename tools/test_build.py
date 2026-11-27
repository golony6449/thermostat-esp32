#!/usr/bin/env python3
"""Unit tests for the tools/build.py ``--upload-only`` path (SCRUM-330).

Only the Python standard library is used. No board, arduino-cli or real
compiler is needed; ``subprocess.run`` is mocked.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import build  # noqa: E402


class BuildArtifactPathTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def test_artifact_path_uses_sketch_folder_name(self) -> None:
        artifact = build.build_artifact_path(
            self.tmp / "firmware" / "thermostat_main", self.tmp / ".build" / "thermostat_main"
        )

        self.assertEqual(
            self.tmp / ".build" / "thermostat_main" / "thermostat_main.ino.bin",
            artifact,
        )


class BuildCommandsUploadOnlyTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def _build_commands(self, *, upload: bool, compile_: bool) -> list[list[str]]:
        return build.build_commands(
            Path("arduino-cli"),
            self.tmp / "sketch",
            self.tmp / "build",
            "esp32:esp32:esp32s3",
            None,
            [self.tmp / "src" / "core"],
            self.tmp / "src",
            upload,
            "COM9",
            compile=compile_,
        )

    def test_upload_only_skips_the_compile_command(self) -> None:
        commands = self._build_commands(upload=True, compile_=False)

        self.assertEqual(1, len(commands))
        upload = commands[0]
        self.assertEqual("upload", upload[1])
        self.assertNotIn("compile", upload)
        self.assertEqual("COM9", upload[upload.index("-p") + 1])
        self.assertEqual(
            "esp32:esp32:esp32s3", upload[upload.index("--fqbn") + 1]
        )
        self.assertEqual(
            str(self.tmp / "build"), upload[upload.index("--input-dir") + 1]
        )
        self.assertEqual(str(self.tmp / "sketch"), upload[-1])

    def test_compile_remains_the_default(self) -> None:
        commands = self._build_commands(upload=False, compile_=True)

        self.assertEqual(1, len(commands))
        self.assertEqual("compile", commands[0][1])
        self.assertNotIn("upload", commands[0])

    def test_compile_with_upload_still_builds_both_commands(self) -> None:
        commands = self._build_commands(upload=True, compile_=True)

        self.assertEqual(2, len(commands))
        self.assertEqual("compile", commands[0][1])
        self.assertEqual("upload", commands[1][1])


class MainUploadOnlyTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.repo = self.tmp / "repo"
        self.sketch = self.repo / "firmware" / "thermostat_main"
        self.sketch.mkdir(parents=True)
        (self.sketch / "thermostat_main.ino").write_text(
            "void setup() {}\nvoid loop() {}\n", encoding="utf-8"
        )
        (self.sketch / "modules.txt").write_text("src/core\n", encoding="utf-8")
        (self.repo / "src" / "core").mkdir(parents=True)
        self.build_dir = self.repo / ".build" / "thermostat_main"

    def _write_artifact(self) -> Path:
        self.build_dir.mkdir(parents=True)
        artifact = self.build_dir / "thermostat_main.ino.bin"
        artifact.write_bytes(b"fake firmware")
        return artifact

    def _run_main(
        self, argv: list[str], run_mock: mock.Mock
    ) -> tuple[int, str, str]:
        config = self.tmp / "arduino-cli.yaml"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(build, "REPO_ROOT", self.repo),
            mock.patch.object(
                build, "find_cli", return_value=Path("arduino-cli")
            ),
            mock.patch.object(
                build, "default_config_path", return_value=config
            ),
            mock.patch.object(build.subprocess, "run", run_mock),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = build.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_upload_only_without_binary_fails_and_uploads_nothing(self) -> None:
        run_mock = mock.Mock(
            side_effect=AssertionError("--upload-only must not run commands")
        )

        code, _stdout, stderr = self._run_main(
            ["firmware/thermostat_main", "--upload-only"], run_mock
        )

        self.assertEqual(2, code)
        self.assertIn("빌드 결과가 없습니다", stderr)
        run_mock.assert_not_called()

    def test_upload_only_uploads_existing_binary_without_compiling(self) -> None:
        self._write_artifact()
        calls: list[list[str]] = []

        def fake_run(command, **kwargs):
            calls.append([str(part) for part in command])
            return mock.Mock(returncode=0, stdout="", stderr="")

        code, stdout, _stderr = self._run_main(
            ["firmware/thermostat_main", "--upload-only"], fake_run
        )

        self.assertEqual(0, code)
        self.assertEqual(1, len(calls))
        self.assertIn("upload", calls[0])
        self.assertNotIn("compile", calls[0])
        self.assertIn(str(self.build_dir), calls[0])
        self.assertIn("[build] upload", stdout)
        self.assertIn("초", stdout)

    def test_upload_only_wins_over_upload(self) -> None:
        self._write_artifact()
        calls: list[list[str]] = []

        def fake_run(command, **kwargs):
            calls.append([str(part) for part in command])
            return mock.Mock(returncode=0, stdout="", stderr="")

        code, _stdout, _stderr = self._run_main(
            ["firmware/thermostat_main", "--upload", "--upload-only"], fake_run
        )

        self.assertEqual(0, code)
        self.assertEqual(1, len(calls))
        self.assertIn("upload", calls[0])
        self.assertNotIn("compile", calls[0])


if __name__ == "__main__":
    unittest.main()
