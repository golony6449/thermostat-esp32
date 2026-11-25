# SmartThings DIY 온도조절기 구현 계획 — Wave 5 (SCRUM-242: 펌웨어 통합)

> 앞 회차: `docs/superpowers/plans/2026-11-25-smartthings-thermostat-wave4-ui.md` (`SCRUM-232~239`, `SCRUM-248~251` 머지 완료)
> 대상 일감: `SCRUM-242` (`plan-smartthings-thermostat`, 에픽 `SCRUM-230`)
> 기준 문서: `CLAUDE.md` (4, 6~12, 15절)

---

## Global Constraints

- **ESP-IDF 5.5 I2C 드라이버 충돌 금지 (`Wire.h` 제거)**: `ESP32_Display_Panel` 1.0.0이 ESP-IDF 구형 I2C 드라이버(`<driver/i2c.h>`, `I2C_NUM_0`, SDA=8, SCL=9)를 사용한다. Arduino ESP32 3.3.12의 `<Wire.h>`는 신형 `<driver/i2c_master.h>`를 링크하므로, 같은 바이너리에 둘이 함께 링크되면 부팅 시 `CONFLICT! driver_ng is not allowed to be used with the legacy driver`로 즉시 패닉이 발생한다. 따라서 `Sht40Driver`와 `ClockRtc`의 `#ifdef ARDUINO` 구현을 `<driver/i2c.h>` (`i2c_port_t`) 기반으로 전환하고, 통합 펌웨어에서는 `<Wire.h>`를 절대 include/링크하지 않는다.
- **CHIPoBLE 유지 (`WiFi.begin()` 수동 호출 금지)**: `CONFIG_ENABLE_CHIPOBLE=1` 빌드에서는 Matter 스택이 커미셔닝으로 저장한 Wi-Fi 자격증명으로 STA를 관리한다. `Matter.selectNetwork(MATTER_NETWORK_WIFI)`를 호출하고 `WiFi.begin()`을 수동 호출하지 않는다(`!CONFIG_ENABLE_CHIPOBLE`일 때만 `matterConnectWiFi` 폴백).
- **스레드 안전성 (데드락 원천 차단)**: Matter 콜백(CHIP 태스크) 안에서 LVGL lock을 잡지 않고, LVGL UI 콜백(LVGL 태스크) 안에서 Matter API를 호출하지 않는다. 두 콜백은 `portMUX_TYPE` 크리티컬 섹션으로 보호된 pending 이벤트 구조체에 기록만 하고 즉시 반환하며, `loop()` 단일 태스크가 이벤트를 소비해 `ThermostatApp` → NVS/Matter/UI로 반영한다.
- **`library.properties` 규칙**: `author=golony6449`, `maintainer=golony6449` 사용.
- **보안 규칙**: `src/config/secrets.h` 생성·커밋 금지(`src/config/secrets.h.example`만 참조). 홈서버 API 키는 NVS에 저장하지 않는다.
- **COM9 점유 금지**: 자동 검증에서는 `python tools/test_host.py`, `python -m unittest discover -s tests -v`, `python tools/build.py demos/02_io`, `python tools/build.py firmware/thermostat_main` 컴파일만 실행한다. 실제 보드 업로드와 시리얼 확인은 머지 후 설계 PL이 수행한다.
- **기존 테스트 유지**: `tools/build.py`와 기존 테스트 스위트(`test_core_control`, `test_sensor_logic`, `test_io_helpers`, `test_storage_codec`, `test_ui_view_model`, `test_smoke`, `tests/test_tools.py`)는 모두 그대로 통과해야 한다.

---

## 실제 표본

### 1. 홈서버 API 응답 계약 (`SCRUM-239`, `GET /api/thermostat/sensors`)
```json
{
  "sensors": [
    {
      "id": "livingroom",
      "name": "거실",
      "temperature": 21.4,
      "updated_at": "2026-11-24T21:15:00+09:00"
    }
  ]
}
```
- `X-API-Key` 헤더(선택), 1분 주기 폴링, 타임아웃 3초.
- `updated_at`이 10분 초과 또는 3회 연속 실패 시 보드 센서로 자동 전환 + UI에 이유 표시.

### 2. `SCRUM-233` Matter 실기 검증 확정 사항
- SmartThings 앱에서 순정 `ThermostatRunningState`(`상태: 난방`)가 표시·루틴 조건·70분 유지 조건까지 모두 지원됨을 확인했다.
- 따라서 통합 펌웨어에서는 `ThermostatEndpoint::begin(callbacks, /*enable_aux_contact=*/false)`로 난방 전용 Thermostat 엔드포인트 1개만 등록한다.
- 유지 신호 대기 시간은 `ZoneConfig::keep_alive_off_sec` 기본값(10초)을 유지한다(추후 `SCRUM-254` 실측값으로 NVS/설정 조정 가능).

### 3. 기존 모듈 공개 시그니처 요약
- `src/core/thermostat_types.h`: `Mode::{kOff, kTemperature, kRepeat}`, `ZoneConfig`, `ZoneInput`, `ZoneOutput`, `kInvalidTempCenti`
- `src/core/thermostat_controller.h`: `MultiZoneController` (`set_active_zones`, `zone(idx).set_config`, `step_all`, `output(idx)`)
- `src/sensor/sensor_logic.h`: `parse_homeserver_sensors_json`, `SensorSelector` (`set_config`, `update_board_temp`, `record_remote_success`, `record_remote_failure`, `resolve`), `SourceMode`, `Freshness`, `ResolvedTemperature`
- `src/storage/settings_codec.h`: `DeviceSettings`, `ZonePersistedSettings`, `SensorSourceSetting`, `default_device_settings`, `sanitize_settings`
- `src/storage/nvs_store.h`: `NvsSettingsStore` (`begin`, `load`, `request_save`, `tick`, `save_now`, `erase`)
- `src/matter/thermostat_matter.h`: `ThermostatEndpoint` (`begin`, `after_matter_begin`, `set_local_temperature_centi`, `set_heating_setpoint_centi`, `set_heat_mode`, `set_heating_active`)
- `src/ui/waveshare43b/ui_view_model.h`: `UiViewModel`, `UiCallbacks`, `UiMode`, `SourceSeverity`
- `src/ui/waveshare43b/thermostat_ui.h`: `ThermostatUi` (`begin`, `update`), `lvgl_port.h`: `lvgl_port_init`, `lvgl_port_lock`, `lvgl_port_unlock`
- `src/net/homeserver_client.h`: `HomeServerClient` (`configure`, `fetch_sensors`, `should_poll`, `mark_polled`), `is_placeholder_value`

---

### 작업 1: I2C 구형 드라이버 통일, 순수 C++ 통합 조정자(`src/app`), 통합 펌웨어(`firmware/thermostat_main`) 및 호스트 테스트 (`SCRUM-242`)

**Files:**
- Modify: `src/sensor/sht40_driver.h`
- Modify: `src/sensor/sht40_driver.cpp`
- Modify: `src/net/clock_rtc.h`
- Modify: `src/net/clock_rtc.cpp`
- Modify: `demos/02_io/02_io.ino`
- Create: `src/app/library.properties`
- Create: `src/app/thermostat_app.h`
- Create: `src/app/thermostat_app.cpp`
- Create: `firmware/thermostat_main/thermostat_main.ino`
- Create: `firmware/thermostat_main/modules.txt`
- Test: `test/test_thermostat_app.cpp`

**요구사항 및 세부 명세:**

1. **I2C 구형 드라이버 통일 (`src/sensor/sht40_driver.*`, `src/net/clock_rtc.*`, `demos/02_io/02_io.ino`)**:
   - `#ifdef ARDUINO` 블록에서 `<Wire.h>`를 제거하고 `<driver/i2c.h>`를 사용한다.
   - `Sht40Driver`:
     - `bool begin(i2c_port_t port = I2C_NUM_0, uint8_t addr = kSht40Address);`
     - `bool read(int16_t* out_temp_centi, uint16_t* out_rh_centi);`
     - `begin()`은 `i2c_master_cmd_begin`으로 주소 ACK 프로브(타임아웃 50ms)하여 미연결 시 `false`를 반환하고 절대 블록/크래시하지 않는다.
     - `read()`는 `i2c_master_write_to_device`로 `kSht40CmdMeasureHighPrecision` 전송 → `delay(kSht40MeasureDelayMs)` → `i2c_master_read_from_device`로 6바이트 수신 → 기존 `sht40_decode_frame()` 호출.
   - `ClockRtc`:
     - `bool begin(i2c_port_t port = I2C_NUM_0, uint8_t addr = kPcf85063aAddress);`
     - `read_registers` / `write_registers`는 `i2c_master_write_read_device` / `i2c_master_write_to_device`(타임아웃 50ms)로 구현.
     - 기존 순수 함수(`bcd_to_dec`, `dec_to_bcd`, `utc_datetime_to_epoch`, `epoch_to_utc_datetime`) 및 `read_rtc_epoch`, `write_rtc_epoch`, `sync_ntp_and_save`, `restore_system_time_from_rtc`, `now_epoch_sec` 시그니처와 동작은 100% 유지.
   - `demos/02_io/02_io.ino`:
     - `<Wire.h>` 대신 `<driver/i2c.h>`를 사용해 `I2C_NUM_0`(SDA=8, SCL=9, `kSensorI2cClockHz`)을 `i2c_param_config` + `i2c_driver_install`로 초기화하고 I2C 스캔, `g_sht40.begin(I2C_NUM_0)`, `g_clock.begin(I2C_NUM_0)`을 호출한다.
   - 호스트 테스트(`test/test_io_helpers.cpp`)는 `#ifdef ARDUINO` 밖 순수 함수만 컴파일하므로 수정 없이 통과해야 한다.

2. **순수 C++17 통합 조정자 (`src/app/library.properties`, `src/app/thermostat_app.h`, `src/app/thermostat_app.cpp`)**:
   - `src/app/library.properties`: `name=thermostat_app`, `version=0.1.0`, `author=golony6449`, `maintainer=golony6449`, `sentence=Thermostat integration coordinator`, `architectures=*`.
   - `Arduino.h`, `LVGL`, `Matter.h`, `Preferences.h` 의존성 전혀 없는 순수 C++17로 작성하여 `test/test_thermostat_app.cpp`에서 `g++ -std=c++17 -Wall -Wextra -Werror`로 컴파일·검증한다.
   - `namespace thermostat::app`:
   - `struct SyncActions`:
     - `bool settings_dirty = false;`
     - `bool push_matter_mode = false; bool matter_heat_mode = false;`
     - `bool push_matter_setpoint = false; int16_t matter_setpoint_centi = 2100;`
     - `bool push_matter_temp = false; int16_t matter_temp_centi = thermostat::core::kInvalidTempCenti;`
     - `bool push_matter_heating = false; bool matter_heating = false;`
     - `bool ui_dirty = false;`
   - `class ThermostatApp`:
     - `void init(const thermostat::storage::DeviceSettings& settings, uint64_t now_ms);`
       - `settings_`에 복사 후 `sanitize_settings(&settings_)` 적용.
       - `controller_.set_active_zones(settings_.active_zones)`, 존 0에 `settings_.zones[0].control` 적용.
       - `last_heat_mode_` 초기화(`mode == kRepeat`이면 `kRepeat`, 그 외 `kTemperature`).
       - `sensor_selector_.set_config(...)`에 존 0의 `sensor_source`, `selected_remote_ids`, `sensor_offset_centi` 반영.
       - 초기 `ui_dirty = true`, `push_matter_mode = true`, `push_matter_setpoint = true`.
     - **UI 입력 핸들러** (값 변경 시 `settings_dirty = true`, `ui_dirty = true`, 해당 시 `push_matter_* = true`):
       - `void on_ui_target_temp(int16_t new_target_centi);`
         - `sanitize_settings` 후 값이 달라졌으면 `controller_` 반영, `settings_dirty = true`, `push_matter_setpoint = true`, `ui_dirty = true`.
       - `void on_ui_mode(thermostat::ui::UiMode new_mode);`
         - `UiMode` ↔ `core::Mode` 변환. `kTemperature`/`kRepeat`이면 `last_heat_mode_` 갱신.
         - `settings_dirty = true`, `ui_dirty = true`. Matter `Heat/Off` 논리 상태(`mode != kOff`)가 바뀌었으면 `push_matter_mode = true`.
       - `void on_ui_repeat_minutes(uint8_t new_minutes);`
         - 값 변경 시 `controller_` 반영, `settings_dirty = true`, `ui_dirty = true`.
       - `void on_ui_freeze_protect(int16_t new_freeze_centi);`
         - 값 변경 시 `controller_` 반영, `settings_dirty = true`, `ui_dirty = true`.
     - **Matter(앱) 입력 핸들러** (에코 루프 방지: Matter로 다시 `push_matter_*` 세팅하지 않음!):
       - `void on_matter_mode(bool heat_mode);`
         - `heat_mode == false` → `Mode::kOff`.
         - `heat_mode == true` → 현재 이미 `kTemperature` 또는 `kRepeat`이면 현재 모드 유지(반복 모드를 덮어쓰지 않음!), 현재 `kOff`이면 `last_heat_mode_`로 복원.
         - 모드가 실제로 바뀌었으면 `controller_` 반영, `settings_dirty = true`, `ui_dirty = true`.
       - `void on_matter_setpoint(int16_t setpoint_centi);`
         - `sanitize_settings` 후 값이 달라졌으면 `controller_` 반영, `settings_dirty = true`, `ui_dirty = true`.
     - **센서 입력 및 설정 갱신**:
       - `void on_board_temp_sample(int16_t raw_centi, int64_t now_epoch_sec);`
       - `bool on_remote_sensors_json(const std::string& json, int64_t now_epoch_sec);`
         - `parse_homeserver_sensors_json` 성공 시 `record_remote_success`, 실패 시 `record_remote_failure` 호출 후 결과 반환.
       - `void on_remote_fetch_failed();`
       - `void set_homeserver_url(const char* url);` (`sanitize_settings`, 변경 시 `settings_dirty = true`)
       - `void set_simulated_temp(int16_t temp_centi);` / `void clear_simulated_temp();` / `bool has_simulated_temp() const;`
         - 시뮬레이션 온도 활성 시 `resolve` 대신 해당 유효 온도와 `"시뮬레이션 · 방금"` / `Freshness::kFresh` 사용(실기에서 SHT40·홈서버 없이도 전체 제어→Matter→UI 검증 가능).
     - **제어 스텝**:
       - `void step(uint64_t now_ms, int64_t now_epoch_sec);`
         - 온도 해석(시뮬레이션 또는 `sensor_selector_.resolve(now_epoch_sec)`) → `ZoneInput` 구성 → `controller_.step_all(...)`.
         - 유효 온도가 최초이거나 마지막 Matter 발행 온도 대비 `abs(temp - last_published_temp) >= 10` (0.1℃ 이상 변화)이면 `push_matter_temp = true`.
         - `last_published_matter_heating_`가 미발행이거나 `output.matter_heating`과 다르면 `push_matter_heating = true`.
         - UI 표시 상태(온도/난방여부/출처/분 단위 시각 등)가 바뀌면 `ui_dirty = true`.
     - **조회 및 액션 수거**:
       - `thermostat::ui::UiViewModel build_view_model(int64_t now_epoch_sec) const;`
         - `heating_active = last_output_.logical_heating` (유지 신호 펄스 중에도 화면의 난방 표시는 유지).
         - `source_severity`: `Freshness::kFresh → kNormal`, `kWarn → kWarning`, `kStaleOrFallback`/`kFault → kError`.
         - `time_text`: `now_epoch_sec >= 1700000000`이면 UTC+9(KST) 기준 `"HH:MM"` (`epoch_to_utc_datetime(now_epoch_sec + 9 * 3600, &dt)` 활용 또는 순수 정수 계산), 미동기화 시 `"--:--"`.
       - `SyncActions take_actions();`
       - `const thermostat::storage::DeviceSettings& settings() const;`
       - `const thermostat::core::ZoneOutput& last_output() const;`
       - `const thermostat::sensor::ResolvedTemperature& last_temp() const;`
       - `bool is_matter_heat_mode() const;` (`settings_.zones[0].control.mode != Mode::kOff`)

3. **통합 펌웨어 (`firmware/thermostat_main/thermostat_main.ino`, `firmware/thermostat_main/modules.txt`)**:
   - `modules.txt`:
     ```
     src/core
     src/sensor
     src/net
     src/storage
     src/matter
     src/ui/waveshare43b
     src/app
     ```
   - `thermostat_main.ino`:
     - `<Wire.h>`를 절대 include하지 않는다.
     - 공용 진입 헤더 포함: `thermostat_core.h`, `sensor_logic.h`, `sht40_driver.h`, `thermostat_net.h`, `thermostat_storage.h`, `thermostat_matter.h`, `lvgl_port.h`, `thermostat_ui.h`, `thermostat_app.h`.
     - `config/secrets.h` 조건부 include (`__has_include("config/secrets.h")` → `config/secrets.h.example` 폴백).
     - `portMUX_TYPE` 스핀락으로 보호되는 `PendingMatterEvents` / `PendingUiEvents` 구조체 구현.
     - **초기화 순서 (`setup()`)**:
       1. `Serial.begin(thermostat::pins::kSerialBaud)` (최대 3초 대기)
       2. `g_store.begin()` → `g_store.load(&loaded_settings)`. `loaded_settings.homeserver_url[0] == '\0'`이고 `!is_placeholder_value(HOMESERVER_BASE_URL)`이면 `HOMESERVER_BASE_URL`을 기본값으로 채운다.
       3. `g_app.init(loaded_settings, millis())`
       4. Matter 초기화:
          - `Matter.selectNetwork(MATTER_NETWORK_WIFI)`
          - `#if !CONFIG_ENABLE_CHIPOBLE`: `secrets.h` 유효 시 `matterConnectWiFi(WIFI_SSID, WIFI_PASSWORD)`
          - `g_matter.begin(matter_callbacks, /*enable_aux_contact=*/false)`
          - `Matter.begin()`
          - `g_matter.after_matter_begin(2100, loaded_settings.zones[0].control.target_temp_centi, g_app.is_matter_heat_mode())`
          - 미커미셔닝이면 `manual pairing code`와 `onboarding QR code` 출력(버퍼 드롭 방지 `delay(20)`).
       5. Waveshare 4.3B 디스플레이·터치·LVGL 초기화:
          - `g_board.init()` → `g_board.begin()` (`I2C_NUM_0` 자동 초기화)
          - `lvgl_port_init(g_board.getLCD(), g_board.getTouch())`
          - `lvgl_port_lock(-1)` → `g_ui.begin(ui_callbacks)` → `g_ui.update(...)` → `lvgl_port_unlock()`
       6. 공유 `I2C_NUM_0` 위에서 `g_sht40.begin(I2C_NUM_0)` (미연결 시 graceful skip) 및 `g_clock.begin(I2C_NUM_0)` (유효 RTC 시각 시 `restore_system_time_from_rtc()`).
       7. 홈서버 클라이언트 설정(`g_home_server.configure(...)`, API 키는 `secrets.h`에서, URL은 `g_app.settings().homeserver_url`에서).
       8. `g_boot_button.begin(BOOT_PIN)` (5초 길게 누르면 `Matter.decommission()`).
     - **메인 루프 (`loop()`)**:
       - `drain_pending_events()`: Matter/UI pending 이벤트를 `g_app`에 전달.
       - Wi-Fi 연결 상태 확인(`Matter.isWiFiConnected() || WiFi.status() == WL_CONNECTED`):
         - 최초 연결 시 비블로킹으로 `configTime(9L * 3600L, 0, "pool.ntp.org")` 1회 시작, 이후 시스템 시각이 유효해지면(`>= 1700000000`) 1회 `g_clock.write_rtc_epoch(...)` 저장(6시간마다 재동기화).
         - `g_home_server.configured() && g_home_server.should_poll(now_ms, 60000)`이면 `fetch_sensors()` 실행 → `mark_polled(now_ms)` → 성공 시 `g_app.on_remote_sensors_json(...)`, 실패 시 `g_app.on_remote_fetch_failed()`.
       - 5초마다 `g_sht40.is_present()`이면 SHT40 읽기 → `g_app.on_board_temp_sample(...)`.
       - 1초마다(또는 pending 이벤트 처리 직후) `g_app.step(now_ms, g_clock.now_epoch_sec())` 실행.
       - `SyncActions actions = g_app.take_actions();`:
         - `actions.settings_dirty` → `g_store.request_save(g_app.settings(), now_ms)`
         - `actions.push_matter_mode` → `g_matter.set_heat_mode(actions.matter_heat_mode)`
         - `actions.push_matter_setpoint` → `g_matter.set_heating_setpoint_centi(actions.matter_setpoint_centi)`
         - `actions.push_matter_temp` → `g_matter.set_local_temperature_centi(actions.matter_temp_centi)`
         - `actions.push_matter_heating` → `g_matter.set_heating_active(actions.matter_heating)`
         - `actions.ui_dirty` → `lvgl_port_lock(50)` 후 `g_ui.update(g_app.build_view_model(now_epoch))` + `lvgl_port_unlock()`
       - `g_store.tick(now_ms)` (실제 저장 시 `[main] NVS flushed` 로그).
       - 시리얼 진단 명령(`status`, `qr`, `sim <temp_c>`, `sim off`, `homeserver <url>`, `flush`, `reboot`, `decommission`, `help`).
       - `poll_boot_button()` + `delay(20)`.

4. **호스트 단위 테스트 (`test/test_thermostat_app.cpp`)**:
   - `// LINK: src/core/*.cpp src/sensor/sensor_logic.cpp src/storage/settings_codec.cpp src/app/thermostat_app.cpp`
   - 테스트 케이스(최소 10개):
     1. `init`: NVS 설정 복원 → 컨트롤러·센서 선택기·초기 `SyncActions`·`UiViewModel` 일치.
     2. `ui_to_matter_and_nvs`: UI에서 목표온도·모드(`kTemperature`)·반복 시간·최저온도 변경 시 `settings_dirty`, `push_matter_*`, `ui_dirty` 생성 및 동일 값 재입력 시 무시.
     3. `matter_to_ui_no_echo`: 앱(Matter)에서 목표온도·모드 변경 시 `settings_dirty` + `ui_dirty`는 켜지되 `push_matter_*` 에코는 꺼짐.
     4. `repeat_mode_preserved_on_matter_heat`: UI에서 `kRepeat` 설정 상태에서 앱 `Heat` 수신 시 `kRepeat` 유지; `kOff` 전환 후 다시 앱 `Heat` 수신 시 `last_heat_mode_`(`kRepeat`) 복원.
     5. `sensor_to_core_to_matter_heating`: 보드 온도 샘플이 목표온도 이하이면 `step()`에서 `matter_heating=true`, `push_matter_heating=true`, `push_matter_temp=true` 발행, 0.05℃ 미세 변동은 `push_matter_temp` 생략, 0.1℃ 이상 변화 시 재발행.
     6. `remote_sensor_json_and_fallback`: `SCRUM-239` 표본 JSON 입력 시 원격 센서 온도·라벨(`거실 센서 · ...`) 사용, 3회 연속 실패 시 보드 센서로 자동 전환 + `SourceSeverity::kError` 확인.
     7. `keep_alive_pulse_ui_vs_matter`: 연속 난방 50분 경과 시 유지 신호 펄스 동안 `matter_heating=false`가 발행되지만 UI의 `heating_active`는 `true`(`logical_heating`)로 유지.
     8. `simulated_temp_override`: `set_simulated_temp(1850)` 시 유효 온도·난방 판단 동작, `clear_simulated_temp()` 시 원래 센서 경로 복귀.
     9. `kst_time_formatting`: `now_epoch_sec`가 유효할 때 KST(`UTC+9`) `"HH:MM"` 포맷, 미동기화(`0`) 시 `"--:--"`.
     10. `homeserver_url_update`: `set_homeserver_url` 변경 시 `settings_dirty=true` 및 `settings()` 반영.

---

## Review Focus

1. `src/sensor/sht40_driver.*`, `src/net/clock_rtc.*`, `demos/02_io/02_io.ino`, `firmware/thermostat_main/thermostat_main.ino` 어디에도 `<Wire.h>`가 남지 않고 `<driver/i2c.h>` 구형 드라이버로 통일되어 `ESP32_Display_Panel`과 부팅 충돌이 없는지.
2. `firmware/thermostat_main/thermostat_main.ino`에서 Matter 콜백(CHIP 태스크)과 UI 콜백(LVGL 태스크)이 상대 스택 락을 잡지 않고 크리티컬 섹션 보호 pending 이벤트로만 `loop()`에 전달해 데드락·데이터 레이스가 없는지, 그리고 `CONFIG_ENABLE_CHIPOBLE=1`에서 `WiFi.begin()`을 수동 호출하지 않는지.
3. `src/app/thermostat_app.*`가 순수 C++17로 분리되어 앱↔UI 양방향 동기화(에코 루프 방지, 반복 모드 보존, 유지 신호 시 UI vs Matter 분리, 원격 센서 자동 전환)가 호스트 테스트로 검증되고, `demos/02_io`와 `firmware/thermostat_main` ESP32 빌드가 모두 성공하는지.
