# SmartThings DIY 온도조절기 구현 계획 — Wave 4 (UI 실기 개선: SCRUM-248 ~ SCRUM-251)

> 앞 회차: `docs/superpowers/plans/2026-11-24-smartthings-thermostat-wave2.md` (`SCRUM-237` 완료 및 실기 터치 검증)
> 대상 일감: `SCRUM-248`, `SCRUM-249`, `SCRUM-250`, `SCRUM-251` (`plan-smartthings-thermostat`, 에픽 `SCRUM-230`)
> 기준 문서: `CLAUDE.md` (11절), 실기 피드백(2026-11-25)

---

## Global Constraints

- **통합 묶음 구현**: `SCRUM-248`~`251` 4개 일감은 모두 같은 UI 파일(`ui_view_model.h`, `thermostat_ui.h/.cpp`, `gen_kr_font.py`, `test_ui_view_model.cpp`)을 수정하므로 하나의 브랜치(`feature/SCRUM-248`)와 PR에서 작업 1~4를 일관되게 구현한다.
- **`library.properties` 유지**: `src/ui/waveshare43b/library.properties`는 수정하지 않는다(`author=golony6449` 유지).
- **순수 C++17 뷰 모델**: `src/ui/waveshare43b/ui_view_model.h`는 `Arduino.h`, `lvgl.h` 의존성 없이 PC 호스트 `g++ -std=c++17 -Wall -Wextra -Werror`로 컴파일·테스트되어야 한다.
- **대형 폰트 생성 및 커밋**: `tools/gen_kr_font.py`를 확장해 `kr_font_48.c` (`LV_FONT_DECLARE(kr_font_48)`)를 `uv run --with pillow python tools/gen_kr_font.py`로 실제 생성해 커밋한다. 기존 `kr_font_20.c`, `kr_font_28.c`도 새 문구(예: `반복 시간`)가 빠짐없이 포함되도록 함께 갱신한다.
- **COM9 점유 금지**: DeepSeek 검증(`--check`)은 호스트 테스트(`python tools/test_host.py --filter ui`)와 컴파일(`python tools/build.py demos/03_ui`)만 실행한다. 실기 업로드·화면 확인은 PR 검증 후 수행한다.

---

## 실제 표본

### 현재 `thermostat_ui.cpp`에서 확인된 문제 (2026-11-25 실기 확인)

1. `create_button()`에서 버튼 라벨의 `lv_obj_set_style_text_color`를 지정하지 않아 LVGL 기본 검정 글자가 다크 회색 버튼(`0x2A323D`) 위에 그려져 `+/-`, `설정`, `닫기` 버튼 글자가 거의 보이지 않았다.
2. 기존 800×480 좌우 분할(좌 470px 상태, 우 270px 조작)에서 조작 영역이 좁고 `+/-` 버튼이 `64×64`, 폰트가 최대 `28px`이라 벽걸이 온도조절기로 쓰기 작았다.
3. `UiMode::kRepeat`에서도 `target_temp_label_`이 항상 `format_temp_celsius(vm_.target_temp_centi, ...)`만 표시하고, `+/-` 버튼이 목표온도만 조절했다.
4. `UiMode::kOff`에서도 목표온도가 그대로 보이고 `+/-` 버튼이 클릭·조절 가능했다.

### 800×480 상하 분할 목표 레이아웃 치수표 (잘림·겹침 없음 보장)

```text
전체 화면: 800 x 480 (배경 #0B1220)

[상단 상태 카드] x=16, y=12, w=768, h=156 (배경 #162032, 테두리 1px #2D3F59, radius=18)
  - 좌상: "현재 온도" (kr_font_20, #94A3B8) x=24, y=14
  - 좌중: 현재 온도 값 "21.4℃" (kr_font_48, #F8FAFC) x=22, y=38
  - 현재 온도 옆: 난방 상태 배지 ("난방 중" / "대기", kr_font_20) x=216, y=46, w=124, h=42
  - 좌하: 온도 출처 배지 ("거실 센서 · 3분 전", kr_font_20) x=22, y=104, w=580, h=40
  - 우상: 현재 시각 "21:15" (kr_font_28, #CBD5E1) x=636, y=16, w=112, 우측 정렬
  - 우하: [설정] 버튼 (kr_font_20, 고대비 테두리) x=628, y=72, w=120, h=64

[하단 메인 조작 카드] x=16, y=180, w=768, h=288 (배경 #162032, 테두리 1px #2D3F59, radius=18)
  - 상단 중앙: 모드별 메인 캡션 ("목표 온도" / "반복 시간" / "꺼짐", kr_font_20, #CBD5E1) y=14, TOP_MID
  - 중단 조작 행 (y=48, h=124):
      * 좌측 대형 [-] 버튼: x=36,  y=54, w=156, h=112, radius=20, kr_font_48 "-"
      * 중앙 대형 조작값:   x=208, y=78, w=352, align=CENTER, kr_font_48 ("22.5℃" / "15분" / "-")
      * 우측 대형 [+] 버튼: x=576, y=54, w=156, h=112, radius=20, kr_font_48 "+"
  - 하단 모드 세그먼트 행 (y=194, h=74):
      * [꺼짐]: x=24,  y=196, w=228, h=72, radius=16, kr_font_28
      * [온도]: x=270, y=196, w=228, h=72, radius=16, kr_font_28
      * [반복]: x=516, y=196, w=228, h=72, radius=16, kr_font_28
```

---

### 작업 1: 버튼·텍스트 고대비 시인성 수정 (`SCRUM-248`)

**Files:**
- Modify: `src/ui/waveshare43b/thermostat_ui.cpp`

**요구사항:**
1. `create_button()`에서 내부 `label` 생성 시 반드시 `lv_obj_set_style_text_color(label, lv_color_hex(kColorText), 0)`을 명시한다(기본 흰색 `#F8FAFC`).
2. 다크 테마 고대비 팔레트 적용:
   - 배경 `kColorBackground = 0x0B1220`, 카드 `kColorCard = 0x162032`, 카드 테두리 `kColorCardBorder = 0x2D3F59`
   - 기본 텍스트 `kColorText = 0xF8FAFC`, 보조 텍스트 `kColorMuted = 0x94A3B8`
   - 활성 버튼 `kColorButton = 0x2B3B52`, 테두리 2px `kColorButtonBorder = 0x64748B`, 눌림 `kColorButtonPressed = 0x3E5474`
   - 선택 강조 `kColorAccent = 0xFF8A3D`, 강조 위 텍스트 `kColorTextOnAccent = 0x140D04`, 강조 테두리 `0xFFB074`
   - 비활성 버튼 `kColorButtonDisabled = 0x1A2332`, 비활성 테두리 `0x283548`, 비활성 텍스트 `0x475569`
3. 설정 오버레이의 `+/-`, `닫기` 버튼에도 동일한 고대비 버튼 텍스트·테두리 스타일이 일관되게 적용되고, 설정 모달 버튼 크기도 터치하기 편하게 확대(`60×56`, 닫기 `180×56`).

---

### 작업 2: 800×480 상하 분할 레이아웃 및 대형 폰트·버튼 (`SCRUM-249`)

**Files:**
- Modify: `tools/gen_kr_font.py`
- Modify: `src/ui/waveshare43b/kr_font_20.c`
- Modify: `src/ui/waveshare43b/kr_font_28.c`
- Create: `src/ui/waveshare43b/kr_font_48.c`
- Modify: `src/ui/waveshare43b/thermostat_ui.h`
- Modify: `src/ui/waveshare43b/thermostat_ui.cpp`

**요구사항:**
1. `tools/gen_kr_font.py`:
   - `UI_TEXT_CORPUS`에 `"반복 시간"` 추가 확인(이미 글자들이 있지만 문자열 목록에 명시).
   - `FONT_SIZES`에 `48: "kr_font_48"` 추가.
   - `uv run --with pillow python tools/gen_kr_font.py`를 실제로 실행해 `kr_font_20.c`, `kr_font_28.c`, `kr_font_48.c`를 생성한다. (`kr_font_48`은 숫자·기호·UI 핵심 한글이 모두 포함돼야 하며 Flash 여유 충분).
2. `thermostat_ui.h` & `.cpp`:
   - `LV_FONT_DECLARE(kr_font_48);` 선언.
   - 기존 `build_status_column`/`build_control_column`을 상하 분할 구조(`build_top_status_bar`, `build_bottom_control_panel`)로 개편하고 위 치수표에 맞춰 배치한다.
   - 메인 `[-]` / `[+]` 버튼 포인터(`primary_minus_btn_`, `primary_plus_btn_`)와 메인 캡션 라벨(`primary_caption_label_`), 메인 값 라벨(`primary_value_label_`)을 멤버로 보관한다.

---

### 작업 3: 반복 모드 메인 영역 시간 표시 및 직접 조절 (`SCRUM-250`)

**Files:**
- Modify: `src/ui/waveshare43b/ui_view_model.h`
- Modify: `src/ui/waveshare43b/thermostat_ui.cpp`
- Modify: `test/test_ui_view_model.cpp`

**요구사항:**
1. `ui_view_model.h`에 순수 C++ 헬퍼 추가:
   - `inline const char* primary_caption_for_mode(UiMode mode)`:
     - `UiMode::kTemperature` → `"목표 온도"`
     - `UiMode::kRepeat` → `"반복 시간"`
     - `UiMode::kOff` → `"꺼짐"`
   - `inline void format_primary_value(const UiViewModel& vm, char* buf, size_t buf_size)`:
     - `UiMode::kTemperature` → `format_temp_celsius(vm.target_temp_centi, true, buf, buf_size)` (예: `"22.5℃"`)
     - `UiMode::kRepeat` → `"%u분"` (예: `"15분"`)
     - `UiMode::kOff` → `"-"`
   - `inline bool can_adjust_primary(UiMode mode)`:
     - `mode != UiMode::kOff`
2. `thermostat_ui.cpp`:
   - `refresh_primary_control()` 추가: `primary_caption_label_`과 `primary_value_label_`을 현재 `vm_.mode`에 맞춰 갱신한다.
   - 모드 버튼(`꺼짐`/`온도`/`반복`) 클릭 시 `refresh_mode_buttons()`뿐 아니라 **`refresh_primary_control()`도 즉시 호출**해 화면이 즉각 전환되게 한다!
   - 메인 `[-]` / `[+]` (`Action::kPrimaryMinus` / `Action::kPrimaryPlus`) 클릭 시:
     - `UiMode::kTemperature`: `step_target_temp(vm_.target_temp_centi, ±50)` → 화면 갱신 + `callbacks_.on_target_temp_change` 호출.
     - `UiMode::kRepeat`: `step_repeat_minutes(vm_.repeat_on_minutes, ±5)` → `refresh_primary_control()` + `refresh_settings_values()` 둘 다 갱신 + `callbacks_.on_repeat_minutes_change` 호출!
   - 설정 모달에서 `kRepeatMinus`/`kRepeatPlus`를 눌렀을 때도 `refresh_primary_control()`을 함께 호출해 메인 화면과 동기화한다.

---

### 작업 4: 꺼짐 모드 `-` 표시 및 `+/-` 버튼 비활성화 (`SCRUM-251`)

**Files:**
- Modify: `src/ui/waveshare43b/thermostat_ui.cpp`
- Modify: `test/test_ui_view_model.cpp`

**요구사항:**
1. `refresh_primary_control()`에서 `const bool adjustable = can_adjust_primary(vm_.mode);`에 따라 `primary_minus_btn_`과 `primary_plus_btn_`의 활성 상태를 전환한다:
   - `adjustable == false` (`UiMode::kOff`):
     - `lv_obj_add_state(btn, LV_STATE_DISABLED);`
     - `lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);`
     - 비활성 스타일 적용(배경 `kColorButtonDisabled`, 테두리 `0x283548`, 버튼 라벨 텍스트 `0x475569`)
     - `primary_value_label_` 텍스트는 `"-"`, 텍스트 색상은 `kColorMuted`.
   - `adjustable == true` (`kTemperature` 또는 `kRepeat`):
     - `lv_obj_clear_state(btn, LV_STATE_DISABLED);`
     - `lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);`
     - 고대비 활성 스타일 복원(배경 `kColorButton`, 테두리 `kColorButtonBorder`, 버튼 라벨 텍스트 `kColorText`)
     - `primary_value_label_` 텍스트 색상은 `kColorText`.
2. `handle_action()`의 `kPrimaryMinus`/`kPrimaryPlus` 분기에서 `!can_adjust_primary(vm_.mode)`이면 아무 값도 바꾸지 않고 콜백도 호출하지 않는다(이중 방어!).
3. `test/test_ui_view_model.cpp`에 테스트 추가:
   - `primary_caption_for_mode` 3개 모드 검증
   - `format_primary_value` 온도 모드(`"22.5℃"`), 반복 모드(`"15분"`, `"0분"`, `"60분"`), 꺼짐 모드(`"-"`) 검증
   - `can_adjust_primary` 꺼짐 `false`, 온도/반복 `true` 검증

---

## Review Focus

1. `create_button()`을 포함한 모든 버튼 라벨에 명시적인 고대비 텍스트 색상과 테두리가 지정되어 다크 테마에서 또렷하게 보이는지 (`SCRUM-248`).
2. 800×480 상하 분할 레이아웃의 모든 위젯 좌표·크기가 화면 경계(800×480) 안에서 잘림·겹침 없이 배치되고 `kr_font_48.c`가 LVGL 8.4와 정상 컴파일되는지 (`SCRUM-249`).
3. 반복 모드(`kRepeat`)에서 메인 캡션·값이 `반복 시간` / `N분`으로 바뀌고 메인 `+/-`가 5분 단위로 조절하며 `on_repeat_minutes_change` 콜백과 설정 모달 값이 동기화되는지 (`SCRUM-250`).
4. 꺼짐 모드(`kOff`)에서 메인 값이 `"-"`로 표시되고 메인 `+/-` 버튼이 시각적·이벤트·로직 모두 차단되며, 온도/반복 모드 복귀 시 즉시 재활성화되는지 (`SCRUM-251`).
5. `python tools/test_host.py --filter ui`와 `python tools/build.py demos/03_ui`가 모두 통과하는지.
