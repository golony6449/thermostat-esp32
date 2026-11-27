# 원격 센서 온라인 판정 계획 (SCRUM-317)

**목표:** 값이 바뀔 때만 보고하는 SmartThings 센서가, 보고가 10분 넘게 없어도 홈서버가 "온라인"이라고 확인해 주면 그 값을 계속 쓴다.

**배경(2026-11-27 실측):** SCRUM-313 실기 확인에서 거실 센서의 마지막 보고가 34분 전이라 `kStaleAgeSec`(10분)에 걸려 제외됐고, 보드 센서(SHT40)가 아직 없어 화면이 "센서 오류 · 온도 확인 불가"가 됐다. 홈서버 쪽 SCRUM-316이 같은 계약으로 `online`·`checked_at` 필드를 추가한다(동시 진행).

리스크하기 때문에 Claude가 최종 검증 진행 (센서 판정은 온도 모드 난방 여부와 최저온도 보호에 바로 닿는다)

## Global Constraints

- `sensor/`의 순수 C++ 부분은 하드웨어 호출 없이 호스트 테스트(`python tools/test_host.py`)로 검증한다. 새 분기마다 테스트를 추가한다.
- 시간 임계값은 `sensor_logic.h`의 이름 있는 상수로 둔다.
- 안전 관련 동작(센서 이상 판정)을 바꾸므로 커밋 메시지에 변경 이유를 적는다(CLAUDE.md 15절).
- 새 필드는 **선택**이다. 필드가 없거나 형식이 틀리면 그 센서는 "온라인 확인 안 됨"으로 보고 지금과 똑같이 동작한다. 새 필드 때문에 응답 전체나 그 센서 항목을 버리지 않는다.
- 비밀 값(홈서버 실제 주소, API 키)은 코드·테스트·로그에 넣지 않는다.

## API 계약 (SCRUM-316과 동일)

```
GET /api/thermostat/sensors
{"sensors":[{"id":"livingroom","name":"거실","temperature":21.0,
             "updated_at":"2026-11-26T22:41:37+09:00",
             "online":true,"checked_at":"2026-11-26T23:15:37+09:00"}]}
```
- `online`(bool): SmartThings 기기 health가 ONLINE이면 true.
- `checked_at`(ISO 8601, 오프셋 포함): 홈서버가 상태와 health를 마지막으로 함께 확인한 시각. 홈서버 조회가 멈추면 이 값이 늙는다.
- 홈서버가 아직 한 번도 health를 못 읽었으면 두 필드는 아예 없다(null로 오지 않는다).

## 실제 표본

홈서버 SCRUM-313 실측 응답(2026-11-26)에 위 계약의 두 필드를 붙인 표본이다(홈서버 배포 전이라 두 필드는 계약 기준).

```
{"sensors":[{"id":"livingroom","name":"거실","temperature":21.0,"updated_at":"2026-11-26T22:41:37+09:00","online":true,"checked_at":"2026-11-26T23:15:37+09:00"}]}
```
- 파싱 기대값: 항목 1개, `temp_centi == 2100`, `updated_epoch_sec == 1795700497`, `online_known == true`, `online == true`, `checked_epoch_sec == 1795702537`.
- 판정 기대값(`kSingleRemote`, 선택 id `livingroom`, 보드 센서 없음, `now = 1795702567`): `valid == true`, `temp_centi == 2100`, `used_fallback_board == false`, `freshness == Freshness::kFresh`, `age_sec == 2070`, `source_label == "거실 센서 · 34분 전"`.
- 같은 표본에서 `"online":false`이면: 보드 센서가 없으므로 `valid == false`, `source_label == "센서 오류 · 온도 확인 불가"`.
- 두 필드를 뺀 원래 SCRUM-313 응답 `{"sensors":[{"id":"livingroom","name":"거실","temperature":21.0,"updated_at":"2026-11-26T22:41:37+09:00"}]}`과 같은 `now`이면: 지금과 같이 `valid == false`(오래됨 → 보드 없음).

## 작업

### 작업 1: 선택 필드 파싱과 온라인 판정

**Files:**
- Modify: `src/sensor/sensor_logic.h`
- Modify: `src/sensor/sensor_logic.cpp`
- Test: `test/test_sensor_logic.cpp`
- Modify: `CLAUDE.md`

1. `RemoteSensorReading`에 필드 추가:
   ```cpp
   bool online_known = false;       // "online"과 "checked_at"을 둘 다 제대로 읽었는가
   bool online = false;
   int64_t checked_epoch_sec = 0;
   ```
2. `parse_sensor_object`: `"online"` 키는 `true`/`false` 리터럴이면 값을 기억하고, 그 밖의 값(null, 문자열, 숫자)은 `skip_value`로 건너뛰고 모름으로 둔다. `"checked_at"`은 문자열이고 `parse_iso8601_to_epoch`가 성공하면 기억하고, 문자열이 아니면 `skip_value`, 시각이 틀리면 모름으로 둔다. 두 값을 모두 얻었을 때만 `online_known = true`. 두 필드는 `complete` 판정(필수 필드)에 넣지 않는다. bool 리터럴을 읽는 작은 함수가 필요하면 `JsonReader`에 `bool parse_bool(bool* out)`을 추가한다(위치를 되돌리는 방식은 `temperature` 처리와 같게).
3. 상수 추가(`kStaleAgeSec` 옆):
   ```cpp
   // A remote value whose last report is older than kStaleAgeSec is still used
   // when the home server confirmed the device online within kStaleAgeSec and
   // the report is not older than this cap (guards a stuck temperature value).
   constexpr int64_t kOnlineMaxReportAgeSec = 3 * 3600;
   ```
4. `resolve_remote`의 후보 판정(선택된 센서마다, 위에서부터 처음 맞는 것):
   1. `age_sec <= kStaleAgeSec` → 후보(지금과 같음). `online_confirmed = false`.
   2. `online_known && online && checked_age <= kStaleAgeSec && age_sec <= kOnlineMaxReportAgeSec` → 후보, `online_confirmed = true`. `checked_age = now - checked_epoch_sec`(음수면 0).
   3. 그 밖 → 제외(지금과 같음). 모두 제외면 기존대로 `"원격 센서 값 오래됨"` 보드 전환.
5. 주의 색: `freshness`는 `online_confirmed == false`인 후보 중 `age_sec > kWarnAgeSec`인 것이 하나라도 있으면 `kWarn`, 아니면 `kFresh`. `age_sec`(표시용 최대 경과)와 라벨 문구는 지금처럼 모든 후보의 `updated_at` 경과 중 최댓값으로 만든다.
6. `SensorSelector` 클래스 설명 주석의 판정 규칙 목록을 새 규칙에 맞게 고친다.
7. `CLAUDE.md` 10절: API 계약 예시에 `online`, `checked_at`을 넣고 뜻을 한 줄씩 적는다. "ESP32 동작"의 자동 전환 줄을 "`updated_at`이 10분 넘게 지났고, 홈서버가 10분 안에 온라인을 확인하지 않았거나 보고가 3시간을 넘었으면 → 보드 센서로 자동 전환"으로 바꾼다. 11절의 "5분 초과는 주의 색상" 줄에 "(홈서버가 온라인을 확인한 센서는 제외)"를 덧붙인다.
8. 테스트(`test_sensor_logic.cpp`, 기존 형식 그대로):
   - "실제 표본"의 세 경우를 그대로 넣은 테스트(online true / false / 필드 없음).
   - 파싱: `online`이 null·문자열·숫자, `checked_at`이 숫자·오프셋 없는 문자열, 한쪽 필드만 있음 → 항목은 남고 `online_known == false`.
   - 판정 경계: `checked_age == kStaleAgeSec`(사용) / `+1`(제외), `age_sec == kOnlineMaxReportAgeSec`(사용) / `+1`(제외).
   - 평균 모드: 온라인 확인 센서(34분 전)와 보통 센서(6분 전)를 평균하면 `kWarn`(보통 센서가 5분 초과), 보통 센서가 2분 전이면 `kFresh`.
   - 연속 실패 3회(`record_remote_failure`)면 온라인이어도 보드 전환(기존 규칙 유지).

## Review Focus

1. 새 필드가 없거나 형식이 틀린 응답에서 동작이 지금과 정확히 같은가(항목을 버리지 않고, 온라인으로도 보지 않는가).
2. `checked_at`이 늙으면(홈서버 조회 멈춤) 온라인 판정을 쓰지 않는가. 3시간 상한이 적용되는가. 경계(`<=`)가 계획과 같은가.
3. 연속 실패 3회 보드 전환, 선택 id 없음, 비현실적 온도 제외 같은 기존 규칙이 그대로인가.
4. 주의 색 판정이 온라인 확인 후보를 빼고 계산되고, 라벨의 경과 시간은 여전히 `updated_at` 기준인가.
5. "실제 표본" 테스트가 계획의 기대값(1795700497, 1795702537, 2070, "거실 센서 · 34분 전")을 그대로 단언하는가.
