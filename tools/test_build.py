#!/usr/bin/env python3
"""Unit tests for the tools/build.py ``--upload-only`` path (SCRUM-330).

Only the Python standard library is used. No board, arduino-cli or real
compiler is needed; ``subprocess.run`` is mocked.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path, PureWindowsPath
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


class ParseSubstOutputTests(unittest.TestCase):
    def test_real_windows_sample(self) -> None:
        text = "T:\\: => D:\\Programming\\thermostat-esp32\r\n"

        self.assertEqual(
            {"T:": "D:\\Programming\\thermostat-esp32"},
            build.parse_subst_output(text),
        )

    def test_empty_output_is_empty_mapping(self) -> None:
        self.assertEqual({}, build.parse_subst_output(""))

    def test_multiple_drives(self) -> None:
        text = (
            "T:\\: => D:\\Programming\\thermostat-esp32\r\n"
            "X:\\: => E:\\other\r\n"
        )

        self.assertEqual(
            {
                "T:": "D:\\Programming\\thermostat-esp32",
                "X:": "E:\\other",
            },
            build.parse_subst_output(text),
        )

    def test_other_lines_are_ignored(self) -> None:
        text = "이상한 줄\r\nT:\\: => D:\\repo\r\n\r\n"

        self.assertEqual({"T:": "D:\\repo"}, build.parse_subst_output(text))

    def test_drive_key_is_uppercase(self) -> None:
        self.assertEqual(
            {"X:": "D:\\repo"}, build.parse_subst_output("x:\\: => D:\\repo\n")
        )


class FixedDriveTests(unittest.TestCase):
    def test_default_drive(self) -> None:
        self.assertEqual("T:", build.fixed_drive({}))

    def test_lowercase_drive_is_normalized(self) -> None:
        self.assertEqual("X:", build.fixed_drive({"THERMOSTAT_BUILD_DRIVE": "x:"}))

    def test_invalid_drive_raises(self) -> None:
        for value in ("X", "TT:", "1:", "", ":", "xx"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    build.fixed_drive({"THERMOSTAT_BUILD_DRIVE": value})


class FixedPathEnabledTests(unittest.TestCase):
    def test_zero_disables_fixed_path(self) -> None:
        self.assertFalse(build.fixed_path_enabled({"THERMOSTAT_FIXED_PATH": "0"}))

    def test_missing_or_one_enables_fixed_path(self) -> None:
        self.assertTrue(build.fixed_path_enabled({}))
        self.assertTrue(build.fixed_path_enabled({"THERMOSTAT_FIXED_PATH": "1"}))


class ToFixedTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def test_repo_path_maps_under_root(self) -> None:
        repo = self.tmp / "repo"

        result = build.to_fixed(
            repo / "src" / "ui" / "waveshare43b", repo, Path("T:\\")
        )

        self.assertEqual(
            PureWindowsPath("T:\\src\\ui\\waveshare43b"),
            PureWindowsPath(str(result)),
        )

    def test_path_outside_repo_raises(self) -> None:
        with self.assertRaises(ValueError):
            build.to_fixed(
                self.tmp / "elsewhere" / "src", self.tmp / "repo", Path("T:\\")
            )


class FixedRootTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def test_windows_root_is_default_drive(self) -> None:
        self.assertEqual("T:\\", str(build.fixed_root("win32", {})))

    def test_windows_root_respects_env_drive(self) -> None:
        root = build.fixed_root("win32", {"THERMOSTAT_BUILD_DRIVE": "x:"})

        self.assertEqual("X:\\", str(root))

    def test_posix_root_is_cache_link(self) -> None:
        with mock.patch.object(build.Path, "home", return_value=self.tmp):
            root = build.fixed_root("linux", {})

        self.assertEqual(
            self.tmp / ".cache" / "thermostat-esp32" / "src-root", root
        )


class FixedPathLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.lock_path = self.tmp / "config" / "fixed-path.lock"

    def test_lock_is_exclusive_and_timeout_notifies_once(self) -> None:
        messages: list[str] = []

        with build.fixed_path_lock(self.lock_path, timeout_sec=5, poll_sec=0.01):
            with self.assertRaises(TimeoutError):
                with build.fixed_path_lock(
                    self.lock_path,
                    timeout_sec=0.2,
                    poll_sec=0.05,
                    notify=messages.append,
                ):
                    self.fail("두 번째 잠금을 잡으면 안 됨")

        self.assertEqual(
            ["[build] 다른 워크트리 빌드가 끝나기를 기다립니다"], messages
        )

        with build.fixed_path_lock(self.lock_path, timeout_sec=1, poll_sec=0.01):
            pass

    def test_lock_is_released_on_exception(self) -> None:
        with self.assertRaises(RuntimeError):
            with build.fixed_path_lock(
                self.lock_path, timeout_sec=1, poll_sec=0.01
            ):
                raise RuntimeError("boom")

        with build.fixed_path_lock(self.lock_path, timeout_sec=1, poll_sec=0.01):
            pass


class _FakeRun:
    def __init__(self, stdout: str = "") -> None:
        self.calls: list[tuple[list[str], dict]] = []
        self.stdout = stdout

    def __call__(self, command, **kwargs):
        self.calls.append(([str(part) for part in command], kwargs))
        return types.SimpleNamespace(returncode=0, stdout=self.stdout, stderr="")


class MappedFixedRootWindowsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.repo = self.tmp / "repo"
        self.repo.mkdir()

    def test_connects_and_always_disconnects(self) -> None:
        run = _FakeRun("")

        with mock.patch.object(build.Path, "exists", return_value=False):
            with build.mapped_fixed_root(
                self.repo, platform="win32", env={}, run=run
            ) as root:
                self.assertEqual("T:\\", str(root))

        self.assertEqual(
            [
                ["subst"],
                ["subst", "T:", str(self.repo)],
                ["subst", "T:", "/D"],
            ],
            [command for command, _kwargs in run.calls],
        )

    def test_leftover_mapping_is_removed_before_connecting(self) -> None:
        run = _FakeRun("T:\\: => E:\\stale\r\n")

        with mock.patch.object(build.Path, "exists", return_value=False):
            with build.mapped_fixed_root(
                self.repo, platform="win32", env={}, run=run
            ):
                pass

        self.assertEqual(
            [
                ["subst"],
                ["subst", "T:", "/D"],
                ["subst", "T:", str(self.repo)],
                ["subst", "T:", "/D"],
            ],
            [command for command, _kwargs in run.calls],
        )

    def test_real_drive_letter_is_never_disconnected(self) -> None:
        run = _FakeRun("")

        with mock.patch.object(build.Path, "exists", return_value=True):
            with self.assertRaises(build.FixedPathUnavailable):
                with build.mapped_fixed_root(
                    self.repo, platform="win32", env={}, run=run
                ):
                    self.fail("실제 드라이브에서는 고정 경로를 쓰면 안 됨")

        self.assertEqual(
            [["subst"]], [command for command, _kwargs in run.calls]
        )

    def test_exception_inside_block_still_disconnects(self) -> None:
        run = _FakeRun("")

        with mock.patch.object(build.Path, "exists", return_value=False):
            with self.assertRaises(RuntimeError):
                with build.mapped_fixed_root(
                    self.repo, platform="win32", env={}, run=run
                ):
                    raise RuntimeError("boom")

        self.assertEqual(["subst", "T:", "/D"], run.calls[-1][0])

    def test_env_drive_letter_is_used(self) -> None:
        run = _FakeRun("")

        with mock.patch.object(build.Path, "exists", return_value=False):
            with build.mapped_fixed_root(
                self.repo,
                platform="win32",
                env={"THERMOSTAT_BUILD_DRIVE": "x:"},
                run=run,
            ) as root:
                self.assertEqual("X:\\", str(root))

        self.assertEqual(["subst", "X:", str(self.repo)], run.calls[1][0])
        self.assertEqual(["subst", "X:", "/D"], run.calls[2][0])


def _posix_mapping_calls() -> tuple[list[tuple], object, object]:
    calls: list[tuple] = []

    def fake_symlink(source, target, *, target_is_directory=False):
        calls.append(("symlink", str(source), str(target), target_is_directory))

    def fake_replace(source, target):
        calls.append(("replace", str(source), str(target)))

    return calls, fake_symlink, fake_replace


class MappedFixedRootPosixTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.home = self.tmp / "home"
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        self.link = self.home / ".cache" / "thermostat-esp32" / "src-root"
        self.temporary = self.link.with_name(
            f".{self.link.name}.{os.getpid()}.tmp"
        )

    def test_symlink_is_installed_atomically_and_kept(self) -> None:
        calls, fake_symlink, fake_replace = _posix_mapping_calls()

        with (
            mock.patch.object(build.Path, "home", return_value=self.home),
            mock.patch.object(build.os, "symlink", side_effect=fake_symlink),
            mock.patch.object(build.os, "replace", side_effect=fake_replace),
        ):
            with build.mapped_fixed_root(
                self.repo, platform="linux", env={}
            ) as root:
                self.assertEqual(self.link, root)
            with build.mapped_fixed_root(self.repo, platform="linux", env={}):
                pass

        expected_call = [
            ("symlink", str(self.repo), str(self.temporary), True),
            ("replace", str(self.temporary), str(self.link)),
        ]
        self.assertEqual(expected_call * 2, calls)

    def test_real_symlink_points_to_repo_and_survives_repeat_calls(self) -> None:
        probe = self.tmp / "probe-link"
        try:
            os.symlink(self.repo, probe, target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"심볼릭 링크를 만들 수 없는 환경: {exc}")
        probe.unlink()

        with mock.patch.object(build.Path, "home", return_value=self.home):
            with build.mapped_fixed_root(
                self.repo, platform="linux", env={}
            ) as root:
                self.assertTrue(root.is_symlink())
                self.assertEqual(self.repo.resolve(), root.resolve())
            with build.mapped_fixed_root(self.repo, platform="linux", env={}):
                self.assertEqual(self.repo.resolve(), self.link.resolve())


class MainCheckoutRootTests(unittest.TestCase):
    def test_returns_parent_of_common_git_dir(self) -> None:
        run = mock.Mock(
            return_value=types.SimpleNamespace(
                returncode=0, stdout="C:/x/repo/.git\n", stderr=""
            )
        )

        root = build.main_checkout_root(Path("C:/x/worktree"), run=run)

        self.assertEqual(Path("C:/x/repo"), root)
        self.assertEqual(
            [
                "git",
                "rev-parse",
                "--path-format=absolute",
                "--git-common-dir",
            ],
            run.call_args[0][0],
        )

    def test_failed_git_returns_none(self) -> None:
        run = mock.Mock(
            return_value=types.SimpleNamespace(
                returncode=128, stdout="", stderr="fatal"
            )
        )

        self.assertIsNone(
            build.main_checkout_root(Path("C:/x/worktree"), run=run)
        )

    def test_missing_git_returns_none(self) -> None:
        run = mock.Mock(side_effect=OSError("git not found"))

        self.assertIsNone(
            build.main_checkout_root(Path("C:/x/worktree"), run=run)
        )


class BuiltWithFixedRootTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.build_dir = self.tmp / "build"
        self.build_dir.mkdir()

    def _write_options(self, text: str) -> None:
        (self.build_dir / "build.options.json").write_text(
            text, encoding="utf-8"
        )

    def test_fixed_sketch_location_matches(self) -> None:
        self._write_options(
            json.dumps(
                {
                    "customBuildProperties": (
                        "compiler.cpp.extra_flags=-IT:/src,"
                        "compiler.c.extra_flags=-IT:/src"
                    ),
                    "sketchLocation": "T:\\firmware\\thermostat_main",
                }
            )
        )

        self.assertTrue(
            build.built_with_fixed_root(self.build_dir, Path("T:\\"))
        )

    def test_real_sketch_location_does_not_match(self) -> None:
        self._write_options(
            json.dumps(
                {
                    "sketchLocation": (
                        "D:\\Programming\\thermostat-esp32\\"
                        "firmware\\thermostat_main"
                    )
                }
            )
        )

        self.assertFalse(
            build.built_with_fixed_root(self.build_dir, Path("T:\\"))
        )

    def test_forward_slashes_and_case_are_accepted(self) -> None:
        self._write_options(
            json.dumps({"sketchLocation": "t:/firmware/thermostat_main"})
        )

        self.assertTrue(
            build.built_with_fixed_root(self.build_dir, Path("T:\\"))
        )

    def test_missing_options_file_does_not_match(self) -> None:
        self.assertFalse(
            build.built_with_fixed_root(self.build_dir, Path("T:\\"))
        )

    def test_broken_json_does_not_match(self) -> None:
        self._write_options("{")

        self.assertFalse(
            build.built_with_fixed_root(self.build_dir, Path("T:\\"))
        )


class SeedBuildDirTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)
        self.source = self.tmp / "main" / ".build" / "thermostat_main"
        (self.source / "sketch").mkdir(parents=True)
        (self.source / "sketch" / "main.o").write_bytes(b"sketch")
        for name in ("core", "waveshare43b", "lvgl"):
            lib = self.source / "libraries" / name
            lib.mkdir(parents=True)
            (lib / f"{name}.o").write_bytes(b"lib")
        (self.source / "core").mkdir()
        (self.source / "core" / "arduino.o").write_bytes(b"core")
        for name in (
            "thermostat_main.ino.bin",
            "thermostat_main.ino.elf",
            "thermostat_main.ino.map",
            "thermostat_main.ino.merged.bin",
            "other.ino.bin",
        ):
            (self.source / name).write_bytes(b"artifact")
        (self.source / "build.options.json").write_text(
            json.dumps({"sketchLocation": "T:\\firmware\\thermostat_main"}),
            encoding="utf-8",
        )
        self.dest = self.tmp / "worktree" / ".build" / "thermostat_main"

    def test_seed_removes_our_objects_and_artifacts(self) -> None:
        seeded = build.seed_build_dir(
            self.dest,
            self.source,
            "thermostat_main",
            ["core", "waveshare43b"],
        )

        self.assertTrue(seeded)
        self.assertFalse((self.dest / "sketch").exists())
        self.assertFalse((self.dest / "libraries" / "core").exists())
        self.assertFalse((self.dest / "libraries" / "waveshare43b").exists())
        self.assertTrue((self.dest / "libraries" / "lvgl" / "lvgl.o").is_file())
        self.assertTrue((self.dest / "core" / "arduino.o").is_file())
        self.assertTrue((self.dest / "build.options.json").is_file())
        self.assertFalse((self.dest / "thermostat_main.ino.bin").exists())
        self.assertFalse((self.dest / "thermostat_main.ino.elf").exists())
        self.assertFalse((self.dest / "thermostat_main.ino.map").exists())
        self.assertFalse(
            (self.dest / "thermostat_main.ino.merged.bin").exists()
        )
        self.assertTrue((self.dest / "other.ino.bin").is_file())

    def test_existing_build_dir_is_not_touched(self) -> None:
        self.dest.mkdir(parents=True)
        (self.dest / "keep.txt").write_text("keep", encoding="utf-8")

        seeded = build.seed_build_dir(
            self.dest, self.source, "thermostat_main", ["core"]
        )

        self.assertFalse(seeded)
        self.assertEqual(
            ["keep.txt"], sorted(item.name for item in self.dest.iterdir())
        )


@contextlib.contextmanager
def _fake_mapping(repo_root, **kwargs):
    yield Path("T:\\")


class MainFixedPathTests(unittest.TestCase):
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
        (self.sketch / "modules.txt").write_text(
            "src/core\nsrc/ui/waveshare43b\n", encoding="utf-8"
        )
        (self.repo / "src" / "core").mkdir(parents=True)
        (self.repo / "src" / "ui" / "waveshare43b").mkdir(parents=True)
        self.build_dir = self.repo / ".build" / "thermostat_main"

    def _run_main(
        self,
        argv: list[str],
        run_mock: mock.Mock,
        env: dict[str, str],
        *,
        mapping: mock.Mock | None = None,
        lock: mock.Mock | None = None,
        main_root_value: Path | None = None,
    ) -> tuple[int, str, str]:
        config = self.tmp / "arduino-cli.yaml"
        stdout = io.StringIO()
        stderr = io.StringIO()
        if mapping is None:
            mapping = mock.Mock(side_effect=_fake_mapping)
        if lock is None:
            lock = mock.Mock(return_value=contextlib.nullcontext())
        main_root = mock.Mock(return_value=main_root_value)
        with (
            mock.patch.object(build, "REPO_ROOT", self.repo),
            mock.patch.object(
                build, "find_cli", return_value=Path("arduino-cli")
            ),
            mock.patch.object(
                build, "default_config_path", return_value=config
            ),
            mock.patch.object(build.subprocess, "run", run_mock),
            mock.patch.object(build, "mapped_fixed_root", mapping),
            mock.patch.object(build, "fixed_path_lock", lock),
            mock.patch.object(build, "main_checkout_root", main_root),
            mock.patch.dict(os.environ, env),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = build.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def _recording_run(self) -> tuple[mock.Mock, list[list[str]]]:
        calls: list[list[str]] = []

        def fake_run(command, **kwargs):
            calls.append([str(part) for part in command])
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        return mock.Mock(side_effect=fake_run), calls

    def test_compile_uses_fixed_root_and_upload_real_paths(self) -> None:
        run_mock, calls = self._recording_run()
        mapping = mock.Mock(side_effect=_fake_mapping)
        lock = mock.Mock(return_value=contextlib.nullcontext())

        code, _stdout, _stderr = self._run_main(
            ["firmware/thermostat_main", "--upload"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "1", "THERMOSTAT_BUILD_DRIVE": "T:"},
            mapping=mapping,
            lock=lock,
        )

        self.assertEqual(0, code)
        self.assertEqual(2, len(calls))
        lock.assert_called_once()

        compile_command = calls[0]
        self.assertIn("compile", compile_command)
        self.assertEqual(
            str(Path("T:\\") / ".build" / "thermostat_main"),
            compile_command[compile_command.index("--build-path") + 1],
        )
        library_values = [
            compile_command[index + 1]
            for index, part in enumerate(compile_command)
            if part == "--library"
        ]
        self.assertEqual(
            [
                str(Path("T:\\") / "src" / "core"),
                str(Path("T:\\") / "src" / "ui" / "waveshare43b"),
            ],
            library_values,
        )
        self.assertIn("compiler.cpp.extra_flags=-IT:/src", compile_command)
        self.assertIn("compiler.c.extra_flags=-IT:/src", compile_command)
        self.assertEqual(
            str(Path("T:\\") / "firmware" / "thermostat_main"),
            compile_command[-1],
        )

        upload_command = calls[1]
        self.assertIn("upload", upload_command)
        self.assertEqual(
            str(self.build_dir),
            upload_command[upload_command.index("--input-dir") + 1],
        )
        self.assertEqual(str(self.sketch), upload_command[-1])

    def test_fresh_worktree_seeds_from_main_checkout(self) -> None:
        main_root = self.tmp / "main"
        source = main_root / ".build" / "thermostat_main"
        (source / "sketch").mkdir(parents=True)
        (source / "sketch" / "main.o").write_bytes(b"sketch")
        (source / "libraries" / "core").mkdir(parents=True)
        (source / "libraries" / "core" / "core.o").write_bytes(b"lib")
        (source / "libraries" / "lvgl").mkdir(parents=True)
        (source / "libraries" / "lvgl" / "lvgl.o").write_bytes(b"lib")
        (source / "core").mkdir()
        (source / "core" / "arduino.o").write_bytes(b"core")
        (source / "thermostat_main.ino.bin").write_bytes(b"artifact")
        (source / "build.options.json").write_text(
            json.dumps({"sketchLocation": "T:\\firmware\\thermostat_main"}),
            encoding="utf-8",
        )
        run_mock, _calls = self._recording_run()

        code, stdout, _stderr = self._run_main(
            ["firmware/thermostat_main"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "1", "THERMOSTAT_BUILD_DRIVE": "T:"},
            main_root_value=main_root,
        )

        self.assertEqual(0, code)
        self.assertIn(
            f"[build] 메인 체크아웃 빌드 폴더에서 시작: {source}", stdout
        )
        self.assertFalse((self.build_dir / "sketch").exists())
        self.assertFalse((self.build_dir / "libraries" / "core").exists())
        self.assertTrue(
            (self.build_dir / "libraries" / "lvgl" / "lvgl.o").is_file()
        )
        self.assertTrue(
            (self.build_dir / "core" / "arduino.o").is_file()
        )
        self.assertFalse(
            (self.build_dir / "thermostat_main.ino.bin").exists()
        )

    def test_fixed_path_disabled_uses_real_paths(self) -> None:
        run_mock, calls = self._recording_run()
        mapping = mock.Mock(
            side_effect=AssertionError("고정 경로를 쓰면 안 됨")
        )
        lock = mock.Mock(side_effect=AssertionError("잠금을 쓰면 안 됨"))

        code, _stdout, _stderr = self._run_main(
            ["firmware/thermostat_main"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "0"},
            mapping=mapping,
            lock=lock,
        )

        self.assertEqual(0, code)
        self.assertEqual(1, len(calls))
        compile_command = calls[0]
        self.assertEqual(
            str(self.build_dir),
            compile_command[compile_command.index("--build-path") + 1],
        )
        self.assertEqual(str(self.sketch), compile_command[-1])
        self.assertIn(
            "compiler.cpp.extra_flags=-I"
            f"{(self.repo / 'src').as_posix()}",
            compile_command,
        )
        mapping.assert_not_called()
        lock.assert_not_called()

    def test_unavailable_drive_falls_back_to_real_paths(self) -> None:
        run_mock, calls = self._recording_run()

        @contextlib.contextmanager
        def unavailable(repo_root, **kwargs):
            raise build.FixedPathUnavailable("T:")
            yield  # pragma: no cover

        mapping = mock.Mock(side_effect=unavailable)

        code, _stdout, stderr = self._run_main(
            ["firmware/thermostat_main"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "1", "THERMOSTAT_BUILD_DRIVE": "T:"},
            mapping=mapping,
        )

        self.assertEqual(0, code)
        self.assertEqual(1, len(calls))
        self.assertEqual(
            str(self.build_dir),
            calls[0][calls[0].index("--build-path") + 1],
        )
        self.assertEqual(str(self.sketch), calls[0][-1])
        self.assertIn("다른 용도로 쓰고 있어", stderr)
        self.assertIn("고정 경로 없이 빌드합니다", stderr)

    def test_lock_timeout_returns_one_without_compiling(self) -> None:
        run_mock, calls = self._recording_run()
        lock = mock.Mock(
            side_effect=TimeoutError(
                "고정 경로 잠금을 기다리다 시간이 초과했습니다"
            )
        )
        mapping = mock.Mock(side_effect=_fake_mapping)

        code, _stdout, stderr = self._run_main(
            ["firmware/thermostat_main"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "1"},
            mapping=mapping,
            lock=lock,
        )

        self.assertEqual(1, code)
        self.assertEqual([], calls)
        self.assertIn("시간이 초과", stderr)
        mapping.assert_not_called()

    def test_dry_run_has_no_side_effects(self) -> None:
        run_mock = mock.Mock(
            side_effect=AssertionError("--dry-run은 명령을 실행하면 안 됨")
        )
        mapping = mock.Mock(
            side_effect=AssertionError("--dry-run은 연결하면 안 됨")
        )
        lock = mock.Mock(side_effect=AssertionError("--dry-run은 잠그면 안 됨"))

        code, stdout, _stderr = self._run_main(
            ["firmware/thermostat_main", "--upload", "--dry-run"],
            run_mock,
            {"THERMOSTAT_FIXED_PATH": "1", "THERMOSTAT_BUILD_DRIVE": "T:"},
            mapping=mapping,
            lock=lock,
        )

        self.assertEqual(0, code)
        lines = [line for line in stdout.splitlines() if line.strip()]
        self.assertEqual(2, len(lines))
        self.assertIn(
            str(Path("T:\\") / ".build" / "thermostat_main"), lines[0]
        )
        self.assertIn(
            str(Path("T:\\") / "firmware" / "thermostat_main"), lines[0]
        )
        self.assertIn(str(self.build_dir), lines[1])
        self.assertIn(str(self.sketch), lines[1])
        run_mock.assert_not_called()
        mapping.assert_not_called()
        lock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
