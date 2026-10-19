/* ____________________________
   This software is licensed under the MIT License:
   https://github.com/cifertech/nrfbox
   ________________________________________ */
   
#ifndef setting_H
#define setting_H

#include <BLEDevice.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <RF24.h>
#include <vector>
#include <string>
#include <SD.h>
#include <Update.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// 4-button UI input layer (UP / DOWN / OK / BACK)
//
// Every screen reads its input through uiRead(), which hands out each physical
// press exactly once, so a press consumed by a tool is never seen again by the
// main menu loop. UP and DOWN auto-repeat while held; OK and BACK never do.
//
// BACK convention: a tool pops its own sub-view if it has one, otherwise it
// calls uiRequestExit() and the main loop returns to the icon menu.
// ---------------------------------------------------------------------------
enum UiButton : uint8_t { BTN_NONE = 0, BTN_UP, BTN_DOWN, BTN_OK, BTN_BACK };

void uiButtonsBegin();
UiButton uiRead();
bool uiHeld(UiButton button);
void uiFlush();

void uiRequestExit();
bool uiExitRequested();
void uiClearExit();

// The hint legend occupies the bottom ~6px. Scrolling lists must stay above it,
// so uiVisibleRows() reports how many 10px rows actually fit. Turning the guide
// off frees that strip and gives every list one extra row.
void uiDrawHint(const char* hint);
int uiVisibleRows();

extern bool showGuide;

void neopixelSetup();
void neopixelLoop();

void setNeoPixelColour(const std::string& colour);
void flash(int numberOfFlashes, const std::vector<std::string>& colors, const std::string& finalColour);

// All four are defined once in setting.cpp; this header is included by six
// translation units, so they must only be declared here.
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern Adafruit_NeoPixel pixels;

extern bool neoPixelActive;
extern uint8_t oledBrightness;

extern RF24 RadioA;
extern RF24 RadioB;
extern RF24 RadioC;

// RadioA sits on HSPI, RadioB/RadioC (and the SD card) on the default VSPI.
extern SPIClass hspiBus;

void initSpiBuses();
SPIClass* radioBus(RF24 &radio);
bool beginRadio(RF24 &radio);

// ---------------------------------------------------------------------------
// nRF power handler - soft start
//
// A PA module keyed straight to full output pulls its whole transmit current
// in one step, which sags the 3.3V rail and can brown the ESP32 out. These
// walk the PA up MIN -> LOW -> HIGH -> target so the supply and its bulk caps
// have time to follow.
// ---------------------------------------------------------------------------
#define NRF_RAMP_STEP_MS 40

void nrfRampPALevel(RF24 &radio, uint8_t targetLevel, uint16_t stepMs = NRF_RAMP_STEP_MS);
void nrfStartCarrierRamped(RF24 &radio, uint8_t targetLevel, uint8_t channel,
                           uint16_t stepMs = NRF_RAMP_STEP_MS);

// ---------------------------------------------------------------------------
// Fast startup: skip the splash and boot straight into Proto Kill, armed on a
// chosen protocol. Screen brightness is restored from EEPROM before the first
// frame either way.
//
// fastStartProto: 0 = off, otherwise (ProtoKill::OperationMode + 1), so the
// stored value keeps 0 free to mean "disabled".
// ---------------------------------------------------------------------------
// EEPROM map (EEPROM.begin(512) in setup)
//   0       neopixel enable
//   1       oled brightness
//   2..129  scanner graph (128 bytes)
//   130     fast startup flag
#define EEPROM_ADDR_NEOPIXEL   0
#define EEPROM_ADDR_BRIGHTNESS 1
#define EEPROM_ADDR_FASTSTART  130
#define EEPROM_ADDR_SHOWGUIDE  131

// Order after "Off" must match ProtoKill::OperationMode exactly.
#define FAST_START_COUNT 9
extern const char* const fastStartNames[FAST_START_COUNT];
extern uint8_t fastStartProto;

void configureNrf(RF24 &radio);

void setRadiosNeutralState();

void setupRadioA();
void setupRadioB();
void setupRadioC();

void initAllRadios();

void Str(uint8_t x, uint8_t y, const uint8_t* asciiArray, size_t len);
void CenteredStr(uint8_t screenWidth, uint8_t y, const uint8_t* asciiArray, size_t len, const uint8_t* font);
void utils();
void conf();

namespace Setting {
  void settingSetup();
  void settingLoop();
}

#endif
