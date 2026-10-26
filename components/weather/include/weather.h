#ifndef WEATHER_H
#define WEATHER_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char date[32];                   // "2026-01-26"
  float temp_max;                  // temperature_2m_max
  float temp_min;                  // temperature_2m_min
  int weather_code;                // weather_code
  float precipitation_sum;         // precipitation_sum
  float uv_index_max;              // uv_index_max
  char sunrise[32];                // sunrise "2026-01-26T06:30"
  char sunset[32];                 // sunset
  float wind_speed_max;            // wind_speed_10m_max
  int wind_direction_dominant;     // wind_direction_10m_dominant
  int humidity_mean;               // relative_humidity_2m_mean
  float apparent_temperature_mean; // apparent_temperature_mean
} daily_forecast_t;

typedef struct {
  // Current Weather ("current" object)
  float current_temp;          // temperature_2m
  int current_humidity;        // relative_humidity_2m
  int current_weather_code;    // weather_code
  float current_wind_speed;    // wind_speed_10m
  int current_wind_direction;  // wind_direction_10m
  float current_precipitation; // precipitation

  // Daily Forecast ("daily" object arrays)
  // JSON returns 7 days, we typically use 5 for display
  daily_forecast_t daily[7];
} weather_data_t;

/**
 * @brief Initialize weather component (e.g., HTTP client config if needed)
 *
 * @return esp_err_t
 */
esp_err_t weather_init(void);

/** Fetch weather and process one queued city lookup. Call from network sync. */
esp_err_t weather_refresh(void);

/**
 * @brief Get the latest weather data from the internal cache (thread-safe,
 * non-blocking)
 *
 * @param[out] out_data Pointer to weather_data_t struct to populate
 * @param[out] is_updated Pointer to bool, set to true if data changed from last
 * call
 * @return esp_err_t ESP_OK if data retrieved (even if not updated),
 * ESP_ERR_TIMEOUT if mutex busy
 */
esp_err_t weather_get_latest_values(weather_data_t *out_data, bool *is_updated);

/**
 * @brief Get the last update timestamp (us) to check for changes without
 * locking
 * @return int64_t Timestamp in microseconds
 */
int64_t weather_get_last_updated_time(void);

/**
 * @brief Set weather location by city name (triggers geocoding + refresh)
 * @param city City name to look up
 * @return ESP_OK on success, ESP_FAIL if geocoding fails
 */
esp_err_t weather_set_city(const char *city);

/**
 * @brief Get the current city name (from NVS)
 * @param[out] city_out Buffer to store city name (min 64 bytes)
 * @param len Buffer length
 * @return ESP_OK if city exists, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t weather_get_city(char *city_out, size_t len);

/**
 * @brief Get the current location coordinates (from NVS)
 * @param[out] lat_out Buffer to store latitude (min 16 bytes)
 * @param[out] lon_out Buffer to store longitude (min 16 bytes)
 * @param lat_len Lat buffer length
 * @param lon_len Lon buffer length
 * @return ESP_OK if location exists, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t weather_get_location(char *lat_out, size_t lat_len, char *lon_out,
                               size_t lon_len);

/**
 * @brief Get result of async city lookup
 * @return 0=pending/none, 1=success, -1=not found, -2=error
 */
int weather_get_city_lookup_result(void);

/**
 * @brief Check if a city lookup is currently pending
 * @return true if lookup is in progress
 */
bool weather_is_city_lookup_pending(void);

#ifdef __cplusplus
}
#endif

#endif // WEATHER_H
