#include "display.h"
#include "bsp.h" // For Pin definitions
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif
#include <inttypes.h>
#include <unistd.h>

static const char *TAG = "DISPLAY";

// LVGL Display Handle
static lv_display_t *disp = NULL;

// SPI Handles
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static lv_indev_t *s_touch_indev = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static lv_timer_t *s_touch_read_timer = NULL;
static TaskHandle_t s_lvgl_task_handle = NULL;
static uint8_t s_selected_brightness = DISPLAY_BACKLIGHT_DEFAULT;
static int s_applied_brightness = -1;
static bool s_display_idle = false;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_display_awake_lock;
#endif
#define DISPLAY_IDLE_DIM_MS 60000
#define DISPLAY_IDLE_BRIGHTNESS_MAX 20
#define DISPLAY_ACTIVE_PERIOD_MS 33
#define DISPLAY_IDLE_PERIOD_MS 100

// Backlight PWM Configuration
#ifdef BSP_LCD_BACKLIGHT
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BACKLIGHT_LEDC_RES LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_LEDC_FREQ 5000 // 5kHz
#endif

// LVGL Mutex
static SemaphoreHandle_t lvgl_mux = NULL;

// SPI Bus Semaphore - protects shared SPI bus between display and touch
// NOTE: Must be binary semaphore (not mutex) because we release from ISR
static SemaphoreHandle_t spi_bus_sem = NULL;
#define SPI_BUS_TIMEOUT_MS 50 // Timeout for acquiring SPI bus lock

// Touch interrupt mode (enabled when BSP_LCD_TOUCH_INT is defined in bsp.h)
#ifdef BSP_LCD_TOUCH_INT
static volatile bool touch_pending = false;
static void IRAM_ATTR touch_isr_handler(void *arg) {
  // Mask the level IRQ until release; held touches are read by the LVGL timer.
  gpio_intr_disable(BSP_LCD_TOUCH_INT);
  touch_pending = true;
  if (s_lvgl_task_handle) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_lvgl_task_handle, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) portYIELD_FROM_ISR();
  }
}
#endif

// Callback: Notify LVGL when flush is ready (called from ISR context when DMA
// completes)
static bool
notify_lvgl_flush_ready_use_static(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx) {
  // Release SPI bus mutex - display DMA transfer complete
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(spi_bus_sem, &xHigherPriorityTaskWoken);

  if (disp) {
    // ESP_LOGI(TAG, "Flush Ready Callback"); // Uncomment for extreme debug
    lv_display_flush_ready(disp);
  }
  return xHigherPriorityTaskWoken == pdTRUE;
}

// Callback: LVGL Flush
static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area,
                          uint8_t *px_map) {
  // ESP_LOGI(TAG, "Flush CB: (%d,%d) -> (%d,%d)", (int)area->x1, (int)area->y1,
  //          (int)area->x2, (int)area->y2);

  int offsetx1 = area->x1;
  int offsetx2 = area->x2;
  int offsety1 = area->y1;
  int offsety2 = area->y2;

  uint32_t w = offsetx2 - offsetx1 + 1;
  uint32_t h = offsety2 - offsety1 + 1;

  // Use LVGL utility for byte swapping (optimized)
  lv_draw_sw_rgb565_swap(px_map, w * h);

  // Acquire SPI bus lock before queuing display DMA (released in flush_ready
  // callback)
  if (xSemaphoreTake(spi_bus_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(TAG, "Flush CB: Failed to acquire SPI bus lock!");
    lv_display_flush_ready(
        display); // Tell LVGL we are done (even if failed) to prevent hang
    return;
  }

  // ESP_LOGI(TAG, "Queueing Bitmap");
  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1,
                            offsety2 + 1, px_map);
}

static uint32_t lvgl_tick_get_cb(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void apply_brightness(bool idle) {
#ifdef BSP_LCD_BACKLIGHT
  uint8_t percent = s_selected_brightness;
  if (idle && percent > DISPLAY_IDLE_BRIGHTNESS_MAX)
    percent = DISPLAY_IDLE_BRIGHTNESS_MAX;
  if (s_applied_brightness == percent) return;

  uint32_t duty = (1023 * percent) / 100;
  ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL, duty);
  ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
  s_applied_brightness = percent;
  ESP_LOGI(TAG, "Backlight set to %u%% (Duty: %" PRIu32 ")", percent, duty);
#else
  (void)idle;
#endif
}

static void set_display_idle(bool idle, bool wake_touch_timer) {
  if (s_display_idle == idle) {
    apply_brightness(idle);
    return;
  }

#if CONFIG_PM_ENABLE
  if (!idle) ESP_ERROR_CHECK(esp_pm_lock_acquire(s_display_awake_lock));
#endif
  uint32_t period_ms = idle ? DISPLAY_IDLE_PERIOD_MS : DISPLAY_ACTIVE_PERIOD_MS;
  if (s_refresh_timer) lv_timer_set_period(s_refresh_timer, period_ms);
  if (s_touch_read_timer) lv_timer_set_period(s_touch_read_timer, period_ms);
  if (!idle && wake_touch_timer && s_touch_read_timer)
    lv_timer_ready(s_touch_read_timer);

  apply_brightness(idle);
#if CONFIG_PM_ENABLE
  if (idle) ESP_ERROR_CHECK(esp_pm_lock_release(s_display_awake_lock));
#endif
  s_display_idle = idle;
  ESP_LOGI(TAG, "Display %s: LVGL period=%" PRIu32 " ms", idle ? "idle" : "active",
           period_ms);
}

// Main LVGL Task
static void lvgl_task(void *arg) {
  ESP_LOGI(TAG, "Starting LVGL Task");
  static uint32_t loop_count = 0;
  static int64_t max_handler_time = 0;

  while (1) {
    uint32_t delay_ms = 5; // Default delay

    if (display_lock(10)) { // Try for 10ms instead of blocking forever
#ifdef BSP_LCD_TOUCH_INT
      bool touch_wakeup = touch_pending || gpio_get_level(BSP_LCD_TOUCH_INT) == 0;
      if (touch_wakeup && disp) lv_display_trigger_activity(disp);
#else
      bool touch_wakeup = false;
#endif
      uint32_t idle_ms = disp ? lv_display_get_inactive_time(disp) : 0;
      set_display_idle(idle_ms >= DISPLAY_IDLE_DIM_MS, touch_wakeup);

      int64_t start = esp_timer_get_time();
      delay_ms = lv_timer_handler();
      idle_ms = disp ? lv_display_get_inactive_time(disp) : 0;
      set_display_idle(idle_ms >= DISPLAY_IDLE_DIM_MS, false);
      display_unlock();
      int64_t duration = esp_timer_get_time() - start;

      // Track max handler time
      if (duration > max_handler_time) {
        max_handler_time = duration;
      }

      // Log every 100 loops or if handler took > 100ms
      loop_count++;
      if (duration > 100000) {
        ESP_LOGI(TAG, "LVGL loop #%" PRIu32 ": handler=%d ms, max=%d ms",
                 loop_count, (int)(duration / 1000),
                 (int)(max_handler_time / 1000));
      }

      if (duration > 200000) { // 200ms
        ESP_LOGW(TAG, "LVGL Handler took %d ms", (int)(duration / 1000));
      }

      if (delay_ms == 0) delay_ms = 1;
      uint32_t max_delay = s_display_idle ? DISPLAY_IDLE_PERIOD_MS
                                          : DISPLAY_ACTIVE_PERIOD_MS;
      if (delay_ms > max_delay) delay_ms = max_delay;
    } else {
      // FAILED to get lock - someone else has it
      // ESP_LOGW(TAG, "LVGL task couldn't get lock, yielding");
      delay_ms = 10; // Wait 10ms before retry
    }

    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    ulTaskNotifyTake(pdTRUE, delay_ticks == 0 ? 1 : delay_ticks);
  }
}

static void calibrate_touch(uint16_t raw_x, uint16_t raw_y, uint16_t *cal_x,
                            uint16_t *cal_y) {
  int32_t x, y;
// Simple calibration constants
#define TOUCH_RAW_MIN_X 14
#define TOUCH_RAW_MAX_X 230
#define TOUCH_RAW_MIN_Y 14
#define TOUCH_RAW_MAX_Y 280
#define SCREEN_WIDTH BSP_LCD_H_RES
#define SCREEN_HEIGHT BSP_LCD_V_RES

  // Map raw X to screen X (Inverted: High Raw -> Low Screen)
  x = (int32_t)(TOUCH_RAW_MAX_X - raw_x) * SCREEN_WIDTH /
      (TOUCH_RAW_MAX_X - TOUCH_RAW_MIN_X);
  // Map raw Y to screen Y
  y = (int32_t)(raw_y - TOUCH_RAW_MIN_Y) * SCREEN_HEIGHT /
      (TOUCH_RAW_MAX_Y - TOUCH_RAW_MIN_Y);

  if (x < 0)
    x = 0;
  if (x >= SCREEN_WIDTH)
    x = SCREEN_WIDTH - 1;
  if (y < 0)
    y = 0;
  if (y >= SCREEN_HEIGHT)
    y = SCREEN_HEIGHT - 1;

  *cal_x = (uint16_t)x;
  *cal_y = (uint16_t)y;
}

static esp_err_t touch_power_down(void) {
  // PD0=0 restores PENIRQ; its conversion result is not a coordinate sample.
  uint8_t discard[2];
  return esp_lcd_panel_io_rx_param(tp->io, 0xD0, discard, sizeof(discard));
}

// Helper: perform SPI touch read and process results
// Returns true if a valid touch was detected, fills cal_x/cal_y
static bool touch_read_and_process(uint16_t *cal_x, uint16_t *cal_y) {
  // Acquire SPI bus lock (prevents contention with display DMA)
  if (xSemaphoreTake(spi_bus_sem, pdMS_TO_TICKS(SPI_BUS_TIMEOUT_MS)) !=
      pdTRUE) {
    ESP_LOGW(TAG, "Touch: SPI bus busy, skipping read");
    return false;
  }

  uint8_t touch_cnt = 0;
  esp_lcd_touch_point_data_t touch_points[1] = {0};

#ifdef BSP_LCD_TOUCH_INT
  ESP_ERROR_CHECK(gpio_intr_disable(BSP_LCD_TOUCH_INT));
  touch_pending = false;
#endif

  esp_err_t read_ret = esp_lcd_touch_read_data(tp);
  esp_err_t ret = read_ret;
  if (read_ret == ESP_OK) {
    ret = esp_lcd_touch_get_data(tp, touch_points, &touch_cnt, 1);
  }
  // Always rearm PENIRQ, including when the read or coordinate lookup failed.
  esp_err_t power_ret = touch_power_down();

#ifdef BSP_LCD_TOUCH_INT
  if (gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
    touch_pending = true;
  } else {
    ESP_ERROR_CHECK(gpio_intr_enable(BSP_LCD_TOUCH_INT));
  }
#endif

  xSemaphoreGive(spi_bus_sem);

  if (read_ret != ESP_OK || ret != ESP_OK || power_ret != ESP_OK) {
    ESP_LOGW(TAG, "Touch SPI: read=%s, data=%s, power-down=%s",
             esp_err_to_name(read_ret), esp_err_to_name(ret),
             esp_err_to_name(power_ret));
  }

  if (read_ret == ESP_OK && ret == ESP_OK && power_ret == ESP_OK &&
      touch_cnt > 0) {
    calibrate_touch(touch_points[0].x, touch_points[0].y, cal_x, cal_y);
    ESP_LOGD(TAG, "Touch LVGL: x=%u, y=%u", *cal_x, *cal_y);
    return true;
  }
  return false;
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  static uint16_t last_x = 0, last_y = 0;

#ifdef BSP_LCD_TOUCH_INT
  // ── Interrupt-driven mode ──────────────────────────────────────────
  // Read after an interrupt or while PENIRQ is already LOW.

  if (!touch_pending && gpio_get_level(BSP_LCD_TOUCH_INT) != 0) {
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  // A press can arrive after the task's idle check, while LVGL is handling timers.
  if (s_display_idle) {
    lv_display_trigger_activity(disp);
    set_display_idle(false, false);
  }
  uint16_t cal_x, cal_y;
  bool touched = touch_read_and_process(&cal_x, &cal_y);

#else
  // ── SW polling mode ────────────────────────────────────────────────
  // Read touch data every cycle via SPI polling (~30Hz).

  uint16_t cal_x, cal_y;
  bool touched = touch_read_and_process(&cal_x, &cal_y);

#endif

  // ── Common: update LVGL state ──────────────────────────────────────
  if (touched) {
    data->point.x = cal_x;
    data->point.y = cal_y;
    data->state = LV_INDEV_STATE_PRESSED;
    last_x = cal_x;
    last_y = cal_y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

esp_err_t display_init(void) {
  esp_err_t ret = ESP_OK;

#ifdef BSP_LCD_BACKLIGHT
  ESP_LOGI(TAG, "Initializing Backlight PWM on GPIO %d...", BSP_LCD_BACKLIGHT);
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
      .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
      .duty_resolution = LCD_BACKLIGHT_LEDC_RES,
      .freq_hz = LCD_BACKLIGHT_LEDC_FREQ,
      .clk_cfg = LEDC_USE_RC_FAST_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ledc_channel_config_t ledc_channel = {
      .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
      .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
      .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = BSP_LCD_BACKLIGHT,
      .duty = 0, // Start off
      .hpoint = 0,
      .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
  apply_brightness(false);
#endif

  // Create mutex
  lvgl_mux = xSemaphoreCreateRecursiveMutex();
  assert(lvgl_mux);

  ESP_LOGI(TAG, "Installing Panel IO...");
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = BSP_LCD_DC,
      .cs_gpio_num = BSP_LCD_SPI_CS,
      .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = BSP_LCD_CMD_BITS,
      .lcd_param_bits = BSP_LCD_PARAM_BITS,
      .spi_mode = 0,
      .trans_queue_depth = 20, // Increased buffer for full refreshes
      .on_color_trans_done = notify_lvgl_flush_ready_use_static,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &io_config, &io_handle));

  ESP_LOGI(TAG, "Installing User Panel Driver...");
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = BSP_LCD_RST,
      .rgb_endian = LCD_RGB_ENDIAN_BGR,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // Create SPI bus semaphore for touch/display synchronization
  // Must be binary semaphore (not mutex) because we release from ISR
  spi_bus_sem = xSemaphoreCreateBinary();
  assert(spi_bus_sem);
  xSemaphoreGive(spi_bus_sem); // Start with semaphore available

  // Initialize Touch
  ESP_LOGI(TAG, "Initializing Touch Controller...");
  esp_lcd_panel_io_handle_t tp_io_handle = NULL;
  esp_lcd_panel_io_spi_config_t tp_io_config =
      ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(BSP_LCD_TOUCH_CS);
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &tp_io_config, &tp_io_handle));

  esp_lcd_touch_config_t tp_cfg = {
      .x_max = BSP_LCD_H_RES,
      .y_max = BSP_LCD_V_RES,
      .rst_gpio_num = -1,
#ifdef BSP_LCD_TOUCH_INT
      .int_gpio_num = BSP_LCD_TOUCH_INT,
#else
      .int_gpio_num = -1,
#endif
      .levels =
          {
              .reset = 0,
              .interrupt = 0,
          },
      .flags =
          {
              .swap_xy = 0,
              .mirror_x = 0,
              .mirror_y = 0,
          },
  };
  ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &tp));
  ESP_ERROR_CHECK(touch_power_down());

#ifdef BSP_LCD_TOUCH_INT
  // Install the handler before enabling the low-level interrupt used for wake.
  ESP_LOGI(TAG, "Configuring touch interrupt on GPIO %d", BSP_LCD_TOUCH_INT);
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BSP_LCD_TOUCH_INT),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(isr_err);
  }
  ESP_ERROR_CHECK(
      gpio_isr_handler_add(BSP_LCD_TOUCH_INT, touch_isr_handler, NULL));
  ESP_ERROR_CHECK(gpio_intr_disable(BSP_LCD_TOUCH_INT));
  ESP_ERROR_CHECK(gpio_set_intr_type(BSP_LCD_TOUCH_INT, GPIO_INTR_LOW_LEVEL));
#if CONFIG_PM_ENABLE
  ESP_ERROR_CHECK(gpio_wakeup_enable(BSP_LCD_TOUCH_INT, GPIO_INTR_LOW_LEVEL));
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
#endif
  ESP_ERROR_CHECK(gpio_intr_enable(BSP_LCD_TOUCH_INT));
  ESP_LOGI(TAG, "Touch interrupt enabled");
#endif

  // Initialize LVGL
  ESP_LOGI(TAG, "Initializing LVGL...");
  lv_init();
  lv_tick_set_cb(lvgl_tick_get_cb);

  // Create a display
  disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);

  // Two 16-row buffers leave internal RAM available for TLS handshakes.
  uint32_t buffer_size = BSP_LCD_H_RES * 16 * sizeof(uint16_t);
  void *buf1 =
      heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *buf2 =
      heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  if (!buf1 || !buf2) {
    heap_caps_free(buf1);
    heap_caps_free(buf2);
    return ESP_ERR_NO_MEM;
  }

  lv_display_set_buffers(disp, buf1, buf2, buffer_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, lvgl_flush_cb);

  // Register Touch Input Device
  s_touch_indev = lv_indev_create();
  lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);
  s_refresh_timer = lv_display_get_refr_timer(disp);
  s_touch_read_timer = lv_indev_get_read_timer(s_touch_indev);
  if (!s_refresh_timer || !s_touch_read_timer) return ESP_ERR_INVALID_STATE;

#if CONFIG_PM_ENABLE
  ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display_active",
                           &s_display_awake_lock);
  if (ret != ESP_OK) return ret;
  ret = esp_pm_lock_acquire(s_display_awake_lock);
  if (ret != ESP_OK) {
    ESP_ERROR_CHECK(esp_pm_lock_delete(s_display_awake_lock));
    s_display_awake_lock = NULL;
    return ret;
  }
#endif

  // Create LVGL Task
  // Priority 5 (Normal), Core 1 (App Core)
  if (xTaskCreatePinnedToCore(lvgl_task, "LVGL", 8192, NULL, 5,
                              &s_lvgl_task_handle, 1) != pdPASS) {
    s_lvgl_task_handle = NULL;
#if CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_lock_release(s_display_awake_lock));
    ESP_ERROR_CHECK(esp_pm_lock_delete(s_display_awake_lock));
    s_display_awake_lock = NULL;
#endif
    return ESP_ERR_NO_MEM;
  }

  return ret;
}

lv_display_t *display_get_handle(void) { return disp; }

bool display_lock(uint32_t timeout_ms) {
  TickType_t timeout_ticks =
      (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  if (!lvgl_mux)
    return false;
  return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

void display_unlock(void) {
  if (lvgl_mux)
    xSemaphoreGiveRecursive(lvgl_mux);
}

void display_set_brightness(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  s_selected_brightness = percent;
  uint32_t idle_ms = disp ? lv_display_get_inactive_time(disp) : 0;
  set_display_idle(idle_ms >= DISPLAY_IDLE_DIM_MS, false);
}
