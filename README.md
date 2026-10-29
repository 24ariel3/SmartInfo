# SmartInfo - IoT Smart Dashboard

## Overview
**SmartInfo** is an advanced IoT dashboard application for the ESP32 (specifically designed for the **ESP32 DevKit V1** or compatible). It integrates Environmental Sensing, Real-time Stock Monitoring, Weather Forecasting, and Smart Home controls into a polished, responsive touch interface.

The project demonstrates high-performance embedded GUI development using **LVGL 9.1.0**, **ESP-IDF v5.4.0**, and the **SquareLine Studio** UI designer.

## ✨ Key Features

### 1. 🖥️ Interactive Touch UI
- **Smooth Dashboard:** Built with LVGL.
- **Tab-Based Navigation:** seamless switching between Network, Stocks, Weather, and Settings tabs.
- **Brightness Control:** Hardware-accelerated backlight control via PWM.

### 2. 🌍 Environmental Sensing (BSEC)
- **Sensor:** Bosch **BME680** Integration via BSEC library.
- **Real-time Data:**
  - Index for Air Quality (IAQ) with accuracy indicators.
  - CO2 Equivalent (ppm).
  - Breath VOC Equivalent (ppm).
  - Temperature, Humidity, and Pressure.
  - Visual color-coded gauges for air quality levels.

### 3. 📈 Stock Market Monitor
- **Dual Stock Tracking:** Monitor two stocks simultaneously.
- **Live Data:** Fetches realtime price, percent change, day high/low, and previous close.
- **Visual Cues:** Color-coded (Green/Red) indicators for positive/negative trends.
- **Customizable:** User can input any ticker symbol via the on-screen keyboard.
- **Thread-safe:** Dedicated background task for API fetching prevents UI freezes.

### 4. 🌦️ Weather Station
- **5-Day Forecast:** Detailed daily forecast with high/low temps, precipitation, UV index, and wind.
- **Rich Visualization:** Dynamic weather icons and detailed environmental metrics.
- **Location Aware:** User-configurable city search.
- **Async Updates:** Non-blocking background updates ensure smooth UI performance.

### 5. 📡 Connectivity & System
- **Wi-Fi Manager:** Integrated Wi-Fi scanning and connection manager with on-screen password entry.
- **NTP Time Sync:** Automatic time and date synchronization with timezone support.
- **Task Architecture:** FreeRTOS task separation for UI, Sensors, Network, and Logic to ensure stability.

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP32 DevKit V1.
- **Display:** 240x320 ILI9341 SPI display with XPT2046 Touch controller.
- **Sensor:** Bosch BME680 connected via I2C.
- **Peripherals:** Wi-Fi (Internal).

## 📂 Software Architecture
The project follows a modular component-based architecture:

- **`main.c`**: Application entry point, hardware initialization, and task creation.
- **`ui_updater.c`**: The "Glue" Logic. Bridges the low-level hardware tasks with the High-Level UI. It listens for data changes and updates LVGL widgets safely within the UI mutex.
- **`ui/`**: SquareLine Studio generated UI code (Screens, Widgets, Styles).
- **`components/`**:
  - `bsec_bme680`: Handles BME680 sensor reading and BSEC algorithm processing.
  - `stock_monitor`: HTTPS client for fetching financial data.
  - `weather`: API client for weather data.
  - `wifi_connect`: Robust Wi-Fi state machine.
  - `device_server`: Authenticated local HTTP server for OTA uploads and device status.
  - `time_manager`: SNTP synchronization.

## ⚙️ Configuration
The project uses **Kconfig** for sensitive or adjustable settings.

1. Run the configuration menu:
   ```bash
   idf.py menuconfig
   ```
2. Navigate to **Component config -> Stock Monitor Configuration**.
3. Enter your **Finnhub API Key**.
4. Save and Exit.

## 🎨 Customizing the UI
The UI is designed using **SquareLine Studio**. The project file is located at:
`SquareLine_GUI/SmartInfo_5/smartInfo5.spj`

To modify the UI:
1. Open the `.spj` file in SquareLine Studio.
2. Make your changes (add screens, widgets, animations).
3. Export the project to the `ui/` folder (ensure export path is set correctly in Project Settings).

## 🚀 Building & Flashing

### 1. Prerequisities
- **ESP-IDF v5.4.0** installed and configured.
- Python 3.x

### 2. Build
Use the provided helper script:
```
idf.py build
```

### 3. Flash
Use the provided helper script:
```
idf.py -p COMx flash monitor
```

> **Note:** `partitions.csv` provides two 1,984 KiB OTA application slots in 4 MB flash. NVS and PHY data keep their original addresses.

### OTA updates on local Wi-Fi

The `device_server` component provides authenticated `POST /ota` and `GET /status`
on port 80. A reusable worker and its 8 KiB stack/4 KiB transfer buffer are reserved
at startup, so runtime heap fragmentation cannot prevent those allocations.
Status requests remain available during an update. Wi-Fi logs and diagnostic
commands are not implemented yet.

For the first installation:

1. Run `idf.py menuconfig`, open **Device Server**, and configure a random
   64-character hexadecimal bearer token. Generate one with
   `python -c "import secrets; print(secrets.token_hex(32))"`. Keep it in local
   `sdkconfig`; do not commit it. An empty or invalid token disables the server.
2. Run `idf.py build` and `idf.py -p COM4 flash`. This installs the bootloader,
   partition table, OTA metadata, and application. Do not use `erase-flash`;
   existing NVS settings and sensor state are preserved.
3. Connect using the Wi-Fi screen once. The device remembers the last successful
   network and attempts a non-blocking reconnection on subsequent boots.
4. Find the device IP in the `got ip:` serial message or your router's DHCP list.
   A DHCP reservation keeps the address stable.

For subsequent updates, build the firmware and use the Windows batch helper:

```powershell
idf.py build
.\ota_update.bat 192.168.0.53
```

Replace the IP with your device's address. The helper defaults to
`build\SmartInfo7.bin`; an optional second argument selects another application:
`ota_update.bat 192.168.0.53 "C:\firmware\SmartInfo7.bin"`.
It uses `SMARTINFO_OTA_TOKEN` when set, otherwise reads the token from local
`sdkconfig`, without printing it. It checks readiness, uploads, and verifies that
the exact build is confirmed in the other slot after reboot. A rollback or failed
verification returns a nonzero exit code. No ESP-IDF shell is needed to upload an
already-built image; Windows PowerShell and `curl.exe` are required.

Alternatively, run these PowerShell commands from the repository root
in an initialized ESP-IDF shell, replacing `<device-ip>`:

```powershell
idf.py build
$env:SMARTINFO_OTA_TOKEN = (Select-String '^CONFIG_DEVICE_SERVER_AUTH_TOKEN="([^"]*)"$' sdkconfig).Matches[0].Groups[1].Value
curl.exe --noproxy '*' --fail-with-body -H "Authorization: Bearer $env:SMARTINFO_OTA_TOKEN" -H "Content-Type: application/octet-stream" --data-binary "@build/SmartInfo7.bin" "http://<device-ip>/ota"
```

Send only the application `.bin`, as raw bytes, not a multipart upload, bootloader,
or merged flash image. Keep the token the same across builds. HTTP is unencrypted:
use a trusted LAN and do not forward this service to the internet. Firmware images
also contain the token and should be kept private.

After acceptance the device restarts, reconnects, and checks startup health:

```powershell
curl.exe --noproxy '*' --fail-with-body -H "Authorization: Bearer $env:SMARTINFO_OTA_TOKEN" "http://<device-ip>/status"
```

Status includes `version`, `build_hash`, `slot`, `pending_verify`, `ota_phase`,
`received`, `total`, `ota_error`, `free_heap`, and `largest_free_block` (bytes).
Boot state follows ESP-IDF's numeric OTA state.
A pending image is confirmed after successful device-server/UI initialization,
30 seconds of operation, and a BSEC sample no older than 10 seconds.
Internet services are not health checks.
Startup failure requests rollback; a reset before confirmation also rolls back.
Further uploads are rejected while boot verification or another upload is active.

Failed, oversized, incompatible, interrupted, or stalled uploads do not select a
new boot slot. Uploads have a 180-second deadline. Keep power stable during updates.
Ordinary OTA changes application firmware only; bootloader/partition layout changes
still require USB. Changes to NVS formats must remain compatible with rollback.

Hardware regression checks (the `--update` option writes both slots and reboots):

```powershell
python tools/test_ota.py <device-ip> --image build/SmartInfo7.bin --update
```

Add `--timeout-test` to test the three-minute stalled-upload deadline. The script
uses only the Python standard library and reads `SMARTINFO_OTA_TOKEN` from the
environment. It also tests invalid images, wrong project/chip, authentication,
concurrent requests, and interrupted transfers.
Add `--batch-helper` on Windows to test the batch helper's argument/token checks
and perform an upload with reboot verification using the token in local `sdkconfig`.

## 🧩 Pin Configuration
Pin mappings are defined in `components/bsp/include/bsp.h`. standard configuration for ESP32 DevKit V1:
- **SPI MOSI:** GPIO 23
- **SPI MISO:** GPIO 19
- **SPI CLK:** GPIO 18
- **LCD CS:** GPIO 25
- **LCD DC:** GPIO 32
- **LCD RST:** GPIO 26
- **Touch CS:** GPIO 13
- **Touch IRQ:** GPIO 27
- **Backlight:** GPIO 4
- **I2C SDA:** GPIO 21
- **I2C SCL:** GPIO 22
- **Battery sense:** GPIO 34 (ADC1 channel 6, `BSP_BAT_SENSE`), white wire from the divider midpoint.
- **Charger sense:** GPIO 35 (ADC1 channel 7, `BSP_CHARGER_SENSE`), from the charger VIN divider midpoint.

### Battery Runtime and Charger Sense

The battery voltage divider uses 100 kΩ from battery BAT+ to GPIO 34 and 100 kΩ from GPIO 34 to GND, with a 100 nF capacitor from GPIO 34 to GND. The charger detector uses the external charger VIN: connect VIN through 100 kΩ to GPIO 35, then connect 33 kΩ and 100 nF from GPIO 35 to common GND. Share GND between the charger, shield, and ESP32. Never connect charger VIN directly to an ESP32 GPIO.

The firmware samples charger presence once per second and requires two consecutive readings beyond its 0.8 V connected / 0.4 V disconnected thresholds. The current and previous battery-run counters are displayed as `HH:MM:SS`; NVS stores them once per minute and on charger-state changes. While charging, the raw-voltage label shows `Charging`; the percentage remains voltage-based.

### Battery Power Efficiency

The display remains on. The clock uses `HH:MM`, and clock/date plus environmental sensor widgets refresh together at wall-clock minute changes. The first real sensor sample is displayed as soon as it is available; BME680/BSEC processing still runs approximately every three seconds. The existing one-second UI timer continues charger detection, battery runtime, and network-status work; battery voltage and percentage are sampled every ten seconds. Settings runtime counters retain `HH:MM:SS` and can redraw every second while visible.

After 60 seconds without touch, the backlight is capped at 20% (or the user-selected value if lower), LVGL refresh/touch periods change from 33 ms to 100 ms, and the display releases its no-light-sleep lock. ESP-IDF uses frequency scaling from 10 to 80 MHz and tickless idle to sleep between work. Touch on GPIO27 wakes the ESP32, restores the 33 ms periods and selected brightness, and blocks light sleep for the next active interval. Data updates do not reset the touch-idle timeout.

Backlight PWM uses the sleep-compatible RC_FAST clock with LEDC keep-alive. GPIO27 uses low-level wake and an interrupt masked until touch release; held presses continue through the existing LVGL input timer. LVGL time comes directly from `esp_timer`, with no recurring 2 ms tick timer.

Wi-Fi sync runs once immediately after boot. With valid time, stock quotes then refresh at five-minute clock boundaries (`:00`, `:05`, and so on); weather and NTP join the hourly `:00` window. If time is unavailable, the scheduler retries on five-minute intervals until it can align to the clock. Manual sync requests can run immediately and share a Wi-Fi session with any pending work. Startup keeps Wi-Fi available for 60 seconds, and the network settings tab, scans, or OTA uploads keep it on while active. Stock, weather, and NTP work runs sequentially in one task so TLS handshakes do not overlap.

The reported runtime before minute-aligned presentation and automatic light sleep was almost 28 hours using two Panasonic NCR18650B cells in parallel. Runtime with the new features is not yet measured. Host checks cover minute scheduling, clock formatting, PM lock transitions, and touch interrupt rearming; physical PWM continuity, sleep entry, touch wake, and battery consumption still require on-device validation. OTA startup confirmation occurs before the normal idle-sleep event and does not validate these behaviors.

A Phase 1 image without PM is saved locally at `build/power-save-phase1/SmartInfo7.bin`. If needed, restore it with `ota_update.bat DEVICE_IP build\power-save-phase1\SmartInfo7.bin`, or use USB recovery if the device is unreachable. Disabling `CONFIG_PM_ENABLE` also allows a recovery build; keep `CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y` for the level-triggered touch ISR.

### BME680 Wiring

| BME680 pin | Wire color | ESP32 DevKit V1 connection |
|---|---|---|
| SDA | Green | GPIO 21 |
| SCL | Yellow | GPIO 22 |
| GND | Black | GND |
| VCC | Red | 3V3 (3.3 V) |

### LCD and Touch Wiring

Bridge the touch clock and data pins to the corresponding LCD SPI pins as listed below.

| LCD/touch pin | Wire color | ESP32 DevKit V1 connection |
|---|---|---|
| LCD_BACKLIGHT | Green | GPIO 4 |
| LCD_SPI_CLK | Blue | GPIO 18 |
| LCD_TOUCH_CLK | Blue | GPIO 18, bridged to LCD_SPI_CLK |
| LCD_SPI_MISO | Green | GPIO 19 |
| LCD_TOUCH_DO | Green | GPIO 19, bridged to LCD_SPI_MISO |
| LCD_SPI_MOSI | Yellow | GPIO 23 |
| LCD_TOUCH_DIN | Yellow | GPIO 23, bridged to LCD_SPI_MOSI |
| LCD_DC | Blue | GPIO 32 |
| LCD_RST | White | GPIO 26 |
| LCD_SPI_CS | Green | GPIO 25 |
| LCD_TOUCH_CS | White | GPIO 13 |
| LCD_TOUCH_INT | Yellow | GPIO 27 |
| GND | Black | GND |
| VCC | Red | 3V3 (3.3 V) |

