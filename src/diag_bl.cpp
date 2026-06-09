// diag_bl.cpp — Waveshare official ST7789 init + fill test
// Uses the EXACT sequence from their Arduino demo (Display_ST7789.cpp)
// to confirm the display hardware works independently of TFT_eSPI.
#ifdef DIAG_BL

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

// ── Pin definitions (from Waveshare Display_ST7789.h) ──────────────────────
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS   14
#define LCD_DC   15
#define LCD_RST  21
#define LCD_BL   22
#define RGB_PIN  8

#define LCD_WIDTH  172
#define LCD_HEIGHT 320
#define Offset_X   34
#define Offset_Y   0

#define SPIFreq  27000000UL   // 27 MHz, safe for C6

Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_RGB + NEO_KHZ800);

// ── Raw SPI helpers ────────────────────────────────────────────────────────
static inline void LCD_WriteCommand(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  SPI.transfer(cmd);
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
}
static inline void LCD_WriteData(uint8_t data) {
  SPI.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, HIGH);
  SPI.transfer(data);
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
}

// ── Reset ──────────────────────────────────────────────────────────────────
static void LCD_Reset() {
  digitalWrite(LCD_CS, LOW);  delay(50);
  digitalWrite(LCD_RST, LOW); delay(50);
  digitalWrite(LCD_RST, HIGH);delay(50);
}

// ── Full Waveshare init sequence (verbatim from Display_ST7789.cpp) ────────
static void LCD_Init() {
  LCD_Reset();
  LCD_WriteCommand(0x11); delay(120);     // SLPOUT

  LCD_WriteCommand(0x36);
  LCD_WriteData(0x70);                    // MADCTL — portrait

  LCD_WriteCommand(0x3A);
  LCD_WriteData(0x05);                    // COLMOD — 16-bit/pixel (RGB565)

  LCD_WriteCommand(0xB0);
  LCD_WriteData(0x00); LCD_WriteData(0xE8);

  LCD_WriteCommand(0xB2);
  LCD_WriteData(0x0C); LCD_WriteData(0x0C); LCD_WriteData(0x00);
  LCD_WriteData(0x33); LCD_WriteData(0x33);

  LCD_WriteCommand(0xB7); LCD_WriteData(0x35);
  LCD_WriteCommand(0xBB); LCD_WriteData(0x35);
  LCD_WriteCommand(0xC0); LCD_WriteData(0x2C);
  LCD_WriteCommand(0xC2); LCD_WriteData(0x01);
  LCD_WriteCommand(0xC3); LCD_WriteData(0x13);
  LCD_WriteCommand(0xC4); LCD_WriteData(0x20);
  LCD_WriteCommand(0xC6); LCD_WriteData(0x0F);

  LCD_WriteCommand(0xD0);
  LCD_WriteData(0xA4); LCD_WriteData(0xA1);

  LCD_WriteCommand(0xD6); LCD_WriteData(0xA1);

  LCD_WriteCommand(0xE0);
  LCD_WriteData(0xF0); LCD_WriteData(0x00); LCD_WriteData(0x04);
  LCD_WriteData(0x04); LCD_WriteData(0x04); LCD_WriteData(0x05);
  LCD_WriteData(0x29); LCD_WriteData(0x33); LCD_WriteData(0x3E);
  LCD_WriteData(0x38); LCD_WriteData(0x12); LCD_WriteData(0x12);
  LCD_WriteData(0x28); LCD_WriteData(0x30);

  LCD_WriteCommand(0xE1);
  LCD_WriteData(0xF0); LCD_WriteData(0x07); LCD_WriteData(0x0A);
  LCD_WriteData(0x0D); LCD_WriteData(0x0B); LCD_WriteData(0x07);
  LCD_WriteData(0x28); LCD_WriteData(0x33); LCD_WriteData(0x3E);
  LCD_WriteData(0x36); LCD_WriteData(0x14); LCD_WriteData(0x14);
  LCD_WriteData(0x29); LCD_WriteData(0x32);

  LCD_WriteCommand(0x21);           // INVON — required for this panel
  LCD_WriteCommand(0x11); delay(120);
  LCD_WriteCommand(0x29);           // DISPON
}

// ── Set window with CGRAM offset applied ──────────────────────────────────
static void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  LCD_WriteCommand(0x2A);
  LCD_WriteData((x0 + Offset_X) >> 8);
  LCD_WriteData((x0 + Offset_X) & 0xFF);
  LCD_WriteData((x1 + Offset_X) >> 8);
  LCD_WriteData((x1 + Offset_X) & 0xFF);

  LCD_WriteCommand(0x2B);
  LCD_WriteData((y0 + Offset_Y) >> 8);
  LCD_WriteData((y0 + Offset_Y) & 0xFF);
  LCD_WriteData((y1 + Offset_Y) >> 8);
  LCD_WriteData((y1 + Offset_Y) & 0xFF);

  LCD_WriteCommand(0x2C);
}

// ── Fill screen with a 16-bit RGB565 color ────────────────────────────────
static void LCD_FillScreen(uint16_t color) {
  LCD_SetWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
  SPI.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, HIGH);
  for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++) {
    SPI.transfer16(color);
  }
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Waveshare ST7789 Direct Init Test ===");

  // RGB LED
  rgb.begin(); rgb.setBrightness(80); rgb.clear(); rgb.show();

  // GPIO
  pinMode(LCD_CS,  OUTPUT); digitalWrite(LCD_CS,  HIGH);
  pinMode(LCD_DC,  OUTPUT); digitalWrite(LCD_DC,  HIGH);
  pinMode(LCD_RST, OUTPUT); digitalWrite(LCD_RST, HIGH);
  pinMode(LCD_BL,  OUTPUT); digitalWrite(LCD_BL,  HIGH);

  // SPI
  SPI.begin(LCD_SCLK, /*MISO*/ 2, LCD_MOSI, LCD_CS);

  // Init display using Waveshare official sequence
  LCD_Init();
  Serial.println("LCD_Init done.");
}

// ── Loop: cycle through 4 solid colors every 2s ───────────────────────────
static const uint16_t COLORS[]  = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
static const uint32_t RGBCOLS[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00 };
static const char*    LABELS[]  = { "RED", "GREEN", "BLUE", "WHITE" };
static int step = 0;

void loop() {
  uint16_t c   = COLORS[step];
  uint32_t rgb_c = RGBCOLS[step];

  LCD_FillScreen(c);
  rgb.setPixelColor(0, rgb_c);
  rgb.show();
  Serial.printf("Screen should be %s (0x%04X)\n", LABELS[step], c);

  delay(2000);
  step = (step + 1) % 4;
}

#endif // DIAG_BL
