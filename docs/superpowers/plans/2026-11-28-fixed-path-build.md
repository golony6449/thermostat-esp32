# SCRUM-333 고정 경로 빌드로 새 워크트리 첫 빌드 줄이기

## 목적

새 워크트리의 첫 `firmware/thermostat_main` 빌드(약 55분)를 10분 이내로 줄인다. 워크트리마다 경로가 달라 arduino-cli 캐시를 못 쓰는 것이 원인이므로, 어느 워크트리든 같은 경로(`T:\`)로 보이게 해서 빌드하고, 새 워크트리는 메인 체크아웃의 빌드 폴더를 복사해 시작한다.

## 배경 (실측·소스 확인, 2026-11-28)

- arduino-cli 1.6.0은 `build.options.json`(스케치·라이브러리·`-I` 절대 경로 포함)이 바뀌면 빌드 폴더를 지운다. `includes.cache`는 순서 있는 목록이라 한 항목이 어긋나면 뒤를 모두 버리고 직렬로 전처리한다(감지 28분). `libraries.cache`가 바뀌면 라이브러리 오브젝트를 모두 지운다(약 20분).
- 오브젝트 재사용 판단은 mtime뿐이다(`utils.ObjFileIsUpToDate`: 소스·헤더 mtime이 `.o`보다 새로우면 다시 컴파일). 컴파일 명령 비교는 없다.
- `subst T: <경로>`로 연결한 드라이브 경로를 arduino-cli는 실제 경로로 바꾸지 않고 그대로 쓴다(`build.options.json`의 `"sketchLocation": "T:\\firmware\\thermostat_main"` 확인).
- `--skip-libraries-discovery`는 감지를 건너뛰지만 라이브러리가 링크되지 않아 실패한다(`ld returned 1`). 쓰지 않는다.

## 설계

1. **고정 루트:** Windows는 `subst T: <REPO_ROOT>`로 연결한 `T:\`를, 리눅스·macOS는 `~/.cache/thermostat-esp32/src-root` 심볼릭 링크를 고정 루트로 쓴다. 드라이브 글자는 환경변수 `THERMOSTAT_BUILD_DRIVE`(기본 `T:`)로 바꿀 수 있고, `THERMOSTAT_FIXED_PATH=0`이면 고정 경로를 쓰지 않는다(지금 방식).
2. **compile 명령만** 고정 루트 기준 경로를 쓴다: 스케치, `--library`, `-I<root>/src`, `--build-path <root>/.build/<스케치>`. 빌드 폴더의 실제 위치는 지금처럼 `<REPO_ROOT>/.build/<스케치>`다. upload 명령은 실제 경로 그대로 둔다(연결이 필요 없다).
3. **잠금:** 고정 루트는 사용자 세션 전체에 하나라서, compile 동안 잠금 파일(`default_config_path().parent / "fixed-path.lock"`)을 잡는다. Windows는 `msvcrt.locking`, POSIX는 `fcntl.flock`(프로세스가 죽으면 OS가 풀어 준다). 다른 빌드가 잡고 있으면 `[build] 다른 워크트리 빌드가 끝나기를 기다립니다` 를 한 번 출력하고 기다린다(1초 간격 재시도, 최대 3600초. 넘으면 실패 코드 1과 안내 출력).
4. **연결·해제:** 잠금을 잡은 뒤 연결한다. Windows는 `subst` 출력에서 드라이브가 이미 연결돼 있으면(잠금을 우리가 잡았으니 남은 찌꺼기) `subst T: /D`로 지우고 다시 연결한다. 드라이브 글자가 `subst`가 아닌 실제 드라이브로 쓰이고 있으면(`Path("T:\\").exists()`인데 `subst` 목록에 없음) 고정 경로를 포기하고 지금 방식으로 빌드하며 경고를 출력한다. compile이 끝나면(실패·예외 포함) `subst T: /D`로 해제하고 잠금을 푼다. POSIX는 링크를 새로 만든 뒤 `os.replace`로 바꿔 끼우고, 끝나도 링크는 남겨 둔다.
5. **새 워크트리 시드:** compile 전에 `<REPO_ROOT>/.build/<스케치>`가 없고, 메인 체크아웃(`git rev-parse --path-format=absolute --git-common-dir`의 부모)이 `REPO_ROOT`와 다르며, 메인 쪽 `.build/<스케치>/build.options.json`의 `sketchLocation`이 고정 루트로 시작하면 그 폴더를 `shutil.copytree`로 복사한다. 복사한 뒤 **우리 소스로 만든 결과를 지운다**(mtime 판단만 하므로, 메인에서 나중에 빌드한 오브젝트가 워크트리의 다른 코드보다 새로워 재사용되는 일을 막는다):
   - `<build>/sketch/` 폴더 전체
   - `<build>/libraries/<이름>/` 폴더: `modules.txt`의 각 모듈 경로의 마지막 이름(`src/ui/waveshare43b` → `waveshare43b`)
   - `<build>/<스케치>.ino.*` 산출물(`.bin`, `.elf`, `.map`, `.merged.bin`, `_flashed.bin` 등 이름이 `<스케치>.ino`로 시작하는 파일) — 컴파일하지 않고 `--upload-only`를 하면 메인 바이너리가 올라가는 것을 막는다
   - 시드 결과를 `[build] 메인 체크아웃 빌드 폴더에서 시작: <경로>` 로 출력한다
6. **처음 한 번 전체 빌드:** 지금 빌드 폴더는 실제 경로로 빌드돼 있어서, 고정 경로로 처음 빌드하면 arduino-cli가 옵션이 바뀐 것으로 보고 전부 다시 빌드한다(1회, 약 50분). 정상 동작이다.
7. **`--dry-run`:** 연결·잠금·시드 없이 고정 루트 기준 명령만 출력한다.

## 실제 표본

`subst` 출력(Windows 11, `subst T: D:\Programming\thermostat-esp32` 뒤 `subst` 실행). 바이트 그대로:

```
T:\: => D:\Programming\thermostat-esp32\r\n
```

연결이 없으면 출력은 빈 문자열이다. 기대값: `parse_subst_output("T:\\: => D:\\Programming\\thermostat-esp32\r\n")` → `{"T:": "D:\\Programming\\thermostat-esp32"}`, `parse_subst_output("")` → `{}`. 키는 대문자 드라이브 글자와 콜론(`T:`)이다.

고정 경로로 빌드한 `build.options.json`의 일부(실측):

```json
{
  "customBuildProperties": "compiler.cpp.extra_flags=-IT:/src,compiler.c.extra_flags=-IT:/src",
  "sketchLocation": "T:\\firmware\\thermostat_main"
}
```

기대값: 고정 루트 `T:\` 기준으로 `sketchLocation`이 `T:\`로 시작하므로 시드 대상이다. 실제 경로(`"D:\\Programming\\thermostat-esp32\\firmware\\thermostat_main"`)면 시드하지 않는다.

`modules.txt`(실제 파일, `firmware/thermostat_main/modules.txt`)의 모듈과 빌드 폴더 `libraries/` 아래 이름(실측):

| modules.txt 줄 | 지울 `libraries/` 폴더 |
|---|---|
| `src/core` | `core` |
| `src/sensor` | `sensor` |
| `src/net` | `net` |
| `src/storage` | `storage` |
| `src/matter_ep` | `matter_ep` |
| `src/ui/waveshare43b` | `waveshare43b` |
| `src/app` | `app` |
| `src/firmware` | `firmware` |

빌드 폴더의 다른 `libraries/` 폴더(`lvgl`, `Matter`, `WiFi`, `Network`, `HTTPClient`, `NetworkClientSecure`, `Preferences`, `ESP32_Display_Panel`, `ESP32_IO_Expander`, `esp-lib-utils`)와 최상위 `core/`(Arduino 코어 오브젝트)는 지우지 않는다.

## 작업

### 작업 1: 고정 경로 도우미와 compile 경로 변환

**Files:**
- Modify: `tools/build.py`
- Test: `tools/test_build.py`

새 함수(모두 `tools/build.py`, 표준 라이브러리만):

```python
DEFAULT_FIXED_DRIVE = "T:"

def fixed_path_enabled(env: Mapping[str, str] = os.environ) -> bool:
    """THERMOSTAT_FIXED_PATH가 "0"이면 False, 그 밖에는 True."""

def fixed_drive(env: Mapping[str, str] = os.environ) -> str:
    """THERMOSTAT_BUILD_DRIVE(예: "x:" -> "X:")를 대문자+콜론으로 정규화. 없으면 DEFAULT_FIXED_DRIVE.
    한 글자 알파벳+콜론이 아니면 ValueError."""

def fixed_root(platform: str = sys.platform, env: Mapping[str, str] = os.environ) -> Path:
    """Windows: Path(fixed_drive(env) + "\\"). 그 밖: Path.home()/".cache"/"thermostat-esp32"/"src-root"."""

def to_fixed(path: Path, repo_root: Path, root: Path) -> Path:
    """repo_root 아래 경로를 root 아래 같은 상대 경로로 바꾼다. repo_root 밖이면 ValueError."""

def parse_subst_output(text: str) -> dict[str, str]:
    """`subst` 출력의 각 줄 "X:\\: => <대상>"을 {"X:": "<대상>"}으로. 형식이 다른 줄은 무시."""
```

`build_commands`는 바꾸지 않는다. `main`이 compile용 인자(스케치, 라이브러리, include 폴더, 빌드 폴더)를 `to_fixed`로 바꿔 넘기고, upload용은 실제 경로로 넘긴다(필요하면 compile 명령과 upload 명령을 `build_commands`를 두 번 불러 만든다: `compile=True, upload=False` / `compile=False, upload=True`).

테스트:
- `parse_subst_output`: 실제 표본 바이트 그대로(`"T:\\: => D:\\Programming\\thermostat-esp32\r\n"`), 빈 문자열, 두 줄, 형식이 다른 줄
- `fixed_drive`: 기본값, 소문자 정규화, 잘못된 값 ValueError
- `fixed_path_enabled`: "0"이면 False, 없거나 "1"이면 True
- `to_fixed`: `repo/src/core` → `T:\src\core`(Windows 경로는 `PureWindowsPath`로 비교하거나 문자열 비교), repo 밖 경로 ValueError
- `fixed_root`: `platform="win32"`와 `"linux"` 각각

### 작업 2: 잠금과 연결

**Files:**
- Modify: `tools/build.py`
- Test: `tools/test_build.py`

```python
class FixedPathUnavailable(Exception):
    """드라이브 글자를 다른 용도로 쓰고 있어 고정 경로를 쓸 수 없음."""

@contextlib.contextmanager
def fixed_path_lock(lock_path: Path, *, timeout_sec: float = 3600.0,
                    poll_sec: float = 1.0, notify=print):
    """잠금 파일을 배타적으로 잡는다(Windows msvcrt.locking LK_NBLCK 1바이트, POSIX fcntl.flock LOCK_EX|LOCK_NB).
    잡지 못하면 notify("[build] 다른 워크트리 빌드가 끝나기를 기다립니다")를 한 번 부르고 poll_sec마다 재시도.
    timeout_sec을 넘으면 TimeoutError. 빠져나갈 때 반드시 푼다."""

@contextlib.contextmanager
def mapped_fixed_root(repo_root: Path, *, platform: str = sys.platform,
                      env: Mapping[str, str] = os.environ, run=subprocess.run):
    """고정 루트를 repo_root에 연결하고 그 루트(Path)를 내준다. 잠금은 호출하는 쪽이 이미 잡았다고 가정.
    Windows: run(["subst"], capture_output=True, text=True)로 목록을 읽어, 드라이브가 이미 있으면
    run(["subst", drive, "/D"]), 드라이브가 목록에 없는데 Path(drive + "\\").exists()면 FixedPathUnavailable.
    그다음 run(["subst", drive, str(repo_root)], check=True). finally에서 run(["subst", drive, "/D"]).
    POSIX: 링크 부모 폴더를 만들고, 임시 이름으로 os.symlink(repo_root, tmp) 뒤 os.replace(tmp, link). 해제하지 않음."""
```

테스트(`run`은 가짜 함수로 호출 인자를 기록, `Path.exists`는 `mock.patch`):
- 연결 없음 → `subst T: <repo>` 연결, 끝나면 `subst T: /D` 호출
- 이미 연결된 찌꺼기 → 먼저 `/D`, 그다음 연결
- 드라이브가 실제 드라이브 → `FixedPathUnavailable`, `subst` 연결 호출 없음
- 블록 안에서 예외가 나도 `/D` 호출
- POSIX(`platform="linux"`, 임시 폴더를 `Path.home`으로 patch): 링크가 repo를 가리킴, 두 번 불러도 성공
- 잠금: 같은 잠금 파일을 두 번째로 잡으려 하면(첫 번째를 잡은 채로, `timeout_sec=0.2, poll_sec=0.05`) `TimeoutError`, notify가 한 번 불림. 첫 번째를 푼 뒤에는 잡힘

### 작업 3: 새 워크트리 시드와 main 연결

**Files:**
- Modify: `tools/build.py`
- Test: `tools/test_build.py`

```python
def main_checkout_root(repo_root: Path, run=subprocess.run) -> Path | None:
    """git rev-parse --path-format=absolute --git-common-dir의 부모. 실패하면 None."""

def built_with_fixed_root(build_dir: Path, root: Path) -> bool:
    """build_dir/build.options.json의 sketchLocation이 str(root)로 시작하면 True(대소문자 무시, 구분자는 \\와 / 모두 허용).
    파일이 없거나 JSON이 깨졌으면 False."""

def seed_build_dir(build_dir: Path, source_build_dir: Path, sketch_name: str,
                   module_names: Sequence[str]) -> bool:
    """build_dir가 이미 있으면 아무것도 안 하고 False. source_build_dir를 복사한 뒤 설계 5번의 세 가지를 지우고 True."""
```

`main`의 compile 흐름(`compile_sketch`이고 `--dry-run`이 아닐 때):
1. `fixed_path_enabled()`가 False면 지금 방식 그대로.
2. `fixed_path_lock(...)`을 잡는다(`TimeoutError`면 안내 출력 후 1 반환).
3. `mapped_fixed_root(REPO_ROOT)` 안에서: `root`를 받는다. 빌드 폴더가 없고 `main_checkout_root`가 `REPO_ROOT`와 다르며 메인 빌드 폴더가 `built_with_fixed_root(..., root)`면 `seed_build_dir`. 그다음 고정 경로 compile 명령을 실행한다.
4. `FixedPathUnavailable`이면 경고(`[build] <드라이브>를 다른 용도로 쓰고 있어 고정 경로 없이 빌드합니다`)를 출력하고 지금 방식으로 compile.
5. upload는 잠금·연결 밖에서 실제 경로로 실행한다. `[build] compile N초` / `[build] upload N초` 출력은 지금처럼 유지한다.

`--dry-run`이면 연결·잠금·시드 없이 `fixed_root()` 기준 compile 명령(고정 경로가 꺼져 있으면 실제 경로)과 upload 명령을 출력한다.

테스트:
- `seed_build_dir`: 임시 폴더에 가짜 빌드 폴더(`sketch/a.o`, `libraries/core/x.o`, `libraries/lvgl/y.o`, `core/z.o`, `thermostat_main.ino.bin`, `thermostat_main.ino.elf`, `build.options.json`)를 만들고 복사 → `sketch/`, `libraries/core/`, `.ino.*`가 없어지고 `libraries/lvgl/y.o`, `core/z.o`, `build.options.json`은 남음. 대상이 이미 있으면 False이고 아무것도 안 바뀜
- `built_with_fixed_root`: 실제 표본 두 가지(T: → True, D: → False), 파일 없음 False, 깨진 JSON False
- `main_checkout_root`: 가짜 run이 `C:/x/repo/.git\n`을 내면 `C:/x/repo`, 실패 코드면 None
- `main` 흐름: `subprocess.run`·`mapped_fixed_root`·`fixed_path_lock`을 patch해서 compile 명령의 `--build-path`·`--library`·스케치 인자가 고정 루트 기준이고 upload 명령의 `--input-dir`는 실제 경로임을 확인. `THERMOSTAT_FIXED_PATH=0`이면 compile 명령이 실제 경로. `FixedPathUnavailable`이면 실제 경로로 compile하고 경고 출력
- 기존 `MainUploadOnlyTests` 등 `tools/test_build.py`의 테스트가 그대로 통과해야 한다(`--upload-only`는 compile을 안 하므로 잠금·연결을 쓰지 않는다)

## 테스트 명령

`python tools/test_build.py` 그리고 `python tools/test_host.py`(바뀌지 않아야 함)

## Review Focus

1. 연결 해제(`subst /D`)와 잠금 해제가 실패·예외·Ctrl+C에서도 반드시 일어나는가
2. 시드 후 우리 소스로 만든 오브젝트(`sketch/`, modules.txt의 라이브러리 폴더)와 `.ino.*` 산출물을 빠짐없이 지우는가. 서드파티 라이브러리와 Arduino 코어 오브젝트는 남기는가
3. upload 명령이 고정 루트가 아니라 실제 경로를 쓰는가(연결 해제 뒤 실행되므로)
4. 드라이브 글자가 실제 드라이브일 때 그 드라이브를 `subst /D`로 건드리지 않는가
5. `--dry-run`이 연결·잠금·복사 같은 부작용을 일으키지 않는가

## 완료 기준

- 위 테스트 통과
- (Claude 직접 측정) 새 워크트리 첫 빌드 시간 전후 비교 표, 목표 10분 이내. 두 워크트리 동시 빌드가 서로 깨지지 않음. 리스크하기 때문에 Claude가 최종 검증 진행(실제 빌드·업로드는 DeepSeek 워크트리에서 할 수 없음)
