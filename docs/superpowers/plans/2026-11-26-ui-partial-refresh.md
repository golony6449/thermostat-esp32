# 화면 그리기 최적화: 바뀐 위젯만 갱신 (SCRUM-314)

**목표:** 화면 갱신 한 번에 드는 LVGL 그리기 시간을 줄여, 터치 순간 화면이 한 프레임 밀리는 현상(RGB 바운스 버퍼 채우기 지연)을 줄인다.

리스크하기 때문에 Claude가 최종 검증 진행(실기 화면 런타임 동작. Claude가 시리얼 계측으로 그리기 시간을 재고, 사용자가 화면을 확인한다).

**배경(2026-11-26 실기 계측, SCRUM-256 조사 중):**
- 값 하나만 바뀌어도 `lv_timer_handler()` 1회가 약 110~125ms 걸린다. 1분마다 시계만 바뀌어도 flush가 11번 일어난다.
- 연타 중에는 1초 중 LVGL이 바쁜 시간이 340~820ms다.
- 원인: `ThermostatUi::update()`가 매번 `refresh_all()`을 불러 모든 라벨의 텍스트와 버튼·배지 스타일을 다시 설정한다. LVGL 8은 같은 텍스트로 `lv_label_set_text()`를 불러도, 같은 값으로 `lv_obj_set_style_*()`를 불러도 그 객체를 무효화해 다시 그린다. 스타일 변경은 자식과 레이아웃까지 다시 계산하게 한다.
- 펌웨어는 목표온도를 한 번 누르면 UI가 먼저 `handle_action()`에서 `vm_`을 바꾸고 `refresh_primary_control()`을 부른 뒤, 잠시 뒤 앱이 같은 값으로 `update(vm)`을 다시 부른다. 지금은 이 두 번째 호출이 화면 전체를 또 그린다.

## Global Constraints

- 화면 모양, 문구, 색, 동작(누르면 바로 바뀌는 반응)은 바꾸지 않는다. 바꾸는 것은 "언제 다시 그리는가"뿐이다.
- 비교 로직은 LVGL 없이 컴파일되는 `ui_view_model.h`(헤더 inline)에 두고 호스트 테스트(`python tools/test_host.py`)로 검증한다.
- LVGL 호출은 지금처럼 `thermostat_ui.cpp` 안에서만 한다.
- 펌웨어 빌드는 `python tools/build.py firmware/thermostat_main`, 데모 빌드는 `python tools/build.py demos/03_ui`(업로드 없이)로 확인한다.

## 작업

### 작업 1: 뷰 모델 비교(순수 C++)

**Files:**
- Modify: `src/ui/waveshare43b/ui_view_model.h`
- Test: `test/test_ui_view_model.cpp`

`ui_view_model.h`에 추가:
```cpp
// Which parts of the screen must be redrawn when the view model changes.
struct UiDirty {
  bool current_temp = false;     // current_temp_centi, temp_valid
  bool primary = false;          // mode, target_temp_centi, repeat_on_minutes
  bool heating = false;          // heating_active
  bool source = false;           // source_text (문자열 비교), source_severity
  bool mode_buttons = false;     // mode
  bool settings_values = false;  // repeat_on_minutes, freeze_protect_centi
  bool time = false;             // time_text (문자열 비교)
  bool any() const;
};
inline UiDirty ui_dirty_all();   // 모든 항목 true
inline UiDirty diff_view_model(const UiViewModel& prev, const UiViewModel& next);
```
테스트: 같은 모델이면 `any() == false`. 필드 하나씩 바꿀 때 해당 항목만 true(예: `time_text`만 바꾸면 `time`만, `mode`를 바꾸면 `primary`와 `mode_buttons`, `repeat_on_minutes`는 `primary`와 `settings_values`). `ui_dirty_all()`은 전부 true.

### 작업 2: 바뀐 부분만 갱신

**Files:**
- Modify: `src/ui/waveshare43b/thermostat_ui.h`
- Modify: `src/ui/waveshare43b/thermostat_ui.cpp`

1. 라벨 텍스트는 도우미 `set_label_text_if_changed(lv_obj_t* label, const char* text)`로만 바꾼다. `lv_label_get_text(label)`와 `strcmp`로 같으면 아무것도 하지 않는다. `refresh_*` 안의 모든 `lv_label_set_text`를 이 도우미로 바꾼다.
2. 스타일은 "마지막으로 적용한 상태"를 멤버로 기억하고, 상태가 바뀔 때만 다시 설정한다. 처음에는 "적용 안 함"(-1)으로 두어 첫 갱신에서 반드시 적용되게 한다.
   - `refresh_primary_control()`: 조절 가능 여부(`can_adjust_primary(vm_.mode)`)가 바뀔 때만 +/- 버튼 스타일·플래그와 값 라벨 색을 바꾼다.
   - `refresh_mode_buttons()`: 선택된 모드가 바뀔 때만 `apply_button_style()`.
   - 난방 배지: `heating_active`가 바뀔 때만 배지 색·글자 색.
   - 출처 배지: `source_severity`가 바뀔 때만 색.
3. `update(vm)`: 시작 전(`started_ == false`)이면 지금처럼 저장만 한다. 시작 뒤에는 `diff_view_model(vm_, vm)`(첫 갱신은 `ui_dirty_all()`)로 바뀐 부분을 구해 `vm_ = vm` 뒤 해당 `refresh_*`만 부른다. `refresh_all()`은 `begin()`에서 처음 한 번 그릴 때만 쓴다.
4. `handle_action()`이 `vm_`을 먼저 바꾸고 해당 `refresh_*`를 부르는 지금 흐름은 유지한다(누르는 즉시 반응). 이어서 앱이 같은 값으로 `update()`를 부르면 비교 결과가 비어 아무것도 다시 그리지 않아야 한다.

## Review Focus

1. 첫 화면과 설정 화면을 연 뒤에도 모든 값이 빠짐없이 그려지는가(초기 -1 상태로 첫 갱신에서 반드시 적용).
2. 모드 전환(꺼짐 ↔ 온도 ↔ 반복) 때 +/- 버튼 비활성 스타일·클릭 플래그·값 라벨 색이 지금과 똑같이 바뀌는가.
3. `handle_action()`으로 먼저 바뀐 값과 뒤이어 온 `update()`가 겹쳐도 화면이 뷰 모델과 어긋나지 않는가(텍스트 비교는 위젯 실제 텍스트 기준).
4. `diff_view_model`이 문자열 필드(`source_text`, `time_text`)를 내용으로 비교하는가(포인터 비교 아님).
