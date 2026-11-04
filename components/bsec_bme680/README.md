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

## Enclosure Heat Compensation

`BSEC_HEAT_SOURCE_OFFSET_C` in `src/bsec_integration.c` supplies an additional
`+3.5` degrees Celsius to `BSEC_INPUT_HEATSOURCE` when BSEC requests that input.
BSEC subtracts this value when calculating compensated temperature and uses the
corrected temperature to calculate compensated relative humidity. Raw temperature
and humidity inputs remain unchanged; no separate UI offset is applied.

The positive offset represents excess enclosure heat, in addition to the
sensor-heater correction already handled by the loaded BSEC profile. It is a
fixed calibration value: verify it against an ambient reference after the device
has warmed up in its normal enclosure and power mode. Adjust the constant and
rebuild if the thermal setup changes. Saved BSEC state and LP sampling are retained.
