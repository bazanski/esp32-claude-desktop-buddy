#pragma once
#ifdef ESP32_S3_LCD_316

#include <stdint.h>

void panelInit();
void panelBlit(const void* buf, int w, int h);
void panelBacklight(uint8_t duty);
void panelFill(uint16_t color);   // solid fill for diagnostics

#endif // ESP32_S3_LCD_316
