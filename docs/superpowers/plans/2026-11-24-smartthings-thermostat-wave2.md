# SmartThings DIY 온도조절기 구현 계획 — Wave 2 (SCRUM-233 ~ SCRUM-237 병렬 5개)

> 앞 회차: `docs/superpowers/plans/2026-11-24-smartthings-thermostat-wave1.md` (`SCRUM-232` 머지 완료)
> 대상 일감: `SCRUM-233`, `SCRUM-234`, `SCRUM-235`, `SCRUM-236`, `SCRUM-237` (`plan-smartthings-thermostat`, 에픽 `SCRUM-230`)
> 기준 문서: `CLAUDE.md` (3, 4, 6~11, 13, 15절)

---

## Global Constraints

- **병렬 충돌 절대 금지**: Wave 2의 5개 일감은 동시에 독립 워크트리(`feature/SCRUM-233`~`237`)에서 실행된다. 각 작업은 자기 `**Files:**`에 적힌 파일만 생성·수정한다. 기존 `.gitkeep` 파일, `tools/build.py`, `tools/test_host.py`, `test/test_harness.h`, `CLAUDE.md`는 건드리지 않는다.
- **`library.properties` 규칙**: 새 Arduino 모듈의 `library.properties`에는 반드시 `author=golony6449`, `maintainer=golony6449`를 쓴다(`author=thermostat-esp32`는 Qwen 보안 검사에서 `auth`+16자 오탐을 유발하므로 금지).
- **순수 C++17 원칙**: `src/core/`와 `src/sensor/sensor_logic.*`, `src/ui/waveshare43b/ui_view_model.h`는 `Arduino.h`, `millis()`, `WiFi`, `ESP` 등 하드웨어·프레임워크 의존성이 전혀 없어야 하며 PC 호스트 `g++ -std=c++17 -Wall -Wextra -Werror`로 컴파일·테스트되어야 한다.
- **ESP32 코어·빌드**: 격리된 `esp32:esp32@3.3.12` + `python tools/build.py demos/<name>` 사용. `demos/<name>/modules.txt`로 필요한 `src/<mod>` 모듈만 선언한다.
- **온도·시간 단위 통일**:
  - 온도: `int16_t` centi-degC (0.01℃ 단위, 예: `2150` = 21.50℃, Matter 표준과 동일). 유효하지 않음은 `constexpr int16_t kInvalidTempCenti = INT16_MIN;` (`-32768`).
  - 시간: `uint64_t now_ms` (단조 증가 밀리초, 외부 주입, `millis()` 직접 호출 금지). 절대 시각은 `int64_t epoch_sec` (Unix UTC 초).
- **보안·비밀값**: 실제 Wi-Fi SSID/비밀번호/API 키를 절대 코드나 테스트에 쓰지 않는다. 데모에서 비밀값이 필요하면 `#if __has_include("config/secrets.h")`로 포함하고 없을 때는 `config/secrets.h.example`의 자리표시자로 폴백해 `secrets.h` 없이도 항상 컴파일되어야 한다.
- **실기 검증**: DeepSeek `--check`는 호스트 테스트 + `python tools/build.py demos/<name>` 컴파일만 실행한다(COM9 점유 금지). 실제 보드 업로드·시리얼/화면/SmartThings 앱 확인은 PR 검증 이후 수행한다.

---

## 실제 표본

### 1. ESP32 Arduino 3.3.12 MatterThermostat 및 RunningState API (실측)

- 헤더: `<Matter.h>`, `<MatterEndpoints/MatterThermostat.h>`, `<MatterEndpoints/MatterContactSensor.h>`, `<esp_matter_attribute.h>`
- `MatterThermostat::begin(MatterThermostat::THERMOSTAT_SEQ_OP_HEATING, MatterThermostat::THERMOSTAT_AUTO_MODE_DISABLED)`
- 기본 Setpoint 한계(실측): `kDefaultMinHeatSetpointLimit = 700` (7.0℃), `kDefaultMaxHeatSetpointLimit = 3000` (30.0℃)
- `MatterThermostat`에는 `ThermostatRunningState` 세터가 기본 제공되지 않으므로, `begin()` 이후 `esp_matter` 클러스터 API로 속성을 추가·갱신한다:
  - `esp_matter::cluster::thermostat::attribute::create_thermostat_running_state(cluster_t *cluster, uint16_t value)` (`esp_matter_attribute.h:502`)
  - bit 0 (`0x0001`): Heat State On (`kHeat`), `0x0000`: Idle
  - 갱신: `esp_matter_attr_val_t val = esp_matter_bitmap16(heating ? 0x0001 : 0x0000); esp_matter::attribute::update(endpoint_id, chip::app::Clusters::Thermostat::Id, chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id, &val);`
- 보조 엔드포인트 대체안: `MatterContactSensor` (`begin()`, `Matter.begin()` 호출 후 `setContact(bool)` 호출 — `Matter/README.md` 규칙: Boolean State 센서는 반드시 `Matter.begin()` 이후에 세터 호출).

### 2. 홈서버 원격 센서 API JSON 표본 (`CLAUDE.md` 10절)

```json
{
  "sensors": [
    {
      "id": "livingroom",
      "name": "거실",
      "temperature": 21.4,
      "updated_at": "2026-11-24T21:15:00+09:00"
    },
    {
      "id": "bedroom",
      "name": "안방",
      "temperature": 20.8,
      "updated_at": "2026-11-24T21:13:00+09:00"
    }
  ]
}
```
- 기대 파싱 결과:
  - `livingroom`: `name="거실"`, `temp_centi=2140`, `updated_epoch_sec=1795522500` (`2026-11-24T12:15:00Z`)
  - `bedroom`: `name="안방"`, `temp_centi=2080`, `updated_epoch_sec=1795522380` (`2026-11-24T12:13:00Z`)
  - 평균 온도: `(2140 + 2080) / 2 = 2110` (21.1℃)

### 3. Waveshare ESP32-S3-Touch-LCD-4.3B 하드웨어 표본 (실측)

- I2C 버스: `SDA=GPIO8`, `SCL=GPIO9` (`src/config/pins.h`)
- 보드 RTC 칩: `PCF85063A`, I2C 주소 `0x51`, 시간 레지스터 `0x04`(초)~`0x0A`(년, BCD 포맷)
- 외부 온도 센서: `SHT40`, I2C 주소 `0x44`, 고정밀 측정 명령 `0xFD`, 응답 6바이트 (`T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc`, CRC-8 poly `0x31` init `0xFF`)
- 디스플레이·터치: `ESP32_Display_Panel` v1.0.0 (`#include <esp_display_panel.hpp>`, `esp_panel::board::Board`), 해상도 `800x480` RGB565, `LVGL` 8.4.0 (`#include <lvgl.h>`). `esp_panel_board_supported_conf.h`에서 `ESP_PANEL_BOARD_DEFAULT_USE_SUPPORTED (1)` + `BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B` 사용.

---

### 작업 1: Matter 스파이크 — Thermostat + RunningState + 보조 엔드포인트 데모 (`SCRUM-233`)

**Files:**
- Create: `src/matter/library.properties`
- Create: `src/matter/thermostat_matter.h`
- Create: `src/matter/thermostat_matter.cpp`
- Create: `demos/01_matter/01_matter.ino`
- Create: `demos/01_matter/modules.txt`

**요구사항 및 세부 명세:**

1. **`src/matter/library.properties`**:
   - `name=thermostat_matter`, `version=0.1.0`, `author=golony6449`, `maintainer=golony6449`, `architectures=esp32`.
2. **`src/matter/thermostat_matter.h` & `.cpp`**:
   - `namespace thermostat::matter`:
   - `struct MatterThermostatCallbacks`:
     - `std::function<bool(bool heat_mode)> on_mode_change;` (`true`=Heat, `false`=Off)
     - `std::function<bool(int16_t setpoint_centi)> on_setpoint_change;`
   - `class ThermostatEndpoint`:
     - `bool begin(const MatterThermostatCallbacks& callbacks, bool enable_aux_contact = true);`
       - `MatterThermostat::begin(THERMOSTAT_SEQ_OP_HEATING, THERMOSTAT_AUTO_MODE_DISABLED)` 호출.
       - 엔드포인트 ID로 `esp_matter::endpoint::get(node::get(), ep_id)` → `cluster::get(ep, Thermostat::Id)` 조회 후 `ThermostatRunningState` 속성이 없으면 `esp_matter::cluster::thermostat::attribute::create_thermostat_running_state(cluster, 0)`으로 추가한다(생성 성공 여부를 멤버 `running_state_supported_`에 저장).
       - `enable_aux_contact`가 `true`이면 보조 엔드포인트 `MatterContactSensor aux_heat_sensor_`의 `begin()`도 호출(SmartThings 루틴에서 `ThermostatRunningState`가 조건으로 안 뜰 경우 대체안 검증용: `contact=true` = 난방 요청 ON, `false` = OFF).
       - `onChangeMode`, `onChangeHeatingSetpoint` 콜백 등록.
     - `void after_matter_begin(int16_t initial_temp_centi, int16_t initial_setpoint_centi, bool heat_mode);`
       - `Matter.begin()` 완료 후 초기 온도·목표온도·모드·RunningState(idle)·보조 ContactSensor(`false`)를 반영.
     - `bool set_local_temperature_centi(int16_t temp_centi);`
     - `bool set_heating_setpoint_centi(int16_t setpoint_centi);` (7.0~30.0℃ 클램프)
     - `bool set_heat_mode(bool heat_mode);`
     - `bool set_heating_active(bool heating);`
       - `running_state_supported_`이면 `esp_matter::attribute::update`로 `ThermostatRunningState`를 `0x0001`(heating) / `0x0000`(idle)로 갱신.
       - 보조 ContactSensor가 활성화돼 있으면 `aux_heat_sensor_.setContact(heating)` 함께 갱신.
     - `bool is_heating_active() const;`
     - `bool is_running_state_supported() const;`
     - `uint16_t endpoint_id() const;`
3. **`demos/01_matter/modules.txt`**:
   - `src/matter`
4. **`demos/01_matter/01_matter.ino`**:
   - `#if __has_include("config/secrets.h")` → 포함, 없으면 `config/secrets.h.example` 폴백.
   - 부팅 시 USB CDC 대기(최대 3초), `ThermostatEndpoint::begin(...)`, `Matter.begin()`, 커미셔닝 안 됐으면 `Matter.getManualPairingCode()` 및 `Matter.getOnboardingQRCodeUrl()`을 시리얼에 출력.
   - 시리얼 대화형 테스트 명령어 지원(루틴 조건·유지 신호 길이 실측용):
     - `t <float>`: 가짜 현재 온도 설정 (예: `t 19.5`)
     - `s <float>`: 목표온도 설정 (예: `s 23.0`)
     - `m off` / `m heat`: 모드 전환
     - `h 1` / `h 0`: 난방 작동 상태(`ThermostatRunningState` + 보조 ContactSensor) 수동 ON/OFF
     - `p <sec>`: 유지 신호 펄스 시험 — 현재 ON이면 `<sec>`초 동안 OFF했다가 다시 ON (루틴이 반응하는 최소 대기 시간 실측용!)
     - `status`: 현재 온도·목표온도·모드·난방상태·커미셔닝 여부·메모리 출력
     - `decommission`: `Matter.decommission()` 호출
   - `BOOT_PIN`(GPIO0) 5초 이상 길게 누르면 `Matter.decommission()`.

---

### 작업 2: core 제어 로직 — 순수 C++ 상태 머신 및 호스트 단위 테스트 (`SCRUM-234`)

**Files:**
- Create: `src/core/thermostat_types.h`
- Create: `src/core/thermostat_controller.h`
- Create: `src/core/thermostat_controller.cpp`
- Modify: `src/core/thermostat_core.h`
- Test: `test/test_core_control.cpp`

**요구사항 및 세부 명세:**

1. **`src/core/thermostat_types.h`** (순수 C++17, 설정 모델 분리 — NVS(`SCRUM-238`)·Matter 커스텀 속성 이전 대비):
   - `namespace thermostat::core`:
   - `constexpr size_t kMaxZones = 4;` (지금은 1존 사용, 다중 존 확장 대비 배열 구조)
   - `constexpr int16_t kInvalidTempCenti = -32768;`
   - `enum class Mode : uint8_t { kOff = 0, kTemperature = 1, kRepeat = 2 };`
   - `enum class StrategyKind : uint8_t { kHysteresis = 0 };` (향후 시간 비례 제어 전략 교체용)
   - `enum class HeatReason : uint8_t { kNone = 0, kTemperatureBelowTarget, kRepeatCycleOn, kFreezeProtection, kMinOnHold, kKeepAlivePulseOff, kMinOffHold, kSensorFaultOff };`
   - `struct ZoneConfig`:
     - `Mode mode = Mode::kOff;`
     - `int16_t target_temp_centi = 2100;` (21.0℃, 범위 500~3000)
     - `uint8_t repeat_on_minutes = 15;` (60분 주기 중 N분, 범위 0~60)
     - `int16_t freeze_protect_centi = 1000;` (최저온도 보호, 기본 10.0℃, 범위 500~1500)
     - `int16_t hysteresis_centi = 50;` (±0.5℃)
     - `uint16_t min_on_sec = 300;` (5분)
     - `uint16_t min_off_sec = 300;` (5분)
     - `uint16_t keep_alive_interval_sec = 3000;` (50분 연속 난방 시 유지 신호 발동)
     - `uint16_t keep_alive_off_sec = 10;` (유지 신호 대기 펄스 길이, 기본 10초, 설정 가능)
     - `StrategyKind strategy = StrategyKind::kHysteresis;`
   - `struct ZoneInput`:
     - `uint64_t now_ms = 0;`
     - `int16_t current_temp_centi = kInvalidTempCenti;`
     - `bool sensor_valid = false;`
   - `struct ZoneOutput`:
     - `bool logical_heating = false;` (논리적 난방 요청 상태 — UI에 "난방 중" 표시용)
     - `bool matter_heating = false;` (Matter RunningState 출력 — 유지 신호 펄스 중에만 잠깐 `false`)
     - `bool freeze_protect_active = false;`
     - `bool sensor_fault_warning = false;`
     - `bool keep_alive_pulsing = false;`
     - `HeatReason reason = HeatReason::kNone;`
2. **`src/core/thermostat_controller.h` & `.cpp`**:
   - 제어 전략 인터페이스 `class ITempStrategy` (`virtual bool decide_heating(bool currently_heating, int16_t current_temp_centi, const ZoneConfig& cfg) const = 0;`)와 기본 구현 `class HysteresisStrategy` (`current <= target - hyst` → true, `current >= target + hyst` → false, 그 사이는 `currently_heating` 유지).
   - `class ZoneController`:
     - `explicit ZoneController(const ZoneConfig& config = {});`
     - `void set_config(const ZoneConfig& config);` (입력 값 범위 클램프: `target_temp_centi` 500~3000, `repeat_on_minutes` 0~60, `freeze_protect_centi` 500~1500, `hysteresis_centi` >= 10)
     - `const ZoneConfig& config() const;`
     - `void set_strategy(const ITempStrategy* strategy);` (`nullptr`이면 기본 `HysteresisStrategy`)
     - `ZoneOutput step(const ZoneInput& input);`
   - `class MultiZoneController`:
     - 최대 `kMaxZones`개 존 배열 관리 (`active_zones`, `zone(size_t idx)`, `step_all(...)`).
   - **제어 규칙 (우선순위 및 엣지 케이스 정확히 구현!)**:
     1. **센서 이상 (`!sensor_valid || current_temp_centi == kInvalidTempCenti`)**:
        - `sensor_fault_warning = true`.
        - 온도 모드(`kTemperature`) 및 꺼짐 모드(`kOff`): 최저온도 보호 판단도 불가하므로 안전하게 난방 끄기(`logical_heating = false`, `reason = kSensorFaultOff`, 최소 켜짐 시간보다 안전이 우선!).
        - 반복 모드(`kRepeat`): 온도와 무관한 모드이므로 반복 주기 제어는 계속 동작하되 `sensor_fault_warning = true`를 표시해 화면 경고는 띄운다(최저온도 보호만 센서 이상으로 비활성).
     2. **최저온도 보호 (센서 정상일 때, 모든 모드에서 최우선!)**:
        - `current_temp_centi < freeze_protect_centi`이면 모드(꺼짐/온도/반복)와 무관하게 강제 난방 ON (`freeze_protect_active = true`, 최소 꺼짐 시간 무시하고 즉시 ON).
        - 해제 히스테리시스: `current_temp_centi >= freeze_protect_centi + hysteresis_centi`일 때 해제(경계 떨림 방지).
     3. **모드별 원시 판단**:
        - `kOff`: `false`.
        - `kTemperature`: 전략(`HysteresisStrategy`) 판단.
        - `kRepeat`: 60분(`3600000ms`) 주기 중 `elapsed_in_cycle < repeat_on_minutes * 60000ULL`이면 `true`, 아니면 `false` (`repeat_on_minutes == 0`이면 항상 `false`, `60`이면 항상 `true`).
     4. **최소 켜짐/꺼짐 시간 (`min_on_sec`, `min_off_sec`)**:
        - 최초 부팅 직후(`!initialized_`)에는 최소 시간 제약 없이 즉시 반영.
        - 상태 전환 후 경과 시간이 `min_on_sec * 1000` 미만이면 꺼짐 전환 보류(`kMinOnHold`), `min_off_sec * 1000` 미만이면 켜짐 전환 보류(`kMinOffHold`).
        - 예외: 최저온도 보호 진입, 센서 이상 안전 차단, 명시적 `kOff` 모드 전환은 최소 시간을 기다리지 않고 즉시 반영!
     5. **50분 유지 신호 (데드맨 스위치 회피 — 사용자 확정 결정)**:
        - **온도 모드뿐만 아니라 반복 모드(`N=60` 등 50분 이상 연속 난방)와 최저온도 보호 연속 난방 모두에 적용한다!**
        - `logical_heating == true`가 연속으로 `keep_alive_interval_sec` (기본 50분 = 3000초)에 도달하면, `keep_alive_off_sec` (기본 10초) 동안 `matter_heating = false`, `keep_alive_pulsing = true`, `reason = kKeepAlivePulseOff`를 출력한다(`logical_heating`은 `true` 유지).
        - `keep_alive_off_sec`가 지나면 `matter_heating = true`로 복귀하고 연속 난방 타이머를 0으로 리셋한다(다음 50분 뒤 다시 반복).
        - `logical_heating`이 `false`가 되면 유지 신호 타이머·펄스 상태를 리셋한다.
3. **`src/core/thermostat_core.h`** 수정:
   - 기존 `#include "core_version.h"` 아래에 `#include "thermostat_types.h"`, `#include "thermostat_controller.h"` 추가.
4. **`test/test_core_control.cpp`** (`// LINK: src/core/*.cpp`):
   - 아래 모든 시나리오 단위 테스트 작성 및 통과:
     1. 꺼짐 모드 정상 유지 및 최저온도 보호 발동/히스테리시스 해제
     2. 온도 모드 히스테리시스(±0.5℃) 경계 동작(목표 21.0℃: 20.5℃ ON, 21.0℃ 유지, 21.5℃ OFF, 21.0℃ 유지)
     3. 최소 켜짐 시간(5분) / 최소 꺼짐 시간(5분) 깜빡임 방지 + `kOff` 전환 즉시 차단
     4. 반복 모드 60분 주기 중 N=15분 ON / 45분 OFF, N=0 항상 OFF
     5. **반복 모드 N=60 연속 난방 시 50분 유지 신호 펄스(10초 OFF 후 복귀, 70분 데드맨 회피)**
     6. 온도 모드 50분 연속 난방 시 유지 신호 펄스 및 100분 시점 2차 펄스
     7. 센서 이상(`sensor_valid=false` 또는 `kInvalidTempCenti`): 온도/꺼짐 모드 즉시 안전 차단 + `sensor_fault_warning=true`, 반복 모드는 동작 유지 + 경고
     8. 커스텀 전략 주입(`set_strategy`) 및 `MultiZoneController` 다중 존 독립 동작 확인

---

### 작업 3: 센서 판단 로직 — 필터, 이상값, 선택·평균, 자동 전환, JSON 파싱, 문구 (`SCRUM-235`)

**Files:**
- Create: `src/sensor/sensor_logic.h`
- Create: `src/sensor/sensor_logic.cpp`
- Test: `test/test_sensor_logic.cpp`

**요구사항 및 세부 명세:**

1. **`src/sensor/sensor_logic.h` & `.cpp`** (순수 C++17, Arduino 의존성 0, 외부 JSON 라이브러리 의존성 0):
   - `namespace thermostat::sensor`:
   - `constexpr size_t kMaxRemoteSensors = 8;`
   - `constexpr size_t kFilterWindowSize = 5;`
   - `constexpr int16_t kInvalidTempCenti = -32768;`
   - `constexpr int16_t kMinPlausibleTempCenti = -1000;` (-10.0℃)
   - `constexpr int16_t kMaxPlausibleTempCenti = 6000;` (60.0℃)
   - `constexpr int16_t kMaxStepDeltaCenti = 500;` (단일 샘플 급변 임계값 5.0℃)
   - `constexpr int64_t kWarnAgeSec = 300;` (5분 초과 → 주의 색상)
   - `constexpr int64_t kStaleAgeSec = 600;` (10분 초과 → 보드 센서 자동 전환)
   - `constexpr uint8_t kMaxConsecutiveFailures = 3;` (3회 연속 실패 → 보드 센서 자동 전환)
   - **이동평균 필터 + 이상값 감지 (`class TempFilter`)**:
     - `bool push(int16_t raw_centi);`
       - 범위 밖(`raw < -1000 || raw > 6000 || raw == kInvalidTempCenti`) → 거부(`false`).
       - 이미 유효한 이전 출력이 있는데 `|raw - last_accepted| > kMaxStepDeltaCenti`이면 급변 이상값으로 1회 거부(단, 연속 3회 같은 급변이 들어오면 실제 환경 변화로 간주해 리셋·수용해 영구 고착 방지).
     - `bool has_value() const;`
     - `int16_t filtered_centi() const;` (최근 최대 `kFilterWindowSize`개 반올림 평균)
     - `void reset();`
   - **ISO-8601 타임스탬프 파서 + 홈서버 JSON 파서**:
     - `bool parse_iso8601_to_epoch(const char* iso, int64_t* out_epoch_sec);`
       - `"2026-11-24T21:15:00+09:00"`, `"2026-11-24T12:15:00Z"`, 소수초(`.123`), `-05:00` 오프셋 모두 정확히 UTC Unix epoch 초로 변환(표준 C++ 순수 구현, `mktime` 로컬 타임존 의존 금지 — Howard Hinnant 달력 공식 사용!).
     - `struct RemoteSensorReading { std::string id; std::string name; int16_t temp_centi; int64_t updated_epoch_sec; };`
     - `bool parse_homeserver_sensors_json(const std::string& json, std::vector<RemoteSensorReading>* out);`
       - `CLAUDE.md` 10절 표본 JSON(`{"sensors": [{"id": ..., "name": ..., "temperature": ..., "updated_at": ...}]}`)을 외부 라이브러리 없이 안전하게 파싱. 잘못된 JSON·누락 필드·비정상 숫자는 안전하게 거부(`false` 또는 해당 항목 무시).
   - **센서 선택·평균·자동 전환 및 화면 출처 문구 (`class SensorSelector`)**:
     - `enum class SourceMode : uint8_t { kBoardOnly = 0, kSingleRemote = 1, kAverageRemote = 2 };`
     - `enum class Freshness : uint8_t { kFresh = 0, kWarn = 1, kStaleOrFallback = 2, kFault = 3 };`
     - `struct SensorSelectionConfig`:
       - `SourceMode mode = SourceMode::kBoardOnly;`
       - `std::vector<std::string> selected_remote_ids;`
       - `int16_t board_offset_centi = 0;` (센서 보정값)
     - `struct ResolvedTemperature`:
       - `bool valid = false;`
       - `int16_t temp_centi = kInvalidTempCenti;`
       - `bool used_fallback_board = false;`
       - `Freshness freshness = Freshness::kFault;`
       - `int64_t age_sec = 0;`
       - `std::string source_label;` (한국어 UI 표기!)
     - 메서드:
       - `void set_config(const SensorSelectionConfig& cfg);`
       - `void update_board_temp(int16_t raw_centi, int64_t now_epoch_sec);`
       - `void record_remote_success(const std::vector<RemoteSensorReading>& readings, int64_t now_epoch_sec);` (연속 실패 횟수 0으로 초기화)
       - `void record_remote_failure();` (연속 실패 횟수 +1)
       - `ResolvedTemperature resolve(int64_t now_epoch_sec) const;`
     - **출처 문구 규약 (`CLAUDE.md` 11절 표본 정확 일치!)**:
       - 경과 시간 포맷 `format_age_korean(int64_t age_sec)`: `< 60초` → `"방금"`, `N분` (`age_sec / 60`) → `"N분 전"`.
       - 보드 전용 정상: `"보드 센서 · 방금"` / `"보드 센서 · 3분 전"`
       - 원격 1개 정상: `"거실 센서 · 3분 전"` (이름 뒤에 `" 센서"` 붙이기, 이미 `"센서"`로 끝나면 중복 안 붙임)
       - 원격 여러 개 평균 정상: `"평균(거실, 안방) · 최근 2분 전"` (유효한 선택 센서 중 가장 최근 또는 최대 경과 시간 기준: 주의 판정은 선택 센서 중 최대 경과 시간 `> 300초`면 `kWarn`, `> 600초`면 stale 제외/전환)
       - 원격 3회 연속 실패로 보드 전환: `"보드 센서 · 원격 센서 응답 없음"` (`used_fallback_board=true`, `freshness=kStaleOrFallback`)
       - 원격 `updated_at` 10분 초과로 보드 전환: `"보드 센서 · 원격 센서 값 오래됨"` (`used_fallback_board=true`, `freshness=kStaleOrFallback`)
       - 보드 센서마저 없음/이상: `valid=false`, `freshness=kFault`, `"센서 오류 · 온도 확인 불가"`
2. **`test/test_sensor_logic.cpp`** (`// LINK: src/sensor/sensor_logic.cpp`):
   - `TempFilter`: 이동평균, 범위 밖 이상값 거부, 급변(5℃ 초과) 거부 및 3회 지속 시 수용
   - `parse_iso8601_to_epoch`: `+09:00`, `Z`, `-05:00`, 소수초, 잘못된 문자열
   - `parse_homeserver_sensors_json`: **실제 표본 JSON 그대로 넣은 테스트** (`livingroom=2140`, `bedroom=2080`, UTC epoch 일치) + 빈 배열/손상 JSON
   - `SensorSelector`: 보드 전용, 원격 1개(`"거실 센서 · 3분 전"`), 원격 2개 평균(`"평균(거실, 안방) · 최근 2분 전"`), 5분 초과 `kWarn`, 10분 초과 보드 자동 전환, 3회 연속 실패 보드 자동 전환(`"보드 센서 · 원격 센서 응답 없음"`), 1~2회 실패까지는 기존 값 유지, 보드 보정값 적용, 전체 센서 불량 시 `valid=false`

---

### 작업 4: 보드 I/O 어댑터 — SHT40, NTP·RTC 시계, 홈서버 HTTP 클라이언트 (`SCRUM-236`)

**Files:**
- Create: `src/sensor/library.properties`
- Create: `src/sensor/thermostat_sensor_hw.h`
- Create: `src/sensor/sht40_driver.h`
- Create: `src/sensor/sht40_driver.cpp`
- Create: `src/net/library.properties`
- Create: `src/net/thermostat_net.h`
- Create: `src/net/clock_rtc.h`
- Create: `src/net/clock_rtc.cpp`
- Create: `src/net/homeserver_client.h`
- Create: `src/net/homeserver_client.cpp`
- Create: `demos/02_io/02_io.ino`
- Create: `demos/02_io/modules.txt`
- Test: `test/test_io_helpers.cpp`

**요구사항 및 세부 명세:**

1. **모듈 메타데이터**:
   - `src/sensor/library.properties`: `name=thermostat_sensor`, `author=golony6449`, `maintainer=golony6449`, `architectures=*` (주의: `src/sensor/sensor_logic.*`는 SCRUM-235가 추가하므로 이 작업에서는 만들지 않는다!).
   - `src/net/library.properties`: `name=thermostat_net`, `author=golony6449`, `maintainer=golony6449`, `architectures=esp32`.
2. **SHT40 I2C 드라이버 (`src/sensor/sht40_driver.h`, `.cpp`, `thermostat_sensor_hw.h`)**:
   - 외부 라이브러리 없이 순수 `Wire` + CRC-8 검증으로 구현(주소 `0x44`, 명령 `0xFD`, 10ms 대기 후 6바이트 읽기).
   - 순수 헬퍼 함수(호스트 테스트 가능하도록 `#ifdef ARDUINO`와 분리):
     - `uint8_t sht40_crc8(const uint8_t* data, size_t len);` (poly `0x31`, init `0xFF`)
     - `bool sht40_decode_frame(const uint8_t frame[6], int16_t* out_temp_centi, uint16_t* out_rh_centi);`
       - 공식: `T(°C) = -45 + 175 * raw_t / 65535`, `RH(%) = -6 + 125 * raw_rh / 65535` (0~100% 클램프).
   - `#ifdef ARDUINO` 클래스 `Sht40Driver`:
     - `bool begin(TwoWire& wire, uint8_t addr = 0x44);` (센서가 연결되어 있지 않아도 크래시·무한대기 없이 `false` 반환!)
     - `bool read(int16_t* out_temp_centi, uint16_t* out_rh_centi);` (미연결/NACK/CRC 오류 시 `false`)
3. **NTP + 보드 RTC (`PCF85063A`, I2C `0x51`) 시계 (`src/net/clock_rtc.h`, `.cpp`)**:
   - 순수 BCD 변환 헬퍼(호스트 테스트 가능): `uint8_t bcd_to_dec(uint8_t)`, `uint8_t dec_to_bcd(uint8_t)`.
   - `#ifdef ARDUINO` 클래스 `ClockRtc`:
     - `bool begin(TwoWire& wire, uint8_t addr = 0x51);`
     - `bool read_rtc_epoch(int64_t* out_epoch_sec);` (PCF85063A 레지스터 `0x04`~`0x0A` 읽기, OS bit 7 확인: 유효하지 않으면 `false`)
     - `bool write_rtc_epoch(int64_t epoch_sec);` (UTC 기준 쓰기)
     - `bool sync_ntp_and_save(const char* ntp_server = "pool.ntp.org", uint32_t timeout_ms = 5000);` (`configTime(9*3600, 0, ntp_server)` 후 유효 시각 수신 시 `write_rtc_epoch` 저장)
     - `bool restore_system_time_from_rtc();` (재부팅 시 Wi-Fi 전에도 RTC에서 시스템 시각 `settimeofday` 복원)
     - `int64_t now_epoch_sec() const;`
4. **홈서버 HTTP 클라이언트 (`src/net/homeserver_client.h`, `.cpp`)**:
   - `#ifdef ARDUINO` 클래스 `HomeServerClient`:
     - `struct FetchResult { bool ok = false; int http_status = 0; std::string body; std::string error; };`
     - `void configure(const char* base_url, const char* api_key = "", uint16_t timeout_ms = 3000);`
     - `FetchResult fetch_sensors();` (`GET <base_url>/api/thermostat/sensors`, 타임아웃 3000ms, `api_key`가 비어 있지 않고 자리표시자(`YOUR_...`)가 아니면 `X-API-Key` 헤더 추가)
     - `bool should_poll(uint64_t now_ms, uint64_t interval_ms = 60000) const;`
     - `void mark_polled(uint64_t now_ms);`
5. **`demos/02_io/02_io.ino` & `modules.txt`**:
   - `modules.txt`: `src/sensor`, `src/net`
   - `Wire.begin(thermostat::pins::kI2cSda, thermostat::pins::kI2cScl);` (GPIO8/9)
   - I2C 버스 스캔 로그 출력(0x51 RTC 등 감지 확인, SHT40 미연결 시 `[02_io] SHT40 not detected (graceful skip)` 출력 — 사용자 결정!).
   - RTC 복원 시각 출력 + Wi-Fi 설정(`secrets.h` 존재하고 자리표시자가 아닐 때만 연결 시도) 후 NTP 동기화·홈서버 HTTP 1회 조회 결과(또는 미설정 안내) 출력.
   - 10초마다 상태 로그 반복.
6. **`test/test_io_helpers.cpp`** (`// LINK: src/sensor/sht40_driver.cpp src/net/clock_rtc.cpp`):
   - `sht40_crc8` 공식 테스트 벡터(`0xBE, 0xEF` → `0x92`), `sht40_decode_frame` 정상 프레임 디코딩 및 CRC 불일치 거부
   - `bcd_to_dec`, `dec_to_bcd` 경계값 테스트

---

### 작업 5: 터치 UI (LVGL) — 가짜 상태 화면·설정·온도 출처·한글 폰트 (`SCRUM-237`)

**Files:**
- Create: `src/ui/waveshare43b/library.properties`
- Create: `src/ui/waveshare43b/ui_view_model.h`
- Create: `src/ui/waveshare43b/thermostat_ui.h`
- Create: `src/ui/waveshare43b/thermostat_ui.cpp`
- Create: `src/ui/waveshare43b/lvgl_port.h`
- Create: `src/ui/waveshare43b/lvgl_port.cpp`
- Create: `src/ui/waveshare43b/kr_font_20.c`
- Create: `src/ui/waveshare43b/kr_font_28.c`
- Create: `tools/gen_kr_font.py`
- Create: `demos/03_ui/esp_panel_board_supported_conf.h`
- Create: `demos/03_ui/03_ui.ino`
- Create: `demos/03_ui/modules.txt`
- Test: `test/test_ui_view_model.cpp`

**요구사항 및 세부 명세:**

1. **`src/ui/waveshare43b/library.properties`**:
   - `name=thermostat_ui_waveshare43b`, `author=golony6449`, `maintainer=golony6449`, `architectures=esp32`.
2. **순수 C++ 뷰 모델 (`src/ui/waveshare43b/ui_view_model.h`)** (Arduino/LVGL 의존성 0, 호스트 테스트 가능):
   - `namespace thermostat::ui`:
   - `enum class UiMode : uint8_t { kOff = 0, kTemperature = 1, kRepeat = 2 };`
   - `enum class SourceSeverity : uint8_t { kNormal = 0, kWarning = 1, kError = 2 };` (5분 초과=`kWarning`, 전환/오류=`kError`)
   - `struct UiViewModel`:
     - `bool temp_valid = true;`
     - `int16_t current_temp_centi = 2140;`
     - `int16_t target_temp_centi = 2250;`
     - `UiMode mode = UiMode::kTemperature;`
     - `bool heating_active = false;`
     - `uint8_t repeat_on_minutes = 15;` (0~60)
     - `int16_t freeze_protect_centi = 1000;` (500~1500)
     - `SourceSeverity source_severity = SourceSeverity::kNormal;`
     - `char source_text[96] = "거실 센서 · 3분 전";`
     - `char time_text[16] = "21:15";`
   - `struct UiCallbacks`:
     - `void (*on_target_temp_change)(int16_t new_target_centi, void* ctx) = nullptr;`
     - `void (*on_mode_change)(UiMode new_mode, void* ctx) = nullptr;`
     - `void (*on_repeat_minutes_change)(uint8_t new_minutes, void* ctx) = nullptr;`
     - `void (*on_freeze_protect_change)(int16_t new_freeze_centi, void* ctx) = nullptr;`
     - `void* ctx = nullptr;`
   - 순수 헬퍼 함수(호스트 테스트 대상):
     - `int16_t step_target_temp(int16_t current_centi, int16_t delta_centi);` (0.5℃=50 단위, 500~3000 클램프)
     - `uint8_t step_repeat_minutes(uint8_t current_min, int delta_min);` (5분 단위, 0~60 클램프)
     - `int16_t step_freeze_protect(int16_t current_centi, int16_t delta_centi);` (0.5℃=50 단위, 500~1500 클램프)
     - `void format_temp_celsius(int16_t temp_centi, bool valid, char* buf, size_t buf_size);` (`"21.4℃"` 또는 `"--.-℃"`)
3. **한글 폰트 생성 (`tools/gen_kr_font.py` → `kr_font_20.c`, `kr_font_28.c`)**:
   - Windows `C:\Windows\Fonts\malgun.ttf` (맑은 고딕) + `Pillow` (없으면 `uv run --with pillow` 또는 순수 4bpp LVGL v8 `lv_font_t` 포맷 생성기)로 UI에서 사용하는 모든 ASCII(0x20~0x7E) + 기호(`℃`, `·`, `±`, `°`) + 한국어 UI 문구 문자셋(예: `꺼짐온도반복난방중대기현재목표설정최저보호거실안방보드원격센서평균최근방금분전응답없음오류주의상태시간주기저장닫기확인`)을 담은 LVGL v8 호환 C 폰트 파일 `kr_font_20.c` (`LV_FONT_DECLARE(kr_font_20)`), `kr_font_28.c` (`LV_FONT_DECLARE(kr_font_28)`)를 생성해 커밋한다.
   - **주의**: 생성된 C 파일이 `lvgl.h` 8.4.0의 `lv_font_fmt_txt_dsc_t` / `lv_font_t` 구조체와 정확히 일치하고 `arduino-cli`로 경고·오류 없이 컴파일되어야 한다!
4. **Waveshare 4.3B LVGL 포트 (`demos/03_ui/esp_panel_board_supported_conf.h`, `src/ui/waveshare43b/lvgl_port.h`, `.cpp`)**:
   - `demos/03_ui/esp_panel_board_supported_conf.h`: `#define ESP_PANEL_BOARD_DEFAULT_USE_SUPPORTED (1)` 및 `#define BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B` 설정(`ESP32_Display_Panel` 1.0.0 공식 4.3B 보드 정의 사용 — 800×480, VBP=8, VFP=8, GT911 터치, CH422G 백라이트 자동 설정!).
   - `lvgl_port.h/.cpp`: `esp_panel::board::Board` 초기화 → RGB LCD + GT911 터치 → PSRAM 기반 더블 버퍼(또는 bounce buffer) LVGL 8.4 디스플레이/입력 드라이버 등록 + FreeRTOS LVGL 태스크 + 뮤텍스 `lvgl_port_lock(int timeout_ms)` / `lvgl_port_unlock()`.
5. **터치 UI 화면 (`src/ui/waveshare43b/thermostat_ui.h`, `.cpp`)**:
   - 800×480 가로 화면에 깔끔한 2-컬럼(또는 카드형) 다크 테마 온도조절기 UI:
     - **좌측 상태 영역**: 큰 현재 온도(`21.4℃`), 난방 상태 배지(`난방 중` 주황/빨강 vs `대기` 회색), **온도 출처 배지**(`거실 센서 · 3분 전` — `kNormal` 연회색, `kWarning` 노랑/주황 주의 색, `kError` 빨강), 현재 시각.
     - **우측 제어 영역**: 목표온도 표시 + `[-]` / `[+]` 버튼(0.5℃ 조절), 모드 세그먼트 버튼(`꺼짐` / `온도` / `반복`), 하단 `[설정]` 버튼.
     - **설정 모달/패널**: 반복 모드 구동 시간 `N분` (`[-]`/`[+]`, 5분 단위 0~60분), 최저온도 보호 (`[-]`/`[+]`, 0.5℃ 단위 5.0~15.0℃), `[닫기]` 버튼.
     - 모든 버튼 조작 시 `UiCallbacks`로 변경 값을 전달(내부 상태도 즉시 반영해 데모가 자연스럽게 동작).
   - `void update(const UiViewModel& vm);` — 외부에서 상태 갱신 시 라벨·색상·배지 업데이트.
6. **`demos/03_ui/03_ui.ino` & `modules.txt`**:
   - `modules.txt`: `src/ui/waveshare43b`
   - 가짜 상태 데이터(`UiViewModel`)로 UI 초기화, 콜백이 불릴 때마다 시리얼에 `[03_ui] cb ...` 로그 출력.
   - Wi-Fi 활성화(`WiFi.mode(WIFI_STA)` — `secrets.h`가 실제 값이면 접속 시도, 아니면 STA 초기화만) 상태에서 5초마다 `[03_ui] heap_free=... psram_free=...` 메모리 사용량 로그 출력(화면과 Wi-Fi 동시 구동 시 메모리/깜빡임 확인용).
   - 10초마다 가짜 온도 출처 상태를 `거실 센서 · 3분 전` (`kNormal`) → `평균(거실, 안방) · 6분 전` (`kWarning`) → `보드 센서 · 원격 센서 응답 없음` (`kError`)로 순환 시연하는 토글(또는 시리얼 명령으로 전환) 지원.
7. **`test/test_ui_view_model.cpp`** (`// LINK:` 없음 — 헤더 전용이므로 `src/core/core_version.cpp` 기본 링크 또는 순수 테스트):
   - `step_target_temp`, `step_repeat_minutes`, `step_freeze_protect`, `format_temp_celsius` 경계값 단위 테스트 통과.

---

## Review Focus

1. **병렬 충돌 방지**: 각 작업이 자기 `**Files:**` 밖의 파일(특히 `.gitkeep`, `tools/build.py`, `tools/test_host.py`, 다른 모듈 폴더)을 건드리지 않고, 모든 `library.properties`가 `author=golony6449`를 사용하는지.
2. **SCRUM-233 (Matter)**: `ThermostatRunningState` 속성이 `esp_matter` API로 안전하게 생성·갱신되고, 보조 `MatterContactSensor` 세터가 `Matter.begin()` 이후에만 호출되며, `secrets.h` 없이도 컴파일되는지.
3. **SCRUM-234 (core)**: `Arduino.h`/`millis()` 의존성이 없고, 최저온도 보호·센서 이상 안전 차단·히스테리시스·최소 시간·**반복 모드 N=60 포함 50분 유지 신호 펄스**가 모두 호스트 단위 테스트로 검증되는지.
4. **SCRUM-235 (sensor 로직)**: `Arduino.h` 의존성이 없고, `CLAUDE.md` 10절 표본 JSON과 ISO-8601 타임존 파싱, 5분 주의·10분 전환·3회 연속 실패 전환, 한국어 출처 문구가 모두 호스트 단위 테스트로 검증되는지.
5. **SCRUM-236 (보드 I/O)**: SHT40 미연결 시 크래시 없이 graceful 스킵하고, CRC-8/BCD 헬퍼 호스트 테스트와 `demos/02_io` ESP32 빌드가 통과하는지.
6. **SCRUM-237 (터치 UI)**: Waveshare 4.3B `ESP32_Display_Panel` + LVGL 8.4.0 + 한글 폰트가 경고·오류 없이 컴파일되고, `UiCallbacks` 콜백과 5분 초과 주의 색상, Wi-Fi 동시 메모리 로깅, 뷰 모델 호스트 테스트가 모두 동작하는지.
