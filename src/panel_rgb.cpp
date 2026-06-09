#ifdef ESP32_S3_LCD_316

#include "panel_rgb.h"
#include <Arduino.h>
#include "driver/ledc.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_panel_io_additions.h"

// ── Pin map (verified from schematic and demo user_config.h) ────────────────
#define PIN_SPI_CS   0
#define PIN_SPI_CLK  2
#define PIN_SPI_SDA  1
#define PIN_RST      16
#define PIN_BL_PWM   6

#define PIN_R0 17
#define PIN_R1 46
#define PIN_R2  3
#define PIN_R3  8
#define PIN_R4 18

#define PIN_G0 14
#define PIN_G1 13
#define PIN_G2 12
#define PIN_G3 11
#define PIN_G4 10
#define PIN_G5  9

#define PIN_B0 21
#define PIN_B1  5
#define PIN_B2 45
#define PIN_B3 48
#define PIN_B4 47

#define PIN_HSYNC 38
#define PIN_VSYNC 39
#define PIN_DE    40
#define PIN_PCLK  41

// ── ST7701S init sequence (verbatim from vendor lvgl_port.c) ────────────────
static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13}, 5, 0},
  {0xEF, (uint8_t []){0x08}, 1, 0},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x10}, 5, 0},
  {0xC0, (uint8_t []){0xE5,0x02}, 2, 0},
  {0xC1, (uint8_t []){0x15,0x0A}, 2, 0},
  {0xC2, (uint8_t []){0x07,0x02}, 2, 0},
  {0xCC, (uint8_t []){0x10}, 1, 0},
  {0xB0, (uint8_t []){0x00,0x08,0x51,0x0D,0xCE,0x06,0x00,0x08,0x08,0x24,0x05,0xD0,0x0F,0x6F,0x36,0x1F}, 16, 0},
  {0xB1, (uint8_t []){0x00,0x10,0x4F,0x0C,0x11,0x05,0x00,0x07,0x07,0x18,0x02,0xD3,0x11,0x6E,0x34,0x1F}, 16, 0},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x11}, 5, 0},
  {0xB0, (uint8_t []){0x4D}, 1, 0},
  {0xB1, (uint8_t []){0x37}, 1, 0},
  {0xB2, (uint8_t []){0x87}, 1, 0},
  {0xB3, (uint8_t []){0x80}, 1, 0},
  {0xB5, (uint8_t []){0x4A}, 1, 0},
  {0xB7, (uint8_t []){0x85}, 1, 0},
  {0xB8, (uint8_t []){0x21}, 1, 0},
  {0xB9, (uint8_t []){0x00,0x13}, 2, 0},
  {0xC0, (uint8_t []){0x09}, 1, 0},
  {0xC1, (uint8_t []){0x78}, 1, 0},
  {0xC2, (uint8_t []){0x78}, 1, 0},
  {0xD0, (uint8_t []){0x88}, 1, 0},
  {0xE0, (uint8_t []){0x80,0x00,0x02}, 3, 100},
  {0xE1, (uint8_t []){0x0F,0xA0,0x00,0x00,0x10,0xA0,0x00,0x00,0x00,0x60,0x60}, 11, 0},
  {0xE2, (uint8_t []){0x30,0x30,0x60,0x60,0x45,0xA0,0x00,0x00,0x46,0xA0,0x00,0x00,0x00}, 13, 0},
  {0xE3, (uint8_t []){0x00,0x00,0x33,0x33}, 4, 0},
  {0xE4, (uint8_t []){0x44,0x44}, 2, 0},
  {0xE5, (uint8_t []){0x0F,0x4A,0xA0,0xA0,0x11,0x4A,0xA0,0xA0,0x13,0x4A,0xA0,0xA0,0x15,0x4A,0xA0,0xA0}, 16, 0},
  {0xE6, (uint8_t []){0x00,0x00,0x33,0x33}, 4, 0},
  {0xE7, (uint8_t []){0x44,0x44}, 2, 0},
  {0xE8, (uint8_t []){0x10,0x4A,0xA0,0xA0,0x12,0x4A,0xA0,0xA0,0x14,0x4A,0xA0,0xA0,0x16,0x4A,0xA0,0xA0}, 16, 0},
  {0xEB, (uint8_t []){0x02,0x00,0x4E,0x4E,0xEE,0x44,0x00}, 7, 0},
  {0xED, (uint8_t []){0xFF,0xFF,0x04,0x56,0x72,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x27,0x65,0x40,0xFF,0xFF}, 16, 0},
  {0xEF, (uint8_t []){0x08,0x08,0x08,0x40,0x3F,0x64}, 6, 0},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13}, 5, 0},
  {0xE8, (uint8_t []){0x00,0x0E}, 2, 0},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x00}, 5, 0},
  {0x11, (uint8_t []){0x00}, 0, 120},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13}, 5, 0},
  {0xE8, (uint8_t []){0x00,0x0C}, 2, 10},
  {0xE8, (uint8_t []){0x00,0x00}, 2, 0},
  {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x00}, 5, 0},
  {0x3A, (uint8_t []){0x55}, 1, 0},   // RGB565
  {0x36, (uint8_t []){0x60}, 1, 0},   // MADCTL MX+MV = 90° CW landscape
  {0x35, (uint8_t []){0x00}, 1, 0},
  {0x29, (uint8_t []){0x00}, 0, 20},  // display on
  {0xC3, (uint8_t []){0x80}, 1, 0},
};

static esp_lcd_panel_handle_t s_panel = NULL;
// Set by the vsync ISR; cleared at the start of each panelBlit wait.
static volatile bool s_vsync_flag = false;

static IRAM_ATTR bool panel_vsync_cb(esp_lcd_panel_handle_t,
                                      const esp_lcd_rgb_panel_event_data_t*,
                                      void*) {
  s_vsync_flag = true;
  return false;
}

// ── Backlight (raw ESP-IDF LEDC, active-LOW: duty 0 = full bright, 255 = off)
// Always reinitialises timer+channel so BLE init can't clobber the config.
void panelBacklight(uint8_t duty) {
  uint8_t hw = 255 - duty;  // invert: caller 0=off,255=on → hw 255=off,0=on

  ledc_timer_config_t tc = {};
  tc.speed_mode      = LEDC_LOW_SPEED_MODE;
  tc.duty_resolution = LEDC_TIMER_8_BIT;
  tc.timer_num       = LEDC_TIMER_3;
  tc.freq_hz         = 50 * 1000;
  tc.clk_cfg         = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_timer_config(&tc));

  ledc_channel_config_t cc = {};
  cc.gpio_num   = PIN_BL_PWM;
  cc.speed_mode = LEDC_LOW_SPEED_MODE;
  cc.channel    = LEDC_CHANNEL_1;
  cc.intr_type  = LEDC_INTR_DISABLE;
  cc.timer_sel  = LEDC_TIMER_3;
  cc.duty       = hw;
  cc.hpoint     = 0;
  ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_channel_config(&cc));
}

// ── Panel initialisation ─────────────────────────────────────────────────────
void panelInit() {
  // 3-wire SPI IO for panel init commands
  spi_line_config_t line_config = {};
  line_config.cs_io_type  = IO_TYPE_GPIO;
  line_config.cs_gpio_num = PIN_SPI_CS;
  line_config.scl_io_type  = IO_TYPE_GPIO;
  line_config.scl_gpio_num = PIN_SPI_CLK;
  line_config.sda_io_type  = IO_TYPE_GPIO;
  line_config.sda_gpio_num = PIN_SPI_SDA;
  line_config.io_expander  = NULL;

  esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
  esp_lcd_panel_io_handle_t io_handle = NULL;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &io_handle));

  // RGB panel config — landscape 820×320 via MADCTL MX+MV (0x60).
  // h_res=820/v_res=320: panel gate driver (820 lines) becomes landscape columns;
  // source driver (320 outputs) becomes landscape rows. No software rotation needed.
  esp_lcd_rgb_panel_config_t rgb_config = {};
  rgb_config.clk_src              = LCD_CLK_SRC_DEFAULT;
  rgb_config.psram_trans_align    = 64;
  rgb_config.bounce_buffer_size_px= 10 * 320;
  rgb_config.num_fbs              = 2;
  rgb_config.data_width           = 16;
  rgb_config.bits_per_pixel       = 16;
  rgb_config.de_gpio_num          = PIN_DE;
  rgb_config.pclk_gpio_num        = PIN_PCLK;
  rgb_config.vsync_gpio_num       = PIN_VSYNC;
  rgb_config.hsync_gpio_num       = PIN_HSYNC;
  rgb_config.flags.fb_in_psram   = 1;
  rgb_config.disp_gpio_num        = -1;

  rgb_config.data_gpio_nums[0]  = PIN_B0;
  rgb_config.data_gpio_nums[1]  = PIN_B1;
  rgb_config.data_gpio_nums[2]  = PIN_B2;
  rgb_config.data_gpio_nums[3]  = PIN_B3;
  rgb_config.data_gpio_nums[4]  = PIN_B4;
  rgb_config.data_gpio_nums[5]  = PIN_G0;
  rgb_config.data_gpio_nums[6]  = PIN_G1;
  rgb_config.data_gpio_nums[7]  = PIN_G2;
  rgb_config.data_gpio_nums[8]  = PIN_G3;
  rgb_config.data_gpio_nums[9]  = PIN_G4;
  rgb_config.data_gpio_nums[10] = PIN_G5;
  rgb_config.data_gpio_nums[11] = PIN_R0;
  rgb_config.data_gpio_nums[12] = PIN_R1;
  rgb_config.data_gpio_nums[13] = PIN_R2;
  rgb_config.data_gpio_nums[14] = PIN_R3;
  rgb_config.data_gpio_nums[15] = PIN_R4;

  rgb_config.timings.pclk_hz          = 18 * 1000 * 1000;
  rgb_config.timings.h_res            = 320;
  rgb_config.timings.v_res            = 820;
  rgb_config.timings.hsync_back_porch = 30;
  rgb_config.timings.hsync_front_porch= 30;
  rgb_config.timings.hsync_pulse_width= 6;
  rgb_config.timings.vsync_back_porch = 20;
  rgb_config.timings.vsync_front_porch= 20;
  rgb_config.timings.vsync_pulse_width= 40;

  st7701_vendor_config_t vendor_config = {};
  vendor_config.rgb_config      = &rgb_config;
  vendor_config.init_cmds       = lcd_init_cmds;
  vendor_config.init_cmds_size  = sizeof(lcd_init_cmds) / sizeof(st7701_lcd_init_cmd_t);
  vendor_config.flags.mirror_by_cmd      = 1;
  vendor_config.flags.enable_io_multiplex= 0;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = PIN_RST;
  panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 16;
  panel_config.vendor_config  = &vendor_config;

  ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io_handle, &panel_config, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = panel_vsync_cb;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr));

  // 3-wire SPI is one-shot; release GP0 so a crash/watchdog reset doesn't
  // leave it OUTPUT-LOW and force bootloader mode. BtnBoot.begin() re-takes it.
  gpio_reset_pin((gpio_num_t)PIN_SPI_CS);

  // Backlight on at 78% duty (200/255)
  panelBacklight(200);
}

// ── Blit sprite buffer to panel ──────────────────────────────────────────────
// Waits for vsync, then immediately writes the 90° CW rotation into fb0.
// Writing starts the instant the panel scan starts row 0, and our write rate
// (63k rows/s) > scan rate (43k rows/s), so we stay ahead of the scan on
// every row — no tearing. Both frame buffers are kept in sync so the panel
// never displays a stale buffer.
void panelBlit(const void* buf, int w, int h) {
  if (!s_panel || !buf) return;
  // Wait for vsync (max 2 frame periods to avoid hanging if ISR is missed).
  s_vsync_flag = false;
  uint32_t deadline = millis() + 40;
  while (!s_vsync_flag && (int32_t)(millis() - deadline) < 0) taskYIELD();

  void *fb0 = nullptr, *fb1 = nullptr;
  esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1);
  if (!fb0) return;
  const uint16_t *src = (const uint16_t*)buf;
  uint16_t       *dst = (uint16_t*)fb0;
  for (int pr = 0; pr < w; pr++) {
    uint16_t *drow = dst + pr * 320;
    for (int pc = 0; pc < h; pc++)
      drow[pc] = src[(h - 1 - pc) * w + pr];
  }
  if (fb1) memcpy(fb1, fb0, (size_t)w * h * 2);
}

// ── Solid-color fill (diagnostic) ───────────────────────────────────────────
void panelFill(uint16_t color) {
  if (!s_panel) return;
  void *fb1 = nullptr, *fb2 = nullptr;
  esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb1, &fb2);
  if (fb1) {
    uint16_t* p = (uint16_t*)fb1;
    for (int i = 0; i < 820 * 320; i++) p[i] = color;
  }
  if (fb2) {
    uint16_t* p = (uint16_t*)fb2;
    for (int i = 0; i < 820 * 320; i++) p[i] = color;
  }
}

#endif // ESP32_S3_LCD_316
