# SmartThings DIY 온도조절기 구현 계획 — Wave 1 (SCRUM-232: 빌드·테스트 뼈대)

> 대상 일감: `SCRUM-232` (`plan-smartthings-thermostat`, 에픽 `SCRUM-230`)
> 기준 문서: `CLAUDE.md` (3, 5, 6, 15절)
> 선행: 없음 (이 뼈대가 머지된 뒤 Wave 2 일감 5개 `SCRUM-233`~`SCRUM-237`이 병렬 착수한다)

---

## Global Constraints

- **보드·코어 격리**: Arduino CLI(`arduino-cli`)를 사용한다. 이 프로젝트는 내장 `Matter` 라이브러리가 포함된 `esp32:esp32@3.3.12` 코어가 필요하므로, 기존 `esp32-rtos-lab`의 전역 `Arduino15`(`3.0.7`)를 건드리지 않도록 프로젝트 전용 격리 설정 파일(`%LOCALAPPDATA%/thermostat-esp32/arduino-cli.yaml`, Linux/macOS는 `~/.local/share/thermostat-esp32/arduino-cli.yaml`)을 사용한다. `directories.user`는 기존 `~/Documents/Arduino`를 공유해 Waveshare 4.3B 오프라인 라이브러리(`ESP32_Display_Panel`, `ESP32_IO_Expander`, `esp-lib-utils`, `lvgl`)를 재사용한다.
- **타깃 보드 FQBN**: `esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`
  - `CDCOnBoot=cdc` 필수(Waveshare 4.3B의 UART0은 RS485에 물려 있어 USB CDC가 꺼지면 시리얼 출력이 안 보임).
  - `PSRAM=opi` 필수(8MB OPI PSRAM, 800×480 LVGL 프레임버퍼 및 Matter 데이터 모델용).
  - `PartitionScheme=app3M_fat9M_16MB`: 16MB 플래시, 3MB×2 OTA(`app0`/`app1`), 9.9MB FATFS, NVS, otadata, coredump. `--partition` 옵션으로 변경 가능.
- **병렬 충돌 방지(가장 중요)**:
  - 모듈 폴더(`src/core`, `src/matter`, `src/sensor`, `src/storage`, `src/net`, `src/ui/waveshare43b`)와 데모 진입점(`demos/<name>/<name>.ino`)을 분리한다.
  - 각 데모 폴더는 필요 모듈을 선언하는 선택 파일 `demos/<name>/modules.txt`(줄바꿈 구분 모듈 상대 경로, 예: `src/core`)를 둘 수 있다. `tools/build.py`가 해당 모듈들을 `--library`로 넘기고 `-I<repo>/src`를 `compiler.cpp.extra_flags`로 추가한다. 병렬 일감들이 공용 `main`이나 공용 빌드 설정을 수정하지 않는다.
  - 호스트 단위 테스트(`test/test_*.cpp`)는 파일마다 독립된 `main()`을 갖고, `tools/test_host.py`가 테스트 파일별로 별도 실행 파일을 빌드·실행한다. 필요한 소스 파일은 테스트 파일 상단 주석 `// LINK: src/core/*.cpp` (생략 시 `src/core/*.cpp`)으로 선언한다.
- **순수 C++ 규칙**: `src/core/`는 `Arduino.h`, `millis()` 등 하드웨어/프레임워크 의존성이 전혀 없는 순수 C++17이어야 하며 PC 호스트 `g++`와 ESP32 `arduino-cli` 양쪽에서 동일하게 컴파일되어야 한다.
- **Python 스크립트 규칙**: 표준 라이브러리만 사용해 `tools/build.py`, `tools/test_host.py`를 작성한다(`subprocess`는 `shell=False`, 인자 리스트 전달). Windows와 Linux 양쪽에서 동작해야 한다.
- **보안·비밀값**: 실제 Wi-Fi 비밀번호나 API 키는 절대 커밋하지 않는다. `src/config/secrets.h.example`만 커밋하고 `src/config/secrets.h`는 `.gitignore`에 등록한다.
- **실기 업로드 주의**: DeepSeek 검증(`--check`)은 결정론적인 컴파일·호스트 테스트·Python 도구 단위 테스트만 실행한다(COM 포트 점유 방지). 실제 보드(`COM9`) 업로드 및 시리얼 로그 확인은 PR 검증 통과 후 실행 PL이 수행한다.

---

## 실제 표본

이 PC에서 실측한 환경 정보:

```text
arduino-cli: C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe (v1.6.0)
격리 config: C:\Users\seongheum\AppData\Local\thermostat-esp32\arduino-cli.yaml
설치된 코어: esp32:esp32 3.3.12 (내장 libraries/Matter/src/MatterEndpoints/MatterThermostat.h 포함)
파티션 스킴: app3M_fat9M_16MB (nvs 0x5000, otadata 0x2000, app0 0x300000, app1 0x300000, ffat 0x9E0000, coredump 0x10000)
호스트 g++:  %LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin\g++.exe (MinGW-W64 GCC 16.2.0)
보드 포트:   COM9 (USB JTAG/serial debug unit, PID 0x1001)
```

---

### 작업 1: 빌드·테스트 뼈대 및 모듈·데모 구조 구축 (`SCRUM-232`)

**Files:**
- Create: `.gitignore`
- Create: `docs/build-environment.md`
- Create: `src/config/pins.h`
- Create: `src/config/secrets.h.example`
- Create: `src/core/library.properties`
- Create: `src/core/core_version.h`
- Create: `src/core/core_version.cpp`
- Create: `src/matter/.gitkeep`
- Create: `src/sensor/.gitkeep`
- Create: `src/storage/.gitkeep`
- Create: `src/net/.gitkeep`
- Create: `src/ui/waveshare43b/.gitkeep`
- Create: `demos/00_smoke/00_smoke.ino`
- Create: `demos/00_smoke/modules.txt`
- Create: `test/test_harness.h`
- Create: `test/test_core_smoke.cpp`
- Create: `tools/build.py`
- Create: `tools/test_host.py`
- Test: `tests/test_tools.py`

**요구사항 및 세부 명세:**

1. **`.gitignore`**:
   - 빌드 산출물: `.build/`, `*.bin`, `*.elf`
   - 비밀 설정: `src/config/secrets.h`
   - Python/로컬: `__pycache__/`, `*.pyc`, `.venv/`, `.pytest_cache/`, `.claude/settings.local.json`, `.handoff/`, `.ds-tmp/`

2. **`docs/build-environment.md`**:
   - Arduino CLI vs PlatformIO(`pioarduino`) 비교 및 **Arduino CLI 선정 이유** 기록:
     1. Arduino IDE 2.x 내장 `arduino-cli`와 기존 `Documents/Arduino/libraries/`의 Waveshare 4.3B 오프라인 라이브러리(`ESP32_Display_Panel`, `ESP32_IO_Expander`, `esp-lib-utils`, `lvgl 8.4.0`)를 재사용 가능.
     2. `esp32-rtos-lab`의 `3.0.7` 코어에는 내장 `Matter`가 없어 `3.3.12`가 필요한데, 프로젝트 전용 격리 config(`%LOCALAPPDATA%/thermostat-esp32/arduino-cli.yaml`)로 기존 `3.0.7`을 건드리지 않고 분리 가능.
     3. 데모별 독립 폴더(`demos/<name>/`) + `modules.txt`로 병렬 일감이 공용 빌드 설정 파일(`platformio.ini` 등)을 건드리지 않아 머지 충돌 없음.
     4. `compile`, `upload`, `monitor`를 터미널 단일 CLI로 일관되게 처리 가능.
   - 16MB + OTA 파티션(`app3M_fat9M_16MB`) 구성과 Waveshare 4.3B FQBN 옵션(`CDCOnBoot=cdc`, `USBMode=hwcdc`, `FlashSize=16M`, `PSRAM=opi`) 설명.
   - 격리 코어 최초 설치 명령, 빌드/업로드/시리얼 모니터 명령, 호스트 테스트 실행 명령 예시 포함.

3. **공용 설정 (`src/config/`)**:
   - `src/config/pins.h`:
     - `#pragma once`
     - `namespace thermostat::pins`: `constexpr int kI2cSda = 8;`, `constexpr int kI2cScl = 9;`, `constexpr int kTouchInt = 4;`, `constexpr uint32_t kSerialBaud = 115200;`
   - `src/config/secrets.h.example`:
     - `#pragma once`
     - `WIFI_SSID`, `WIFI_PASSWORD`, `HOMESERVER_BASE_URL`, `HOMESERVER_API_KEY` 자리표시자 매크로(실제 비밀값 없음, 복사 안내 주석 포함).

4. **모듈 폴더 및 `src/core/` 스모크 모듈**:
   - `src/matter/.gitkeep`, `src/sensor/.gitkeep`, `src/storage/.gitkeep`, `src/net/.gitkeep`, `src/ui/waveshare43b/.gitkeep` 생성.
   - `src/core/library.properties`: 표준 Arduino 라이브러리 메타데이터 (`name=thermostat_core`, `version=0.1.0`, `author=thermostat-esp32`, `maintainer=thermostat-esp32`, `sentence=Pure C++ core control logic`, `paragraph=Hardware-independent thermostat core logic`, `category=Other`, `url=https://github.com/golony6449/thermostat-esp32`, `architectures=*`).
   - `src/core/core_version.h` & `src/core/core_version.cpp`:
     - 순수 C++17 (Arduino 헤더 금지).
     - `namespace thermostat::core`:
       - `const char* version_string();` → `"0.1.0"` 반환
       - `constexpr int kDefaultMinTempCenti = 1000;` (10.0℃ = 1000 centi-degC, Matter 표준 0.01℃ 단위와 일치)
       - `int clamp_setpoint_centi(int setpoint_centi, int min_centi = 500, int max_centi = 3000);` — 범위 [min_centi, max_centi]로 클램프하는 순수 함수 (호스트 테스트 + ESP32 스모크 스케치 양쪽에서 호출해 링크 검증).

5. **빈 펌웨어 스모크 데모 (`demos/00_smoke/`)**:
   - `demos/00_smoke/modules.txt`:
     ```text
     src/core
     ```
   - `demos/00_smoke/00_smoke.ino`:
     - `#include <Arduino.h>`
     - `#include "config/pins.h"`
     - `#include "core_version.h"`
     - `setup()`: `Serial.begin(thermostat::pins::kSerialBaud);`, USB CDC 연결 대기(최대 3000ms non-blocking 루프 `while (!Serial && millis() < 3000) { delay(10); }`), 부팅 배너 출력 (`[00_smoke] thermostat-esp32 boot core=0.1.0 flash_mb=... psram_bytes=...`).
     - `loop()`: 1초마다 `[00_smoke] tick=<n> uptime_ms=<ms> clamp_test=<val>` 출력 (`delay(1000)`).

6. **빌드·업로드·시리얼 모니터 도구 (`tools/build.py`)**:
   - `esp32-rtos-lab/tools/build.py`의 검증된 패턴을 확장한다.
   - 상수:
     - `BASE_FQBN = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PSRAM=opi"`
     - `DEFAULT_PARTITION = "app3M_fat9M_16MB"`
     - `DEFAULT_BAUD = 115200`
     - `ESP32_CORE_PACKAGE = "esp32:esp32@3.3.12"`
     - `ESP32_BOARD_MANAGER_URL = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"`
   - 주요 함수:
     - `make_fqbn(partition: str | None = DEFAULT_PARTITION) -> str`
     - `find_cli(env: Mapping[str, str] = os.environ) -> Path | None`: `ARDUINO_CLI` → `shutil.which("arduino-cli")` → Windows IDE 내장 경로.
     - `default_config_path(env: Mapping[str, str] = os.environ, platform: str = sys.platform) -> Path`: `THERMOSTAT_ARDUINO_CONFIG` → Windows `%LOCALAPPDATA%/thermostat-esp32/arduino-cli.yaml`, 기타 `~/.local/share/thermostat-esp32/arduino-cli.yaml`.
     - `ensure_config(config_path: Path, ...) -> Path`: 파일이 없으면 격리 `arduino-cli.yaml`을 생성한다(이미 있으면 덮어쓰지 않고 그대로 반환).
     - `read_demo_modules(sketch_dir: Path, repo_root: Path) -> list[Path]`: `sketch_dir / "modules.txt"`가 있으면 빈 줄·`#` 주석을 제외한 각 상대 경로를 `repo_root / line`으로 해석해 반환(존재하지 않는 경로면 `FileNotFoundError`). 파일이 없으면 빈 리스트 반환.
     - `build_commands(cli, sketch, build_dir, fqbn, config, libraries, include_dir, upload, port) -> list[list[str]]`:
       - `compile`: `--fqbn`, `--build-path`, 각 library마다 `--library <path>`, `--build-property compiler.cpp.extra_flags=-I<include_dir.as_posix()>`, `--build-property compiler.c.extra_flags=-I<include_dir.as_posix()>`, `<sketch>`.
       - `upload` (`--upload` 시): `upload -p <port> --fqbn <fqbn> --input-dir <build_dir> <sketch>`.
     - `build_monitor_command(cli, config, port, baud=DEFAULT_BAUD) -> list[str]`:
       - `[str(cli), "--config-file", str(config), "monitor", "-p", port, "-c", f"baudrate={baud}"]` (`config`가 None이면 `--config-file` 생략).
     - `port_busy_hint(output: str) -> str | None`: 포트 점유 오류 감지 시 한국어 안내.
   - CLI 인터페이스:
     - `python tools/build.py <sketch> [--upload] [--port COM9] [--partition app3M_fat9M_16MB] [--dry-run]`
     - `python tools/build.py --monitor [--port COM9] [--baud 115200] [--dry-run]`
     - `--sketch` 없이 `--monitor`만 주면 시리얼 모니터만 실행. `--sketch`와 `--upload`와 `--monitor`를 함께 주면 컴파일 → 업로드 → 모니터 순으로 실행.
     - 빌드 출력 캐시 위치: `REPO_ROOT / ".build" / sketch.name`.

7. **호스트 C++ 단위 테스트 틀 (`test/test_harness.h`, `test/test_core_smoke.cpp`, `tools/test_host.py`)**:
   - `test/test_harness.h`:
     - 외부 의존성 없는 경량 C++17 헤더.
     - `TEST_CASE(name)` 매크로로 테스트 함수를 자동 등록(`static` 등록자 + `std::vector` 레지스트리).
     - `EXPECT_TRUE(cond)`, `EXPECT_FALSE(cond)`, `EXPECT_EQ(expected, actual)`, `EXPECT_STREQ(expected, actual)`, `EXPECT_NEAR(expected, actual, tol)` 매크로.
     - `THERMOSTAT_TEST_MAIN()` 매크로가 `int main()`을 정의해 등록된 모든 테스트를 실행하고, 각 테스트 `PASS`/`FAIL`과 요약(`Passed: N, Failed: M`)을 출력하며 실패가 있으면 종료 코드 `1`, 모두 통과하면 `0`을 반환.
   - `test/test_core_smoke.cpp`:
     - 첫 줄 주석: `// LINK: src/core/*.cpp`
     - `version_string()`이 `"0.1.0"`인지, `clamp_setpoint_centi()`의 정상 범위·하한·상한·경계값(500, 3000) 테스트 포함.
   - `tools/test_host.py`:
     - `find_cxx(env: Mapping[str, str] = os.environ, platform: str = sys.platform) -> Path | None`:
       1. `env.get("CXX")` (파일 존재 확인)
       2. `shutil.which("g++")` 또는 `shutil.which("clang++")`
       3. Windows일 때 `%LOCALAPPDATA%/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.*/mingw64/bin/g++.exe` (glob으로 최신 정렬) 및 `%LOCALAPPDATA%/Microsoft/WinGet/Links/g++.exe`
     - `parse_link_globs(test_file: Path) -> list[str]`: 테스트 파일 앞 20줄에서 `// LINK: <패턴들>`을 파싱. 없으면 기본 `["src/core/*.cpp"]`.
     - `discover_tests(test_dir: Path, pattern: str | None = None) -> list[Path]`: `test_*.cpp` 정렬 반환(필터 지원).
     - `compile_and_run_test(cxx, test_file, repo_root, build_dir) -> int`:
       - `g++ -std=c++17 -Wall -Wextra -Werror -I<repo>/src -I<repo>/src/core -I<repo>/test <test_file> <linked_cpps...> -o <exe>`
       - 실행 시 `cxx.parent`를 `PATH` 맨 앞에 추가해 Windows MinGW UCRT/libstdc++ DLL 로드 오류 방지.
     - CLI: `python tools/test_host.py [--filter <부분문자열>] [--dry-run]`. 테스트가 0개면 실패(종료 코드 1). 모두 통과하면 요약 출력 후 종료 코드 0.

8. **Python 도구 단위 테스트 (`tests/test_tools.py`)**:
   - 파이썬 표준 라이브러리 `unittest` 기반(`python -m unittest discover -s tests -v`, 보드나 실제 `arduino-cli`/`g++` 없이도 동작하도록 `tempfile.TemporaryDirectory`, `unittest.mock.patch` 사용).
   - `tools/build.py` 테스트:
     - `make_fqbn` 기본 파티션(`app3M_fat9M_16MB`) 및 커스텀 파티션
     - `read_demo_modules` 정상 파싱(주석/빈줄 무시), 파일 없음 시 빈 리스트, 없는 모듈 경로 시 `FileNotFoundError`
     - `build_commands` compile-only, upload 포함, `--library`와 `-I` extra_flags 포함 여부
     - `build_monitor_command` baudrate 포맷
     - `ensure_config` 신규 생성 및 기존 파일 비덮어쓰기
     - `port_busy_hint` 감지
     - `main(["demos/00_smoke", "--dry-run"])` 및 `main(["--monitor", "--dry-run"])`
   - `tools/test_host.py` 테스트:
     - `parse_link_globs` 명시/기본값/복수 패턴
     - `discover_tests` 정렬 및 `--filter`
     - `find_cxx` 환경변수 `CXX` 우선순위
     - `main(["--dry-run"])`

---

## Review Focus

1. `tools/build.py`가 `esp32-rtos-lab`의 전역 Arduino15를 건드리지 않고 격리된 `thermostat-esp32/arduino-cli.yaml`을 기본으로 사용하며, `demos/<name>/modules.txt` 기반 `--library` 링크와 `app3M_fat9M_16MB` 파티션이 올바르게 조립되는지.
2. `tools/test_host.py`가 Windows(WinLibs MinGW fallback + DLL PATH 처리)와 Linux(`g++`) 양쪽에서 `shell=False`로 안전하게 동작하고, 테스트 파일별 독립 실행 파일을 만들어 병렬 일감 간 `main()` 충돌이 없는지.
3. `src/core/`에 Arduino/ESP32 의존성이 전혀 없고, `src/config/secrets.h`가 `.gitignore`에 포함되어 실제 비밀값이 커밋되지 않는지.
4. `demos/00_smoke`가 `esp32:esp32@3.3.12`로 실제 컴파일 성공하고 `python tools/test_host.py` 및 `python -m unittest discover -s tests -v`가 모두 통과하는지.
