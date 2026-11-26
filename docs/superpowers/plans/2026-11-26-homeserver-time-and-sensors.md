# 홈서버 시각·원격 온도 연동 계획 (SCRUM-313)

**목표:** 펌웨어가 홈서버의 `GET /api/thermostat/sensors`(원격 온도)와 `GET /api/thermostat/time`(시각)을 실제로 받아 쓴다.

**배경(2026-11-26 실측):**
- 홈서버 LAN 주소에서 두 API 모두 HTTP 200(아래 "실제 표본").
- 펌웨어는 센서 API를 1분마다 부르지만 매번 `home server fetch failed: Wi-Fi is not connected`로 실패한다. `HomeServerClient::fetch_sensors()`가 `WiFi.status() != WL_CONNECTED`로 막는데, Matter가 STA를 소유하는 빌드(CHIPoBLE)에서는 Matter로 연결돼 있어도 `WiFi.status()`가 `254`다. 루프(`thermostat_main.ino`의 `loop()`)는 이미 `Matter.isWiFiConnected() || WiFi.status() == WL_CONNECTED`로 연결을 판단한 뒤에만 `poll_home_server()`를 부른다.
- 시각 API를 부르는 펌웨어 코드는 없다(NTP + RTC만).

## Global Constraints

- `core/`·`sensor/`의 순수 C++ 부분은 하드웨어 호출 없이 호스트 테스트(`python tools/test_host.py`)로 검증한다. 새 순수 함수에는 테스트를 함께 추가한다.
- 하드웨어 상수·시간 값은 코드에 흩지 않고 이름 있는 상수로 둔다.
- 비밀 값(홈서버 주소의 실제 값, API 키)은 커밋하지 않는다. 주소는 기존처럼 `secrets.h`/시리얼 `homeserver <url>`(NVS 저장)로 넣는다. `secrets.h.example`의 예시 주소는 바꾸지 않는다.
- Arduino 전용 코드는 `#ifdef ARDUINO` 안에 둔다. 펌웨어 빌드는 `python tools/build.py firmware/thermostat_main`(업로드 없이)로 확인한다.
- `<Wire.h>`를 include하지 않는다(ESP-IDF 레거시 I2C와 충돌).

## 실제 표본

홈서버 응답 원문(2026-11-26):

```
GET /api/thermostat/time
{"now":"2026-11-26T22:48:16+09:00","epoch":1795700896}
```
- 기대값: `parse_homeserver_time_json` → `true`, `*out_epoch_sec == 1795700896`

```
GET /api/thermostat/sensors
{"sensors":[{"id":"livingroom","name":"거실","temperature":21.0,"updated_at":"2026-11-26T22:41:37+09:00"}]}
```
- 기대값: `parse_homeserver_sensors_json` → `true`, 항목 1개, `id == "livingroom"`, `name == "거실"`, `temp_centi == 2100`, `updated_epoch_sec == 1795700497`

## 작업

### 작업 1: 시각 응답 파서와 시각 조회 정책(순수 C++)

**Files:**
- Modify: `src/sensor/sensor_logic.h`
- Modify: `src/sensor/sensor_logic.cpp`
- Modify: `src/net/homeserver_client.h`
- Test: `test/test_sensor_logic.cpp`
- Create: `test/test_home_time_policy.cpp`

1. `sensor_logic.h`/`.cpp`에 추가:
   ```cpp
   // Parses GET /api/thermostat/time: {"now": "<ISO-8601 with offset>", "epoch": <int>}.
   // Uses "now" (parse_iso8601_to_epoch). Unknown keys are skipped. Returns false
   // on structural errors or when "now" is missing/invalid; *out is untouched then.
   bool parse_homeserver_time_json(const std::string& json, int64_t* out_epoch_sec);
   ```
   기존 `JsonReader`(`parse_string`, `skip_value`, `consume`)를 그대로 쓴다. `epoch` 값은 `skip_value`로 건너뛴다(정수 파서를 새로 만들지 않는다).
2. `homeserver_client.h`의 `#ifdef ARDUINO` **밖**(호스트에서도 컴파일되는 곳)에 inline 순수 함수 추가:
   ```cpp
   struct HomeTimePolicyInput {
     bool ntp_synced = false;          // SNTP가 한 번이라도 동기화를 끝냈는가
     bool has_home_time = false;       // 홈서버 시각을 한 번이라도 적용했는가
     uint64_t now_ms = 0;
     uint64_t ntp_started_ms = 0;      // configTime() 호출 시각
     bool has_attempted = false;       // 홈서버 시각 조회를 한 번이라도 시도했는가
     uint64_t last_attempt_ms = 0;
   };
   constexpr uint64_t kHomeTimeNtpGraceMs = 30000;          // NTP에 먼저 기회를 준다
   constexpr uint64_t kHomeTimeRetryMs = 60000;             // 성공 전 재시도 간격
   constexpr uint64_t kHomeTimeRefreshMs = 6ULL * 3600ULL * 1000ULL;  // 성공 뒤 갱신 간격
   inline bool should_fetch_home_time(const HomeTimePolicyInput& in);
   ```
   규칙: `ntp_synced`이면 false. `now_ms - ntp_started_ms < kHomeTimeNtpGraceMs`이면 false. 시도한 적 없으면 true. 성공 전(`!has_home_time`)에는 마지막 시도에서 `kHomeTimeRetryMs` 이상, 성공 뒤에는 `kHomeTimeRefreshMs` 이상 지났을 때 true.
3. 테스트: "실제 표본"의 두 응답을 그대로 넣은 테스트(시각 1795700896, 센서 값). 시각 파서 실패 사례(`{}`, `now` 없음, `now`가 숫자, 깨진 JSON, `now`에 오프셋 없음). 정책 함수의 각 분기(NTP 동기화됨, 유예 중, 첫 시도, 재시도 간격 전·후, 성공 뒤 6시간 전·후). 새 테스트 파일 맨 위에는 필요한 `// LINK:` 줄을 기존 테스트 파일 형식대로 둔다(정책 함수는 헤더 inline이라 링크할 파일이 없을 수 있다).

### 작업 2: 홈서버 클라이언트 연결 판단 수정과 시각 조회

**Files:**
- Modify: `src/net/homeserver_client.h`
- Modify: `src/net/homeserver_client.cpp`

1. `fetch_sensors()`에서 `WiFi.status() != WL_CONNECTED` 검사를 뺀다. 연결 판단은 호출하는 쪽(루프)이 한다는 주석을 클래스 설명에 적는다. 연결이 없으면 `HTTPClient` 요청이 실패하고 기존 오류 경로(`HTTP request failed (...)`)로 간다.
2. 공통 GET을 private 함수 `FetchResult fetch_path(const char* path)`로 빼고, `fetch_sensors()`는 `/api/thermostat/sensors`, 새 `FetchResult fetch_time()`은 `/api/thermostat/time`을 부른다. API 키 헤더·타임아웃 처리는 지금과 같다.
3. `should_poll`/`mark_polled`(센서 1분 주기)는 그대로 둔다.

### 작업 3: 펌웨어에서 시각 적용

**Files:**
- Modify: `firmware/thermostat_main/thermostat_main.ino`
- Modify: `src/net/clock_rtc.h`
- Modify: `src/net/clock_rtc.cpp`

1. `ClockRtc`에 `bool set_system_epoch(int64_t epoch_sec);` 추가(`settimeofday`, `restore_system_time_from_rtc`와 같은 방식).
2. `poll_network_and_time()`에서 `configTime()`을 부른 시각을 기억하고, SNTP 동기화 완료를 `esp_sntp.h`의 `sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`로 확인해 정적 변수에 **한 번 래치**한다(이 함수는 완료를 한 번만 돌려주고 다시 RESET으로 돌아간다).
3. 새 함수 `poll_home_time(uint64_t now_ms)`를 `poll_home_server()` 옆에 두고 루프에서 `wifi_connected`일 때 부른다. `should_fetch_home_time()`이 true면 `fetch_time()` → `parse_homeserver_time_json()` → 성공 시 `kSaneEpochSec` 이상인지 확인하고 `set_system_epoch()`, RTC가 있으면 `write_rtc_epoch()`. 로그: `[main] time set from home server epoch=<값> (diff=<초>s)`, 실패 시 `[main] home time fetch failed: <이유>`.
4. `print_status()` 출력에 `ntp_synced=<0|1> home_time=<0|1>`를 덧붙인다.

## Review Focus

1. Matter 소유 STA에서 센서·시각 조회가 실제로 호출되는가(`WiFi.status()` 검사 제거, 루프의 연결 판단 사용).
2. 시각 파서가 실제 표본에서 1795700896을 내는가, 오프셋 없는 시각·`now` 누락을 거부하는가.
3. 정책 함수: NTP가 동기화된 뒤에는 홈서버 시각을 절대 적용하지 않는가, 재시도·갱신 간격 경계가 맞는가.
4. `sntp_get_sync_status()` 래치(한 번만 COMPLETED를 주는 동작)를 놓치지 않는가.
5. 비밀 값(실제 주소·키)이 코드·테스트·로그에 들어가지 않았는가.
