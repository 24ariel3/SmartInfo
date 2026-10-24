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
#include <inttypes.h>
#include <unistd.h>

static const char *TAG = "DISPLAY";

// LVGL Display Handle
static lv_display_t *disp = NULL;

// SPI Handles
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_touch_handle_t tp = NULL;

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
static volatile int64_t last_touch_isr_time = 0;
#define TOUCH_DEBOUNCE_US 10000 // 10ms debounce

// Touch interrupt ISR - called when XPT2046 PENIRQ goes LOW (touch detected)
static void IRAM_ATTR touch_isr_handler(void *arg) {
  int64_t now = esp_timer_get_time();
  if ((now - last_touch_isr_time) > TOUCH_DEBOUNCE_US) {
    touch_pending = true;
    last_touch_isr_time = now;
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

// Tick Timer for LVGL
static void increase_lvgl_tick(void *arg) {
  // Tell LVGL how much time has passed
  lv_tick_inc(2); // 2ms period
}

// Main LVGL Task
static void lvgl_task(void *arg) {
  ESP_LOGI(TAG, "Starting LVGL Task");
  static uint32_t loop_count = 0;
  static int64_t max_handler_time = 0;

  while (1) {
    uint32_t delay_ms = 5; // Default delay

    if (display_lock(10)) { // Try for 10ms instead of blocking forever
      int64_t start = esp_timer_get_time();
      delay_ms = lv_timer_handler();
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

      if (delay_ms == 0)
        delay_ms = 1;
      if (delay_ms > 33)
        delay_ms = 33; // Cap at 33ms (~30FPS)
    } else {
      // FAILED to get lock - someone else has it
      // ESP_LOGW(TAG, "LVGL task couldn't get lock, yielding");
      delay_ms = 10; // Wait 10ms before retry
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
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
  ESP_ERROR_CHECK(gpio_intr_enable(BSP_LCD_TOUCH_INT));
  // A held press won't produce another falling edge after interrupts resume.
  if (gpio_get_level(BSP_LCD_TOUCH_INT) == 0) touch_pending = true;
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
  static int64_t last_read_time = 0;
  static uint16_t last_x = 0, last_y = 0;
  static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;

  int64_t now = esp_timer_get_time();
  if ((now - last_read_time) < 33000) { // Limit to ~30Hz (33ms)
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
    return;
  }
  last_read_time = now;

#ifdef BSP_LCD_TOUCH_INT
  // ── Interrupt-driven mode ──────────────────────────────────────────
  // Read after an interrupt or while PENIRQ is already LOW.

  if (!touch_pending && gpio_get_level(BSP_LCD_TOUCH_INT) != 0) {
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_RELEASED;
    last_state = LV_INDEV_STATE_RELEASED;
    return;
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
    last_state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    last_state = LV_INDEV_STATE_RELEASED;
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
      .clk_cfg = LEDC_AUTO_CLK,
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
  };
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
  // Set initial brightness
  display_set_brightness(DISPLAY_BACKLIGHT_DEFAULT);
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
  // Interrupt-driven touch: configure GPIO for PENIRQ with pull-up and
  // falling edge trigger. The XPT2046 driver already configures the pin as
  // input, but we add pull-up (open-drain output) and install our ISR.
  ESP_LOGI(TAG, "Configuring touch interrupt on GPIO %d", BSP_LCD_TOUCH_INT);
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BSP_LCD_TOUCH_INT),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(isr_err);
  }
  ESP_ERROR_CHECK(
      gpio_isr_handler_add(BSP_LCD_TOUCH_INT, touch_isr_handler, NULL));
  ESP_LOGI(TAG, "Touch interrupt enabled");
#endif

  // Initialize LVGL
  ESP_LOGI(TAG, "Initializing LVGL...");
  lv_init();

  // Create a display
  disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);

  // Allocate draw buffers
  uint32_t buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES / 10 * sizeof(uint16_t);
  void *buf1 =
      heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *buf2 =
      heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  lv_display_set_buffers(disp, buf1, buf2, buffer_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, lvgl_flush_cb);

  // Register Touch Input Device
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

  // Register Tick Timer
  ESP_LOGI(TAG, "Install LVGL tick timer");
  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &increase_lvgl_tick, .name = "lvgl_tick"};
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2000)); // 2ms

  // Create LVGL Task
  // Priority 5 (Normal), Core 1 (App Core)
  xTaskCreatePinnedToCore(lvgl_task, "LVGL", 8192, NULL, 5, NULL, 1);

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
#ifdef BSP_LCD_BACKLIGHT
  if (percent > 100)
    percent = 100;

  // Calculate duty cycle (Resolution is 10-bit: 0-1023)
  uint32_t duty = (1023 * percent) / 100;

  ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL, duty);
  ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
  ESP_LOGI(TAG, "Backlight set to %d%% (Duty: %" PRIu32 ")", percent, duty);
#else
  ESP_LOGW(TAG,
           "Backlight control not available (BSP_LCD_BACKLIGHT not defined)");
#endif
}
