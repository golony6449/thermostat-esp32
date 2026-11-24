"""Unit tests for tools/build.py and tools/test_host.py.

These tests must run without a board, arduino-cli or a real compiler. They use
temporary directories and unittest.mock only.
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
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import build  # noqa: E402
import test_host  # noqa: E402


class BuildToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def test_make_fqbn_default_partition(self) -> None:
        fqbn = build.make_fqbn()
        self.assertTrue(fqbn.endswith(",PartitionScheme=app3M_fat9M_16MB"))
        self.assertIn("CDCOnBoot=cdc", fqbn)
        self.assertIn("PSRAM=opi", fqbn)

    def test_make_fqbn_custom_partition(self) -> None:
        fqbn = build.make_fqbn("huge_app")
        self.assertTrue(fqbn.endswith(",PartitionScheme=huge_app"))

    def test_make_fqbn_without_partition(self) -> None:
        fqbn = build.make_fqbn(None)
        self.assertNotIn("PartitionScheme", fqbn)
        self.assertEqual(build.BASE_FQBN, fqbn)

    def test_read_demo_modules_parses_and_ignores_comments(self) -> None:
        repo = self.tmp / "repo"
        (repo / "src" / "core").mkdir(parents=True)
        (repo / "src" / "net").mkdir(parents=True)
        sketch = repo / "demos" / "demo"
        sketch.mkdir(parents=True)
        (sketch / "modules.txt").write_text(
            "# required modules\n\nsrc/core\nsrc/net\n", encoding="utf-8"
        )

        modules = build.read_demo_modules(sketch, repo)

        self.assertEqual([repo / "src" / "core", repo / "src" / "net"], modules)

    def test_read_demo_modules_without_file_returns_empty(self) -> None:
        sketch = self.tmp / "demo"
        sketch.mkdir()
        self.assertEqual([], build.read_demo_modules(sketch, self.tmp))

    def test_read_demo_modules_rejects_missing_module(self) -> None:
        sketch = self.tmp / "demo"
        sketch.mkdir()
        (sketch / "modules.txt").write_text("src/missing\n", encoding="utf-8")

        with self.assertRaises(FileNotFoundError):
            build.read_demo_modules(sketch, self.tmp)

    def test_build_commands_compile_only(self) -> None:
        cli = Path("arduino-cli")
        sketch = self.tmp / "repo" / "demos" / "00_smoke"
        build_dir = self.tmp / "build"
        include_dir = self.tmp / "repo" / "src"
        library = self.tmp / "repo" / "src" / "core"
        config = self.tmp / "arduino-cli.yaml"

        commands = build.build_commands(
            cli,
            sketch,
            build_dir,
            "esp32:esp32:esp32s3",
            config,
            [library],
            include_dir,
            False,
            "COM9",
        )

        self.assertEqual(1, len(commands))
        command = commands[0]
        self.assertIn("compile", command)
        self.assertNotIn("upload", command)
        self.assertEqual("esp32:esp32:esp32s3", command[command.index("--fqbn") + 1])
        self.assertEqual(str(build_dir), command[command.index("--build-path") + 1])
        self.assertEqual(str(library), command[command.index("--library") + 1])
        self.assertEqual(str(sketch), command[-1])

        include_flag = f"-I{include_dir.as_posix()}"
        self.assertIn(f"compiler.cpp.extra_flags={include_flag}", command)
        self.assertIn(f"compiler.c.extra_flags={include_flag}", command)

    def test_build_commands_with_upload(self) -> None:
        cli = Path("arduino-cli")
        sketch = self.tmp / "sketch"
        build_dir = self.tmp / "build"
        include_dir = self.tmp / "src"

        commands = build.build_commands(
            cli,
            sketch,
            build_dir,
            "fqbn",
            None,
            [],
            include_dir,
            True,
            "COM9",
        )

        self.assertEqual(2, len(commands))
        upload = commands[1]
        self.assertIn("upload", upload)
        self.assertNotIn("--config-file", upload)
        self.assertEqual("COM9", upload[upload.index("-p") + 1])
        self.assertEqual("fqbn", upload[upload.index("--fqbn") + 1])
        self.assertEqual(str(build_dir), upload[upload.index("--input-dir") + 1])
        self.assertEqual(str(sketch), upload[-1])

    def test_build_commands_include_config_when_given(self) -> None:
        config = self.tmp / "arduino-cli.yaml"
        commands = build.build_commands(
            Path("arduino-cli"),
            self.tmp / "sketch",
            self.tmp / "build",
            "fqbn",
            config,
            [],
            self.tmp / "src",
            False,
            "COM9",
        )

        command = commands[0]
        self.assertEqual(
            str(config), command[command.index("--config-file") + 1]
        )

    def test_build_monitor_command_baudrate_format(self) -> None:
        command = build.build_monitor_command(
            Path("arduino-cli"), Path("cfg.yaml"), "COM9", 115200
        )

        self.assertEqual(
            [
                "arduino-cli",
                "--config-file",
                "cfg.yaml",
                "monitor",
                "-p",
                "COM9",
                "-c",
                "baudrate=115200",
            ],
            command,
        )

    def test_build_monitor_command_custom_baud_and_no_config(self) -> None:
        command = build.build_monitor_command(
            Path("arduino-cli"), None, "COM3", 9600
        )

        self.assertEqual(
            ["arduino-cli", "monitor", "-p", "COM3", "-c", "baudrate=9600"],
            command,
        )

    def test_ensure_config_creates_isolated_file(self) -> None:
        config = self.tmp / "isolated" / "arduino-cli.yaml"

        result = build.ensure_config(config)

        self.assertEqual(config, result)
        self.assertTrue(config.is_file())
        content = config.read_text(encoding="utf-8")
        self.assertIn(build.ESP32_BOARD_MANAGER_URL, content)
        self.assertIn("additional_urls:", content)
        self.assertIn("directories:", content)
        self.assertIn("data:", content)
        self.assertIn("user:", content)

    def test_ensure_config_does_not_overwrite_existing(self) -> None:
        config = self.tmp / "arduino-cli.yaml"
        config.write_text("sentinel: true\n", encoding="utf-8")

        build.ensure_config(config)

        self.assertEqual("sentinel: true\n", config.read_text(encoding="utf-8"))

    def test_port_busy_hint_detects_busy_output(self) -> None:
        output = "Failed uploading: could not open port 'COM9': Access is denied."
        hint = build.port_busy_hint(output)
        self.assertIsNotNone(hint)
        self.assertIn("포트", hint)

    def test_port_busy_hint_detects_linux_busy_output(self) -> None:
        self.assertIsNotNone(
            build.port_busy_hint("Error opening serial port: Device or resource busy")
        )

    def test_port_busy_hint_ignores_unrelated_output(self) -> None:
        self.assertIsNone(build.port_busy_hint("Compiling sketch... OK"))

    def _make_dry_run_repo(self) -> Path:
        repo = self.tmp / "repo"
        sketch = repo / "demos" / "00_smoke"
        sketch.mkdir(parents=True)
        (sketch / "00_smoke.ino").write_text(
            "void setup() {}\nvoid loop() {}\n", encoding="utf-8"
        )
        (sketch / "modules.txt").write_text("src/core\n", encoding="utf-8")
        (repo / "src" / "core").mkdir(parents=True)
        return repo

    def test_main_dry_run_compile(self) -> None:
        repo = self._make_dry_run_repo()
        config = self.tmp / "arduino-cli.yaml"
        stdout = io.StringIO()

        with (
            mock.patch.object(build, "REPO_ROOT", repo),
            mock.patch.object(build, "find_cli", return_value=Path("arduino-cli")),
            mock.patch.object(
                build, "default_config_path", return_value=config
            ),
            mock.patch.object(
                build.subprocess,
                "run",
                side_effect=AssertionError("dry-run must not execute commands"),
            ) as run_mock,
            contextlib.redirect_stdout(stdout),
        ):
            code = build.main(["demos/00_smoke", "--dry-run"])

        self.assertEqual(0, code)
        output = stdout.getvalue()
        self.assertIn("compile", output)
        self.assertIn("--library", output)
        self.assertIn("app3M_fat9M_16MB", output)
        self.assertTrue(config.is_file())
        run_mock.assert_not_called()

    def test_main_dry_run_monitor_only(self) -> None:
        config = self.tmp / "arduino-cli.yaml"
        stdout = io.StringIO()

        with (
            mock.patch.object(
                build, "find_cli", return_value=Path("arduino-cli")
            ),
            mock.patch.object(
                build, "default_config_path", return_value=config
            ),
            mock.patch.object(
                build.subprocess,
                "run",
                side_effect=AssertionError("dry-run must not execute commands"),
            ) as run_mock,
            contextlib.redirect_stdout(stdout),
        ):
            code = build.main(
                ["--monitor", "--dry-run", "--port", "COM7", "--baud", "9600"]
            )

        self.assertEqual(0, code)
        output = stdout.getvalue()
        self.assertIn("monitor", output)
        self.assertIn("-p COM7", output)
        self.assertIn("baudrate=9600", output)
        run_mock.assert_not_called()

    def test_main_requires_sketch_or_monitor(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = build.main(["--dry-run"])
        self.assertEqual(2, code)
        self.assertIn("--monitor", stderr.getvalue())


class TestHostToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.tmp = Path(self._temp.name)

    def _write_test_file(self, name: str, content: str) -> Path:
        path = self.tmp / name
        path.write_text(content, encoding="utf-8")
        return path

    def test_parse_link_globs_explicit_single_pattern(self) -> None:
        test_file = self._write_test_file(
            "test_demo.cpp", "// LINK: src/core/*.cpp\n#include <cstdio>\n"
        )
        self.assertEqual(["src/core/*.cpp"], test_host.parse_link_globs(test_file))

    def test_parse_link_globs_multiple_patterns(self) -> None:
        test_file = self._write_test_file(
            "test_demo.cpp",
            "// LINK: src/core/*.cpp, src/sensor/*.cpp src/net/*.cpp\n",
        )
        self.assertEqual(
            ["src/core/*.cpp", "src/sensor/*.cpp", "src/net/*.cpp"],
            test_host.parse_link_globs(test_file),
        )

    def test_parse_link_globs_default_when_absent(self) -> None:
        test_file = self._write_test_file("test_demo.cpp", "int main() {}\n")
        self.assertEqual(
            test_host.DEFAULT_LINK_GLOBS,
            test_host.parse_link_globs(test_file),
        )

    def test_parse_link_globs_only_reads_first_twenty_lines(self) -> None:
        lines = ["\n"] * 20 + ["// LINK: src/late/*.cpp\n"]
        test_file = self._write_test_file("test_demo.cpp", "".join(lines))
        self.assertEqual(
            test_host.DEFAULT_LINK_GLOBS,
            test_host.parse_link_globs(test_file),
        )

    def test_discover_tests_sorted(self) -> None:
        test_dir = self.tmp / "test"
        test_dir.mkdir()
        for name in ("test_b.cpp", "test_a.cpp"):
            (test_dir / name).write_text("", encoding="utf-8")
        (test_dir / "helper.cpp").write_text("", encoding="utf-8")

        tests = test_host.discover_tests(test_dir)

        self.assertEqual(["test_a.cpp", "test_b.cpp"], [p.name for p in tests])

    def test_discover_tests_filter(self) -> None:
        test_dir = self.tmp / "test"
        test_dir.mkdir()
        for name in ("test_core_a.cpp", "test_core_b.cpp", "test_sensor.cpp"):
            (test_dir / name).write_text("", encoding="utf-8")

        tests = test_host.discover_tests(test_dir, "core")

        self.assertEqual(
            ["test_core_a.cpp", "test_core_b.cpp"], [p.name for p in tests]
        )

    def test_find_cxx_prefers_cxx_environment_variable(self) -> None:
        fake = self.tmp / "fake-g++"
        fake.write_text("", encoding="utf-8")

        result = test_host.find_cxx(
            {"CXX": str(fake), "PATH": ""}, platform="linux"
        )

        self.assertEqual(fake, result)

    def test_find_cxx_falls_back_to_path(self) -> None:
        fake = self.tmp / "g++"

        with mock.patch.object(
            test_host.shutil, "which", return_value=str(fake)
        ) as which_mock:
            result = test_host.find_cxx({"PATH": "somewhere"}, platform="linux")

        self.assertEqual(fake, result)
        which_mock.assert_called_with("g++", path="somewhere")

    def test_compile_command_contains_required_flags(self) -> None:
        cxx = Path("g++")
        test_file = self.tmp / "test_demo.cpp"
        repo = self.tmp / "repo"
        build_dir = self.tmp / "build"
        source = repo / "src" / "core" / "core.cpp"

        command = test_host.compile_command(
            cxx, test_file, repo, build_dir, [source]
        )

        self.assertEqual("g++", command[0])
        self.assertIn("-std=c++17", command)
        self.assertIn("-Wall", command)
        self.assertIn("-Wextra", command)
        self.assertIn("-Werror", command)
        self.assertIn(f"-I{repo / 'src'}", command)
        self.assertIn(f"-I{repo / 'src' / 'core'}", command)
        self.assertIn(f"-I{repo / 'test'}", command)
        self.assertEqual(str(test_file), command[8])
        self.assertIn(str(source), command)
        self.assertEqual("-o", command[-2])

    def test_compile_and_run_test_executes_built_binary(self) -> None:
        repo = self.tmp / "repo"
        test_dir = repo / "test"
        test_dir.mkdir(parents=True)
        test_file = test_dir / "test_demo.cpp"
        test_file.write_text("// LINK: src/core/*.cpp\n", encoding="utf-8")
        core_dir = repo / "src" / "core"
        core_dir.mkdir(parents=True)
        (core_dir / "core_version.cpp").write_text("", encoding="utf-8")
        build_dir = self.tmp / "build"
        executable = build_dir / (
            "test_demo" + test_host.executable_suffix()
        )

        completed = mock.Mock(returncode=0)
        with mock.patch.object(
            test_host.subprocess, "run", return_value=completed
        ) as run_mock, contextlib.redirect_stdout(io.StringIO()):
            code = test_host.compile_and_run_test(
                Path("g++"), test_file, repo, build_dir
            )

        self.assertEqual(0, code)
        self.assertEqual(2, run_mock.call_count)
        second_command = run_mock.call_args_list[1].args[0]
        self.assertEqual([str(executable)], second_command)

    def test_main_dry_run(self) -> None:
        repo = self.tmp / "repo"
        test_dir = repo / "test"
        test_dir.mkdir(parents=True)
        (test_dir / "test_demo.cpp").write_text(
            "// LINK: src/core/*.cpp\n", encoding="utf-8"
        )
        core_dir = repo / "src" / "core"
        core_dir.mkdir(parents=True)
        (core_dir / "core_version.cpp").write_text("", encoding="utf-8")

        fake_cxx = self.tmp / "g++"
        fake_cxx.write_text("", encoding="utf-8")
        stdout = io.StringIO()

        with (
            mock.patch.object(test_host, "REPO_ROOT", repo),
            mock.patch.object(
                test_host, "find_cxx", return_value=fake_cxx
            ),
            mock.patch.object(
                test_host.subprocess,
                "run",
                side_effect=AssertionError("dry-run must not execute commands"),
            ) as run_mock,
            contextlib.redirect_stdout(stdout),
        ):
            code = test_host.main(["--dry-run"])

        self.assertEqual(0, code)
        output = stdout.getvalue()
        self.assertIn("-std=c++17", output)
        self.assertIn("test_demo.cpp", output)
        self.assertIn("core_version.cpp", output)
        run_mock.assert_not_called()

    def test_main_returns_failure_when_no_tests(self) -> None:
        repo = self.tmp / "repo"
        (repo / "test").mkdir(parents=True)
        fake_cxx = self.tmp / "g++"
        fake_cxx.write_text("", encoding="utf-8")
        stderr = io.StringIO()

        with (
            mock.patch.object(test_host, "REPO_ROOT", repo),
            mock.patch.object(
                test_host, "find_cxx", return_value=fake_cxx
            ),
            contextlib.redirect_stderr(stderr),
        ):
            code = test_host.main(["--dry-run"])

        self.assertEqual(1, code)
        self.assertIn("테스트", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
