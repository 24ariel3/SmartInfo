#include "weather.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// UI queues city requests; the network sync task performs geocoding.
static char s_pending_city[64] = "";
static bool s_city_lookup_pending = false;
static int s_city_lookup_result = 0; // 0=none, 1=success, -1=not found, -2=error
static uint32_t s_city_request_id;

// Forward declarations
static esp_err_t geocode_city(const char *city, char *lat_out, char *lon_out);
static esp_err_t weather_save_location_to_nvs(const char *city,
                                              const char *latitude,
                                              const char *longitude);

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
static esp_err_t weather_save_location_to_nvs(const char *city,
                                              const char *latitude,
                                              const char *longitude) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;

  err = nvs_set_str(handle, WEATHER_NVS_KEY_CITY, city);
  if (err == ESP_OK) err = nvs_set_str(handle, WEATHER_NVS_KEY_LAT, latitude);
  if (err == ESP_OK) err = nvs_set_str(handle, WEATHER_NVS_KEY_LON, longitude);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err == ESP_OK)
    ESP_LOGI(TAG, "Saved weather location: %s (%s, %s)", city, latitude,
             longitude);
  return err;
}

esp_err_t weather_init(void) {
  if (s_weather_mutex == NULL) {
    s_weather_mutex = xSemaphoreCreateMutex();
  }
  if (s_weather_mutex == NULL) return ESP_ERR_NO_MEM;
  weather_load_location_from_nvs();
  return ESP_OK;
}

// Helper to perform HTTP request and parse JSON (No cache update)
static esp_err_t execute_weather_fetch(weather_data_t *data,
                                       const char *city,
                                       const char *latitude,
                                       const char *longitude, bool loaded) {
  if (data == NULL)
    return ESP_ERR_INVALID_ARG;

  // Build URL - use saved coordinates or default
  char url[512];
  if (loaded && latitude[0] && longitude[0]) {
    snprintf(url, sizeof(url), WEATHER_LAT_LON_URL, latitude, longitude);
    ESP_LOGI(TAG, "Fetching weather for %s (%s, %s)", city, latitude,
             longitude);
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

esp_err_t weather_refresh(void) {
  if (!s_weather_mutex) return ESP_ERR_INVALID_STATE;

  char pending_city[sizeof(s_pending_city)] = "";
  uint32_t request_id = 0;
  bool lookup_pending = false;
  if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  lookup_pending = s_city_lookup_pending;
  if (lookup_pending) {
    strlcpy(pending_city, s_pending_city, sizeof(pending_city));
    request_id = s_city_request_id;
  }
  xSemaphoreGive(s_weather_mutex);

  if (lookup_pending) {
    ESP_LOGI(TAG, "Processing city lookup: %s", pending_city);
    char lat[16], lon[16];
    esp_err_t lookup_err = geocode_city(pending_city, lat, lon);
    bool save_location = false;
    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
      return ESP_ERR_TIMEOUT;
    if (request_id == s_city_request_id) {
      s_city_lookup_pending = false;
      if (lookup_err == ESP_OK) {
        strlcpy(s_city_name, pending_city, sizeof(s_city_name));
        strlcpy(s_latitude, lat, sizeof(s_latitude));
        strlcpy(s_longitude, lon, sizeof(s_longitude));
        s_location_loaded = true;
        s_city_lookup_result = 1;
        save_location = true;
        ESP_LOGI(TAG, "City lookup success: %s -> (%s, %s)", pending_city,
                 lat, lon);
      } else if (lookup_err == ESP_ERR_NOT_FOUND) {
        s_city_lookup_result = -1;
        ESP_LOGW(TAG, "City not found: %s", pending_city);
      } else {
        s_city_lookup_result = -2;
        ESP_LOGE(TAG, "City lookup failed: %s", pending_city);
      }
    }
    xSemaphoreGive(s_weather_mutex);
    if (save_location &&
        weather_save_location_to_nvs(pending_city, lat, lon) != ESP_OK) {
      ESP_LOGW(TAG, "Could not persist weather location");
    }
  }

  char city[sizeof(s_city_name)];
  char latitude[sizeof(s_latitude)];
  char longitude[sizeof(s_longitude)];
  bool loaded;
  if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  strlcpy(city, s_city_name, sizeof(city));
  strlcpy(latitude, s_latitude, sizeof(latitude));
  strlcpy(longitude, s_longitude, sizeof(longitude));
  loaded = s_location_loaded;
  xSemaphoreGive(s_weather_mutex);

  weather_data_t temp = {0};
  esp_err_t err = execute_weather_fetch(&temp, city, latitude, longitude, loaded);
  if (err != ESP_OK) return err;

  if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  s_cached_weather = temp;
  s_cache_initialized = true;
  s_last_update_ts = esp_timer_get_time();
  xSemaphoreGive(s_weather_mutex);
  ESP_LOGI(TAG, "Weather cache updated");
  return ESP_OK;
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
  if (!s_weather_mutex) return ESP_ERR_INVALID_STATE;

  if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  strlcpy(s_pending_city, city, sizeof(s_pending_city));
  s_city_request_id++;
  s_city_lookup_result = 0; // Reset result
  s_city_lookup_pending = true;
  xSemaphoreGive(s_weather_mutex);

  ESP_LOGI(TAG, "City lookup queued: %s", city);
  return ESP_OK;
}

// Get the result of the last city lookup
// Returns: 0=pending, 1=success, -1=not found, -2=error
int weather_get_city_lookup_result(void) {
  if (!s_weather_mutex ||
      xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return -2;
  int result = s_city_lookup_result;
  xSemaphoreGive(s_weather_mutex);
  return result;
}

// Check if a city lookup is pending
bool weather_is_city_lookup_pending(void) {
  if (!s_weather_mutex ||
      xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;
  bool pending = s_city_lookup_pending;
  xSemaphoreGive(s_weather_mutex);
  return pending;
}

esp_err_t weather_get_city(char *city_out, size_t len) {
  if (!city_out || len == 0)
    return ESP_ERR_INVALID_ARG;

  if (!s_weather_mutex ||
      xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  if (!s_location_loaded || strlen(s_city_name) == 0) {
    xSemaphoreGive(s_weather_mutex);
    return ESP_ERR_NOT_FOUND;
  }
  strncpy(city_out, s_city_name, len - 1);
  city_out[len - 1] = '\0';
  xSemaphoreGive(s_weather_mutex);
  return ESP_OK;
}

esp_err_t weather_get_location(char *lat_out, size_t lat_len, char *lon_out,
                               size_t lon_len) {
  if (!lat_out || lat_len == 0 || !lon_out || lon_len == 0)
    return ESP_ERR_INVALID_ARG;

  if (!s_weather_mutex ||
      xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  if (!s_location_loaded || strlen(s_latitude) == 0 ||
      strlen(s_longitude) == 0) {
    xSemaphoreGive(s_weather_mutex);
    return ESP_ERR_NOT_FOUND;
  }

  strncpy(lat_out, s_latitude, lat_len - 1);
  lat_out[lat_len - 1] = '\0';
  strncpy(lon_out, s_longitude, lon_len - 1);
  lon_out[lon_len - 1] = '\0';
  xSemaphoreGive(s_weather_mutex);
  return ESP_OK;
}
