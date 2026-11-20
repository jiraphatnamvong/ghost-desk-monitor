// Gateway pin map: Waveshare ESP32-S3-DEV-KIT-N8R8 (DevKitC-1 layout).
// Avoid: 0 3 45 46 (strapping), 19 20 (USB), 35-37 (PSRAM), 38 (RGB LED). Wiring: docs/guide/06-wire-the-gateway.md
#pragma once

// SCD41 + BH1750 share one I2C bus (3.3 V)
#define PIN_I2C_SDA  8
#define PIN_I2C_SCL  9

// MAX4466 OUT. Must be an ADC1 pin (GPIO 1-10): ADC2 stops working once Wi-Fi is on.
#define PIN_MIC      4
