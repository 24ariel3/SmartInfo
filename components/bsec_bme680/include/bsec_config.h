#ifndef BSEC_CONFIG_H
#define BSEC_CONFIG_H

#include <stdint.h>

/*
 * configuration for BSEC
 *
 * This array must be populated with the content of the config file from the
 * BSEC release. Path in BSEC release: config/generic_33v_3s_4d/bsec_iaq.c (or
 * .h)
 */
extern const uint8_t bsec_config_iaq[492];
extern const int bsec_config_iaq_len;

#endif
