/**
 * @file ui_updater.c
 * @brief UI updater module - handles periodic UI updates via LVGL timer
 */

#include "ui_updater.h"
#include "battery.h"
#include "bsec_integration.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "network_sync.h"
#include "stock_monitor.h"
#include "time_manager.h"
#include "ui.h"
#include "weather.h"
#include "wifi_connect.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "UI_UPDATER";

// State variables for weather (persist across timer calls)
static weather_data_t s_weather_data = {0};

// Cache variables to avoid redundant UI updates
static char s_cached_time[32] = "";
static char s_cached_date[32] = "";
static bool s_cached_wifi_connected = false;
static bool s_first_run = true;
static int s_battery_mv;
static int s_battery_percent;
static bool s_battery_sample_available;
static uint64_t s_battery_current_seconds;
static uint64_t s_battery_previous_seconds;
static bool s_charger_state_known;
static bool s_charger_connected;

// Forward declaration
static void check_wifi_scan_results(void);
static void check_wifi_connection_status(void);

// Weather UI element arrays (initialized once on first use)
static lv_obj_t *s_weather_labels_day[5];
static lv_obj_t *s_weather_labels_high[5];
static lv_obj_t *s_weather_labels_low[5];
static lv_obj_t *s_weather_images[5];
static lv_obj_t *s_weather_labels_mean[5];
static lv_obj_t *s_weather_labels_hum[5];
static lv_obj_t *s_weather_labels_uv[5];
static lv_obj_t *s_weather_labels_precip[5];
static lv_obj_t *s_weather_labels_wind[5];
static lv_obj_t *s_weather_labels_dir[5];
static lv_obj_t *s_weather_labels_rise[5];
static lv_obj_t *s_weather_labels_set[5];

static void init_weather_ui_arrays(void) {
  s_weather_labels_day[0] = ui_LabelDay1;
  s_weather_labels_day[1] = ui_LabelDay2;
  s_weather_labels_day[2] = ui_LabelDay3;
  s_weather_labels_day[3] = ui_LabelDay4;
  s_weather_labels_day[4] = ui_LabelDay5;
  s_weather_labels_high[0] = ui_LabelDay1TempHigh;
  s_weather_labels_high[1] = ui_LabelDay2TempHigh;
  s_weather_labels_high[2] = ui_LabelDay3TempHigh;
  s_weather_labels_high[3] = ui_LabelDay4TempHigh;
  s_weather_labels_high[4] = ui_LabelDay5TempHigh;
  s_weather_labels_low[0] = ui_LabelDay1TempLow;
  s_weather_labels_low[1] = ui_LabelDay2TempLow;
  s_weather_labels_low[2] = ui_LabelDay3TempLow;
  s_weather_labels_low[3] = ui_LabelDay4TempLow;
  s_weather_labels_low[4] = ui_LabelDay5TempLow;
  s_weather_images[0] = ui_ImageDay1Status;
  s_weather_images[1] = ui_ImageDay2Status;
  s_weather_images[2] = ui_ImageDay3Status;
  s_weather_images[3] = ui_ImageDay4Status;
  s_weather_images[4] = ui_ImageDay5Status;
  s_weather_labels_mean[0] = ui_LabelDay1MeanTemp;
  s_weather_labels_mean[1] = ui_LabelDay2MeanTemp;
  s_weather_labels_mean[2] = ui_LabelDay3MeanTemp;
  s_weather_labels_mean[3] = ui_LabelDay4MeanTemp;
  s_weather_labels_mean[4] = ui_LabelDay5MeanTemp;
  s_weather_labels_hum[0] = ui_LabelDay1Hum;
  s_weather_labels_hum[1] = ui_LabelDay2Hum;
  s_weather_labels_hum[2] = ui_LabelDay3Hum;
  s_weather_labels_hum[3] = ui_LabelDay4Hum;
  s_weather_labels_hum[4] = ui_LabelDay5Hum;
  s_weather_labels_uv[0] = ui_LabelDay1Uv;
  s_weather_labels_uv[1] = ui_LabelDay2Uv;
  s_weather_labels_uv[2] = ui_LabelDay3Uv;
  s_weather_labels_uv[3] = ui_LabelDay4Uv;
  s_weather_labels_uv[4] = ui_LabelDay5Uv;
  s_weather_labels_precip[0] = ui_LabelDay1Precipitation;
  s_weather_labels_precip[1] = ui_LabelDay2Precipitation;
  s_weather_labels_precip[2] = ui_LabelDay3Precipitation;
  s_weather_labels_precip[3] = ui_LabelDay4Precipitation;
  s_weather_labels_precip[4] = ui_LabelDay5Precipitation;
  s_weather_labels_wind[0] = ui_LabelDay1WindSpeed;
  s_weather_labels_wind[1] = ui_LabelDay2WindSpeed;
  s_weather_labels_wind[2] = ui_LabelDay3WindSpeed;
  s_weather_labels_wind[3] = ui_LabelDay4WindSpeed;
  s_weather_labels_wind[4] = ui_LabelDay5WindSpeed;
  s_weather_labels_dir[0] = ui_LabelDay1WindDir;
  s_weather_labels_dir[1] = ui_LabelDay2WindDir;
  s_weather_labels_dir[2] = ui_LabelDay3WindDir;
  s_weather_labels_dir[3] = ui_LabelDay4WindDir;
  s_weather_labels_dir[4] = ui_LabelDay5WindDir;
  s_weather_labels_rise[0] = ui_LabelDay1SunriseTime;
  s_weather_labels_rise[1] = ui_LabelDay2SunriseTime;
  s_weather_labels_rise[2] = ui_LabelDay3SunriseTime;
  s_weather_labels_rise[3] = ui_LabelDay4SunriseTime;
  s_weather_labels_rise[4] = ui_LabelDay5SunriseTime;
  s_weather_labels_set[0] = ui_LabelDay1SunsetTime;
  s_weather_labels_set[1] = ui_LabelDay2SunsetTime;
  s_weather_labels_set[2] = ui_LabelDay3SunsetTime;
  s_weather_labels_set[3] = ui_LabelDay4SunsetTime;
  s_weather_labels_set[4] = ui_LabelDay5SunsetTime;
}

// Helper to get day name from date string "YYYY-MM-DD"
static void get_day_name(const char *date_str, char *day_out) {
  int y, m, d;
  if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) == 3) {
    struct tm time_in = {0};
    time_in.tm_year = y - 1900;
    time_in.tm_mon = m - 1;
    time_in.tm_mday = d;
    time_in.tm_isdst = -1;
    mktime(&time_in);

    const char *days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (time_in.tm_wday >= 0 && time_in.tm_wday < 7) {
      strcpy(day_out, days[time_in.tm_wday]);
    } else {
      strcpy(day_out, "---");
    }
  } else {
    strcpy(day_out, "Err");
  }
}

// Helper to get weather icon based on WMO code
static const void *get_weather_icon_day(int code) {
  if (code >= 0 && code <= 1) {
    return &ui_img_01d_2x_png;
  } else if (code == 2) {
    return &ui_img_02d_2x_png;
  } else if (code == 3) {
    return &ui_img_03d_2x_png;
  } else if (code == 51 || code == 53 || (code >= 55 && code <= 57) ||
             (code >= 80 && code <= 82)) {
    return &ui_img_09d_2x_png;
  } else if (code == 61 || code == 63 || (code >= 65 && code <= 67)) {
    return &ui_img_10d_2x_png;
  } else if ((code >= 95 && code <= 96) || code == 99) {
    return &ui_img_11d_2x_png;
  } else if (code == 71 || code == 73 || code == 75 || code == 77 ||
             (code >= 85 && code <= 86)) {
    return &ui_img_13d_2x_png;
  } else if (code == 45 || code == 48) {
    return &ui_img_50d_2x_png;
  }
  return &ui_img_01d_2x_png; // Default
}

static void get_time_from_iso(const char *iso_str, char *out_buf,
                              size_t buf_size) {
  const char *t = strchr(iso_str, 'T');
  if (t && strlen(t) >= 6) {
    strncpy(out_buf, t + 1, 5);
    out_buf[5] = '\0';
  } else {
    strncpy(out_buf, "--:--", buf_size);
  }
}

static const char *get_wind_dir_str(int d) {
  const char *dirs[] = {"N", "ne", "E", "se", "S", "sw", "W", "nw"};
  int idx = (int)((d + 22.5) / 45.0);
  return dirs[idx % 8];
}

// Helper function to update a single day's weather
static void update_day_weather(const weather_data_t *data, int idx,
                               lv_obj_t *label_day, lv_obj_t *label_high,
                               lv_obj_t *label_low, lv_obj_t *img_status,
                               lv_obj_t *label_mean, lv_obj_t *label_hum,
                               lv_obj_t *label_uv, lv_obj_t *label_precip,
                               lv_obj_t *label_wind, lv_obj_t *label_dir,
                               lv_obj_t *label_rise, lv_obj_t *label_set) {
  int64_t start = esp_timer_get_time();
  char day_buf[4];
  char temp_buf[16];
  char time_str[8];

  get_day_name(data->daily[idx].date, day_buf);
  lv_label_set_text(label_day, day_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%.0f", data->daily[idx].temp_max);
  lv_label_set_text(label_high, temp_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%.0f", data->daily[idx].temp_min);
  lv_label_set_text(label_low, temp_buf);
  lv_image_set_src(img_status,
                   get_weather_icon_day(data->daily[idx].weather_code));
  snprintf(temp_buf, sizeof(temp_buf), "%.0f",
           data->daily[idx].apparent_temperature_mean);
  lv_label_set_text(label_mean, temp_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%d", data->daily[idx].humidity_mean);
  lv_label_set_text(label_hum, temp_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%.0f", data->daily[idx].uv_index_max);
  lv_label_set_text(label_uv, temp_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%.1f",
           data->daily[idx].precipitation_sum);
  lv_label_set_text(label_precip, temp_buf);
  snprintf(temp_buf, sizeof(temp_buf), "%.0f", data->daily[idx].wind_speed_max);
  lv_label_set_text(label_wind, temp_buf);
  lv_label_set_text(label_dir,
                    get_wind_dir_str(data->daily[idx].wind_direction_dominant));
  get_time_from_iso(data->daily[idx].sunrise, time_str, sizeof(time_str));
  lv_label_set_text(label_rise, time_str);
  get_time_from_iso(data->daily[idx].sunset, time_str, sizeof(time_str));
  lv_label_set_text(label_set, time_str);

  ESP_LOGD(TAG, "Day %d update took %lld us", idx,
           esp_timer_get_time() - start);
}

// Update sensor UI
static void update_sensor_ui(const bsec_values_t *data) {
  int64_t start = esp_timer_get_time();
  char buf[64];

  snprintf(buf, sizeof(buf), "%.1f", data->comp_temperature);
  if (ui_Label1)
    lv_label_set_text(ui_Label1, buf);

  snprintf(buf, sizeof(buf), "%.1f", data->comp_humidity);
  if (ui_Label2)
    lv_label_set_text(ui_Label2, buf);

  snprintf(buf, sizeof(buf), "%.1f", data->breath_voc_equivalent);
  if (ui_Label3)
    lv_label_set_text(ui_Label3, buf);

  snprintf(buf, sizeof(buf), "%.0f", data->co2_equivalent);
  if (ui_Label4)
    lv_label_set_text(ui_Label4, buf);

  snprintf(buf, sizeof(buf), "%.0f", data->raw_pressure / 100.0f);
  if (ui_Label5)
    lv_label_set_text(ui_Label5, buf);

  snprintf(buf, sizeof(buf), "%.1f", data->gas_percentage);
  if (ui_Label6)
    lv_label_set_text(ui_Label6, buf);

  snprintf(buf, sizeof(buf), "%.0f", data->iaq);
  if (ui_LabelAQI)
    lv_label_set_text(ui_LabelAQI, buf);
  if (ui_SliderAQI)
    lv_slider_set_value(ui_SliderAQI, (int32_t)data->iaq, LV_ANIM_ON);

  // Update accuracy bars
  switch (data->iaq_accuracy) {
  case 0:
    lv_obj_add_flag(ui_ImageAqiAcuBar1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageAqiAcuBar2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageAqiAcuBar3, LV_OBJ_FLAG_HIDDEN);
    break;
  case 1:
    lv_obj_clear_flag(ui_ImageAqiAcuBar1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageAqiAcuBar2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageAqiAcuBar3, LV_OBJ_FLAG_HIDDEN);
    break;
  case 2:
    lv_obj_clear_flag(ui_ImageAqiAcuBar1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageAqiAcuBar2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageAqiAcuBar3, LV_OBJ_FLAG_HIDDEN);
    break;
  case 3:
    lv_obj_clear_flag(ui_ImageAqiAcuBar1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageAqiAcuBar2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageAqiAcuBar3, LV_OBJ_FLAG_HIDDEN);
    break;
  }

  // Update Label X Position: Range 0-500 maps to X 43-203
  int lbl_aqi_start_x = 42;
  if (data->iaq < 10) {
    lbl_aqi_start_x = 47;
  } else if (data->iaq < 100) {
    lbl_aqi_start_x = 44;
  }
  int x_pos = lbl_aqi_start_x + ((int)data->iaq * 160) / 500;
  lv_obj_set_x(ui_LabelAQI, x_pos);

  // Update Slider Knob Color
  lv_color_t knob_color;
  if (data->iaq <= 50) {
    knob_color = lv_color_hex(0x00E400);
  } else if (data->iaq <= 100) {
    knob_color = lv_color_hex(0x92D050);
  } else if (data->iaq <= 150) {
    knob_color = lv_color_hex(0xFFFF00);
  } else if (data->iaq <= 200) {
    knob_color = lv_color_hex(0xFF7E00);
  } else if (data->iaq <= 250) {
    knob_color = lv_color_hex(0xFF0000);
  } else if (data->iaq <= 350) {
    knob_color = lv_color_hex(0x99004C);
  } else {
    knob_color = lv_color_hex(0x663300);
  }
  lv_obj_set_style_bg_color(ui_SliderAQI, knob_color,
                            LV_PART_KNOB | LV_STATE_DEFAULT);
  ESP_LOGD(TAG, "Sensor UI update took %lld us", esp_timer_get_time() - start);
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text) {
  if (label) {
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) {
      lv_label_set_text(label, text);
    }
  }
}

static void format_runtime(uint64_t total_seconds, char *buffer,
                           size_t buffer_size) {
  unsigned long long hours = (unsigned long long)(total_seconds / 3600ULL);
  unsigned long long minutes = (unsigned long long)((total_seconds / 60ULL) % 60ULL);
  unsigned long long seconds = (unsigned long long)(total_seconds % 60ULL);
  snprintf(buffer, buffer_size, "%02llu:%02llu:%02llu", hours, minutes,
           seconds);
}

// Update stock UI
static void update_stock_ui(stock_values_t *data) {
  if (!data)
    return;
  char buf[32];

  // --- STOCK 1 ---
  snprintf(buf, sizeof(buf), "$%.2f", data->stock_1.current_price);
  lv_label_set_text(ui_LabelStock1Price, buf);
  lv_label_set_text(ui_LabelStock1PriceEx, buf);

  snprintf(buf, sizeof(buf), "%.2f%%", data->stock_1.percent_change);
  lv_label_set_text(ui_LabelStock1Change, buf);
  lv_label_set_text(ui_LabelStock1ChangeEx, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_1.previous_close);
  lv_label_set_text(ui_LabelStock1PrevClose, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_1.high_price);
  lv_label_set_text(ui_LabelStock1HighPrice, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_1.low_price);
  lv_label_set_text(ui_LabelStock1LowPrice, buf);

  // Stock 1 Images
  if (data->stock_1.percent_change >= 0) {
    lv_image_set_src(ui_ImageStock1Dir, &ui_img_up_green_18_3_png);
    lv_image_set_src(ui_ImageStock1DirEx, &ui_img_up_green_18_3_png);

    lv_obj_set_style_text_color(ui_LabelStock1Change, lv_color_hex(0x2ECC71),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelStock1ChangeEx, lv_color_hex(0x2ECC71),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  } else {
    lv_image_set_src(ui_ImageStock1Dir, &ui_img_down_red_18_png);
    lv_image_set_src(ui_ImageStock1DirEx, &ui_img_down_red_18_png);

    lv_obj_set_style_text_color(ui_LabelStock1Change, lv_color_hex(0xFF0000),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelStock1ChangeEx, lv_color_hex(0xFF0000),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  // --- STOCK 2 ---
  snprintf(buf, sizeof(buf), "$%.2f", data->stock_2.current_price);
  lv_label_set_text(ui_LabelStock2Price, buf);
  lv_label_set_text(ui_LabelStock2PriceEx, buf);

  snprintf(buf, sizeof(buf), "%.2f%%", data->stock_2.percent_change);
  lv_label_set_text(ui_LabelStock2Change, buf);
  lv_label_set_text(ui_LabelStock2ChangeEx, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_2.previous_close);
  lv_label_set_text(ui_LabelStock2PrevClose, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_2.high_price);
  lv_label_set_text(ui_LabelStock2HighPrice, buf);

  snprintf(buf, sizeof(buf), "$%.2f", data->stock_2.low_price);
  lv_label_set_text(ui_LabelStock2LowPrice, buf);

  // Stock 2 Images
  if (data->stock_2.percent_change >= 0) {
    lv_image_set_src(ui_ImageStock2Dir, &ui_img_up_green_18_3_png);
    lv_image_set_src(ui_ImageStock2DirEx, &ui_img_up_green_18_3_png);
    lv_obj_set_style_text_color(ui_LabelStock2Change, lv_color_hex(0x2ECC71),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelStock2ChangeEx, lv_color_hex(0x2ECC71),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  } else {
    lv_image_set_src(ui_ImageStock2Dir, &ui_img_down_red_18_png);
    lv_image_set_src(ui_ImageStock2DirEx, &ui_img_down_red_18_png);
    lv_obj_set_style_text_color(ui_LabelStock2Change, lv_color_hex(0xFF0000),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelStock2ChangeEx, lv_color_hex(0xFF0000),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}

// Main UI update timer callback - runs every 1 second inside LVGL task
// context
static void ui_update_timer_cb(lv_timer_t *timer) {
  (void)timer;
  int64_t start_all = esp_timer_get_time();
  int64_t t1, t2;

  // 1. Update Sensor UI
  int64_t current_sensor_ts = bsec_get_last_updated_time();
  static int64_t last_sensor_ts = 0;

  if (current_sensor_ts != last_sensor_ts || s_first_run) {
    bsec_values_t sensor_data = {0};
    bool sensor_updated = false;
    t1 = esp_timer_get_time();
    if (bsec_get_latest_values(&sensor_data, &sensor_updated) == ESP_OK) {
      t2 = esp_timer_get_time();
      ESP_LOGD(TAG, "Sensor fetch took %lld us", t2 - t1);
      update_sensor_ui(&sensor_data);
      last_sensor_ts = current_sensor_ts;
      ESP_LOGD(TAG, "Sensor UI updated (TS: %lld)", current_sensor_ts);
    }
  }

  // Charger state and battery runtime use the existing one-second UI timer.
  esp_err_t runtime_err = battery_runtime_update(
      &s_battery_current_seconds, &s_battery_previous_seconds,
      &s_charger_connected);
  if (runtime_err == ESP_OK) {
    s_charger_state_known = true;
  }

  char runtime_buf[32];
  format_runtime(s_battery_current_seconds, runtime_buf, sizeof(runtime_buf));
  set_label_text_if_changed(ui_LabelSettingsBatteryTimeCounter, runtime_buf);
  format_runtime(s_battery_previous_seconds, runtime_buf, sizeof(runtime_buf));
  set_label_text_if_changed(ui_LabelSettingsBatteryPrevTimeCounter, runtime_buf);

  // Keep voltage and percentage sampling on the existing 10-second cadence.
  static int64_t last_battery_sample_us = 0;
  int64_t now = esp_timer_get_time();
  if (s_first_run || now - last_battery_sample_us >= 10LL * 1000000) {
    last_battery_sample_us = now;
    if (battery_read(&s_battery_mv, &s_battery_percent) == ESP_OK) {
      s_battery_sample_available = true;
      char battery_buf[16];
      snprintf(battery_buf, sizeof(battery_buf), "%d%%", s_battery_percent);
      set_label_text_if_changed(ui_LabelSettingsBatteryLevelPercentage,
                                battery_buf);
    }
  }

  char battery_raw_buf[16];
  if (s_charger_state_known && s_charger_connected) {
    set_label_text_if_changed(ui_LabelSettingsBatteryLevelRaw, "Charging");
  } else if (s_battery_sample_available) {
    snprintf(battery_raw_buf, sizeof(battery_raw_buf), "%d mV", s_battery_mv);
    set_label_text_if_changed(ui_LabelSettingsBatteryLevelRaw, battery_raw_buf);
  }

  // 2. Update Stock UI
  int64_t current_stock_ts = stock_get_last_updated_time();
  static int64_t last_stock_ts = 0;

  if (current_stock_ts != last_stock_ts || s_first_run) {
    stock_values_t stock_data = {0};
    bool stock_updated = false;
    t1 = esp_timer_get_time();
    if (stock_get_latest_values(&stock_data, &stock_updated) == ESP_OK) {
      t2 = esp_timer_get_time();
      ESP_LOGD(TAG, "Stock fetch took %lld us", t2 - t1);
      update_stock_ui(&stock_data);
      last_stock_ts = current_stock_ts;
      ESP_LOGD(TAG, "Stock UI updated (TS: %lld)", current_stock_ts);
    }
  }

  // 3. Update Weather UI
  int64_t current_weather_ts = weather_get_last_updated_time();
  static int64_t last_weather_ts = 0;

  if (current_weather_ts != last_weather_ts || s_first_run) {
    bool weather_updated = false;
    t1 = esp_timer_get_time();
    if (weather_get_latest_values(&s_weather_data, &weather_updated) ==
        ESP_OK) {
      t2 = esp_timer_get_time();
      ESP_LOGD(TAG, "Weather fetch took %lld us", t2 - t1);

      // Update all 5 days using pre-initialized arrays
      for (int i = 0; i < 5; i++) {
        update_day_weather(&s_weather_data, i, s_weather_labels_day[i],
                           s_weather_labels_high[i], s_weather_labels_low[i],
                           s_weather_images[i], s_weather_labels_mean[i],
                           s_weather_labels_hum[i], s_weather_labels_uv[i],
                           s_weather_labels_precip[i], s_weather_labels_wind[i],
                           s_weather_labels_dir[i], s_weather_labels_rise[i],
                           s_weather_labels_set[i]);
      }

      // Update Day 1 current weather labels
      char cur_buf[16];
      snprintf(cur_buf, sizeof(cur_buf), "%.0f", s_weather_data.current_temp);
      if (ui_LabelDay1CurTemp)
        lv_label_set_text(ui_LabelDay1CurTemp, cur_buf);
      snprintf(cur_buf, sizeof(cur_buf), "%d", s_weather_data.current_humidity);
      if (ui_LabelDay1CurHum)
        lv_label_set_text(ui_LabelDay1CurHum, cur_buf);
      snprintf(cur_buf, sizeof(cur_buf), "%.1f",
               s_weather_data.current_precipitation);
      if (ui_LabelDay1CurPrecipitation)
        lv_label_set_text(ui_LabelDay1CurPrecipitation, cur_buf);
      snprintf(cur_buf, sizeof(cur_buf), "%.0f",
               s_weather_data.current_wind_speed);
      if (ui_LabelDay1CurWindSpeed)
        lv_label_set_text(ui_LabelDay1CurWindSpeed, cur_buf);
      if (ui_LabelDay1CurWindDir)
        lv_label_set_text(
            ui_LabelDay1CurWindDir,
            get_wind_dir_str(s_weather_data.current_wind_direction));

      last_weather_ts = current_weather_ts;
      ESP_LOGD(TAG, "Weather UI updated (TS: %lld)", current_weather_ts);
    }
  }

  // 4. Update Time and Date
  char time_buf[32];
  char date_buf[32];
  time_manager_get_time(time_buf, sizeof(time_buf), true);
  time_manager_get_date(date_buf, sizeof(date_buf));

  if (s_first_run || strcmp(time_buf, s_cached_time) != 0) {
    lv_label_set_text(ui_LabelTime, time_buf);
    strncpy(s_cached_time, time_buf, sizeof(s_cached_time));
  }
  if (s_first_run || strcmp(date_buf, s_cached_date) != 0) {
    lv_label_set_text(ui_LabelDate, date_buf);
    strncpy(s_cached_date, date_buf, sizeof(s_cached_date));
  }

  // 5. Update WiFi status
  bool wifi_connected = wifi_is_connected();
  if (s_first_run || wifi_connected != s_cached_wifi_connected) {
    if (wifi_connected) {
      lv_obj_clear_flag(ui_ImageWifiConn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_ImageWifiDis, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui_ImageWifiConn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_ImageWifiDis, LV_OBJ_FLAG_HIDDEN);
    }
    s_cached_wifi_connected = wifi_connected;
  }

  // 6. Check for WiFi scan results
  check_wifi_scan_results();
  // 7. Check for WiFi connection status
  check_wifi_connection_status();

  // 8. Check for async weather city lookup result
  int city_result = weather_get_city_lookup_result();
  if (city_result != 0) {
    if (city_result == 1) {
      // Success
      char lat[16], lon[16];
      if (weather_get_location(lat, sizeof(lat), lon, sizeof(lon)) == ESP_OK) {
        char status[48];
        snprintf(status, sizeof(status), "%s, %s", lat, lon);
        if (ui_LabelWeatherStatus)
          lv_label_set_text(ui_LabelWeatherStatus, status);
      } else {
        if (ui_LabelWeatherStatus)
          lv_label_set_text(ui_LabelWeatherStatus, "");
      }
      ESP_LOGI(TAG, "City lookup succeeded");
    } else if (city_result == -1) {
      // Not found
      if (ui_LabelWeatherStatus) {
        lv_label_set_text(ui_LabelWeatherStatus, "City not found");
      }
      ESP_LOGW(TAG, "City lookup: not found");
    } else {
      // Error
      if (ui_LabelWeatherStatus) {
        lv_label_set_text(ui_LabelWeatherStatus, "Lookup failed");
      }
      ESP_LOGE(TAG, "City lookup failed");
    }
  }

  s_first_run = false;

  int64_t diff = esp_timer_get_time() - start_all;
  if (diff > 5000)
    ESP_LOGI(TAG, "UI update loop took %lld us", diff);
}

// --- WiFi Scan Logic ---

static void check_wifi_scan_results(void) {
  if (wifi_get_scan_state() == WIFI_SCAN_COMPLETED) {
    char *net_str = NULL;
    if (wifi_get_scan_results(&net_str) == ESP_OK) {
      ESP_LOGI(TAG, "Scan results retrieved");
      if (net_str) {
        if (ui_DropdownFoundNetworks) {
          lv_dropdown_set_options(ui_DropdownFoundNetworks, net_str);
        }
        free(net_str); // We own the string now

        if (ui_LabelNetworkStatus)
          lv_label_set_text(ui_LabelNetworkStatus, "Select Network");

        // Check if the default selected network has saved usage
        if (ui_DropdownFoundNetworks) {
          char ssid[33] = {0};
          lv_dropdown_get_selected_str(ui_DropdownFoundNetworks, ssid,
                                       sizeof(ssid));
          if (strlen(ssid) > 0 && wifi_has_saved_credentials(ssid)) {
            if (ui_TextAreaNetworkPassword) {
              lv_textarea_set_text(ui_TextAreaNetworkPassword, "******");
            }
          } else {
            if (ui_TextAreaNetworkPassword) {
              lv_textarea_set_text(ui_TextAreaNetworkPassword, "");
            }
          }
        }

      } else {
        if (ui_DropdownFoundNetworks) {
          lv_dropdown_set_options(ui_DropdownFoundNetworks, "");
        }
        if (ui_LabelNetworkStatus)
          lv_label_set_text(ui_LabelNetworkStatus, "No Networks");
      }
    } else {
      // Fallback if getter failed despite state being COMPLETED
      if (ui_LabelNetworkStatus)
        lv_label_set_text(ui_LabelNetworkStatus, "Scan Error");
    }

    // Reset UI Elements
    if (ui_SpinnerNetworkScanningSpinner)
      lv_obj_add_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);

    if (ui_ButtonNetworkScan)
      lv_obj_clear_flag(ui_ButtonNetworkScan, LV_OBJ_FLAG_HIDDEN);
  } else if (wifi_get_scan_state() == WIFI_SCAN_FAILED) {
    ESP_LOGE(TAG, "WiFi scan failed");
    if (ui_LabelNetworkStatus) {
      lv_label_set_text(ui_LabelNetworkStatus, "Scan Error");
    }

    // Clear dropdown on error
    if (ui_DropdownFoundNetworks) {
      lv_dropdown_set_options(ui_DropdownFoundNetworks, "");
    }

    // Reset UI Elements
    if (ui_SpinnerNetworkScanningSpinner)
      lv_obj_add_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);

    if (ui_ButtonNetworkScan)
      lv_obj_clear_flag(ui_ButtonNetworkScan, LV_OBJ_FLAG_HIDDEN);
  }
}

// --- WiFi Connection Logic ---
static void check_wifi_connection_status(void) {
  wifi_connection_state_t state = wifi_get_connection_state();
  static wifi_connection_state_t last_state = WIFI_CONNECTION_IDLE;

  if (state == last_state)
    return;
  last_state = state;

  if (state == WIFI_CONNECTION_CONNECTING) {
    if (ui_LabelNetworkStatus)
      lv_label_set_text(ui_LabelNetworkStatus, "Connecting...");
  } else if (state == WIFI_CONNECTION_CONNECTED) {
    if (ui_LabelNetworkStatus)
      lv_label_set_text(ui_LabelNetworkStatus, "Connected");
  } else if (state == WIFI_CONNECTION_FAILED) {
    if (ui_LabelNetworkStatus)
      lv_label_set_text(ui_LabelNetworkStatus, "Connection Failed");
  }
}

// Tab indices for Screen2 TabView
#define TAB_IDX_NETWORK 0
#define TAB_IDX_STOCK 1
#define TAB_IDX_WEATHER 2
#define TAB_IDX_SETTINGS 3

static bool s_network_ui_wifi_held;

static void set_network_ui_wifi_hold(bool held) {
  if (held == s_network_ui_wifi_held) return;
  if (held) {
    if (wifi_power_acquire(WIFI_POWER_NETWORK_UI) == ESP_OK)
      s_network_ui_wifi_held = true;
    else
      ESP_LOGW(TAG, "Could not keep Wi-Fi on for Network tab");
  } else {
    wifi_power_release(WIFI_POWER_NETWORK_UI);
    s_network_ui_wifi_held = false;
  }
}

// Helper: navigate to Screen2 and select a specific tab
static void navigate_to_screen2_tab(uint32_t tab_idx) {
  _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_MOVE_LEFT, 100, 0,
                    &ui_Screen2_screen_init);
  if (ui_TabView1) {
    lv_tabview_set_active(ui_TabView1, tab_idx, LV_ANIM_OFF);
  }
  set_network_ui_wifi_hold(tab_idx == TAB_IDX_NETWORK);
  // Show/hide keyboard based on target tab
  if (ui_Keyboard) {
    if (tab_idx == TAB_IDX_SETTINGS) {
      lv_obj_add_flag(ui_Keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(ui_Keyboard, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// Helper: hide all Stock and Forecast Extra panels when returning to Screen1
static void ui_event_CancelButton_HideExtras(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    // Hide Stock Extra panels
    if (ui_PanelStock1Extra)
      lv_obj_add_flag(ui_PanelStock1Extra, LV_OBJ_FLAG_HIDDEN);
    if (ui_PanelStock2Extra)
      lv_obj_add_flag(ui_PanelStock2Extra, LV_OBJ_FLAG_HIDDEN);
    // Hide Forecast Extra panels
    if (ui_PanelForecastDay1Extra)
      lv_obj_add_flag(ui_PanelForecastDay1Extra, LV_OBJ_FLAG_HIDDEN);
    if (ui_PanelForecastDay2Extra)
      lv_obj_add_flag(ui_PanelForecastDay2Extra, LV_OBJ_FLAG_HIDDEN);
    if (ui_PanelForecastDay3Extra)
      lv_obj_add_flag(ui_PanelForecastDay3Extra, LV_OBJ_FLAG_HIDDEN);
    if (ui_PanelForecastDay4Extra)
      lv_obj_add_flag(ui_PanelForecastDay4Extra, LV_OBJ_FLAG_HIDDEN);
    if (ui_PanelForecastDay5Extra)
      lv_obj_add_flag(ui_PanelForecastDay5Extra, LV_OBJ_FLAG_HIDDEN);

    _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_MOVE_LEFT, 100, 0,
                      &ui_Screen1_screen_init);
    set_network_ui_wifi_hold(false);
  }
}

// Navigation: PanelHeader Click -> Screen2 / Network tab
static void ui_event_PanelHeader_Click(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    ESP_LOGI(TAG, "PanelHeader clicked - Navigating to Network tab");
    navigate_to_screen2_tab(TAB_IDX_NETWORK);

    // Set Keyboard target to Network Password
    if (ui_Keyboard && ui_TextAreaNetworkPassword) {
      lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaNetworkPassword);
    }

    // Preserve state if connected, otherwise cleanup
    if (wifi_is_connected()) {
      if (ui_TextAreaNetworkPassword) {
        lv_textarea_set_text(ui_TextAreaNetworkPassword, "******");
      }
      if (ui_SpinnerNetworkScanningSpinner) {
        lv_obj_add_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      if (ui_DropdownFoundNetworks) {
        lv_dropdown_set_options(ui_DropdownFoundNetworks, "");
        lv_dropdown_set_selected(ui_DropdownFoundNetworks, 0);
      }
      if (ui_TextAreaNetworkPassword) {
        lv_textarea_set_text(ui_TextAreaNetworkPassword, "");
        lv_textarea_set_placeholder_text(ui_TextAreaNetworkPassword,
                                         "Password");
      }
      if (ui_LabelNetworkStatus) {
        lv_label_set_text(ui_LabelNetworkStatus, "");
      }
      if (ui_SpinnerNetworkScanningSpinner) {
        lv_obj_add_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

// Event handler for brightness slider value change
static void ui_event_BrightnessSlider_ValueChanged(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_VALUE_CHANGED) {
    int32_t value = lv_slider_get_value(lv_event_get_target_obj(e));
    ESP_LOGI(TAG, "Brightness slider: %ld%%", (long)value);
    display_set_brightness((uint8_t)value);
  }
}

// TabView change handler: hide Keyboard on Settings tab, show on others
static void ui_event_TabView_ValueChanged(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_VALUE_CHANGED) {
    uint32_t active_tab = lv_tabview_get_tab_active(ui_TabView1);
    set_network_ui_wifi_hold(active_tab == TAB_IDX_NETWORK);
    if (ui_Keyboard) {
      if (active_tab == TAB_IDX_SETTINGS) {
        lv_obj_add_flag(ui_Keyboard, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_clear_flag(ui_Keyboard, LV_OBJ_FLAG_HIDDEN);
        // Set keyboard textarea target based on active tab
        if (active_tab == TAB_IDX_NETWORK && ui_TextAreaNetworkPassword) {
          lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaNetworkPassword);
        } else if (active_tab == TAB_IDX_STOCK && ui_TextAreaStockTicker) {
          lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaStockTicker);
        } else if (active_tab == TAB_IDX_WEATHER &&
                   ui_TextAreaWeatherCityName) {
          lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaWeatherCityName);
        }
      }
    }
  }
}

// Event handler for Dropdown Selection Change
static void ui_event_DropdownFoundNetworks_ValueChanged(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_VALUE_CHANGED) {
    char ssid[33] = {0};
    lv_dropdown_get_selected_str(ui_DropdownFoundNetworks, ssid, sizeof(ssid));
    ESP_LOGI(TAG, "Dropdown Selection Changed: %s", ssid);

    if (strlen(ssid) > 0) {
      if (wifi_has_saved_credentials(ssid)) {
        if (ui_TextAreaNetworkPassword) {
          lv_textarea_set_text(ui_TextAreaNetworkPassword, "******");
        }
      } else {
        if (ui_TextAreaNetworkPassword) {
          lv_textarea_set_text(ui_TextAreaNetworkPassword, "");
        }
      }
    }
  }
}

// Event handler for OK (Connect) button
void ui_event_ButtonNetworkOK(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    char ssid[33] = {0};
    if (ui_DropdownFoundNetworks) {
      lv_dropdown_get_selected_str(ui_DropdownFoundNetworks, ssid,
                                   sizeof(ssid));
    }

    const char *input_text = "";
    if (ui_TextAreaNetworkPassword) {
      input_text = lv_textarea_get_text(ui_TextAreaNetworkPassword);
    }

    char password[64] = {0};
    if (input_text)
      strncpy(password, input_text, sizeof(password) - 1);

    // Check if password is masked and we have saved creds
    if (strcmp(password, "******") == 0) {
      if (wifi_has_saved_credentials(ssid)) {
        ESP_LOGI(TAG, "Using saved credentials for %s", ssid);
        wifi_load_credentials(ssid, password, sizeof(password));
      } else {
        ESP_LOGW(TAG, "Password is masked but no saved creds found");
      }
    }

    if (strlen(ssid) == 0) {
      if (ui_LabelNetworkStatus)
        lv_label_set_text(ui_LabelNetworkStatus, "Select Network");
      if (ui_ContainerNetworkStatus)
        lv_obj_clear_flag(ui_ContainerNetworkStatus, LV_OBJ_FLAG_HIDDEN);
      return;
    }

    ESP_LOGI(TAG, "Connect requested to: %s", ssid);

    // Update UI immediately
    if (ui_LabelNetworkStatus)
      lv_label_set_text(ui_LabelNetworkStatus, "Connecting...");

    wifi_connect_to_ap(ssid, password);
  }
}

// Global scope so it can be linked if needed, though we attach manually
void ui_event_ButtonNetworkScan(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    ESP_LOGI(TAG, "Scan button clicked");
    if (ui_ButtonNetworkScan)
      lv_obj_add_flag(ui_ButtonNetworkScan, LV_OBJ_FLAG_HIDDEN);

    if (ui_SpinnerNetworkScanningSpinner)
      lv_obj_clear_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);

    if (ui_LabelNetworkStatus)
      lv_label_set_text(ui_LabelNetworkStatus, "Scanning...");

    // Removed container visibility toggle

    // Clear dropdown during scan
    if (ui_DropdownFoundNetworks)
      lv_dropdown_set_options(ui_DropdownFoundNetworks, "");

    // Start non-blocking scan
    esp_err_t err = wifi_start_scan();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to start scan");
      if (ui_LabelNetworkStatus) {
        lv_label_set_text(ui_LabelNetworkStatus, "Start Failed");
      }
      // Removed container visibility toggle
      if (ui_LabelNetworkStatus)
        lv_label_set_text(ui_LabelNetworkStatus, "Start Failed");
      // Revert UI state if failed
      if (ui_ButtonNetworkScan)
        lv_obj_clear_flag(ui_ButtonNetworkScan, LV_OBJ_FLAG_HIDDEN);
      if (ui_SpinnerNetworkScanningSpinner)
        lv_obj_add_flag(ui_SpinnerNetworkScanningSpinner, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// Event handler for Stock Panel Long Press -> Screen2 / Stock tab
static void ui_event_StockPanel_LongPress(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_LONG_PRESSED) {
    // Get slot number from user_data (1 or 2)
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Stock Panel Long Pressed - Slot %d", slot);

    navigate_to_screen2_tab(TAB_IDX_STOCK);

    // Pre-select the stock slot in DropdownStocks (0 = Stock 1, 1 = Stock 2)
    if (ui_DropdownStocks) {
      lv_dropdown_set_selected(ui_DropdownStocks, (uint32_t)(slot - 1));
    }

    // Setup Ticker Input
    if (ui_TextAreaStockTicker) {
      lv_textarea_set_text(ui_TextAreaStockTicker, "");
      lv_textarea_set_placeholder_text(ui_TextAreaStockTicker, "Ticker");

      if (ui_Keyboard) {
        lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaStockTicker);
      }

      if (ui_LabelStockStatus) {
        lv_label_set_text(ui_LabelStockStatus, "");
      }
    }
  }
}

// Event handler for Stock OK button
static void ui_event_ButtonStockOK(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    // Get ticker from input
    const char *ticker = "";
    if (ui_TextAreaStockTicker) {
      ticker = lv_textarea_get_text(ui_TextAreaStockTicker);
    }

    // Get stock slot from DropdownStocks (0-based -> 1-based)
    int s_stock_edit_slot = 1;
    if (ui_DropdownStocks) {
      s_stock_edit_slot = (int)lv_dropdown_get_selected(ui_DropdownStocks) + 1;
    }

    if (ticker && strlen(ticker) > 0) {
      ESP_LOGI(TAG, "Setting stock slot %d to: %s", s_stock_edit_slot, ticker);

      // Set the new symbol
      esp_err_t save_err = stock_set_symbol(s_stock_edit_slot, ticker);
      if (save_err != ESP_OK) {
        if (ui_LabelStockStatus)
          lv_label_set_text(ui_LabelStockStatus, "Save failed");
        return;
      }

      // Update the UI label immediately
      if (s_stock_edit_slot == 1) {
        if (ui_LabelStock1Symbol)
          lv_label_set_text(ui_LabelStock1Symbol, ticker);
        if (ui_LabelStock1SymbolEx)
          lv_label_set_text(ui_LabelStock1SymbolEx, ticker);
        // Reset price/change labels to placeholders
        if (ui_LabelStock1Price)
          lv_label_set_text(ui_LabelStock1Price, "$");
        if (ui_LabelStock1Change)
          lv_label_set_text(ui_LabelStock1Change, "%");
        if (ui_LabelStock1PriceEx)
          lv_label_set_text(ui_LabelStock1PriceEx, "$");
        if (ui_LabelStock1ChangeEx)
          lv_label_set_text(ui_LabelStock1ChangeEx, "%");
        if (ui_LabelStock1PrevClose)
          lv_label_set_text(ui_LabelStock1PrevClose, "$");
        if (ui_LabelStock1HighPrice)
          lv_label_set_text(ui_LabelStock1HighPrice, "$");
        if (ui_LabelStock1LowPrice)
          lv_label_set_text(ui_LabelStock1LowPrice, "$");
      } else {
        if (ui_LabelStock2Symbol)
          lv_label_set_text(ui_LabelStock2Symbol, ticker);
        if (ui_LabelStock2SymbolEx)
          lv_label_set_text(ui_LabelStock2SymbolEx, ticker);
        // Reset price/change labels to placeholders
        if (ui_LabelStock2Price)
          lv_label_set_text(ui_LabelStock2Price, "$");
        if (ui_LabelStock2Change)
          lv_label_set_text(ui_LabelStock2Change, "%");
        if (ui_LabelStock2PriceEx)
          lv_label_set_text(ui_LabelStock2PriceEx, "$");
        if (ui_LabelStock2ChangeEx)
          lv_label_set_text(ui_LabelStock2ChangeEx, "%");
        if (ui_LabelStock2PrevClose)
          lv_label_set_text(ui_LabelStock2PrevClose, "$");
        if (ui_LabelStock2HighPrice)
          lv_label_set_text(ui_LabelStock2HighPrice, "$");
        if (ui_LabelStock2LowPrice)
          lv_label_set_text(ui_LabelStock2LowPrice, "$");
      }

      // Update status
      if (ui_LabelStockStatus) {
        lv_label_set_text(ui_LabelStockStatus, "Saved!");
      }

      // Trigger immediate refresh of stock data
      network_sync_request(NETWORK_SYNC_STOCKS);
    } else {
      if (ui_LabelStockStatus) {
        lv_label_set_text(ui_LabelStockStatus, "Enter ticker");
      }
      return; // Don't navigate back
    }
  }
}

// Event handler for Weather Panel Long Press -> Screen2 / Weather tab
static void ui_event_WeatherPanel_LongPress(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_LONG_PRESSED) {
    ESP_LOGI(TAG, "Weather Panel Long Pressed - Entering Weather tab");

    navigate_to_screen2_tab(TAB_IDX_WEATHER);

    // Setup City Input - show current city or placeholder
    if (ui_TextAreaWeatherCityName) {
      char current_city[64];
      if (weather_get_city(current_city, sizeof(current_city)) == ESP_OK) {
        lv_textarea_set_text(ui_TextAreaWeatherCityName, current_city);
      } else {
        lv_textarea_set_text(ui_TextAreaWeatherCityName, "");
        lv_textarea_set_placeholder_text(ui_TextAreaWeatherCityName, "City");
      }

      if (ui_Keyboard) {
        lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaWeatherCityName);
      }

      // Show lat/lon in status if city exists
      char lat[16], lon[16];
      if (weather_get_location(lat, sizeof(lat), lon, sizeof(lon)) == ESP_OK) {
        char status[48];
        snprintf(status, sizeof(status), "%s, %s", lat, lon);
        lv_label_set_text(ui_LabelWeatherStatus, status);
      } else {
        lv_label_set_text(ui_LabelWeatherStatus, "");
      }
    }
  }
}

// Event handler for Weather OK button
static void ui_event_ButtonWeatherOK(lv_event_t *e) {
  lv_event_code_t event_code = lv_event_get_code(e);
  if (event_code == LV_EVENT_CLICKED) {
    // Get city from input
    const char *city = "";
    if (ui_TextAreaWeatherCityName) {
      city = lv_textarea_get_text(ui_TextAreaWeatherCityName);
    }

    if (city && strlen(city) > 0) {
      ESP_LOGI(TAG, "Queueing weather city lookup: %s", city);
      if (ui_LabelWeatherStatus) {
        lv_label_set_text(ui_LabelWeatherStatus, "Looking up...");
      }

      // Queue async city lookup (non-blocking)
      esp_err_t city_err = weather_set_city(city);
      if (city_err != ESP_OK) {
        if (ui_LabelWeatherStatus)
          lv_label_set_text(ui_LabelWeatherStatus, "Request failed");
        return;
      }
      network_sync_request(NETWORK_SYNC_WEATHER);

      // Reset all weather day labels to placeholders while lookup in progress
      for (int i = 0; i < 5; i++) {
        if (s_weather_images[i])
          lv_image_set_src(s_weather_images[i], &ui_img_01d_2x_png);
        if (s_weather_labels_high[i])
          lv_label_set_text(s_weather_labels_high[i], "?");
        if (s_weather_labels_low[i])
          lv_label_set_text(s_weather_labels_low[i], "?");
        if (s_weather_labels_mean[i])
          lv_label_set_text(s_weather_labels_mean[i], "?");
        if (s_weather_labels_hum[i])
          lv_label_set_text(s_weather_labels_hum[i], "?");
        if (s_weather_labels_uv[i])
          lv_label_set_text(s_weather_labels_uv[i], "?");
        if (s_weather_labels_precip[i])
          lv_label_set_text(s_weather_labels_precip[i], "?");
        if (s_weather_labels_wind[i])
          lv_label_set_text(s_weather_labels_wind[i], "?");
        if (s_weather_labels_dir[i])
          lv_label_set_text(s_weather_labels_dir[i], "?");
        if (s_weather_labels_rise[i])
          lv_label_set_text(s_weather_labels_rise[i], "?");
        if (s_weather_labels_set[i])
          lv_label_set_text(s_weather_labels_set[i], "?");
      }
      // Reset current weather labels for Day 1
      if (ui_LabelDay1CurTemp)
        lv_label_set_text(ui_LabelDay1CurTemp, "?");
      if (ui_LabelDay1CurHum)
        lv_label_set_text(ui_LabelDay1CurHum, "?");
      if (ui_LabelDay1CurPrecipitation)
        lv_label_set_text(ui_LabelDay1CurPrecipitation, "?");
      if (ui_LabelDay1CurWindSpeed)
        lv_label_set_text(ui_LabelDay1CurWindSpeed, "?");
      if (ui_LabelDay1CurWindDir)
        lv_label_set_text(ui_LabelDay1CurWindDir, "?");
      // Result will be polled in the UI update loop
    } else {
      if (ui_LabelWeatherStatus) {
        lv_label_set_text(ui_LabelWeatherStatus, "Enter city");
      }
    }
  }
}

void ui_updater_init(void) {
  ESP_LOGI(TAG, "Initializing UI updater timer");

  if (ui_LabelSettingsBatteryLevelPercentage)
    lv_label_set_text(ui_LabelSettingsBatteryLevelPercentage, "--");
  if (ui_LabelSettingsBatteryLevelRaw)
    lv_label_set_text(ui_LabelSettingsBatteryLevelRaw, "--");
  if (ui_LabelSettingsBatteryTimeCounter)
    lv_label_set_text(ui_LabelSettingsBatteryTimeCounter, "00:00:00");
  if (ui_LabelSettingsBatteryPrevTimeCounter)
    lv_label_set_text(ui_LabelSettingsBatteryPrevTimeCounter, "00:00:00");

  // Initialize stock symbol labels from NVS (before WiFi)
  char sym1[16] = {0};
  char sym2[16] = {0};
  if (stock_get_symbol(1, sym1, sizeof(sym1)) == ESP_OK && strlen(sym1) > 0) {
    if (ui_LabelStock1Symbol)
      lv_label_set_text(ui_LabelStock1Symbol, sym1);
    if (ui_LabelStock1SymbolEx)
      lv_label_set_text(ui_LabelStock1SymbolEx, sym1);
    ESP_LOGI(TAG, "Set stock 1 label to: %s", sym1);
  }
  if (stock_get_symbol(2, sym2, sizeof(sym2)) == ESP_OK && strlen(sym2) > 0) {
    if (ui_LabelStock2Symbol)
      lv_label_set_text(ui_LabelStock2Symbol, sym2);
    if (ui_LabelStock2SymbolEx)
      lv_label_set_text(ui_LabelStock2SymbolEx, sym2);
    ESP_LOGI(TAG, "Set stock 2 label to: %s", sym2);
  }

  // Initialize weather UI element arrays
  init_weather_ui_arrays();

  // Create timer that runs every 1000ms (1 second)
  lv_timer_create(ui_update_timer_cb, 1000, NULL);

  // Manually attach event handler to the scan button
  // ui_ButtonNetworkScan should be available from ui.h
  if (ui_ButtonNetworkScan) {
    lv_obj_add_event_cb(ui_ButtonNetworkScan, ui_event_ButtonNetworkScan,
                        LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Attached scan event handler");
  } else {
    ESP_LOGE(TAG, "ui_ButtonNetworkScan is NULL!");
  }

  if (ui_ButtonNetworkOK) {
    lv_obj_add_event_cb(ui_ButtonNetworkOK, ui_event_ButtonNetworkOK,
                        LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Attached connect event handler");
  } else {
    ESP_LOGE(TAG, "ui_ButtonNetworkOK is NULL!");
  }

  if (ui_PanelHeader) {
    lv_obj_add_event_cb(ui_PanelHeader, ui_event_PanelHeader_Click,
                        LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Attached PanelHeader click handler");
  } else {
    ESP_LOGE(TAG, "ui_PanelHeader is NULL!");
  }

  // Attach TabView change handler for keyboard visibility
  if (ui_TabView1) {
    lv_obj_add_event_cb(ui_TabView1, ui_event_TabView_ValueChanged,
                        LV_EVENT_VALUE_CHANGED, NULL);
    ESP_LOGI(TAG, "Attached TabView change handler");
  }

  // Attach brightness slider handler
  if (ui_SliderSettingsScreenBrightness) {
    lv_obj_add_event_cb(ui_SliderSettingsScreenBrightness,
                        ui_event_BrightnessSlider_ValueChanged,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_slider_set_value(ui_SliderSettingsScreenBrightness,
                        DISPLAY_BACKLIGHT_DEFAULT, LV_ANIM_OFF);
    ESP_LOGI(TAG, "Attached brightness slider handler");
  } else {
    ESP_LOGW(TAG, "ui_SliderSettingsScreenBrightness is NULL");
  }

  // Attach dropdown change handler
  if (ui_DropdownFoundNetworks) {
    lv_obj_add_event_cb(ui_DropdownFoundNetworks,
                        ui_event_DropdownFoundNetworks_ValueChanged,
                        LV_EVENT_VALUE_CHANGED, NULL);
    ESP_LOGI(TAG, "Attached Dropdown ValueChanged handler");
  } else {
    ESP_LOGE(TAG, "ui_DropdownFoundNetworks is NULL!");
  }

  // Attach Stock Panel Long Press handlers (using user_data for slot)
  if (ui_Panel7) {
    lv_obj_add_event_cb(ui_Panel7, ui_event_StockPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, (void *)(intptr_t)1);
    ESP_LOGI(TAG, "Attached Panel7 Stock Finder handler");
  } else {
    ESP_LOGW(TAG, "ui_Panel7 is NULL - Stock Finder trigger not attached");
  }

  if (ui_Panel8) {
    lv_obj_add_event_cb(ui_Panel8, ui_event_StockPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, (void *)(intptr_t)2);
    ESP_LOGI(TAG, "Attached Panel8 Stock Finder handler");
  } else {
    ESP_LOGW(TAG, "ui_Panel8 is NULL - Stock Finder trigger not attached");
  }

  // Attach Stock OK button handler
  if (ui_ButtonStockOK) {
    lv_obj_add_event_cb(ui_ButtonStockOK, ui_event_ButtonStockOK,
                        LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Attached ButtonStockOK handler");
  } else {
    ESP_LOGW(TAG, "ui_ButtonStockOK is NULL");
  }

  // Attach Weather Panel Long Press handlers (Days 1-5)
  if (ui_PanelForecastDay1) {
    lv_obj_add_event_cb(ui_PanelForecastDay1, ui_event_WeatherPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, NULL);
  }
  if (ui_PanelForecastDay2) {
    lv_obj_add_event_cb(ui_PanelForecastDay2, ui_event_WeatherPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, NULL);
  }
  if (ui_PanelForecastDay3) {
    lv_obj_add_event_cb(ui_PanelForecastDay3, ui_event_WeatherPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, NULL);
  }
  if (ui_PanelForecastDay4) {
    lv_obj_add_event_cb(ui_PanelForecastDay4, ui_event_WeatherPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, NULL);
  }
  if (ui_PanelForecastDay5) {
    lv_obj_add_event_cb(ui_PanelForecastDay5, ui_event_WeatherPanel_LongPress,
                        LV_EVENT_LONG_PRESSED, NULL);
  }
  ESP_LOGI(TAG, "Attached Weather Panel Long Press handlers");

  // Attach Weather OK button handler
  if (ui_ButtonWeatherOK) {
    lv_obj_add_event_cb(ui_ButtonWeatherOK, ui_event_ButtonWeatherOK,
                        LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Attached ButtonWeatherOK handler");
  } else {
    ESP_LOGW(TAG, "ui_ButtonWeatherOK is NULL");
  }

  // Attach Extra panel hiding to all Cancel buttons
  if (ui_ButtonNetworkCancel)
    lv_obj_add_event_cb(ui_ButtonNetworkCancel,
                        ui_event_CancelButton_HideExtras, LV_EVENT_CLICKED,
                        NULL);
  if (ui_ButtonStockCancel)
    lv_obj_add_event_cb(ui_ButtonStockCancel, ui_event_CancelButton_HideExtras,
                        LV_EVENT_CLICKED, NULL);
  if (ui_ButtonWeatherCancel)
    lv_obj_add_event_cb(ui_ButtonWeatherCancel,
                        ui_event_CancelButton_HideExtras, LV_EVENT_CLICKED,
                        NULL);
  if (ui_ButtonSettingsCancel)
    lv_obj_add_event_cb(ui_ButtonSettingsCancel,
                        ui_event_CancelButton_HideExtras, LV_EVENT_CLICKED,
                        NULL);
  ESP_LOGI(TAG, "Attached Cancel button Extra-panel-hide handlers");
}
