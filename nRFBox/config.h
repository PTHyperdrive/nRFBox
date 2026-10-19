/* ____________________________
   This software is licensed under the MIT License:
   https://github.com/cifertech/nrfbox
   ________________________________________ */

#ifndef CONFIG_H
#define CONFIG_H

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// OLED-specific Pins (I2C)
// Moved off the default 21/22 pair: those are now nRF24 #2 CSN/CE.
#define OLED_SDA_PIN         4
#define OLED_SCL_PIN        17

// Push Buttons-specific Pins (4-button layout: UP / DOWN / OK / BACK)
// GPIO 27 used to be the d-pad RIGHT button and is now spare.
#define BUTTON_UP_PIN       26
#define BUTTON_DOWN_PIN     25
#define BUTTON_OK_PIN       33
#define BUTTON_BACK_PIN     32

// NeoPixel-specific Pin
// Moved off GPIO 14: that is now the HSPI clock.
#define NEOPIXEL_PIN         2

// SD Card Slot-specific Pins (shares the VSPI bus with nRF24 #2)
#define SD_CS_PIN 5
#define FIRMWARE_FILE "/firmware.bin"

// nRF24 module #1 -> HSPI
#define NRF_CE_PIN_A    16
#define NRF_CSN_PIN_A   15
#define NRF_SCK_PIN_A   14
#define NRF_MOSI_PIN_A  13
#define NRF_MISO_PIN_A  12

// nRF24 module #2 -> VSPI
#define NRF_CE_PIN_B    22
#define NRF_CSN_PIN_B   21
#define NRF_SCK_PIN_B   18
#define NRF_MOSI_PIN_B  23
#define NRF_MISO_PIN_B  19

// Spare pin freed by dropping the 5th button. Wire the 3rd module's CE here
// (and CSN to GPIO 0) if you ever populate it.
#define SPARE_GPIO          27

// nRF24 module #3 -> not part of this two-module layout.
// The two SPI buses, the OLED, the 5 buttons, the NeoPixel and the SD CS use
// every output-capable GPIO this chip has left, so RadioC is parked on two
// input-only pins (34/35 cannot drive a line). The code below still builds and
// still calls RadioC; begin() simply returns false and it reports "Inactive",
// exactly as it does today when no third module is plugged in.
#define NRF_CE_PIN_C    34
#define NRF_CSN_PIN_C   35

// Common dependencies
#include "setting.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <vector>
#include <string>
#include <SD.h>
#include <Update.h>

// u8g2 and pixels are DEFINED once in setting.cpp and only declared here.
// This header is included by six translation units, so defining them here
// would multiply-define them at link time.

// BLE-specific dependencies
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// nRF24-specific dependencies
#include <nRF24L01.h>
#include <RF24.h>

// WiFi-specific dependencies
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_system.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <string>

// ESP-specific configurations
#include <esp_bt.h>
#include <esp_wifi.h>

// External declarations
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern Adafruit_NeoPixel pixels;

// BLE-related namespaces
namespace BleJammer {
  void blejammerSetup();
  void blejammerLoop();
}

namespace BleScan {
  void blescanSetup();
  void blescanLoop();
}

namespace SourApple {
  void sourappleSetup();
  void sourappleLoop();
}

namespace Spoofer {
  void spooferSetup();
  void spooferLoop();
}

// nRF24-related namespaces
namespace Analyzer {
  void analyzerSetup();
  void analyzerLoop();
}

namespace ProtoKill {
  void blackoutSetup();
  void blackoutLoop();
}

namespace Scanner {
  void scannerSetup();
  void scannerLoop();
}

namespace Jammer {
  void jammerSetup();
  void jammerLoop();
}

// WiFi-related namespaces
namespace WifiScan {
  void wifiscanSetup();
  void wifiscanLoop();
}

namespace Deauther {
  void deautherSetup();
  void deautherLoop();
}

#endif // CONFIG_H
