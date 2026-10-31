#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Persistence ---
/**
 * @brief Save WiFi credentials to NVS
 * @param ssid SSID to save
 * @param password Password to save
 * @return ESP_OK on success
 */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials from NVS
 * @param ssid SSID to look up
 * @param out_password Buffer to store password
 * @param len Size of buffer
 * @return ESP_OK if found, ESP_ERR_NVS_NOT_FOUND if not
 */
esp_err_t wifi_load_credentials(const char *ssid, char *out_password,
                                size_t len);

/**
 * @brief Check if credentials exist for an SSID
 * @param ssid SSID to check
 * @return true if found
 */
bool wifi_has_saved_credentials(const char *ssid);

/**
 * @brief Initialize Wi-Fi connect
 *
 * This function initiates the Wi-Fi connection using the defined credentials.
 * It blocks until connection is established or fails.
 */
void wifi_connect_init(void);

typedef enum {
  WIFI_POWER_STARTUP,
  WIFI_POWER_SYNC,
  WIFI_POWER_NETWORK_UI,
  WIFI_POWER_SCAN,
  WIFI_POWER_OTA,
  WIFI_POWER_OWNER_COUNT
} wifi_power_owner_t;

/** Keep the radio enabled while a subsystem needs it. Each acquire must pair with a release. */
esp_err_t wifi_power_acquire(wifi_power_owner_t owner);
void wifi_power_release(wifi_power_owner_t owner);
bool wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief Check if Wi-Fi is connected
 *
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

/**
 * @brief Perform a GET request and return a dynamically allocated buffer with
 * the response
 * @note  Caller is responsible for freeing the buffer
 * @return ESP_OK on success
 */
esp_err_t wifi_perform_http_get(const char *url, char **out_buffer);

/** Sequential GETs sharing a client; returns the first error, or ESP_OK.
 * Each result is independent. Caller frees every non-NULL response buffer.
 * No connection survives the batch, including when a request fails.
 */
esp_err_t wifi_perform_http_get_batch(const char *const *urls, char **out_buffers,
                                     esp_err_t *out_results, size_t count);

/**
 * @brief Scan for available Wi-Fi networks
 *
 * Performs a blocking scan and returns a newline-separated string of SSIDs
 * suitable for LVGL dropdowns.
 *
 * @note Caller is responsible for freeing the returned string.
 * @return char* Allocated string containing SSIDs, or NULL on failure
 */
char *wifi_scan_networks(void);

/**
 * @brief WiFi Scan State
 */
typedef enum {
  WIFI_SCAN_IDLE,
  WIFI_SCANNING,
  WIFI_SCAN_COMPLETED,
  WIFI_SCAN_FAILED
} wifi_scan_state_t;

/**
 * @brief Start a non-blocking WiFi scan
 *
 * Launches a background task to perform the scan.
 * Use wifi_get_scan_state() to check progress.
 *
 * @return esp_err_t ESP_OK if started, error otherwise
 */
esp_err_t wifi_start_scan(void);

/**
 * @brief Get the current scan state
 * @return wifi_scan_state_t
 */
wifi_scan_state_t wifi_get_scan_state(void);

/**
 * @brief Get the scan results
 *
 * Returns the results if the scan is COMPLETED.
 * The caller is responsible for freeing the string if it is not NULL.
 * Calling this function will reset the state to IDLE.
 *
 * @param out_networks Pointer to char* where result string will be stored
 * @return esp_err_t ESP_OK if results retrieved
 */
esp_err_t wifi_get_scan_results(char **out_networks);

// --- Connection Logic ---

/**
 * @brief WiFi Connection State
 */
typedef enum {
  WIFI_CONNECTION_IDLE,
  WIFI_CONNECTION_CONNECTING,
  WIFI_CONNECTION_CONNECTED,
  WIFI_CONNECTION_FAILED
} wifi_connection_state_t;

/**
 * @brief Connect to a specific AP
 *
 * Disconnects from any current network and attempts to connect to the new one.
 * Non-blocking. Use wifi_get_connection_state() to check progress.
 *
 * @param ssid SSID to connect to
 * @param password Password for the network
 * @return esp_err_t ESP_OK if connection attempt started
 */
esp_err_t wifi_connect_to_ap(const char *ssid, const char *password);

/**
 * @brief Get the current connection state
 * @return wifi_connection_state_t
 */
wifi_connection_state_t wifi_get_connection_state(void);

#ifdef __cplusplus
}
#endif
