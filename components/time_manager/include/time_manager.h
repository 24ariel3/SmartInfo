#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initialize the NTP time manager.
 *        Starts SNTP synchronization.
 */
void time_manager_init(void);

/**
 * @brief Get the current time in formatted string.
 * 
 * @param buf [out] Buffer to store the string.
 * @param buf_len [in] Size of the buffer.
 * @param is_24h [in] True for 24H format (HH:MM:SS), False for 12H (HH:MM:SS AM/PM).
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
