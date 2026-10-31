#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/**
 * @brief Initialize timezone and NTP synchronization state.
 */
void time_manager_init(void);

/** Synchronize time with NTP, returning after success or timeout. */
esp_err_t time_manager_sync(uint32_t timeout_ms);
bool time_manager_has_valid_time(void);

/** Convert UTC to New York time without changing the Israel display timezone. */
bool time_manager_get_new_york_time(time_t utc_epoch, struct tm *out);

/**
 * @brief Get the current time in formatted string.
 * 
 * @param buf [out] Buffer to store the string.
 * @param buf_len [in] Size of the buffer.
 * @param is_24h [in] True for 24H format (HH:MM), False for 12H (HH:MM AM/PM).
 */
void time_manager_get_time(char *buf, size_t buf_len, bool is_24h);

/**
 * @brief Get the current date in DD/MM/YYYY format.
 * 
 * @param buf [out] Buffer to store the string.
 * @param buf_len [in] Size of the buffer.
 */
void time_manager_get_date(char *buf, size_t buf_len);

#endif // TIME_MANAGER_H
