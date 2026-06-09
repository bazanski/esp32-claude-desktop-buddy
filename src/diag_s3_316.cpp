// diag_s3_316.cpp — Panel bring-up test for Waveshare ESP32-S3-LCD-3.16.
// Cycles RED/GREEN/BLUE/WHITE on the 320×820 RGB panel every 2 seconds.
// Serial prints confirm each step; watch for correct colors to verify:
//   - pin map is right
//   - byte order is correct (RED should look red, GREEN green, etc.)
// Flash with [env:esp32-s3-lcd-316-diag].
#ifdef DIAG_S3_316

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "panel_rgb.h"

#define W 320
#define H 820

static uint16_t* fbuf = nullptr;

static void fillColor(uint16_t color) {
  if (!fbuf) return;
  for (int i = 0; i < W * H; i++) fbuf[i] = color;
  panelBlit(fbuf, W, H);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3-LCD-3.16 Panel Bring-up ===");

  fbuf = (uint16_t*)heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!fbuf) {
    Serial.println("ERROR: PSRAM alloc failed!");
    return;
  }
  Serial.printf("Frame buffer: %p (%u bytes)\n", fbuf, W * H * 2);

  panelInit();
  Serial.println("panelInit() done — backlight should be on");
  delay(200);

  // Initial fill: black
  fillColor(0x0000);
}

static const uint16_t COLORS[]  = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
static const char*    LABELS[]  = { "RED", "GREEN", "BLUE", "WHITE" };
static int step = 0;

void loop() {
  fillColor(COLORS[step]);
  Serial.printf("Screen should be %s (0x%04X)\n", LABELS[step], COLORS[step]);
  delay(2000);
  step = (step + 1) % 4;
}

#endif // DIAG_S3_316
