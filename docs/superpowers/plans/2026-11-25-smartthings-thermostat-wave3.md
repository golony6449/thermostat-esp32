# SmartThings DIY 온도조절기 구현 계획 — Wave 3 (SCRUM-238: 설정 저장 NVS)

> 앞 회차: `docs/superpowers/plans/2026-11-24-smartthings-thermostat-wave2.md` (`SCRUM-234` 머지 완료)
> 대상 일감: `SCRUM-238` (`plan-smartthings-thermostat`, 에픽 `SCRUM-230`)
> 기준 문서: `CLAUDE.md` (6, 12, 15절), 실제 타입: `src/core/thermostat_types.h`

---

## Global Constraints

- **병렬 충돌 방지**: Wave 2의 다른 일감(`SCRUM-233`, `236`, `237`)이 동시에 진행 중일 수 있다. 이 작업은 `src/storage/*`, `demos/04_storage/*`, `test/test_storage_codec.cpp`만 생성하며, 기존 `.gitkeep`·`src/core/*`·`tools/*`·`CLAUDE.md`는 건드리지 않는다.
- **`library.properties` 규칙**: `author=golony6449`, `maintainer=golony6449` 사용.
- **순수 C++ 직렬화 분리**: 바이너리 직렬화/역직렬화·체크섬(CRC-32)·스키마 버전 검증·쓰기 지연(debounce/wear-leveling) 상태 머신은 `Arduino.h`/`Preferences.h` 의존성이 전혀 없는 순수 C++17로 구현하고 호스트 단위 테스트로 검증한다.
- **ESP32 NVS 어댑터**: `#ifdef ARDUINO` 블록에서만 ESP32 내장 `<Preferences.h>`를 사용한다(네임스페이스 `"thermostat"`, 키 `"cfg_blob"`).
- **COM9 점유 금지**: DeepSeek `--check`는 호스트 테스트 + `python tools/build.py demos/04_storage` 컴파일만 실행한다. 실제 보드 재부팅 복원 시험은 설계 PL이 머지 후 수행한다.

---

## 실제 표본

이미 머지된 `src/core/thermostat_types.h`의 실제 `ZoneConfig` 구조:

```cpp
namespace thermostat::core {
constexpr size_t kMaxZones = 4;
enum class Mode : uint8_t { kOff = 0, kTemperature = 1, kRepeat = 2 };
enum class StrategyKind : uint8_t { kHysteresis = 0 };
struct ZoneConfig {
  Mode mode = Mode::kOff;
  int16_t target_temp_centi = 2100;
  uint8_t repeat_on_minutes = 15;
  int16_t freeze_protect_centi = 1000;
  int16_t hysteresis_centi = 50;
  uint16_t min_on_sec = 300;
  uint16_t min_off_sec = 300;
  uint16_t keep_alive_interval_sec = 3000;
  uint16_t keep_alive_off_sec = 10;
  StrategyKind strategy = StrategyKind::kHysteresis;
};
}
```

---

### 작업 1: 설정 저장(NVS) — 직렬화 코덱, 쓰기 지연, ESP32 Preferences 어댑터 및 데모 (`SCRUM-238`)

**Files:**
- Create: `src/storage/library.properties`
- Create: `src/storage/thermostat_storage.h`
- Create: `src/storage/settings_codec.h`
- Create: `src/storage/settings_codec.cpp`
- Create: `src/storage/nvs_store.h`
- Create: `src/storage/nvs_store.cpp`
- Create: `demos/04_storage/04_storage.ino`
- Create: `demos/04_storage/modules.txt`
- Test: `test/test_storage_codec.cpp`

**요구사항 및 세부 명세:**

1. **`src/storage/library.properties`**:
   - `name=thermostat_storage`, `version=0.1.0`, `author=golony6449`, `maintainer=golony6449`, `sentence=Thermostat NVS settings storage`, `architectures=*`.
2. **영구 설정 모델 및 순수 C++ 직렬화 코덱 (`src/storage/settings_codec.h`, `.cpp`)**:
   - `namespace thermostat::storage`:
   - `constexpr uint32_t kSettingsMagic = 0x54485354u;` (`"THST"`)
   - `constexpr uint16_t kSettingsSchemaVersion = 1;`
   - `constexpr size_t kMaxSensorIdLen = 32;`
   - `constexpr size_t kMaxSelectedRemoteSensors = 4;`
   - `constexpr size_t kMaxUrlLen = 128;`
   - `enum class SensorSourceSetting : uint8_t { kBoardOnly = 0, kSingleRemote = 1, kAverageRemote = 2 };`
   - `struct ZonePersistedSettings`:
     - `thermostat::core::ZoneConfig control;` (목표온도, 모드, 반복 N분, 최저온도, 히스테리시스, 최소 켜짐/꺼짐 시간, 유지 신호 간격/펄스, 전략)
     - `SensorSourceSetting sensor_source = SensorSourceSetting::kBoardOnly;`
     - `uint8_t selected_remote_count = 0;`
     - `char selected_remote_ids[kMaxSelectedRemoteSensors][kMaxSensorIdLen] = {};`
     - `int16_t sensor_offset_centi = 0;` (센서 보정값, 범위 -500..500)
   - `struct DeviceSettings`:
     - `uint8_t active_zones = 1;` (1..`kMaxZones`)
     - `ZonePersistedSettings zones[thermostat::core::kMaxZones];`
     - `char homeserver_url[kMaxUrlLen] = "";` (비밀 API 키는 여기에 저장하지 않음!)
   - `DeviceSettings default_device_settings();`
   - `void sanitize_settings(DeviceSettings* settings);` (모든 필드 범위 클램프 및 문자열 null-terminate 보장)
   - **결정론적 리틀엔디언 직렬화 + CRC-32 검증** (`struct` 메모리 패딩/정렬에 의존하지 않고 필드별 바이트 패킹!):
     - 헤더: `magic(4B)` + `schema_version(2B)` + `payload_len(2B)` + `payload` + `crc32(4B)`
     - `uint32_t compute_crc32(const uint8_t* data, size_t len);` (표준 IEEE 802.3 CRC-32, poly `0xEDB88320`)
     - `std::vector<uint8_t> serialize_settings(const DeviceSettings& settings);`
     - `enum class DecodeStatus : uint8_t { kOk = 0, kEmptyOrShort, kBadMagic, kUnsupportedVersion, kBadLength, kBadCrc };`
     - `DecodeStatus deserialize_settings(const uint8_t* data, size_t len, DeviceSettings* out);`
       - 실패 시 `*out = default_device_settings();`로 안전하게 초기화하고 오류 상태 반환.
   - **플래시 수명 보호용 쓰기 지연(Debounce) 상태 머신 (`class DeferredWriteGuard`)**:
     - `explicit DeferredWriteGuard(uint64_t debounce_ms = 5000, uint64_t min_write_interval_ms = 30000);`
     - `void mark_dirty(uint64_t now_ms, const std::vector<uint8_t>& candidate_blob);`
       - 마지막으로 실제 저장한 `last_written_blob`과 바이트가 완전히 동일하면 dirty를 해제해 불필요한 플래시 쓰기를 막는다!
     - `bool should_flush(uint64_t now_ms) const;`
       - `dirty == true` 이고, 마지막 변경으로부터 `debounce_ms` 이상 경과했으며, 마지막 실제 쓰기로부터 `min_write_interval_ms` 이상 경과했을 때만 `true`(최초 쓰기는 `min_write_interval_ms` 제약 없이 `debounce_ms`만 지나면 허용).
     - `void mark_flushed(uint64_t now_ms, const std::vector<uint8_t>& written_blob);`
     - `bool is_dirty() const;`
3. **ESP32 NVS 저장소 어댑터 (`src/storage/nvs_store.h`, `.cpp`, `thermostat_storage.h`)**:
   - `thermostat_storage.h`: `settings_codec.h`와 `nvs_store.h`를 포함하는 공용 진입 헤더.
   - `#ifdef ARDUINO` 클래스 `NvsSettingsStore`:
     - `bool begin(const char* ns = "thermostat", const char* key = "cfg_blob");`
     - `DecodeStatus load(DeviceSettings* out);` (NVS에 키가 없거나 깨졌으면 기본값으로 복원)
     - `void request_save(const DeviceSettings& settings, uint64_t now_ms);` (`DeferredWriteGuard`에 위임)
     - `bool tick(uint64_t now_ms);` (`should_flush` 시 `Preferences::putBytes`로 실제 저장 후 `true` 반환)
     - `bool save_now(const DeviceSettings& settings, uint64_t now_ms);` (테스트/명시적 즉시 저장용)
     - `bool erase();`
4. **`demos/04_storage/04_storage.ino` & `modules.txt`**:
   - `modules.txt`: `src/core`, `src/storage`
   - 부팅 시 `NvsSettingsStore::load` 호출해 복원 결과(`kOk` vs `kEmptyOrShort` 기본값)와 현재 저장된 목표온도·모드·반복 N분·최저온도·홈서버 URL을 시리얼에 출력.
   - 시리얼 명령어 지원:
     - `show`: 현재 메모리 설정 출력
     - `set <temp_c> <mode:off|temp|repeat> <repeat_min>`: 설정 변경(지연 저장 예약)
     - `flush`: 즉시 저장 (`save_now`)
     - `erase`: NVS 초기화
     - `reboot`: `ESP.restart()` (재부팅 뒤 복원 확인용!)
   - 매 `loop()`에서 `store.tick(millis())` 호출, 실제 저장이 발생하면 `[04_storage] NVS flushed` 출력.
5. **`test/test_storage_codec.cpp`** (`// LINK: src/storage/settings_codec.cpp`):
   - 기본 설정 왕복 직렬화/역직렬화 일치
   - 커스텀 설정(존별 목표온도·모드·반복 N분·최저온도·센서 선택·보정값·히스테리시스·최소 시간·홈서버 URL) 왕복 일치
   - 빈 버퍼/짧은 버퍼(`kEmptyOrShort`), 잘못된 매직(`kBadMagic`), 미래 스키마 버전(`kUnsupportedVersion`), 1비트 손상(`kBadCrc`) 시 모두 기본값으로 안전 복원
   - `sanitize_settings` 범위 클램프 확인
   - `DeferredWriteGuard`: 연속 변경 시 `debounce_ms` 동안 쓰기 보류, 변경 후 원래 값으로 되돌리면 쓰기 생략, `min_write_interval_ms` 쿨다운 동작 확인

---

## Review Focus

1. `src/storage/settings_codec.*`가 `Arduino.h`/`Preferences.h` 의존성 없는 순수 C++17이고, 구조체 패딩에 의존하지 않는 리틀엔디언 패킹 + CRC-32 + 스키마 버전 검증을 구현하는지.
2. `DeferredWriteGuard`가 동일 내용 재저장 생략, 디바운스 대기, 최소 쓰기 간격 제한을 정확히 지켜 플래시 수명을 보호하는지.
3. `src/storage/library.properties`가 `author=golony6449`를 사용하고, Wave 2의 다른 일감 파일들과 충돌하지 않으며, 호스트 단위 테스트와 `demos/04_storage` ESP32 컴파일이 모두 통과하는지.
