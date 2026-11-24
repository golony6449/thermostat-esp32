# 빌드·테스트 환경

이 문서는 thermostat-esp32 펌웨어를 빌드·업로드·모니터링하고, PC에서 `core/`
로직을 단위 테스트하는 방법을 정리한다. 대상 보드는 Waveshare
ESP32-S3-Touch-LCD-4.3B(ESP32-S3-WROOM-1-N16R8, 16MB 플래시, 8MB OPI PSRAM)다.

## 요약

| 항목 | 값 |
| --- | --- |
| 빌드 도구 | Arduino CLI (Arduino IDE 2.x 내장 `arduino-cli`) |
| ESP32 코어 | `esp32:esp32@3.3.12` (내장 `Matter` 라이브러리 포함) |
| 타깃 FQBN | `esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB` |
| 격리 설정 (Windows) | `%LOCALAPPDATA%\thermostat-esp32\arduino-cli.yaml` |
| 격리 설정 (Linux/macOS) | `~/.local/share/thermostat-esp32/arduino-cli.yaml` |
| 공유 라이브러리 | `~/Documents/Arduino/libraries` (Waveshare 4.3B 오프라인 라이브러리) |
| 호스트 C++ | g++ (C++17) — Windows는 WinGet MinGW-W64 UCRT 사용 |

## Arduino CLI vs PlatformIO (pioarduino)

| 항목 | Arduino CLI (선정) | PlatformIO / pioarduino |
| --- | --- | --- |
| ESP32 코어 | `esp32:esp32@3.3.12` 공식 코어를 그대로 사용 | pioarduino 포크 릴리스에 의존. 내장 `Matter`가 포함된 버전을 별도로 추적해야 함 |
| 기존 오프라인 라이브러리 | `Documents/Arduino/libraries`의 `ESP32_Display_Panel`, `ESP32_IO_Expander`, `esp-lib-utils`, `lvgl 8.4.0`을 그대로 재사용 | `lib_deps`로 옮기거나 심볼릭 링크 등 별도 관리 필요 |
| 다른 프로젝트 코어와 격리 | `--config-file` 하나로 완전히 분리 (`esp32-rtos-lab`의 전역 `Arduino15` 3.0.7을 건드리지 않음) | 프로젝트별 `.pio`로 자동 격리되지만 포크 코어를 써야 함 |
| 병렬 일감 충돌 | 데모별 `demos/<name>/` + `modules.txt`만 수정. 공용 빌드 설정 파일 없음 | 공용 `platformio.ini`를 여러 일감이 수정하면 머지 충돌 위험 |
| 명령 일관성 | `compile`, `upload`, `monitor`를 단일 CLI로 처리 | `pio run`, `pio run -t upload`, `pio device monitor`로 분산 |

### Arduino CLI를 고른 이유

1. Arduino IDE 2.x에 내장된 `arduino-cli`와 기존 `Documents/Arduino/libraries/`의
   Waveshare 4.3B 오프라인 라이브러리(`ESP32_Display_Panel`, `ESP32_IO_Expander`,
   `esp-lib-utils`, `lvgl 8.4.0`)를 그대로 재사용할 수 있다.
2. `esp32-rtos-lab`이 쓰는 전역 `Arduino15`(코어 `3.0.7`)에는 내장 `Matter`가 없다.
   이 프로젝트는 `3.3.12`가 필요한데, 프로젝트 전용 격리
   `arduino-cli.yaml`로 두 코어를 충돌 없이 분리할 수 있다.
3. 데모별 독립 폴더(`demos/<name>/`)와 선택 파일 `modules.txt`로 병렬 일감이
   공용 빌드 설정 파일(`platformio.ini` 등)을 건드리지 않아 머지 충돌이 없다.
4. `compile`, `upload`, `monitor`를 터미널 단일 CLI로 일관되게 처리할 수 있고,
   스크립트(`tools/build.py`)에서 인자 리스트로 안전하게 호출할 수 있다.

## 디렉터리 구조

```text
src/
  config/            pins.h, secrets.h.example (secrets.h는 git 제외)
  core/              순수 C++17 제어 로직 (Arduino 의존성 금지, library.properties 포함)
  matter/            Matter 엔드포인트 (예정)
  sensor/            로컬·원격 센서 (예정)
  storage/           NVS 설정 저장 (예정)
  net/               Wi-Fi, NTP, 홈서버 HTTP (예정)
  ui/waveshare43b/   보드별 UI (예정)
demos/
  <name>/<name>.ino  데모 진입점
  <name>/modules.txt 선택: 이 데모가 링크할 모듈 경로(줄바꿈 구분, # 주석 허용)
test/
  test_*.cpp         호스트 단위 테스트 (파일마다 독립 main)
  test_harness.h     의존성 없는 경량 테스트 틀
tools/
  build.py           컴파일·업로드·시리얼 모니터
  test_host.py       호스트 테스트 빌드·실행
tests/
  test_tools.py      tools/*.py 자체 단위 테스트
```

`demos/<name>/modules.txt` 예:

```text
src/core
```

`tools/build.py`는 나열된 모듈을 arduino-cli `--library`로 넘기고,
`-I<repo>/src`를 `compiler.cpp.extra_flags`/`compiler.c.extra_flags`에 추가한다.
따라서 스케치는 `#include "config/pins.h"`와 `#include "thermostat_core.h"`를 모두
사용할 수 있다.

> **`core_version.h`를 스케치에서 직접 include하지 않는다.** ESP32 코어 3.3.12는
> 자체 `cores/esp32/core_version.h`를 제공한다. 스케치가 `core_version.h`를
> include하면 코어 헤더가 먼저 해석되어 Arduino CLI가 `thermostat_core`
> 라이브러리를 "사용되지 않음"으로 판단하고 링크에서 뺀다(undefined reference).
> `src/core/thermostat_core.h`가 이 충돌을 피하는 공용 진입 헤더다.

## 16MB 플래시와 OTA 파티션

`PartitionScheme=app3M_fat9M_16MB` 구성:

| 파티션 | 오프셋 | 크기 | 용도 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 0x5000 (20KB) | 설정 저장 |
| otadata | 0xE000 | 0x2000 (8KB) | OTA 슬롯 선택 |
| app0 | 0x10000 | 0x300000 (3MB) | 애플리케이션 슬롯 0 |
| app1 | 0x310000 | 0x300000 (3MB) | 애플리케이션 슬롯 1 (OTA) |
| ffat | 0x610000 | 0x9E0000 (9.875MB) | FATFS (UI 리소스 등) |
| coredump | 0xFF0000 | 0x10000 (64KB) | 크래시 덤프 |

FQBN 옵션:

- `CDCOnBoot=cdc`: Waveshare 4.3B의 UART0이 RS485 단자에 물려 있어, USB CDC가
  꺼지면 시리얼 로그가 보이지 않는다. 반드시 켠다.
- `USBMode=hwcdc`: USB-Serial/JTAG 하드웨어 CDC 사용.
- `FlashSize=16M`: ESP32-S3-WROOM-1-N16R8의 16MB 플래시.
- `PSRAM=opi`: 8MB OPI PSRAM. 800×480 LVGL 프레임버퍼와 Matter 데이터 모델에 필요.
- `PartitionScheme=app3M_fat9M_16MB`: 3MB×2 OTA 슬롯 + 9.9MB FATFS.
  `tools/build.py --partition <스킴>`으로 바꿀 수 있다.

## 격리된 arduino-cli 설정

`tools/build.py`는 설정 파일이 없으면 자동으로 만든다. 기존 파일은 절대
덮어쓰지 않는다. 생성되는 내용의 핵심:

```yaml
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
directories:
  data: <config 폴더>/arduino15
  downloads: <config 폴더>/arduino15/staging
  user: ~/Documents/Arduino
```

`directories.data`가 전역 `%LOCALAPPDATA%\Arduino15`가 아니라 설정 파일 옆
`arduino15/`를 가리키므로 `esp32-rtos-lab`의 코어 `3.0.7`을 건드리지 않는다.
`directories.user`는 기존 스케치북을 공유해 Waveshare 오프라인 라이브러리를
재사용한다.

### 최초 코어 설치

설정 파일을 먼저 만든 뒤(예: `python tools/build.py demos/00_smoke --dry-run`)
다음을 실행한다.

Windows (PowerShell):

```powershell
$cli = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$cfg = "$env:LOCALAPPDATA\thermostat-esp32\arduino-cli.yaml"
& $cli --config-file $cfg core update-index
& $cli --config-file $cfg core install esp32:esp32@3.3.12
& $cli --config-file $cfg core list
```

Linux/macOS:

```bash
cfg="$HOME/.local/share/thermostat-esp32/arduino-cli.yaml"
arduino-cli --config-file "$cfg" core update-index
arduino-cli --config-file "$cfg" core install esp32:esp32@3.3.12
arduino-cli --config-file "$cfg" core list
```

설치 확인: `esp32:esp32 3.3.12`가 보이고, 해당 코어의
`libraries/Matter/src/MatterEndpoints/MatterThermostat.h`가 존재해야 한다.

## 빌드·업로드·시리얼 모니터

```bash
# 컴파일만
python tools/build.py demos/00_smoke

# 컴파일 + 업로드
python tools/build.py demos/00_smoke --upload --port COM9

# 시리얼 모니터만
python tools/build.py --monitor --port COM9

# 컴파일 → 업로드 → 모니터
python tools/build.py demos/00_smoke --upload --monitor --port COM9

# 실행 없이 명령만 확인
python tools/build.py demos/00_smoke --upload --port COM9 --dry-run
```

- 스케치 경로는 저장소 루트 기준 상대 경로 또는 절대 경로다.
- 빌드 캐시는 `.build/<스케치 폴더 이름>/`에 생긴다.
- 포트는 `--port` → `THERMOSTAT_PORT` 환경변수 → 플랫폼 기본값
  (Windows `COM9`, 그 외 `/dev/ttyUSB0`) 순으로 정한다.
- 업로드가 포트 점유 오류로 실패하면 한국어 안내를 출력한다.
- `arduino-cli`는 `ARDUINO_CLI` 환경변수 → `PATH` → Arduino IDE 내장 경로 순으로
  찾는다.

## 호스트 단위 테스트

`src/core/`는 Arduino 의존성이 없는 순수 C++17이므로 PC에서 바로 컴파일한다.

```bash
python tools/test_host.py                # test/test_*.cpp 전부
python tools/test_host.py --filter core  # 파일 이름 부분 문자열 필터
python tools/test_host.py --dry-run      # 컴파일 명령만 출력
```

- 테스트 파일마다 별도 실행 파일을 만들어 `main()` 충돌이 없다.
- 링크할 소스는 테스트 파일 상단 20줄 안의 주석으로 선언한다.
  생략하면 `src/core/*.cpp`를 링크한다.

```cpp
// LINK: src/core/*.cpp
```

- 컴파일 플래그: `-std=c++17 -Wall -Wextra -Werror`, include 경로
  `src/`, `src/core/`, `test/`.
- 실행 시 컴파일러 폴더를 `PATH` 맨 앞에 넣어 Windows MinGW UCRT DLL 로드
  오류를 막는다.

## Python 도구 테스트

보드·컴파일러·arduino-cli 없이 실행된다.

```bash
python -m unittest discover -s tests -v
```

## 문제 해결

- **시리얼 로그가 안 보임**: FQBN에 `CDCOnBoot=cdc`가 있는지 확인한다. UART0은
  RS485에 연결되어 있다.
- **`arduino-cli: core esp32:esp32@3.3.12 not found`**: 격리 설정의 `data`
  디렉터리에 코어가 설치되지 않은 것이다. 위 "최초 코어 설치"를 실행한다.
- **다른 프로젝트의 코어가 바뀐 것 같음**: `arduino-cli.yaml`의
  `directories.data`가 `thermostat-esp32/arduino15`를 가리키는지 확인한다.
  전역 `Arduino15`를 가리키면 안 된다.
- **포트 점유 오류**: Arduino IDE 시리얼 모니터 등 포트를 잡은 프로그램을
  닫는다. Linux는 `dialout` 그룹 권한도 확인한다.
- **`C++ 컴파일러를 찾지 못했습니다`**: `CXX` 환경변수로 g++/clang++ 경로를
  지정하거나 PATH에 추가한다. Windows는 WinGet MinGW-W64 UCRT 패키지를 찾는다.
- **`undefined reference to thermostat::core::...`**: 스케치가
  `thermostat_core.h` 대신 `core_version.h`를 직접 include했을 가능성이 크다.
  ESP32 코어의 동명 헤더가 먼저 잡혀 라이브러리가 링크되지 않은 것이다.
- **첫 컴파일이 느림**: ESP32 코어 전체를 빌드하므로 수 분 걸릴 수 있다.
  이후에는 `.build/` 캐시로 빨라진다.

## 자동 검증 범위

자동화된 검증(`--check`)은 컴파일, 호스트 테스트, Python 도구 테스트까지만
실행하고 실제 보드 업로드(COM9)는 하지 않는다. 업로드와 시리얼 로그 확인은
PR 검증 통과 후 실행 담당(PL)이 수행한다.
