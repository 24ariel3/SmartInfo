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

> **Note:** The project uses a custom partition table (`partitions.csv`) to maximize app space (`1.8MB` factory app). This is automatically handled by the build system via `sdkconfig.defaults`.

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

