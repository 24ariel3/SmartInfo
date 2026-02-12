# BSEC BME680 Component

This component integrates the Bosch BSEC library for the BME680 sensor.
Because the BSEC library is proprietary, you must download it from Bosch Sensortec and place the files here manually.

## Required Files

1.  **Download BSEC Software**:
    Go to [Bosch Sensortec BSEC Downloads](https://www.bosch-sensortec.com/software-tools/software/bsec/) and download the package.

2.  **Copy Headers**:
    Copy the following header files from the BSEC package (usually in `algo/normal_version/inc` or similar) to `components/bsec_bme680/include/`:
    *   `bsec_interface.h`
    *   `bsec_datatypes.h`

    Also copy the BME68x sensor API headers if needed (or ensure they are available):
    *   `bme68x.h` (if using BME68x driver)
    *   `bme68x_defs.h`

3.  **Copy Library**:
    Copy the precompiled library for ESP32 (check `algo/normal_version/bin/esp32` or similar) to `components/bsec_bme680/lib/`:
    *   `libalgobsec.a`

## Structure

*   `include/`: Header files (BSEC API + `bsec_integration.h`).
*   `lib/`: Static library `libalgobsec.a`.
*   `src/`: Integration source code.
