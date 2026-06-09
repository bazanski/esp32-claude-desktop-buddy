# Port "Claude Desktop Buddy" to Waveshare ESP32-S3-LCD-3.16

## Context

The firmware (`src/`) is a Tamagotchi-style desk companion for Claude Code. A desktop
bridge streams session JSON (running/waiting counts, tokens, transcript lines, and
permission prompts) to the device; the device shows an animated buddy and lets the user
**approve / deny** Claude's permission prompts with two buttons. It already builds for
two boards via a hand-rolled M5Stack shim (`src/m5_compat.h`): the original M5StickC Plus
(now Seeed XIAO ESP32-S3) and an in-progress Waveshare **ESP32-C6-LCD-1.47**.

The user wants it running on their **Waveshare ESP32-S3-LCD-3.16** (COM3). Key facts
established during research:

- **MCU**: ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
- **Display**: **320 × 820** panel, RGB565, **RGB-parallel interface (ST7701-class controller)**.
  This is the crux: the firmware draws with **TFT_eSPI**, which only supports simple SPI
  panels (GC9A01 / ST7789). **TFT_eSPI cannot drive this RGB-parallel panel at all** — the
  panel layer must be replaced with an `esp_lcd` RGB driver.
- **IMU**: QMI8658 (currently a stub in `m5_compat.h` returning fake data).
- **Buttons**: BOOT (GPIO0), RESET; plus a power toggle switch. No spare onboard user button.
- **Other**: PCF85063 RTC, SD slot. **No user-addressable RGB LED and no buzzer/speaker.**

### Decisions locked with the user
- **Transport**: **BLE**, same as the original (Nordic UART Service + passkey). `ble_bridge.cpp`
  uses Bluedroid `BLEDevice`, which works on ESP32-S3.
- **Controls**: **No shake gesture.** Match the original M5 button UX: **two external user
  buttons A and B**, and the built-in **BOOT/GPIO0 acts as a secondary "approve" button**
  (mirrors Button A's approve action). (The firmware's "Button C" was an unused XIAO extra —
  it stays unused, matching the M5StickC Plus's two-button layout.)
- **Layout**: **Full tall redesign** for 320 × 820 (not letterboxed).
- IMU is no longer needed for reject. It remains **optional** — implement the real QMI8658
  driver only if we want full parity on the original face-down-nap and auto-rotate behaviors.
- **No LVGL.** The official demo ships LVGL V8.3.11 (`07_*`) and V9.3.0 (`08_*`) UI examples,
  but our buddy already renders its entire UI with **TFT_eSPI sprites** — pulling in either
  LVGL version would mean rewriting all UI/character/stats code and running two graphics
  stacks. So we **keep TFT_eSPI** and reuse **only the demo's `esp_lcd` ST7701 panel driver**
  (which is identical between the V8 and V9 examples). One rendering stack = consistent
  firmware across all three board targets. (The LVGL-version choice is therefore moot.)

### Why this is the chosen approach
The entire UI renders into a single full-screen offscreen buffer (`TFT_eSprite spr` in
`main.cpp:9`) and is pushed once per frame with `spr.pushSprite(0,0)`. A `TFT_eSprite` is
just a RAM/PSRAM bitmap with TFT_eSPI's drawing/font/GIF primitives — it needs no display
hardware. So we **keep all existing drawing code and the sprite**, and only swap the final
push: instead of `pushSprite`, blit the sprite's pixel buffer into an `esp_lcd` RGB
framebuffer. This preserves ~all UI/animation/character/stats logic and isolates the port
to the panel driver + hardware shim + layout constants.

---

## Step 0 — Panel driver config (VERIFIED from schematic + official demo)

Panel: **ST7701S, 320×820, RGB565**, driven as **3-wire SPI (init) + 16-bit RGB-parallel
(pixels)**. **No I/O expander** on this board — all control lines are direct GPIO.
Authoritative pin map decoded from `docs/ESP32-S3-LCD-3.16-schematic.pdf` (ESP32-S3R8 = U11)
and cross-checked against `ESP32-S3-LCD-3.16-Demo/Arduino/examples/07_LVGL_V8_Test`:

| Function | GPIO |
|---|---|
| R0–R4 | 17, 46, 3, 8, 18 |
| G0–G5 | 14, 13, 12, 11, 10, 9 |
| B0–B4 | 21, 5, 45, 48, 47 |
| HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 |
| 3-wire SPI: CLK / SDA / CS | 2 / 1 / **0** |
| LCD reset | 16 |
| Backlight (PWM) | **6** |
| I²C SDA / SCL (QMI8658 + PCF85063) | 15 / 7 |
| Battery ADC | 4 |
| Exposed UART header (free for buttons) | TXD 43 / RXD 44 |

RGB timing (vendor `rgb_port_init()`): **pclk 18 MHz**, HSYNC bp/fp/pw = 30/30/6,
VSYNC bp/fp/pw = 20/20/40. Pixel format set by init `0x3A,0x55` (RGB565), `0x36,0x00` (MADCTL).

**ST7701S init + RGB driver — take the vendor's directly.** The official demo bundles a
standalone `esp_lcd` ST7701 driver and the exact init list (verified read of
`Arduino/examples/07_LVGL_V8_Test/lvgl_port.c` + `src/`). Reuse these files as-is:
- `…/07_LVGL_V8_Test/src/st7701_bsp/` (`esp_lcd_st7701*.c/.h` — `esp_lcd_new_panel_st7701`)
- `…/07_LVGL_V8_Test/src/io_additions/` (`esp_lcd_panel_io_3wire_spi.c` + headers)
- the `lcd_init_cmds[]` table and `rgb_port_init()` config from `lvgl_port.c`
- `lcd_bl_pwm_bsp.c` (backlight: **LEDC ch1, 8-bit, 50 kHz, GP6, active-high**)

Verified `esp_lcd_rgb_panel_config_t`: `data_width=16`, `bits_per_pixel=16`, `pclk_hz=18 MHz`,
`fb_in_psram=true`, `num_fbs=2`, `bounce_buffer_size_px=10*320`, porches H 30/30/6 V 20/20/40,
`disp_gpio_num=-1`. **Data-pin order is BGR** (`data_gpio_nums[0..4]=B0..B4, [5..10]=G0..G5,
[11..15]=R0..R4`) with `rgb_ele_order=RGB` and MADCTL `0x36=0x00` → a **standard RGB565
framebuffer is correct** (the BGR wiring is handled by the pin map). `io_expander=NULL`
confirms no expander. (QMI8658 / PCF85063 drivers live in `Arduino/libraries/SensorLib`,
demoed by `03_QMI8658_Test` / `02_PCF85063_Test`.)

**Resolved earlier conflicts:**
- **GPIO0 = BOOT button AND LCD_CS** (tied via 0 Ω R38). QMI8658 INT is **NOT** on GP0
  (R14 unpopulated). CS is only exercised during 3-wire SPI init; at runtime pixels go over
  the RGB bus, so **BOOT works as a runtime "approve" button** (just avoid SPI traffic while
  it may be pressed — we only init SPI once at boot).
- Reset (16) / backlight (6) are **direct GPIO**, no expander.

**Button wiring**: external **Button A → GP43**, **Button B → GP44** (exposed UART header
J3/J4), plus **BOOT/GP0** as the secondary-approve. No spare onboard button footprints
besides RESET/BOOT.

**Board has no user RGB LED and no buzzer/speaker** → the buddy's LED-pulse and beep paths
become no-ops on this target (the beep stub already is). Battery voltage is real via GP4 ADC.

The existing `src/diag_bl.cpp` + `[env:esp32-c6-lcd-diag]` is the template for a minimal
bring-up sketch to validate the panel before touching the app.

---

## Implementation

### 1. Build target — `platformio.ini` + `boards/`
- Add `[env:esp32-s3-lcd-316]` modeled on `[env:esp32-c6-lcd]` (lines 40–83) but:
  - Use the **pioarduino** platform with **arduino-esp32 ≥ 3.1.0 (ESP-IDF 5.x)** — required so
    `esp_lcd` RGB-panel APIs are available under the Arduino framework (the demo itself is
    Arduino + `esp_lcd`). Board `esp32-s3-devkitc-1` or a custom `boards/esp32-s3-lcd-316.json`
    (copy `boards/esp32-c6-lcd.json`, set `mcu: esp32s3`, `flash_size: 16MB`, **octal PSRAM**).
  - Remove all `-DST7789_DRIVER` / `-DTFT_*` panel pin flags (TFT_eSPI is **canvas-only** now).
  - Add `-DESP32_S3_LCD_316=1` as the new conditional-compile flag (parallels `ESP32_C6_LCD`).
  - Add `board_build.arduino.memory_type = qio_opi` (PSRAM) and `-DBOARD_HAS_PSRAM`.
  - Keep `lib_deps`: TFT_eSPI (canvas), Bounce2, AnimatedGIF, ArduinoJson.
- Add an `[env:esp32-s3-lcd-316-diag]` panel bring-up env (mirrors the C6 diag env).

### 2. New panel driver — `src/panel_rgb.cpp` / `panel_rgb.h` (new) + vendored driver
- **Vendor the demo's `esp_lcd` ST7701 driver** into the project (e.g. `src/panel/st7701_bsp/`
  + `src/panel/io_additions/`, copied from `…/07_LVGL_V8_Test/src/`), and add their dirs to
  `build_src_filter`. These are framework-agnostic C — no LVGL dependency.
- `panel_rgb.cpp` wraps `rgb_port_init()` from the demo's `lvgl_port.c` (the `lcd_init_cmds[]`
  table + the verified `esp_lcd_rgb_panel_config_t` in Step 0) and `esp_lcd_new_panel_st7701`.
  Reset (GP16) / SPI CS (GP0) are handled by the driver; **no expander code**.
- Expose:
  - `panelInit()` → builds the panel, returns the `esp_lcd_panel_handle_t`.
  - `panelBlit(uint16_t* buf)` → `esp_lcd_panel_draw_bitmap(panel, 0,0, 320,820, buf)`
    (same as the demo `flush_cb`). Standard RGB565 buffer; verify TFT_eSprite byte order at
    bring-up (set `spr.setSwapBytes(true)` if colors are swapped — the diag step catches this).
  - `panelBacklight(uint8_t duty)` → port `lcd_bl_pwm_bsp.c` (LEDC ch1, GP6, 8-bit, 50 kHz).
- **PSRAM budget**: panel keeps `num_fbs=2` framebuffers (2 × 320·820·2 ≈ 1.05 MB) + our one
  TFT_eSprite (≈ 0.52 MB) ≈ 1.6 MB of 8 MB. (Skipping LVGL saves ~1 MB of draw buffers.)

### 3. Hardware shim — `src/m5_compat.h` / `m5_compat.cpp`
- **Button pins**: add an `#elif defined(ESP32_S3_LCD_316)` block: `PIN_BTN_A 43`,
  `PIN_BTN_B 44`, `PIN_BL 6`. Add a new **`BtnBoot` `CompatibilityButton(0)`** member and
  `update()` it alongside A/B. GPIO0 has a pullup; `INPUT_PULLUP` + Bounce2 works at runtime
  (it doubles as LCD_CS but CS is only used during the one-time SPI init).
- **Display**: in `M5Compat::begin()`, under the S3 macro, replace `Lcd.init()/fillScreen`
  with `panelInit()`. Keep the `TFT_eSPI Lcd` member **only** so `TFT_eSprite(&M5.Lcd)`
  constructs; never call its hardware draws.
- **Backlight / Axp**: route `CompatibilityAxp::ScreenBreath/SetLDO2` to `panelBacklight()`
  (LEDC PWM on **GP6**, not digital HIGH/LOW). `GetBatVoltage()` can read the real divider on
  **GP4** (ADC); USB/temp stay stubbed; `GetBtnPress()` stays `0` (no AXP power button — auto
  screen-off + menu, as on the C6 build).
- **IMU (optional, for parity)**: implement `CompatibilityImu::Init/getAccelData` against the
  **QMI8658** on I²C **SDA=15 / SCL=7** (reuse **SensorLib**, bundled at
  `…/Demo/Arduino/libraries/SensorLib`; poll, INT is unconnected). If skipped, leave the stub
  returning "flat" → cleanly disables face-down nap and auto-rotate with no other code changes.
- **RTC**: keep the POSIX-time shim (`CompatibilityRtc`); bridge time-sync drives it
  (`data.h:77-90`). The onboard **PCF85063** (same I²C bus, 15/7) is optional, only for
  persisting time across power loss.

### 4. App layout & button wiring — `src/main.cpp`
- **Dimensions**: add `#elif defined(ESP32_S3_LCD_316) → W=320, H=820` (lines 25–29).
  This board has **no user LED** → `#define` the LED-pulse feature out for this target
  (no `LED_PIN`). Beep is already a no-op (no buzzer).
- **`getSafeX(y)`** (lines 33–42): add an S3 branch returning a small flat margin (rectangular
  panel, no rounded corners) — like the C6 branch.
- **Final push**: replace `spr.pushSprite(0,0)` (main.cpp:1256) with
  `panelBlit(spr.getPointer(), W, H)` under the S3 macro.
- **BOOT as secondary approve**: in the button block (~main.cpp:1098–1106), where Button A
  approves a prompt, also fire the same approve path when `M5.BtnBoot.wasReleased()` and
  `inPrompt`. Outside a prompt, BOOT can be a no-op (or duplicate A's "cycle screen").
- **Remove shake**: delete the shake→`P_DIZZY` reject/easter-egg path (`checkShake()` /
  lines ~515–522 & ~1032–1034). Keep face-down nap (lines ~113–117, ~1264–1281) and clock
  auto-orient (lines ~386–423) **only if** the QMI8658 driver is implemented; otherwise they
  are inert via the flat stub.
- **Tall redesign**: rework the layout for 320×820. Constants live in `main.cpp` (they
  `extern` into `buddy_common.h`): `CX`, `CY_BASE`, `BUDDY_X_CENTER`, `BUDDY_CANVAS_W`,
  `BUDDY_Y_BASE/OVERLAY`, and the panel coordinates for `drawApproval()`, `drawHUD()`,
  the menu, and info pages. Target: larger 2× buddy in the upper region, a roomy stacked
  status panel (sessions / tokens / transcript), and a full-width approval card with big
  **A: approve (BOOT too)** / **B: deny** affordances at the bottom. The species art in
  `src/buddies/*.cpp` is centered on `BUDDY_X_CENTER` and scales, so it follows automatically.

### 5. BLE — `src/ble_bridge.cpp`
- Expected to work as-is on S3 (Bluedroid). Verify `sdkconfig`/build enables BT; the device
  still advertises `Claude-XXXX` (`main.cpp:14-20`). No protocol changes — desktop bridge,
  JSON schema, and `{"cmd":"permission",...}` responses are unchanged.

---

## Files to create / change
- **New**: `src/panel_rgb.cpp`, `src/panel_rgb.h`, `boards/esp32-s3-lcd-316.json`,
  optional `src/diag_s3_316.cpp` (panel bring-up).
- **Vendored from the demo** (copied in, ~unmodified): `src/panel/st7701_bsp/*`,
  `src/panel/io_additions/*`, backlight from `lcd_bl_pwm_bsp.c`; SensorLib via `lib_deps`/`lib/`
  if the IMU is implemented.
- **Edit**: `platformio.ini` (two new envs), `src/m5_compat.h` (+`.cpp`), `src/main.cpp`
  (dims, getSafeX, blit, BOOT button, layout, shake removal).
- **Likely untouched**: `src/data.h`, `src/ble_bridge.*`, `src/character.*`,
  `src/buddy*.cpp`, `src/stats.h`, `src/buddies/*` (follow `BUDDY_*` constants).

## Verification (end to end)
0. **Sanity-check the hardware first**: flash the stock `07_LVGL_V8_Test` demo to confirm the
   panel/board are healthy and the pin map is right before porting anything.
1. **Panel**: flash `esp32-s3-lcd-316-diag` (our vendored driver + a color/test-pattern fill);
   confirm correct 320×820 output and colors (fixes byte-order/`setSwapBytes`). Keep pclk at
   18 MHz; only revisit timing if tearing/shift appears.
2. **Canvas blit**: in the app, confirm the buddy/HUD renders correctly via the sprite→RGB
   blit (colors correct → byte order right).
3. **Buttons**: serial-log A / B / BOOT edges; verify A=approve/cycle, long-A=menu,
   B=deny/page, BOOT=approve.
4. **BLE**: pair from the desktop (passkey shown on screen), confirm `Claude-XXXX` connects
   and live JSON updates the UI.
5. **Full loop**: trigger a real Claude permission prompt → device shows approval card →
   press A or BOOT (approve) / B (deny) → desktop receives `{"cmd":"permission",...}` and
   the action proceeds. Confirm stats (`stats.h`) update.
6. (If IMU implemented) lay device face-down → naps; rotate → clock re-orients.
