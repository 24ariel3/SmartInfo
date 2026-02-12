#include "weather.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_connect.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "WEATHER";

// Default location, Pardes Hanna
#define WEATHER_URL                                                            \
  "https://api.open-meteo.com/v1/"                                             \
  "forecast?latitude=32.4741&longitude=34.9778&daily=uv_index_max,weather_"    \
  "code,sunrise,sunset,temperature_2m_max,temperature_2m_min,precipitation_"   \
  "sum,wind_speed_10m_max,wind_direction_10m_dominant,relative_humidity_2m_"   \
  "mean,apparent_temperature_mean&current=temperature_2m,relative_humidity_"   \
  "2m,weather_code,"                                                           \
  "precipitation,wind_speed_10m,wind_direction_10m&timezone=auto"

#define WEATHER_LAT_LON_URL                                                    \
  "https://api.open-meteo.com/v1/"                                             \
  "forecast?latitude=%s&longitude=%s&daily=uv_index_max,weather_"              \
  "code,sunrise,sunset,temperature_2m_max,temperature_2m_min,precipitation_"   \
  "sum,wind_speed_10m_max,wind_direction_10m_dominant,relative_humidity_2m_"   \
  "mean,apparent_temperature_mean&current=temperature_2m,relative_humidity_"   \
  "2m,weather_code,"                                                           \
  "precipitation,wind_speed_10m,wind_direction_10m&timezone=auto"

#define WEATHER_NAME_TO_LAT_LON_URL                                            \
  "https://nominatim.openstreetmap.org/search?city=%s&format=json"

// Static cache and synchronization
static weather_data_t s_cached_weather = {0};
static bool s_cache_initialized = false;
static SemaphoreHandle_t s_weather_mutex = NULL;

// NVS storage for city location
#define WEATHER_NVS_NAMESPACE "weather"
#define WEATHER_NVS_KEY_CITY "city"
#define WEATHER_NVS_KEY_LAT "lat"
#define WEATHER_NVS_KEY_LON "lon"

static char s_city_name[64] = "";
static char s_latitude[16] = "";
static char s_longitude[16] = "";
static bool s_location_loaded = false;
static volatile bool s_refresh_requested = false;

// Async city request - geocoding will run in weather task
static char s_pending_city[64] = "";
static volatile bool s_city_lookup_pending = false;
static volatile int s_city_lookup_result =
    0; // 0=none, 1=success, -1=not found, -2=error

// Forward declarations
static esp_err_t geocode_city(const char *city, char *lat_out, char *lon_out);
static esp_err_t weather_save_location_to_nvs(void);

// Load location from NVS
static void weather_load_location_from_nvs(void) {
  nvs_handle_t handle;
  if (nvs_open(WEATHER_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    size_t len = sizeof(s_city_name);
    if (nvs_get_str(handle, WEATHER_NVS_KEY_CITY, s_city_name, &len) ==
        ESP_OK) {
      len = sizeof(s_latitude);
      nvs_get_str(handle, WEATHER_NVS_KEY_LAT, s_latitude, &len);
      len = sizeof(s_longitude);
      nvs_get_str(handle, WEATHER_NVS_KEY_LON, s_longitude, &len);
      s_location_loaded = true;
      ESP_LOGI(TAG, "Loaded weather location: %s (%s, %s)", s_city_name,
               s_latitude, s_longitude);
    }
    nvs_close(handle);
  }
}

// Save location to NVS
static esp_err_t weather_save_location_to_nvs(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;

  nvs_set_str(handle, WEATHER_NVS_KEY_CITY, s_city_name);
  nvs_set_str(handle, WEATHER_NVS_KEY_LAT, s_latitude);
  nvs_set_str(handle, WEATHER_NVS_KEY_LON, s_longitude);
  err = nvs_commit(handle);
  nvs_close(handle);
  ESP_LOGI(TAG, "Saved weather location: %s (%s, %s)", s_city_name, s_latitude,
           s_longitude);
  return err;
}

esp_err_t weather_init(void) {
  if (s_weather_mutex == NULL) {
    s_weather_mutex = xSemaphoreCreateMutex();
  }
  weather_load_location_from_nvs();
  return ESP_OK;
}

// Helper to perform HTTP request and parse JSON (No cache update)
static esp_err_t execute_weather_fetch(weather_data_t *data) {
  if (data == NULL)
    return ESP_ERR_INVALID_ARG;

  // Build URL - use saved coordinates or default
  char url[512];
  if (s_location_loaded && strlen(s_latitude) > 0 && strlen(s_longitude) > 0) {
    snprintf(url, sizeof(url), WEATHER_LAT_LON_URL, s_latitude, s_longitude);
    ESP_LOGI(TAG, "Fetching weather for %s (%s, %s)", s_city_name, s_latitude,
             s_longitude);
  } else {
    strncpy(url, WEATHER_URL, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    ESP_LOGI(TAG, "Fetching weather for default location");
  }

  char *response_buffer = NULL;
  esp_err_t err = wifi_perform_http_get(url, &response_buffer);

  if (err == ESP_OK && response_buffer != NULL) {
    cJSON *root = cJSON_Parse(response_buffer);
    if (root) {
      // Parse "current" object
      cJSON *current = cJSON_GetObjectItem(root, "current");
      if (current) {
        data->current_temp =
            (float)cJSON_GetObjectItem(current, "temperature_2m")->valuedouble;
        data->current_humidity =
            cJSON_GetObjectItem(current, "relative_humidity_2m")->valueint;
        data->current_weather_code =
            cJSON_GetObjectItem(current, "weather_code")->valueint;
        data->current_wind_speed =
            (float)cJSON_GetObjectItem(current, "wind_speed_10m")->valuedouble;
        data->current_wind_direction =
            cJSON_GetObjectItem(current, "wind_direction_10m")->valueint;
        data->current_precipitation =
            (float)cJSON_GetObjectItem(current, "precipitation")->valuedouble;
      }

      // Parse "daily" object
      cJSON *daily = cJSON_GetObjectItem(root, "daily");
      if (daily) {
        cJSON *dates = cJSON_GetObjectItem(daily, "time");
        cJSON *temp_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *temp_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
        cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *precip_sum = cJSON_GetObjectItem(daily, "precipitation_sum");
        cJSON *uv_max = cJSON_GetObjectItem(daily, "uv_index_max");
        cJSON *sunrise = cJSON_GetObjectItem(daily, "sunrise");
        cJSON *sunset = cJSON_GetObjectItem(daily, "sunset");
        cJSON *wind_spd = cJSON_GetObjectItem(daily, "wind_speed_10m_max");
        cJSON *wind_dir =
            cJSON_GetObjectItem(daily, "wind_direction_10m_dominant");
        cJSON *hum_mean =
            cJSON_GetObjectItem(daily, "relative_humidity_2m_mean");
        cJSON *app_temp_mean =
            cJSON_GetObjectItem(daily, "apparent_temperature_mean");

        int count = cJSON_GetArraySize(dates);
        if (count > 7)
          count = 7;

        for (int i = 0; i < count; i++) {
          cJSON *item;
          item = cJSON_GetArrayItem(dates, i);
          if (item && item->valuestring)
            strncpy(data->daily[i].date, item->valuestring, 31);

          item = cJSON_GetArrayItem(temp_max, i);
          if (item)
            data->daily[i].temp_max = item->valuedouble;
          item = cJSON_GetArrayItem(temp_min, i);
          if (item)
            data->daily[i].temp_min = item->valuedouble;
          item = cJSON_GetArrayItem(codes, i);
          if (item)
            data->daily[i].weather_code = item->valueint;
          item = cJSON_GetArrayItem(precip_sum, i);
          if (item)
            data->daily[i].precipitation_sum = item->valuedouble;
          item = cJSON_GetArrayItem(uv_max, i);
          if (item)
            data->daily[i].uv_index_max = item->valuedouble;

          item = cJSON_GetArrayItem(sunrise, i);
          if (item && item->valuestring)
            strncpy(data->daily[i].sunrise, item->valuestring, 31);

          item = cJSON_GetArrayItem(sunset, i);
          if (item && item->valuestring)
            strncpy(data->daily[i].sunset, item->valuestring, 31);

          item = cJSON_GetArrayItem(wind_spd, i);
          if (item)
            data->daily[i].wind_speed_max = item->valuedouble;
          item = cJSON_GetArrayItem(wind_dir, i);
          if (item)
            data->daily[i].wind_direction_dominant = item->valueint;
          item = cJSON_GetArrayItem(hum_mean, i);
          if (item)
            data->daily[i].humidity_mean = item->valueint;
          item = cJSON_GetArrayItem(app_temp_mean, i);
          if (item)
            data->daily[i].apparent_temperature_mean = item->valuedouble;
        }
      }
      cJSON_Delete(root);
    } else {
      ESP_LOGE(TAG, "Failed to parse JSON");
      err = ESP_FAIL;
    }
    free(response_buffer);
  } else {
    // If we failed but err claims OK, ensure it's fail
    if (err == ESP_OK)
      err = ESP_FAIL;
  }

  return err;
}

// Static buffer for change detection
static weather_data_t s_last_read_weather = {0};
static int64_t s_last_update_ts = 0;

int64_t weather_get_last_updated_time(void) { return s_last_update_ts; }

// Thread-safe getter
esp_err_t weather_get_latest_values(weather_data_t *out_data,
                                    bool *is_updated) {
  if (s_weather_mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  if (is_updated)
    *is_updated = false;

  if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (out_data)
      *out_data = s_cached_weather;

    if (s_cache_initialized) {
      if (is_updated) {
        // Check if data changed since last read
        if (memcmp(&s_last_read_weather, &s_cached_weather,
                   sizeof(weather_data_t)) != 0) {
          *is_updated = true;
          s_last_read_weather = s_cached_weather;
        }
      }
    }

    xSemaphoreGive(s_weather_mutex);
    return s_cache_initialized ? ESP_OK : ESP_ERR_NOT_FOUND;
  }
  return ESP_ERR_TIMEOUT;
}

// Background task
static void weather_task(void *arg) {
  ESP_LOGI(TAG, "Weather Task Started");

  // Wait for WiFi
  while (!wifi_is_connected()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  while (1) {
    // Check for pending city lookup (async geocoding)
    if (s_city_lookup_pending) {
      ESP_LOGI(TAG, "Processing pending city lookup: %s", s_pending_city);
      char lat[16], lon[16];
      esp_err_t err = geocode_city(s_pending_city, lat, lon);

      if (err == ESP_OK) {
        // Save to static vars
        strncpy(s_city_name, s_pending_city, sizeof(s_city_name) - 1);
        s_city_name[sizeof(s_city_name) - 1] = '\0';
        strncpy(s_latitude, lat, sizeof(s_latitude) - 1);
        s_latitude[sizeof(s_latitude) - 1] = '\0';
        strncpy(s_longitude, lon, sizeof(s_longitude) - 1);
        s_longitude[sizeof(s_longitude) - 1] = '\0';
        s_location_loaded = true;

        weather_save_location_to_nvs();
        s_city_lookup_result = 1; // Success
        s_refresh_requested = true;
        ESP_LOGI(TAG, "City lookup success: %s -> (%s, %s)", s_city_name, lat,
                 lon);
      } else if (err == ESP_ERR_NOT_FOUND) {
        s_city_lookup_result = -1; // Not found
        ESP_LOGW(TAG, "City not found: %s", s_pending_city);
      } else {
        s_city_lookup_result = -2; // Error
        ESP_LOGE(TAG, "City lookup error: %s", s_pending_city);
      }
      s_city_lookup_pending = false;
    }

    // Clear refresh flag
    s_refresh_requested = false;

    ESP_LOGI(TAG, "Fetching weather data (background)...");
    weather_data_t temp = {0};

    if (execute_weather_fetch(&temp) == ESP_OK) {
      if (s_weather_mutex &&
          xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_cached_weather = temp;
        s_cache_initialized = true;
        s_last_update_ts = esp_timer_get_time();
        xSemaphoreGive(s_weather_mutex);
        ESP_LOGI(TAG, "Weather cache updated");
      }
    }
    ESP_LOGI(TAG, "Weather Stack Remaining: %lu bytes",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));

    // Calculate time until next whole hour
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Seconds remaining until next hour
    int seconds_to_next_hour = (60 - timeinfo.tm_min) * 60 - timeinfo.tm_sec;
    if (seconds_to_next_hour <= 0) {
      seconds_to_next_hour = 3600; // Fallback to 1 hour if at exact hour
    }

    ESP_LOGI(TAG, "Next weather fetch in %d seconds (at next whole hour)",
             seconds_to_next_hour);

    // Wait with periodic checks for refresh or city lookup request
    int wait_time_ms = seconds_to_next_hour * 1000;
    while (wait_time_ms > 0 && !s_refresh_requested && !s_city_lookup_pending) {
      int delay = (wait_time_ms > 1000) ? 1000 : wait_time_ms;
      vTaskDelay(pdMS_TO_TICKS(delay));
      wait_time_ms -= delay;
    }

    if (s_refresh_requested) {
      ESP_LOGI(TAG, "Weather refresh requested - fetching immediately");
    }
  }
}

void weather_start_task(void) {
  if (s_weather_mutex == NULL)
    weather_init();
  xTaskCreate(weather_task, "weather_task", 6144, NULL, 5, NULL);
}

// Geocoding: look up city coordinates from Nominatim
static esp_err_t geocode_city(const char *city, char *lat_out, char *lon_out) {
  // URL-encode city name (replace spaces with '+')
  char encoded_city[128];
  size_t j = 0;
  for (size_t i = 0; city[i] && j < sizeof(encoded_city) - 1; i++) {
    if (city[i] == ' ') {
      encoded_city[j++] = '+';
    } else {
      encoded_city[j++] = city[i];
    }
  }
  encoded_city[j] = '\0';

  char url[256];
  snprintf(url, sizeof(url), WEATHER_NAME_TO_LAT_LON_URL, encoded_city);

  char *response_buffer = NULL;
  esp_err_t err = wifi_perform_http_get(url, &response_buffer);

  if (err != ESP_OK || response_buffer == NULL) {
    ESP_LOGE(TAG, "Geocoding HTTP request failed");
    return ESP_FAIL;
  }

  // Parse JSON array - use first result
  cJSON *root = cJSON_Parse(response_buffer);
  free(response_buffer);

  if (!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
    ESP_LOGE(TAG, "Geocoding: No results for '%s'", city);
    if (root)
      cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
  }

  cJSON *first = cJSON_GetArrayItem(root, 0);
  cJSON *lat_json = cJSON_GetObjectItem(first, "lat");
  cJSON *lon_json = cJSON_GetObjectItem(first, "lon");

  if (!lat_json || !lon_json || !lat_json->valuestring ||
      !lon_json->valuestring) {
    ESP_LOGE(TAG, "Geocoding: Missing lat/lon in response");
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  strncpy(lat_out, lat_json->valuestring, 15);
  lat_out[15] = '\0';
  strncpy(lon_out, lon_json->valuestring, 15);
  lon_out[15] = '\0';

  ESP_LOGI(TAG, "Geocoded '%s' to (%s, %s)", city, lat_out, lon_out);
  cJSON_Delete(root);
  return ESP_OK;
}

// Queue an async city lookup (non-blocking - actual geocoding done in weather
// task)
esp_err_t weather_set_city(const char *city) {
  if (!city || strlen(city) == 0)
    return ESP_ERR_INVALID_ARG;

  // Copy city to pending buffer and set flag
  strncpy(s_pending_city, city, sizeof(s_pending_city) - 1);
  s_pending_city[sizeof(s_pending_city) - 1] = '\0';
  s_city_lookup_result = 0; // Reset result
  s_city_lookup_pending = true;

  ESP_LOGI(TAG, "City lookup queued: %s", city);
  return ESP_OK;
}

// Get the result of the last city lookup
// Returns: 0=pending, 1=success, -1=not found, -2=error
int weather_get_city_lookup_result(void) { return s_city_lookup_result; }

// Check if a city lookup is pending
bool weather_is_city_lookup_pending(void) { return s_city_lookup_pending; }

esp_err_t weather_get_city(char *city_out, size_t len) {
  if (!city_out || len == 0)
    return ESP_ERR_INVALID_ARG;

  if (!s_location_loaded || strlen(s_city_name) == 0) {
    return ESP_ERR_NOT_FOUND;
  }

  strncpy(city_out, s_city_name, len - 1);
  city_out[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t weather_get_location(char *lat_out, size_t lat_len, char *lon_out,
                               size_t lon_len) {
  if (!lat_out || lat_len == 0 || !lon_out || lon_len == 0)
    return ESP_ERR_INVALID_ARG;

  if (!s_location_loaded || strlen(s_latitude) == 0 ||
      strlen(s_longitude) == 0) {
    return ESP_ERR_NOT_FOUND;
  }

  strncpy(lat_out, s_latitude, lat_len - 1);
  lat_out[lat_len - 1] = '\0';
  strncpy(lon_out, s_longitude, lon_len - 1);
  lon_out[lon_len - 1] = '\0';
  return ESP_OK;
}

void weather_trigger_refresh(void) {
  s_refresh_requested = true;
  ESP_LOGI(TAG, "Weather refresh requested");
}
