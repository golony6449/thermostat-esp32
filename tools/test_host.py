#!/usr/bin/env python3
"""Build and run host-side C++ unit tests for thermostat-esp32.

Each ``test/test_*.cpp`` file is compiled into its own executable so every
test file can define an independent ``main()`` and several parallel work
streams never collide.

Only the Python standard library is used. Commands are executed with
``subprocess`` and an argument list (``shell=False``).
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

DEFAULT_LINK_GLOBS = ["src/core/*.cpp"]
LINK_LINE_LIMIT = 20
LINK_PATTERN = re.compile(r"^\s*//\s*LINK:\s*(.+?)\s*$")
LINK_SPLIT_PATTERN = re.compile(r"[\s,]+")


def find_cxx(
    env: Mapping[str, str] = os.environ, platform: str = sys.platform
) -> Path | None:
    """Locate a host C++ compiler: CXX, PATH, then the WinGet MinGW bundle."""
    override = env.get("CXX")
    if override:
        candidate = Path(override).expanduser()
        if candidate.is_file():
            return candidate

    for name in ("g++", "clang++"):
        found = shutil.which(name, path=env.get("PATH"))
        if found:
            return Path(found)

    if platform.startswith("win"):
        local_appdata = env.get("LOCALAPPDATA")
        if local_appdata:
            package_glob = str(
                Path(local_appdata)
                / "Microsoft"
                / "WinGet"
                / "Packages"
                / "BrechtSanders.WinLibs.POSIX.UCRT_*"
                / "mingw64"
                / "bin"
                / "g++.exe"
            )
            matches = [Path(path) for path in glob.glob(package_glob)]
            matches = [path for path in matches if path.is_file()]
            if matches:
                matches.sort(
                    key=lambda path: (path.stat().st_mtime, path.name),
                    reverse=True,
                )
                return matches[0]

            link = (
                Path(local_appdata)
                / "Microsoft"
                / "WinGet"
                / "Links"
                / "g++.exe"
            )
            if link.is_file():
                return link

    return None


def parse_link_globs(test_file: Path) -> list[str]:
    """Parse ``// LINK: <globs>`` from the first 20 lines of a test file."""
    patterns: list[str] = []
    try:
        with Path(test_file).open("r", encoding="utf-8", errors="replace") as handle:
            for index, line in enumerate(handle):
                if index >= LINK_LINE_LIMIT:
                    break
                match = LINK_PATTERN.match(line)
                if match:
                    patterns.extend(
                        part
                        for part in LINK_SPLIT_PATTERN.split(match.group(1))
                        if part
                    )
    except OSError:
        return list(DEFAULT_LINK_GLOBS)

    return patterns or list(DEFAULT_LINK_GLOBS)


def discover_tests(test_dir: Path, pattern: str | None = None) -> list[Path]:
    """Return sorted ``test_*.cpp`` files, optionally filtered by substring."""
    tests = sorted(Path(test_dir).glob("test_*.cpp"))
    if pattern:
        tests = [test for test in tests if pattern in test.name]
    return tests


def expand_link_sources(repo_root: Path, patterns: Sequence[str]) -> list[Path]:
    """Expand LINK globs relative to repo_root, keeping order and de-duplicating."""
    sources: list[Path] = []
    seen: set[Path] = set()
    for pattern in patterns:
        matches = [path for path in sorted(Path(repo_root).glob(pattern)) if path.is_file()]
        if not matches:
            raise FileNotFoundError(f"LINK 패턴에 맞는 소스 파일이 없습니다: {pattern}")
        for match in matches:
            key = match.resolve()
            if key not in seen:
                seen.add(key)
                sources.append(match)
    return sources


def executable_suffix(platform: str = sys.platform) -> str:
    return ".exe" if platform.startswith("win") else ""


def compile_command(
    cxx: Path,
    test_file: Path,
    repo_root: Path,
    build_dir: Path,
    linked_sources: Sequence[Path],
) -> list[str]:
    """Assemble the g++ command for one test executable."""
    executable = Path(build_dir) / (Path(test_file).stem + executable_suffix())
    return [
        str(cxx),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{Path(repo_root) / 'src'}",
        f"-I{Path(repo_root) / 'src' / 'core'}",
        f"-I{Path(repo_root) / 'test'}",
        str(test_file),
        *(str(source) for source in linked_sources),
        "-o",
        str(executable),
    ]


def compile_and_run_test(
    cxx: Path, test_file: Path, repo_root: Path, build_dir: Path
) -> int:
    """Compile one test file and run it. Returns the executable exit code."""
    linked_sources = expand_link_sources(
        repo_root, parse_link_globs(test_file)
    )
    Path(build_dir).mkdir(parents=True, exist_ok=True)
    command = compile_command(cxx, test_file, repo_root, build_dir, linked_sources)
    print(f"$ {shlex.join(command)}", flush=True)

    env = os.environ.copy()
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    completed = subprocess.run(command, env=env, check=False)
    if completed.returncode != 0:
        return completed.returncode

    executable = Path(build_dir) / (Path(test_file).stem + executable_suffix())
    print(f"$ {shlex.join([str(executable)])}", flush=True)
    return subprocess.run([str(executable)], env=env, check=False).returncode


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="thermostat-esp32 host-side C++ unit test runner"
    )
    parser.add_argument(
        "--filter",
        help="테스트 파일 이름에 포함될 부분 문자열 (예: core)",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="컴파일 명령만 출력하고 실행하지 않음"
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    cxx = find_cxx()
    if cxx is None:
        print(
            "C++ 컴파일러를 찾지 못했습니다. CXX 환경변수나 PATH를 확인하세요.",
            file=sys.stderr,
        )
        return 1

    tests = discover_tests(REPO_ROOT / "test", args.filter)
    if not tests:
        print("실행할 테스트가 없습니다.", file=sys.stderr)
        return 1

    build_dir = REPO_ROOT / ".build" / "host_tests"

    if args.dry_run:
        for test_file in tests:
            linked_sources = expand_link_sources(
                REPO_ROOT, parse_link_globs(test_file)
            )
            command = compile_command(
                cxx, test_file, REPO_ROOT, build_dir, linked_sources
            )
            print(shlex.join(command))
        return 0

    failed = 0
    for test_file in tests:
        print(f"=== {test_file.name} ===", flush=True)
        if compile_and_run_test(cxx, test_file, REPO_ROOT, build_dir) != 0:
            failed += 1

    print(f"Tests: {len(tests)}, Failed: {failed}", flush=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
