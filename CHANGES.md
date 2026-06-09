# Changes vs Upstream

Upstream: https://github.com/anthropics/claude-desktop-buddy

## ESP32-S3 Port (Waveshare ESP32-S3-LCD-3.16)

- Dual build system: PlatformIO + ESP-IDF (`CMakeLists.txt`, `idf_component.yml`)
- Added ST7701 RGB panel driver for the 3.16" RGB LCD (`src/panel_rgb.cpp/h`, `src/panel/`)
- M5StickC+ compatibility shim so shared buddy logic compiles on both targets (`src/m5_compat.cpp/h`)
- Custom board definitions for ESP32-S3-LCD-3.16 and ESP32-C6-LCD (`boards/`)
- BLE diagnostics (`src/diag_bl.cpp`) and S3 display diagnostics (`src/diag_s3_316.cpp`)
- Updated `platformio.ini` for S3 target and no-OTA partition table (`no_ota.csv`)
- Updated BLE bridge, buddy core, character, and stats modules for S3 hardware differences
- Port plan and schematic added to `docs/`
