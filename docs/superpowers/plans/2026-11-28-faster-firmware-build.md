# 펌웨어 빌드 시간 줄이기 계획 (SCRUM-330)

**목표:** `firmware/thermostat_main` 빌드·업로드 주기를 줄인다.

**배경(2026-11-27 실측, main 체크아웃, 아무것도 바꾸지 않은 빌드 399초):**

| 구간 | 시간 | 무엇 |
|---|---|---|
| 준비 | 14초 | 파티션·sdkconfig 복사 |
| 라이브러리 탐색 | 180초 | 스케치(`thermostat_main.ino.cpp`)를 전처리(`-E`)해 새 헤더를 찾을 때마다 다시 전처리. 라이브러리 소스는 "Using cached library dependencies"로 캐시를 쓴다 |
| 프로토타입 생성 | 62초 | 스케치를 다시 전처리 + ctags |
| 스케치 컴파일 | 67초 | 바뀐 것이 없어도 스케치를 매번 다시 컴파일 |
| 라이브러리·코어 | 22초 | 이전 오브젝트 사용 |
| 링크 | 44초 | |
| 이미지·merged.bin | 4초 | |

- 헤더 하나(`sensor_logic.h`)만 바꾼 빌드는 550초, 업로드 자체는 18~20초였다.
- 새 워크트리 첫 빌드는 약 55분(SCRUM-317). 빌드 폴더가 워크트리마다 새로 생겨 코어·LVGL·Matter 등을 모두 다시 컴파일한다.
- 결론: 시간의 절반 가까이(약 190초)가 **스케치 파일 하나**에 든다. 스케치가 Matter·LVGL·화면 드라이버 헤더를 직접 포함하고 있어서 전처리가 무겁고, arduino-cli는 스케치에는 캐시를 쓰지 않는다.

## Global Constraints

- 펌웨어 동작은 바꾸지 않는다. 코드를 옮기기만 한다(함수 본문, 상수, 로그 문구, 초기화 순서, 전역 객체 생성 순서 그대로).
- `<Wire.h>`를 include하지 않는다(ESP-IDF 레거시 I2C와 충돌).
- 비밀 값은 커밋하지 않는다. `config/secrets.h`가 있으면 쓰고 없으면 `config/secrets.h.example`을 쓰는 지금의 `__has_include` 분기를 그대로 옮긴다.
- 호스트 테스트(`python tools/test_host.py`)와 펌웨어 빌드(`PYTHONIOENCODING=utf-8 PYTHONUTF8=1 python tools/build.py firmware/thermostat_main`)가 통과해야 한다.
- 다른 데모 스케치(`demos/*`)는 바꾸지 않는다.

## 작업

### 작업 1: 스케치를 얇게(본문을 라이브러리로)

**Files:**
- Create: `src/firmware/library.properties`
- Create: `src/firmware/thermostat_firmware.h`
- Create: `src/firmware/thermostat_firmware.cpp`
- Modify: `firmware/thermostat_main/thermostat_main.ino`
- Modify: `firmware/thermostat_main/modules.txt`

1. 새 Arduino 라이브러리 `src/firmware`(`name=thermostat_firmware`, 다른 `src/*/library.properties` 형식 그대로).
2. `thermostat_firmware.h`:
   ```cpp
   #pragma once
   namespace thermostat::firmware {
   void setup();   // 기존 thermostat_main.ino의 setup() 본문
   void loop();    // 기존 thermostat_main.ino의 loop() 본문
   }
   ```
   이 헤더는 `<Arduino.h>`조차 include하지 않는다(스케치 전처리를 가볍게 하는 것이 목적).
3. `thermostat_firmware.cpp`: 지금 `thermostat_main.ino`의 머리 주석(스레딩·I2C·Wi-Fi 노트), include, 익명 namespace 안의 전역·함수 전부를 그대로 옮긴다. `setup()`/`loop()`는 `thermostat::firmware::setup()`/`loop()`로 정의한다.
   - `.ino`는 Arduino가 함수 프로토타입을 자동으로 만들어 주지만 `.cpp`는 아니다. 정의보다 먼저 쓰이는 함수가 있으면 익명 namespace 맨 앞에 전방 선언을 둔다. 함수 순서는 바꾸지 않는다.
   - `#include <Arduino.h>`를 맨 앞에 둔다.
4. `thermostat_main.ino`는 이것만 남긴다:
   ```cpp
   // SCRUM-330: 본문은 src/firmware/thermostat_firmware.cpp에 있다.
   // 스케치는 arduino-cli가 매 빌드마다 전처리·컴파일하므로 가볍게 둔다.
   #include "thermostat_firmware.h"
   void setup() { thermostat::firmware::setup(); }
   void loop() { thermostat::firmware::loop(); }
   ```
   `firmware/thermostat_main/esp_panel_board_supported_conf.h`는 그대로 둔다(화면 드라이버 라이브러리가 스케치 폴더에서 찾는다). 빌드 뒤 화면 설정이 그대로인지 보려면 빌드 로그에 경고가 늘지 않았는지 확인한다.
5. `modules.txt` 맨 끝에 `src/firmware`를 추가한다.

### 작업 2: build.py — 새 워크트리 빌드 폴더 미리 채우기, 업로드만 하기, 시간 출력

**Files:**
- Modify: `tools/build.py`
- Create: `tools/test_build.py`

1. 빌드 폴더 미리 채우기: `build_dir`(= `REPO_ROOT/.build/<스케치 이름>`)가 없을 때, 이 저장소가 git 워크트리이고 주 체크아웃에 같은 이름의 빌드 폴더가 있으면 그 폴더를 `shutil.copytree`로 복사하고 `[build] 빌드 폴더를 <원본>에서 복사했습니다(<초>초)`를 출력한다.
   - 주 체크아웃 경로는 `git -C <REPO_ROOT> worktree list --porcelain`의 **첫 번째** `worktree <경로>` 줄이다. 그 경로가 `REPO_ROOT`와 같으면(주 체크아웃 자신) 복사하지 않는다. git 명령이 실패하면 조용히 건너뛴다.
   - 순수 함수로 나눈다: `main_worktree_from_porcelain(text: str) -> Path | None`, `seed_source(build_dir: Path, repo_root: Path, main_root: Path | None) -> Path | None`(복사할 원본 또는 None: `build_dir`가 이미 있거나, `main_root`가 None이거나 `repo_root`와 같거나, 원본이 없으면 None).
   - 실측(2026-11-28): 259MB 복사 16초.
2. `--upload-only`: 컴파일 없이 업로드 명령만 실행한다(`--input-dir`는 기존과 같은 빌드 폴더). 빌드 폴더에 `<스케치 이름>.ino.bin`이 없으면 한국어 오류와 함께 2를 돌려준다. `--upload`와 함께 쓰면 `--upload-only`가 우선한다.
3. 각 명령이 끝나면 `[build] <compile|upload> <초>초`를 출력한다(`time.monotonic()`).
4. `tools/test_build.py`(표준 `unittest`, 외부 의존성 없음): `main_worktree_from_porcelain`(정상, 빈 문자열, `worktree` 줄 없음, Windows 경로), `seed_source`의 각 분기(임시 폴더로), `build_commands`가 `--upload-only`일 때 업로드 명령 하나만 만드는지(필요하면 `build_commands`에 `compile: bool = True` 인자 추가). `python tools/test_build.py`로 돈다.

## Review Focus

1. 작업 1에서 옮긴 코드가 원래와 같은가: 함수 본문·상수·로그 문구·전역 객체(특히 `g_board`, `g_ui`, `g_matter`)의 정의 순서가 바뀌지 않았는가. 빠진 함수나 `#if CONFIG_ENABLE_CHIPOBLE` 같은 조건 분기가 없는가.
2. 스케치(`thermostat_main.ino`)와 `thermostat_firmware.h`가 무거운 헤더를 include하지 않는가.
3. `seed_source`가 주 체크아웃 자신에서는 복사하지 않고, 이미 있는 빌드 폴더를 덮어쓰지 않는가.
4. `--upload-only`가 컴파일을 건너뛰고, 빌드 결과가 없으면 업로드하지 않는가.
