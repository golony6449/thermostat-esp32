# SCRUM-338 펌웨어 본문을 기능별 파일로 나눠 본문 수정 빌드 줄이기

## 목적

`src/firmware/thermostat_firmware.cpp`(811줄) 한 파일이 Matter·LVGL·화면 드라이버 헤더를 모두 포함한다. 그래서 본문 어디를 고쳐도 빌드가 약 304초 걸린다(SCRUM-330 실측). Matter·화면과 상관없는 부분(네트워크·시각·센서 조회, 시리얼 진단 명령)을 무거운 헤더가 없는 파일로 떼어, 그 부분을 고칠 때의 빌드를 줄인다. **코드를 옮기기만 하고 동작은 바꾸지 않는다.**

## Global Constraints

- 옮긴 함수의 본문(로직, 로그 문구, 상수 값)은 바꾸지 않는다. 바꾸는 것은 위치, 링크 범위(익명 namespace → 내부 namespace), 그리고 아래 "Matter 연결 함수"로 바꾸는 호출뿐이다.
- 새 파일(`firmware_internal.h`, `firmware_net.cpp`, `firmware_serial.cpp`)은 다음을 **직접이든 간접이든 포함하지 않는다**: `<Matter.h>`, `thermostat_matter.h`, `<lvgl.h>`, `<esp_display_panel.hpp>`, `lvgl_port.h`, `thermostat_ui.h`. 포함해도 되는 것: `<Arduino.h>`, `<WiFi.h>`, `<esp_sntp.h>`, `<cstdio>` 등 표준 헤더, `thermostat_core.h`, `thermostat_app.h`, `thermostat_net.h`, `thermostat_storage.h`, `sensor_logic.h`, `sht40_driver.h`, `config/pins.h`, secrets 헤더.
- `<Wire.h>`는 어디에도 넣지 않는다(파일 머리 주석의 I2C 충돌 규칙).
- 같은 라이브러리(`src/firmware`) 안의 파일이라 `modules.txt`와 `library.properties`는 바꾸지 않는다.
- 전역 객체의 **정의**는 지금처럼 `thermostat_firmware.cpp`에 둔다(정의가 한 파일에 있으면 정적 초기화 순서가 지금과 같다). 다른 파일은 `extern` 선언만 쓴다.
- 이름 충돌을 막기 위해, 지금 익명 namespace에 있는 것 중 다른 파일과 나눠 써야 하는 것만 `namespace thermostat::firmware::internal`로 옮긴다. 한 파일에서만 쓰는 것은 그 파일의 익명 namespace에 둔다.

## 설계

### 나눌 범위 (현재 줄 번호, main `ed80718` 기준)

| 새 파일 | 옮길 것 |
|---|---|
| `firmware_net.cpp` | `homeserver_api_key`(280), `configure_home_server`(286), `poll_network_and_time`(290), `poll_home_server`(326), `poll_home_time`(347), `poll_sht40`(400), `run_control_step`(423). secrets 헤더 포함(`#if __has_include("config/secrets.h")` 블록 그대로). 이 함수들만 쓰는 상수(`kHomeServerPollIntervalMs`, `kRtcSyncIntervalMs`, `kSaneEpochSec`, `kSensorPollIntervalMs`, `kStepIntervalMs`)도 함께 옮긴다 |
| `firmware_serial.cpp` | `g_line`, `g_line_len`, `parse_float_arg`(444), `celsius_arg_to_centi`(457), `print_help`(467), `print_status`(483), `handle_command`(523), `poll_serial`(627) |
| `thermostat_firmware.cpp`(남김) | 전역 정의, Matter·UI 콜백, `drain_pending_events`, `apply_sync_actions`, `print_pairing_codes`, `poll_boot_button`, `init_matter`, `init_display`, `init_board_io`, `setup`, `loop` |

`secrets.h`의 값이 `thermostat_firmware.cpp`에서도 쓰이면(예: `!CONFIG_ENABLE_CHIPOBLE` 쪽 Wi-Fi 접속) 두 파일 모두 같은 `#if __has_include` 블록을 둔다. 매크로 정의만 있는 헤더라 두 번 포함해도 된다.

### 내부 헤더 `firmware_internal.h`

```cpp
#pragma once
// Shared state between the thermostat_firmware library sources.
// Must stay free of Matter, LVGL and display-panel headers (see plan).
#include <cstdint>
#include "thermostat_app.h"
#include "thermostat_net.h"
#include "thermostat_storage.h"
#include "sht40_driver.h"

namespace thermostat::firmware::internal {

// Defined in thermostat_firmware.cpp.
extern thermostat::app::ThermostatApp g_app;
extern thermostat::storage::NvsSettingsStore g_store;
extern thermostat::sensor::Sht40Driver g_sht40;
extern thermostat::net::ClockRtc g_clock;
extern thermostat::net::HomeServerClient g_home_server;
extern bool g_sht40_ok;
extern bool g_rtc_ok;
extern bool g_ntp_started;
extern uint64_t g_ntp_started_ms;
extern bool g_ntp_synced;
extern bool g_home_time_applied;
extern bool g_home_time_attempted;
extern uint64_t g_home_time_last_attempt_ms;

// Matter glue implemented in thermostat_firmware.cpp so the other sources
// never include <Matter.h>.
bool matter_is_commissioned();   // Matter.isDeviceCommissioned()
void matter_decommission();      // Matter.decommission()
void print_pairing_codes();      // existing function, unchanged body

// firmware_net.cpp
void configure_home_server();
void poll_network_and_time(uint64_t now_ms);
void poll_home_server(uint64_t now_ms);
void poll_home_time(uint64_t now_ms);
void poll_sht40(uint64_t now_ms);
void run_control_step(uint64_t now_ms, bool events_drained);

// firmware_serial.cpp
void poll_serial();

}  // namespace thermostat::firmware::internal
```

- 위 전역들의 정의를 `thermostat_firmware.cpp`의 익명 namespace에서 `namespace thermostat::firmware::internal { ... }`로 옮긴다(타입·초기값 그대로). `g_ui`, `g_matter`, `g_board`, `g_boot_button`, `g_ui_ready`, 이벤트 구조체와 mux는 본 파일 익명 namespace에 그대로 둔다.
- 실제 헤더 경로와 `using` 선언은 지금 `thermostat_firmware.cpp`의 include 줄(38~47)과 `using`(69~85)을 따른다. 위 코드의 타입 이름이 실제와 다르면 실제를 따른다.
- `print_status`의 `Matter.isDeviceCommissioned()` → `matter_is_commissioned()`, `handle_command`의 `Matter.decommission()` → `matter_decommission()`. `poll_boot_button`의 `Matter.decommission()`은 본 파일이라 그대로 둔다.
- `print_status`의 `WiFi.status()`, `ESP.getFreeHeap()`은 그대로 쓴다(`<WiFi.h>`·`<Arduino.h>` 허용).

### 라이브러리 감지 순서 (참고)

arduino-cli는 스케치 다음에 `src/firmware`의 소스를 파일 이름 순으로 감지한다. 새 파일(`firmware_net.cpp`, `firmware_serial.cpp`)이 `thermostat_firmware.cpp`보다 먼저 오고, 이 파일들은 Matter 없이 WiFi·Network·HTTPClient·Preferences 등을 먼저 찾는다. 그래서 `thermostat_firmware.cpp`는 남은 라이브러리(화면, LVGL, Matter)만 찾으면 된다.

## 실제 표본

메인 체크아웃에서 `src/core/thermostat_controller.h`의 시각만 바꾸고 `--only-compilation-database -v`로 잰 감지 로그(2026-11-28, `실측`). `thermostat_firmware.cpp`가 다시 감지되면서 라이브러리를 **17번** 하나씩 찾았고, 마지막 `Matter.h` 한 번에 56초가 들었다.

```
 63.7 Using cached library dependencies for file: ...\sketch\thermostat_main.ino.cpp.merged
 63.7 ResolveLibrary(thermostat_firmware.h)
 69.4 ResolveLibrary(WiFi.h)
 71.7 ResolveLibrary(Network.h)
 93.0 ResolveLibrary(esp_display_panel.hpp)
 93.6 ResolveLibrary(esp_lib_utils.h)
101.0 ResolveLibrary(esp_io_expander.hpp)
105.2 ResolveLibrary(lvgl.h)
106.4 ResolveLibrary(thermostat_core.h)
106.8 ResolveLibrary(sensor_logic.h)
107.2 ResolveLibrary(thermostat_net.h)
107.4 ResolveLibrary(HTTPClient.h)
107.7 ResolveLibrary(NetworkClientSecure.h)
112.4 ResolveLibrary(thermostat_storage.h)
112.6 ResolveLibrary(Preferences.h)
112.8 ResolveLibrary(lvgl_port.h)
113.0 ResolveLibrary(thermostat_app.h)
113.2 ResolveLibrary(Matter.h)
169.7 ResolveLibrary(thermostat_matter.h)
170.2 Using cached library dependencies for file: ...\libraries\WiFi\src\...
```

빌드 시간 기준값(`실측`, `[build] compile`):

| 상황 | 시간 |
|---|---|
| 바꾼 것 없음(고정 경로 빌드, SCRUM-333 뒤) | 90초 |
| `thermostat_firmware.cpp` 수정(SCRUM-330 측정, 고정 경로 전) | 304초 |

## 작업

### 작업 1: 내부 헤더와 네트워크·시각·센서 조회 분리

**Files:**
- Create: `src/firmware/firmware_internal.h`
- Create: `src/firmware/firmware_net.cpp`
- Modify: `src/firmware/thermostat_firmware.cpp`

설계대로 `firmware_internal.h`를 만들고, `firmware_net.cpp`로 함수 7개와 상수를 옮긴다. `thermostat_firmware.cpp`는 공유 전역의 정의를 `thermostat::firmware::internal`로 옮기고, `matter_is_commissioned()`·`matter_decommission()`을 추가하고, `print_pairing_codes`를 `internal`로 옮기고, 옮긴 함수를 `internal::`로 부른다(또는 `using namespace thermostat::firmware::internal;`).

확인: 펌웨어 컴파일(아래 테스트 명령). 이 작업 뒤 `firmware_net.cpp`에서 금지 헤더를 찾는다: `rg -n "Matter|lvgl|esp_display_panel|thermostat_ui" src/firmware/firmware_net.cpp src/firmware/firmware_internal.h`는 주석 말고 결과가 없어야 한다.

### 작업 2: 시리얼 진단 명령 분리

**Files:**
- Create: `src/firmware/firmware_serial.cpp`
- Modify: `src/firmware/thermostat_firmware.cpp`

`g_line`, `g_line_len`과 함수 6개를 옮긴다. `Matter.isDeviceCommissioned()`·`Matter.decommission()` 호출만 연결 함수로 바꾼다. `loop()`의 `poll_serial()` 호출은 `internal::poll_serial()`이 된다.

확인: 펌웨어 컴파일, 금지 헤더 검색을 `firmware_serial.cpp`에도 한다.

### 작업 3: 빌드 시간 측정

**Files:**
- Modify: (없음. 결과는 PR 본문에 쓴다)

워크트리에서 아래 순서로 재고 `[build] compile N초`를 PR 본문 표에 적는다. 첫 빌드는 메인 빌드 폴더를 복사해 오므로 약 9분 걸린다(측정에 넣지 않는다).

1. 첫 빌드: `python tools/build.py firmware/thermostat_main`
2. 바꾼 것 없음: 같은 명령 다시
3. `firmware_serial.cpp` 수정: 파일 끝에 빈 줄을 하나 넣고 빌드, 측정 뒤 되돌리고 다시 빌드
4. `firmware_net.cpp` 수정: 같은 방식
5. `thermostat_firmware.cpp` 수정: 같은 방식

## 테스트 명령

`python tools/test_host.py && python tools/test_build.py && python tools/build.py firmware/thermostat_main`

(펌웨어 빌드는 처음 약 9분, 그 뒤 몇 분 걸린다. 타임아웃을 30분 이상 둔다.)

## Review Focus

1. 옮긴 함수의 본문이 그대로인가(로직·로그 문구·상수 값). 바뀐 것은 위치, namespace, `Matter.*` → 연결 함수 두 곳뿐인가
2. 새 파일 세 개가 금지 헤더를 직접·간접으로 포함하지 않는가(`firmware_internal.h`가 포함하는 헤더까지)
3. 전역 정의가 `thermostat_firmware.cpp` 한 곳에만 있고, 다른 파일은 `extern`만 쓰는가. 같은 이름이 두 번 정의되지 않는가
4. `setup()`·`loop()`의 호출 순서가 그대로인가(`configure_home_server`, `poll_*`, `run_control_step`, `poll_serial`)
5. `<Wire.h>`를 넣지 않았는가

## 완료 기준

- 테스트 명령 통과
- 작업 3의 측정 표. 목표: `firmware_serial.cpp`·`firmware_net.cpp` 수정 빌드 150초 이하, `thermostat_firmware.cpp` 수정 빌드 304초보다 나빠지지 않음
- 리스크하기 때문에 Claude가 최종 검증 진행: 펌웨어 런타임 코드를 옮기는 일이라, 머지 전에 보드에 올려 부팅·`status`·시리얼 명령(`help`, `sensor remote livingroom`)·원격 온도 표시가 그대로인지 Claude(설계 PL)가 직접 확인한다. 실행 PL은 PR을 정식으로 바꾸되 **머지하지 않고** 보고에서 멈춘다
