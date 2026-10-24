/*
 * BSEC Integration Example
 */

#include "bsec_integration.h"
#include "battery.h"
#include "bsp.h"
#include "display.h"
#include "device_server.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "stock_monitor.h"
#include "time_manager.h"
#include "ui.h"
#include "ui_updater.h"
#include "weather.h"
#include "wifi_connect.h"

static const char *TAG = "MAIN";

void mem_info(void) {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  uint32_t flash_size;
  esp_flash_get_size(NULL, &flash_size);

  printf("\n--- HARDWARE REPORT ---\n");
  printf("Chip:      ESP32 (Revision %d)\n", chip_info.revision);
  printf("Cores:     %d\n", chip_info.cores);
  printf("Flash:     %lu MB\n", (unsigned long)(flash_size / (1024 * 1024)));

  // Check Internal RAM
  size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t total_sram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  printf("SRAM:      %lu KB total / %lu KB free\n",
         (unsigned long)(total_sram / 1024), (unsigned long)(free_sram / 1024));

  // Check PSRAM (External RAM)
  size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (total_psram > 0) {
    printf("PSRAM:     %lu KB detected\n", (unsigned long)(total_psram / 1024));
  } else {
    printf("PSRAM:     None detected\n");
  }
  printf("-----------------------\n");
}

#include "driver/i2c.h"

static void i2c_scanner() {
  ESP_LOGI(TAG, ">> I2C Scanner starting...");
  int devices_found = 0;
  for (int i = 1; i < 127; i++) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret =
        i2c_master_cmd_begin(BSP_I2C_NUM, cmd, 50 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, ">> Found I2C device at address 0x%02x", i);
      devices_found++;
    }
  }
  if (devices_found == 0) {
    ESP_LOGW(TAG, ">> No I2C devices found!");
  } else {
    ESP_LOGI(TAG, ">> I2C Scan complete. Found %d devices.", devices_found);
  }
}

void app_main(void) {
  mem_info();
  // Reconfigure Task Watchdog to 10 seconds
  const esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&twdt_config);

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_LOGI(TAG, "Starting BME680 BSEC Application");

  // Initialize Wi-Fi
  wifi_connect_init();

  // Initialize NTP Time Manager
  time_manager_init();

  // Initialize Weather
  weather_start_task();

  // Initialize BSP (GPIOs, SPI, I2C)
  ESP_ERROR_CHECK(bsp_init());
  ESP_ERROR_CHECK(battery_init());

  // Run I2C Scanner
  i2c_scanner();

  // Initialize BSEC Helper
  bsec_wrapper_init(); // This creates the mutex

  // Start the BSEC processing task
  bsec_start_task();

  // Start Stock Monitor
  stock_monitor_start();

  // Initialize Display
  ESP_ERROR_CHECK(display_init());

  // Initialize SquareLine Studio UI and start UI updater
  if (display_lock(0)) {

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG,
             "LVGL Memory BEFORE: %" PRIu32 " used, %" PRIu32
             " free, %d%% frag",
             (uint32_t)(mon.total_size - mon.free_size),
             (uint32_t)mon.free_size, mon.frag_pct);

    int64_t start_time = esp_timer_get_time();
    ui_init();
    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "ui_init() took %lld us (%lld ms)", end_time - start_time,
             (end_time - start_time) / 1000);

    ui_updater_init(); // Start the UI update timer
    lv_mem_monitor_t mon_after;
    lv_mem_monitor(&mon_after);
    ESP_LOGI(TAG, "LVGL Memory: %" PRIu32 " used, %" PRIu32 " free, %d%% frag",
             (uint32_t)(mon_after.total_size - mon_after.free_size),
             (uint32_t)mon_after.free_size, mon_after.frag_pct);

    display_unlock();
  }

  ESP_LOGI(TAG, "UI Initialized - All updates handled by LVGL timer");

  ret = device_server_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Device server unavailable: %s", esp_err_to_name(ret));
  }
  vTaskDelay(pdMS_TO_TICKS(30000));
  int64_t sensor_ts = bsec_get_last_updated_time();
  bool healthy = ret == ESP_OK && display_get_handle() != NULL && sensor_ts > 0 &&
                 esp_timer_get_time() - sensor_ts <= 10LL * 1000000;
  ESP_ERROR_CHECK(device_server_confirm_boot(healthy));

  // Main task has nothing else to do - it can sleep or be deleted
  // The UI updates are now handled by the LVGL timer inside the display task
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000)); // Sleep, nothing to do here
  }
}
