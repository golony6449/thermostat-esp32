#!/usr/bin/env python3
"""Build, upload and monitor thermostat-esp32 ESP32-S3 sketches.

Uses a project-isolated arduino-cli configuration so the global Arduino15
data directory shared with other projects (for example esp32-rtos-lab and the
Arduino IDE) is never modified.

Only the Python standard library is used. Commands are executed with
``subprocess`` and an argument list (``shell=False``).
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Mapping, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

BASE_FQBN = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PSRAM=opi"
DEFAULT_PARTITION = "app3M_fat9M_16MB"
DEFAULT_BAUD = 115200
ESP32_CORE_PACKAGE = "esp32:esp32@3.3.13"
ESP32_BOARD_MANAGER_URL = (
    "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
)

DEFAULT_PORT_WINDOWS = "COM9"
DEFAULT_PORT_POSIX = "/dev/ttyUSB0"

DEFAULT_FIXED_DRIVE = "T:"
FIXED_PATH_LOCK_NAME = "fixed-path.lock"

_PORT_BUSY_PATTERNS = (
    "access is denied",
    "could not open port",
    "can't open serial port",
    "cannot open serial port",
    "failed to open serial port",
    "error opening serial port",
    "serial port busy",
    "port is busy",
    "resource temporarily unavailable",
    "device or resource busy",
    "permission denied",
)


def make_fqbn(partition: str | None = DEFAULT_PARTITION) -> str:
    """Return the target FQBN, appending the partition scheme when given."""
    if partition:
        return f"{BASE_FQBN},PartitionScheme={partition}"
    return BASE_FQBN


def default_port(platform: str = sys.platform) -> str:
    """Best-effort default serial port for this platform."""
    if platform.startswith("win"):
        return DEFAULT_PORT_WINDOWS
    return DEFAULT_PORT_POSIX


def find_cli(env: Mapping[str, str] = os.environ) -> Path | None:
    """Locate arduino-cli: explicit env var, PATH, then the Arduino IDE bundle."""
    override = env.get("ARDUINO_CLI")
    if override:
        candidate = Path(override).expanduser()
        if candidate.is_file():
            return candidate

    for name in ("arduino-cli", "arduino-cli.exe"):
        found = shutil.which(name, path=env.get("PATH"))
        if found:
            return Path(found)

    if sys.platform.startswith("win"):
        candidates = []
        program_files = env.get("ProgramFiles", r"C:\Program Files")
        if program_files:
            candidates.append(
                Path(program_files)
                / "Arduino IDE"
                / "resources"
                / "app"
                / "lib"
                / "backend"
                / "resources"
                / "arduino-cli.exe"
            )
        local_appdata = env.get("LOCALAPPDATA")
        if local_appdata:
            candidates.append(
                Path(local_appdata)
                / "Programs"
                / "Arduino IDE"
                / "resources"
                / "app"
                / "lib"
                / "backend"
                / "resources"
                / "arduino-cli.exe"
            )
        for candidate in candidates:
            if candidate.is_file():
                return candidate

    return None


def default_config_path(
    env: Mapping[str, str] = os.environ, platform: str = sys.platform
) -> Path:
    """Return the project-isolated arduino-cli.yaml path."""
    override = env.get("THERMOSTAT_ARDUINO_CONFIG")
    if override:
        return Path(override).expanduser()

    if platform.startswith("win"):
        local_appdata = env.get("LOCALAPPDATA")
        if local_appdata:
            base = Path(local_appdata)
        else:
            base = Path.home() / "AppData" / "Local"
        return base / "thermostat-esp32" / "arduino-cli.yaml"

    return Path.home() / ".local" / "share" / "thermostat-esp32" / "arduino-cli.yaml"


def fixed_path_enabled(env: Mapping[str, str] = os.environ) -> bool:
    """Return False only when ``THERMOSTAT_FIXED_PATH`` is exactly ``"0"``."""
    return env.get("THERMOSTAT_FIXED_PATH") != "0"


def fixed_drive(env: Mapping[str, str] = os.environ) -> str:
    """Return the mapped-drive letter as uppercase ``"X:"``.

    ``THERMOSTAT_BUILD_DRIVE`` (for example ``"x:"``) overrides the default
    ``T:``. Anything that is not one letter followed by a colon raises
    ``ValueError``.
    """
    raw = env.get("THERMOSTAT_BUILD_DRIVE")
    if raw is None:
        return DEFAULT_FIXED_DRIVE
    drive = raw.upper()
    if len(drive) != 2 or drive[1] != ":" or not ("A" <= drive[0] <= "Z"):
        raise ValueError(
            "THERMOSTAT_BUILD_DRIVE는 드라이브 글자와 콜론이어야 합니다"
            f" (예: T:). 현재 값: {raw!r}"
        )
    return drive


def fixed_root(
    platform: str = sys.platform, env: Mapping[str, str] = os.environ
) -> Path:
    """Return the stable build root: ``T:\\`` on Windows, a cache link elsewhere."""
    if platform.startswith("win"):
        return Path(fixed_drive(env) + "\\")
    return Path.home() / ".cache" / "thermostat-esp32" / "src-root"


def to_fixed(path: Path, repo_root: Path, root: Path) -> Path:
    """Map a path under ``repo_root`` to the same relative path under ``root``."""
    path = Path(path)
    repo_root = Path(repo_root)
    try:
        relative = path.relative_to(repo_root)
    except ValueError:
        raise ValueError(
            f"저장소 밖 경로는 고정 경로로 바꿀 수 없습니다: {path}"
        ) from None
    return Path(root) / relative


_SUBST_LINE_RE = re.compile(r"^([A-Za-z]):\\:\s*=>\s*(.+)$")


def parse_subst_output(text: str) -> dict[str, str]:
    """Parse ``subst`` output lines (``T:\\: => D:\\path``) into ``{"T:": ...}``.

    Lines that do not match the expected format are ignored.
    """
    mappings: dict[str, str] = {}
    for line in text.splitlines():
        match = _SUBST_LINE_RE.match(line.strip())
        if not match:
            continue
        mappings[match.group(1).upper() + ":"] = match.group(2).rstrip()
    return mappings


class FixedPathUnavailable(Exception):
    """드라이브 글자를 다른 용도로 쓰고 있어 고정 경로를 쓸 수 없음."""


def _lock_handle(handle) -> None:
    if sys.platform.startswith("win"):
        import msvcrt

        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl

        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)


def _unlock_handle(handle) -> None:
    try:
        if sys.platform.startswith("win"):
            import msvcrt

            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
    except OSError:
        pass


@contextlib.contextmanager
def fixed_path_lock(
    lock_path: Path,
    *,
    timeout_sec: float = 3600.0,
    poll_sec: float = 1.0,
    notify=print,
):
    """Hold an exclusive lock for the shared fixed build root.

    The OS releases the lock when the process dies, so a crashed build never
    leaves the lock stuck. If another build holds it, ``notify`` is called once
    and the lock is retried every ``poll_sec`` until ``timeout_sec``.
    """
    lock_path = Path(lock_path)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    handle = lock_path.open("a+b")
    handle.seek(0, os.SEEK_END)
    if handle.tell() == 0:
        handle.write(b"\0")
        handle.flush()

    locked = False
    waited = False
    deadline = time.monotonic() + timeout_sec
    try:
        while True:
            try:
                _lock_handle(handle)
                locked = True
                break
            except OSError:
                if not waited:
                    notify("[build] 다른 워크트리 빌드가 끝나기를 기다립니다")
                    waited = True
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "고정 경로 잠금을 기다리다 시간이 초과했습니다: "
                        f"{lock_path}"
                    ) from None
                time.sleep(poll_sec)
        yield
    finally:
        if locked:
            _unlock_handle(handle)
        handle.close()


def _read_subst_mappings(run) -> dict[str, str]:
    completed = run(["subst"], capture_output=True, text=True)
    if getattr(completed, "returncode", 0) != 0:
        return {}
    return parse_subst_output(getattr(completed, "stdout", "") or "")


@contextlib.contextmanager
def mapped_fixed_root(
    repo_root: Path,
    *,
    platform: str = sys.platform,
    env: Mapping[str, str] = os.environ,
    run=subprocess.run,
):
    """Map the fixed root onto ``repo_root`` for the duration of the block.

    Windows: connect ``<drive>`` with ``subst``, removing any leftover mapping
    first, and always disconnect in ``finally``. If the drive letter belongs to
    a real drive, raise ``FixedPathUnavailable`` without touching it.
    POSIX: atomically install a symlink to ``repo_root`` and leave it in place.
    The caller is expected to hold ``fixed_path_lock`` already.
    """
    repo_root = Path(repo_root)
    drive = fixed_drive(env)

    if platform.startswith("win"):
        mappings = _read_subst_mappings(run)
        if drive in mappings:
            run(["subst", drive, "/D"], check=False)
        elif Path(drive + "\\").exists():
            raise FixedPathUnavailable(drive)
        run(["subst", drive, str(repo_root)], check=True)
        try:
            yield Path(drive + "\\")
        finally:
            run(["subst", drive, "/D"], check=False)
        return

    link = fixed_root(platform, env)
    link.parent.mkdir(parents=True, exist_ok=True)
    temporary = link.with_name(f".{link.name}.{os.getpid()}.tmp")
    try:
        if temporary.is_symlink() or temporary.exists():
            temporary.unlink()
        os.symlink(repo_root, temporary, target_is_directory=True)
        os.replace(temporary, link)
    finally:
        if temporary.is_symlink() or temporary.exists():
            temporary.unlink()
    yield link


def fixed_compile_command(
    cli,
    config,
    sketch_dir: Path,
    build_dir: Path,
    fqbn,
    libraries: Sequence[Path],
    include_dir: Path,
    root: Path,
    repo_root: Path,
) -> list[str]:
    """Return the compile command with every repository path under ``root``."""
    return build_commands(
        cli,
        to_fixed(sketch_dir, repo_root, root),
        to_fixed(build_dir, repo_root, root),
        fqbn,
        config,
        [to_fixed(library, repo_root, root) for library in libraries],
        to_fixed(include_dir, repo_root, root),
        False,
        None,
        compile=True,
    )[0]


def main_checkout_root(repo_root: Path, run=subprocess.run) -> Path | None:
    """Return the main checkout root that owns ``repo_root``, or ``None``."""
    try:
        completed = run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            cwd=str(repo_root),
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    output = (completed.stdout or "").strip()
    if not output:
        return None
    return Path(output).parent


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(os.path.abspath(str(left))) == os.path.normcase(
        os.path.abspath(str(right))
    )


def _normalized_location(value: str) -> str:
    return value.replace("/", "\\").lower()


def built_with_fixed_root(build_dir: Path, root: Path) -> bool:
    """Return True if ``build_dir`` was built with ``sketchLocation`` under ``root``."""
    options_path = Path(build_dir) / "build.options.json"
    try:
        options = json.loads(options_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    if not isinstance(options, dict):
        return False
    location = options.get("sketchLocation")
    if not isinstance(location, str):
        return False

    root_text = _normalized_location(str(root))
    if not root_text.endswith("\\"):
        root_text += "\\"
    return _normalized_location(location).startswith(root_text)


def seed_build_dir(
    build_dir: Path,
    source_build_dir: Path,
    sketch_name: str,
    module_names: Sequence[str],
) -> bool:
    """Copy a main-checkout build folder and drop everything built from our sources.

    arduino-cli decides object reuse by mtime only, so objects compiled from
    another worktree's sources must not survive the copy. Sketch objects, the
    project modules' library folders and the sketch binaries are removed; the
    third-party libraries, the Arduino core objects and ``build.options.json``
    stay so only our modules are rebuilt.
    """
    build_dir = Path(build_dir)
    if build_dir.exists():
        return False

    shutil.copytree(Path(source_build_dir), build_dir)

    sketch_output = build_dir / "sketch"
    if sketch_output.is_dir():
        shutil.rmtree(sketch_output)
    elif sketch_output.exists():
        sketch_output.unlink()

    libraries_dir = build_dir / "libraries"
    for name in module_names:
        module_output = libraries_dir / name
        if module_output.is_dir():
            shutil.rmtree(module_output)
        elif module_output.exists():
            module_output.unlink()

    prefix = f"{sketch_name}.ino"
    for artifact in build_dir.iterdir():
        if not artifact.name.startswith(prefix):
            continue
        if artifact.is_dir():
            shutil.rmtree(artifact)
        else:
            artifact.unlink()

    return True


def ensure_config(
    config_path: Path,
    *,
    user_dir: Path | None = None,
    data_dir: Path | None = None,
    board_manager_url: str = ESP32_BOARD_MANAGER_URL,
) -> Path:
    """Create the isolated arduino-cli.yaml if missing; never overwrite it.

    The data directory lives next to the config file so installed cores are
    independent from the global ``Arduino15`` used by other projects. The user
    directory is shared with the regular Arduino sketchbook so offline
    Waveshare libraries keep working.
    """
    config_path = Path(config_path).expanduser()
    if config_path.exists():
        return config_path

    config_path.parent.mkdir(parents=True, exist_ok=True)

    if data_dir is None:
        data_dir = config_path.parent / "arduino15"
    data_dir = Path(data_dir).expanduser()

    if user_dir is None:
        user_dir = Path.home() / "Documents" / "Arduino"
    user_dir = Path(user_dir).expanduser()

    content = (
        "# Generated by tools/build.py: project-isolated arduino-cli config.\n"
        "# The global Arduino15 data directory is intentionally not used.\n"
        "board_manager:\n"
        "  additional_urls:\n"
        f"    - {board_manager_url}\n"
        "\n"
        "directories:\n"
        f"  data: {data_dir.as_posix()}\n"
        f"  downloads: {(data_dir / 'staging').as_posix()}\n"
        f"  user: {user_dir.as_posix()}\n"
    )
    with config_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    return config_path


def read_demo_modules(sketch_dir: Path, repo_root: Path) -> list[Path]:
    """Read ``<sketch>/modules.txt`` and resolve module paths under repo_root."""
    sketch_dir = Path(sketch_dir)
    repo_root = Path(repo_root)
    modules_file = sketch_dir / "modules.txt"
    if not modules_file.is_file():
        return []

    modules: list[Path] = []
    text = modules_file.read_text(encoding="utf-8")
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        module = repo_root / line
        if not module.exists():
            raise FileNotFoundError(
                f"{modules_file}:{lineno}: module path not found: {module}"
            )
        modules.append(module)
    return modules


def build_artifact_path(sketch_dir: Path, build_dir: Path) -> Path:
    """Return the compiled sketch binary that an upload-only run uploads."""
    return Path(build_dir) / f"{Path(sketch_dir).name}.ino.bin"


def build_commands(
    cli,
    sketch,
    build_dir,
    fqbn,
    config,
    libraries,
    include_dir,
    upload,
    port,
    compile: bool = True,
) -> list[list[str]]:
    """Assemble arduino-cli compile and (optional) upload commands.

    ``compile=False`` skips the compile command entirely, for ``--upload-only``
    runs that reuse the existing build directory.
    """
    prefix = [str(cli)]
    if config is not None:
        prefix.extend(["--config-file", str(config)])

    commands = []

    if compile:
        compile_command = prefix + [
            "compile",
            "--fqbn",
            str(fqbn),
            "--build-path",
            str(build_dir),
        ]
        for library in libraries:
            compile_command.extend(["--library", str(library)])
        include_flag = f"-I{Path(include_dir).as_posix()}"
        compile_command.extend(
            [
                "--build-property",
                f"compiler.cpp.extra_flags={include_flag}",
                "--build-property",
                f"compiler.c.extra_flags={include_flag}",
                str(sketch),
            ]
        )
        commands.append(compile_command)

    if upload:
        upload_command = prefix + ["upload"]
        if port:
            upload_command.extend(["-p", str(port)])
        upload_command.extend(
            [
                "--fqbn",
                str(fqbn),
                "--input-dir",
                str(build_dir),
                str(sketch),
            ]
        )
        commands.append(upload_command)

    return commands


def build_monitor_command(
    cli, config, port, baud: int = DEFAULT_BAUD
) -> list[str]:
    """Assemble the arduino-cli monitor command."""
    command = [str(cli)]
    if config is not None:
        command.extend(["--config-file", str(config)])
    command.append("monitor")
    if port:
        command.extend(["-p", str(port)])
    command.extend(["-c", f"baudrate={baud}"])
    return command


def port_busy_hint(output: str) -> str | None:
    """Return a Korean hint when output looks like a serial port busy error."""
    lowered = output.lower()
    if not any(pattern in lowered for pattern in _PORT_BUSY_PATTERNS):
        return None
    return (
        "시리얼 포트를 열지 못했습니다. Arduino IDE 시리얼 모니터처럼 포트를 점유한 "
        "프로그램을 닫고 다시 시도하세요. Linux에서는 dialout 그룹 권한도 확인하세요."
    )


def run_logged(command: Sequence[str]) -> tuple[int, str]:
    """Run a command, capture its output, echo it, and return (code, output)."""
    print(f"$ {shlex.join([str(part) for part in command])}", flush=True)
    completed = subprocess.run(
        [str(part) for part in command],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    if output:
        print(output, end="" if output.endswith("\n") else "\n", flush=True)
    return completed.returncode, output


def run_interactive(command: Sequence[str]) -> int:
    """Run a long-lived interactive command such as the serial monitor."""
    print(f"$ {shlex.join([str(part) for part in command])}", flush=True)
    completed = subprocess.run([str(part) for part in command], check=False)
    return completed.returncode


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="thermostat-esp32 ESP32-S3 build / upload / monitor helper"
    )
    parser.add_argument(
        "sketch",
        nargs="?",
        help="스케치 폴더 또는 .ino 경로 (예: demos/00_smoke, 저장소 루트 기준)",
    )
    parser.add_argument("--upload", action="store_true", help="컴파일 후 업로드")
    parser.add_argument(
        "--upload-only",
        action="store_true",
        help="컴파일 없이 기존 빌드 결과만 업로드 (--upload보다 우선)",
    )
    parser.add_argument("--monitor", action="store_true", help="시리얼 모니터 실행")
    parser.add_argument("--port", help="시리얼 포트 (예: COM9, /dev/ttyUSB0)")
    parser.add_argument(
        "--baud", type=int, default=DEFAULT_BAUD, help="모니터 baudrate"
    )
    parser.add_argument(
        "--partition",
        default=DEFAULT_PARTITION,
        help=f"파티션 스킴 (기본: {DEFAULT_PARTITION})",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="명령만 출력하고 실행하지 않음"
    )
    return parser.parse_args(argv)


def resolve_sketch_dir(sketch: str) -> Path:
    """Resolve a sketch argument relative to the repository root."""
    sketch_path = Path(sketch)
    if not sketch_path.is_absolute():
        sketch_path = REPO_ROOT / sketch_path
    sketch_path = sketch_path.resolve()
    if sketch_path.is_file():
        return sketch_path.parent
    return sketch_path


def _run_logged_step(label: str, command: Sequence[str]) -> tuple[int, str]:
    started_sec = time.monotonic()
    code, output = run_logged(command)
    elapsed_sec = time.monotonic() - started_sec
    print(f"[build] {label} {elapsed_sec:.1f}초", flush=True)
    return code, output


def _seed_from_main_checkout(
    build_dir: Path,
    sketch_name: str,
    libraries: Sequence[Path],
    root: Path,
    repo_root: Path,
) -> None:
    """Seed a fresh worktree's build folder from the main checkout when possible."""
    if Path(build_dir).exists():
        return
    main_root = main_checkout_root(repo_root)
    if main_root is None or _same_path(main_root, repo_root):
        return
    source_build_dir = Path(main_root) / ".build" / sketch_name
    if not built_with_fixed_root(source_build_dir, root):
        return
    if seed_build_dir(
        build_dir,
        source_build_dir,
        sketch_name,
        [Path(library).name for library in libraries],
    ):
        print(f"[build] 메인 체크아웃 빌드 폴더에서 시작: {source_build_dir}")


def _run_compile(
    cli,
    config,
    sketch_dir: Path,
    build_dir: Path,
    fqbn,
    libraries: Sequence[Path],
    port,
    repo_root: Path,
    root: Path | None,
) -> int:
    """Run the compile command, using the fixed root when available."""
    real_command = build_commands(
        cli,
        sketch_dir,
        build_dir,
        fqbn,
        config,
        libraries,
        repo_root / "src",
        False,
        port,
        compile=True,
    )[0]

    if root is None:
        code, _output = _run_logged_step("compile", real_command)
        return code

    lock_path = Path(config).parent / FIXED_PATH_LOCK_NAME
    try:
        with fixed_path_lock(lock_path):
            with mapped_fixed_root(repo_root) as fixed_root_path:
                _seed_from_main_checkout(
                    build_dir,
                    sketch_dir.name,
                    libraries,
                    fixed_root_path,
                    repo_root,
                )
                command = fixed_compile_command(
                    cli,
                    config,
                    sketch_dir,
                    build_dir,
                    fqbn,
                    libraries,
                    repo_root / "src",
                    fixed_root_path,
                    repo_root,
                )
                code, _output = _run_logged_step("compile", command)
                return code
    except FixedPathUnavailable:
        print(
            f"[build] {fixed_drive()}를 다른 용도로 쓰고 있어"
            " 고정 경로 없이 빌드합니다",
            file=sys.stderr,
        )
        code, _output = _run_logged_step("compile", real_command)
        return code
    except TimeoutError as exc:
        print(f"[build] {exc}", file=sys.stderr)
        return 1


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    if not args.sketch and not args.monitor:
        print(
            "스케치 경로 또는 --monitor 중 하나는 지정해야 합니다. "
            "(예: python tools/build.py demos/00_smoke)",
            file=sys.stderr,
        )
        return 2

    if args.upload_only and not args.sketch:
        print(
            "--upload-only에는 스케치 경로가 필요합니다. "
            "(예: python tools/build.py firmware/thermostat_main --upload-only)",
            file=sys.stderr,
        )
        return 2

    port = args.port or os.environ.get("THERMOSTAT_PORT") or default_port()
    config = ensure_config(default_config_path())

    cli = find_cli()
    if cli is None:
        print(
            "arduino-cli를 찾지 못했습니다. ARDUINO_CLI 환경변수나 PATH를 확인하세요.",
            file=sys.stderr,
        )
        return 1

    if args.sketch:
        sketch_dir = resolve_sketch_dir(args.sketch)
        if not sketch_dir.is_dir():
            print(f"스케치 폴더가 없습니다: {sketch_dir}", file=sys.stderr)
            return 2

        build_dir = REPO_ROOT / ".build" / sketch_dir.name
        if args.upload_only:
            artifact = build_artifact_path(sketch_dir, build_dir)
            if not artifact.is_file():
                print(
                    f"업로드할 빌드 결과가 없습니다: {artifact}\n"
                    f"먼저 컴파일하세요: python tools/build.py {args.sketch}",
                    file=sys.stderr,
                )
                return 2
        libraries = read_demo_modules(sketch_dir, REPO_ROOT)
        compile_sketch = not args.upload_only
        do_upload = args.upload or args.upload_only
        fqbn = make_fqbn(args.partition)

        root = None
        if compile_sketch and fixed_path_enabled():
            try:
                root = fixed_root()
            except ValueError as exc:
                print(f"[build] {exc}", file=sys.stderr)
                return 2

        if args.dry_run:
            if compile_sketch:
                if root is None:
                    compile_command = build_commands(
                        cli,
                        sketch_dir,
                        build_dir,
                        fqbn,
                        config,
                        libraries,
                        REPO_ROOT / "src",
                        False,
                        port,
                        compile=True,
                    )[0]
                else:
                    compile_command = fixed_compile_command(
                        cli,
                        config,
                        sketch_dir,
                        build_dir,
                        fqbn,
                        libraries,
                        REPO_ROOT / "src",
                        root,
                        REPO_ROOT,
                    )
                print(shlex.join([str(part) for part in compile_command]))
            if do_upload:
                upload_command = build_commands(
                    cli,
                    sketch_dir,
                    build_dir,
                    fqbn,
                    config,
                    libraries,
                    REPO_ROOT / "src",
                    True,
                    port,
                    compile=False,
                )[0]
                print(shlex.join([str(part) for part in upload_command]))
        else:
            if compile_sketch:
                code = _run_compile(
                    cli,
                    config,
                    sketch_dir,
                    build_dir,
                    fqbn,
                    libraries,
                    port,
                    REPO_ROOT,
                    root,
                )
                if code != 0:
                    return code
            if do_upload:
                upload_command = build_commands(
                    cli,
                    sketch_dir,
                    build_dir,
                    fqbn,
                    config,
                    libraries,
                    REPO_ROOT / "src",
                    True,
                    port,
                    compile=False,
                )[0]
                code, output = _run_logged_step("upload", upload_command)
                if code != 0:
                    hint = port_busy_hint(output)
                    if hint:
                        print(hint, file=sys.stderr)
                    return code

    if args.monitor:
        monitor_command = build_monitor_command(cli, config, port, args.baud)
        if args.dry_run:
            print(shlex.join([str(part) for part in monitor_command]))
        else:
            return run_interactive(monitor_command)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
