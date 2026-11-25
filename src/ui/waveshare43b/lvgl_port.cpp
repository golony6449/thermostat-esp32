#include "lvgl_port.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace thermostat::ui {
namespace {

constexpr uint32_t kTickPeriodMs = 2;
constexpr uint32_t kTaskMaxDelayMs = 500;
constexpr uint32_t kTaskMinDelayMs = 2;
constexpr uint32_t kTaskStackSize = 6 * 1024;
constexpr UBaseType_t kTaskPriority = 2;

// Anti-tearing: LVGL renders straight into one of two RGB panel frame
// buffers (direct mode) and the panel switches to it on the next frame.
constexpr int kFrameBufferCount = 2;
// One frame at 16 MHz PCLK (800x480 + porches) takes about 26 ms.  If no
// refresh-finish interrupt arrives within this time we stop waiting, so the
// LVGL task can never hang on a missing vsync.
constexpr uint32_t kVsyncWaitTimeoutMs = 100;

#ifdef ARDUINO_RUNNING_CORE
constexpr BaseType_t kTaskCore = ARDUINO_RUNNING_CORE;
#else
constexpr BaseType_t kTaskCore = tskNO_AFFINITY;
#endif

SemaphoreHandle_t g_lvgl_mutex = nullptr;
// Read from the refresh-finish ISR through a pointer, so keep it in DRAM.
DRAM_ATTR TaskHandle_t g_lvgl_task = nullptr;
esp_timer_handle_t g_tick_timer = nullptr;
lv_disp_draw_buf_t g_draw_buf;
lv_disp_drv_t g_disp_drv;
lv_indev_drv_t g_indev_drv;
bool g_initialized = false;
uint32_t g_vsync_timeouts = 0;

void tick_increment(void*) {
  lv_tick_inc(kTickPeriodMs);
}

bool tick_init() {
  esp_timer_create_args_t args = {};
  args.callback = &tick_increment;
  args.name = "lvgl_tick";
  if (esp_timer_create(&args, &g_tick_timer) != ESP_OK) {
    return false;
  }
  if (esp_timer_start_periodic(g_tick_timer, kTickPeriodMs * 1000) != ESP_OK) {
    esp_timer_delete(g_tick_timer);
    g_tick_timer = nullptr;
    return false;
  }
  return true;
}

// Runs in ISR context at the end of every panel frame.  user_data points at
// g_lvgl_task; the handle may still be null right after attach.
IRAM_ATTR bool on_refresh_finish(void* user_data) {
  BaseType_t need_yield = pdFALSE;
  TaskHandle_t task = *static_cast<TaskHandle_t*>(user_data);
  if (task != nullptr) {
    vTaskNotifyGiveFromISR(task, &need_yield);
  }
  return need_yield == pdTRUE;
}

// Direct mode: color_map is the frame buffer LVGL just rendered into (one of
// the panel's own buffers), so nothing is copied here.  After the last area
// of a refresh we hand that buffer to the panel and wait until the panel has
// finished the frame, i.e. it no longer scans out the other buffer that LVGL
// will draw into next.
void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area,
                    lv_color_t* color_map) {
  (void)area;
  auto* lcd = static_cast<esp_panel::drivers::LCD*>(drv->user_data);
  if (lcd != nullptr && lv_disp_flush_is_last(drv)) {
    lcd->switchFrameBufferTo(color_map);
    // Only the LVGL task receives the vsync notification.  Flushes are only
    // driven by lv_timer_handler() in that task, but never block another
    // task on a notification it will not get.
    if (xTaskGetCurrentTaskHandle() == g_lvgl_task) {
      // Drop notifications counted from earlier frames, then wait for the
      // frame that shows the new buffer to finish.
      ulTaskNotifyValueClear(nullptr, ULONG_MAX);
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kVsyncWaitTimeoutMs)) == 0) {
        ++g_vsync_timeouts;
      }
    }
  }
  lv_disp_flush_ready(drv);
}

void rounder_callback(lv_disp_drv_t* drv, lv_area_t* area) {
  auto* lcd = static_cast<esp_panel::drivers::LCD*>(drv->user_data);
  if (lcd == nullptr) {
    return;
  }
  const auto& bus_spec = lcd->getBasicAttributes().basic_bus_spec;
  const int x_align = bus_spec.x_coord_align;
  const int y_align = bus_spec.y_coord_align;
  if (x_align > 1) {
    area->x1 &= ~(x_align - 1);
    area->x2 = (area->x2 & ~(x_align - 1)) + x_align - 1;
  }
  if (y_align > 1) {
    area->y1 &= ~(y_align - 1);
    area->y2 = (area->y2 & ~(y_align - 1)) + y_align - 1;
  }
}

void touch_read_callback(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  auto* touch = static_cast<esp_panel::drivers::Touch*>(drv->user_data);
  esp_panel::drivers::TouchPoint point;
  const int read = (touch != nullptr) ? touch->readPoints(&point, 1, 0) : 0;
  if (read > 0) {
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void lvgl_task(void*) {
  uint32_t delay_ms = kTaskMaxDelayMs;
  for (;;) {
    if (lvgl_port_lock(-1)) {
      delay_ms = lv_timer_handler();
      lvgl_port_unlock();
    }
    if (delay_ms > kTaskMaxDelayMs) {
      delay_ms = kTaskMaxDelayMs;
    } else if (delay_ms < kTaskMinDelayMs) {
      delay_ms = kTaskMinDelayMs;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}

}  // namespace

bool lvgl_port_prepare_lcd(esp_panel::drivers::LCD* lcd) {
  if (lcd == nullptr) {
    return false;
  }
  return lcd->configFrameBufferNumber(kFrameBufferCount);
}

bool lvgl_port_init(esp_panel::drivers::LCD* lcd,
                    esp_panel::drivers::Touch* touch) {
  if (g_initialized) {
    return false;
  }
  if (lcd == nullptr || lcd->getRefreshPanelHandle() == nullptr) {
    return false;
  }

  const int width = lcd->getFrameWidth();
  const int height = lcd->getFrameHeight();
  if (width <= 0 || height <= 0) {
    return false;
  }
  // Both buffers are allocated by the RGB panel driver in PSRAM; they exist
  // only if lvgl_port_prepare_lcd() ran before board.begin().
  void* frame_buffer_a = lcd->getFrameBufferByIndex(0);
  void* frame_buffer_b = lcd->getFrameBufferByIndex(1);
  if (frame_buffer_a == nullptr || frame_buffer_b == nullptr ||
      frame_buffer_a == frame_buffer_b) {
    return false;
  }

  lv_init();

#if !LV_TICK_CUSTOM
  if (!tick_init()) {
    return false;
  }
#endif

  const size_t pixel_count = static_cast<size_t>(width) * height;
  lv_disp_draw_buf_init(&g_draw_buf, frame_buffer_a, frame_buffer_b,
                        static_cast<uint32_t>(pixel_count));

  lv_disp_drv_init(&g_disp_drv);
  g_disp_drv.hor_res = width;
  g_disp_drv.ver_res = height;
  g_disp_drv.flush_cb = flush_callback;
  g_disp_drv.draw_buf = &g_draw_buf;
  g_disp_drv.direct_mode = 1;
  g_disp_drv.user_data = lcd;
  const auto& bus_spec = lcd->getBasicAttributes().basic_bus_spec;
  if (bus_spec.x_coord_align > 1 || bus_spec.y_coord_align > 1) {
    g_disp_drv.rounder_cb = rounder_callback;
  }
  if (lv_disp_drv_register(&g_disp_drv) == nullptr) {
    return false;
  }

  if (touch != nullptr && touch->getPanelHandle() != nullptr) {
    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = touch_read_callback;
    g_indev_drv.user_data = touch;
    if (lv_indev_drv_register(&g_indev_drv) == nullptr) {
      return false;
    }
  }

  g_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
  if (g_lvgl_mutex == nullptr) {
    return false;
  }
  // Attach before the task exists so the vsync source is in place before the
  // first flush; the ISR skips the notification while g_lvgl_task is null.
  if (!lcd->attachRefreshFinishCallback(&on_refresh_finish, &g_lvgl_task)) {
    return false;
  }
  const BaseType_t created =
      xTaskCreatePinnedToCore(lvgl_task, "lvgl", kTaskStackSize, nullptr,
                              kTaskPriority, &g_lvgl_task, kTaskCore);
  if (created != pdPASS) {
    return false;
  }

  g_initialized = true;
  return true;
}

uint32_t lvgl_port_vsync_timeouts() {
  return g_vsync_timeouts;
}

bool lvgl_port_lock(int timeout_ms) {
  if (g_lvgl_mutex == nullptr) {
    return false;
  }
  const TickType_t timeout_ticks =
      (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(g_lvgl_mutex, timeout_ticks) == pdTRUE;
}

bool lvgl_port_unlock() {
  if (g_lvgl_mutex == nullptr) {
    return false;
  }
  return xSemaphoreGiveRecursive(g_lvgl_mutex) == pdTRUE;
}

}  // namespace thermostat::ui
