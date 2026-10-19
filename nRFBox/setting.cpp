/* ____________________________
   This software is licensed under the MIT License:
   https://github.com/cifertech/nrfbox
   ________________________________________ */

#include "setting.h"
#include "icon.h"
#include "config.h"

// The single definition of each shared global; config.h / setting.h only
// declare them, since those headers are included by six translation units.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE,
                                         /* clock=*/ OLED_SCL_PIN, /* data=*/ OLED_SDA_PIN);
Adafruit_NeoPixel pixels(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

bool neoPixelActive = false;
uint8_t oledBrightness = 100;
// 0 = off; 1..8 map onto ProtoKill::OperationMode in declaration order:
// WiFi, VIDEO_TX, RC, BLE, Bluetooth, USB_WIRELESS, ZIGBEE, NRF24.
bool showGuide = true;
uint8_t fastStartProto = 0;
const char* const fastStartNames[FAST_START_COUNT] = {
  "Off", "WiFi", "Video TX", "RC", "BLE", "Bluetooth", "USB Wless", "Zigbee", "NRF24"
};

// ---------------------------------------------------------------------------
// 4-button UI input layer
// ---------------------------------------------------------------------------
namespace {
  const uint8_t kBtnCount = 4;
  const uint8_t kBtnPins[kBtnCount] = {
    BUTTON_UP_PIN, BUTTON_DOWN_PIN, BUTTON_OK_PIN, BUTTON_BACK_PIN
  };
  // Only UP and DOWN repeat while held; OK and BACK must be deliberate.
  const bool kBtnRepeats[kBtnCount] = { true, true, false, false };

  const unsigned long kBtnDebounce    = 30;   // ignore chatter after a release
  const unsigned long kBtnRepeatDelay = 450;  // hold this long before repeating
  const unsigned long kBtnRepeatRate  = 110;  // then one event per this long

  bool          btnDown[kBtnCount]     = { false, false, false, false };
  unsigned long btnReleased[kBtnCount] = { 0, 0, 0, 0 };
  unsigned long btnNextRepeat[kBtnCount] = { 0, 0, 0, 0 };

  bool exitFlag = false;
}

void uiButtonsBegin() {
  for (uint8_t i = 0; i < kBtnCount; i++) {
    pinMode(kBtnPins[i], INPUT_PULLUP);
    btnDown[i] = false;
    btnReleased[i] = millis();
  }
}

UiButton uiRead() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < kBtnCount; i++) {
    bool down = (digitalRead(kBtnPins[i]) == LOW);

    if (!down) {
      if (btnDown[i]) btnReleased[i] = now;
      btnDown[i] = false;
      continue;
    }

    if (!btnDown[i]) {
      if (now - btnReleased[i] < kBtnDebounce) continue;  // contact bounce
      btnDown[i] = true;
      btnNextRepeat[i] = now + kBtnRepeatDelay;
      return static_cast<UiButton>(i + 1);
    }

    if (kBtnRepeats[i] && (long)(now - btnNextRepeat[i]) >= 0) {
      btnNextRepeat[i] = now + kBtnRepeatRate;
      return static_cast<UiButton>(i + 1);
    }
  }

  return BTN_NONE;
}

bool uiHeld(UiButton button) {
  if (button == BTN_NONE) return false;
  return digitalRead(kBtnPins[button - 1]) == LOW;
}

void uiFlush() {
  unsigned long start = millis();
  bool anyDown;
  do {
    anyDown = false;
    for (uint8_t i = 0; i < kBtnCount; i++) {
      if (digitalRead(kBtnPins[i]) == LOW) anyDown = true;
    }
  } while (anyDown && (millis() - start) < 2000);

  unsigned long now = millis();
  for (uint8_t i = 0; i < kBtnCount; i++) {
    btnDown[i] = false;
    btnReleased[i] = now;
  }
}

void uiRequestExit() { exitFlag = true; }
bool uiExitRequested() { return exitFlag; }
void uiClearExit() { exitFlag = false; }

// Bottom-of-screen legend so the four buttons are always discoverable.
// Skipped entirely when the guide is off, which frees the bottom strip.
void uiDrawHint(const char* hint) {
  if (!showGuide) return;
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 63, hint);
}

// Rows at y = 23 + i*10: the 5th lands on 63, exactly where the hint draws.
// So lists show 4 rows with the guide on, 5 with it off.
int uiVisibleRows() { return showGuide ? 4 : 5; }

RF24 RadioA(NRF_CE_PIN_A, NRF_CSN_PIN_A);
RF24 RadioB(NRF_CE_PIN_B, NRF_CSN_PIN_B);
RF24 RadioC(NRF_CE_PIN_C, NRF_CSN_PIN_C);

SPIClass hspiBus(HSPI);

void initSpiBuses() {
  hspiBus.begin(NRF_SCK_PIN_A, NRF_MISO_PIN_A, NRF_MOSI_PIN_A, NRF_CSN_PIN_A);
  SPI.begin(NRF_SCK_PIN_B, NRF_MISO_PIN_B, NRF_MOSI_PIN_B, NRF_CSN_PIN_B);
}

SPIClass* radioBus(RF24 &radio) {
  return (&radio == &RadioA) ? &hspiBus : &SPI;
}

bool beginRadio(RF24 &radio) {
  return radio.begin(radioBus(radio));
}

// ---------------------------------------------------------------------------
// nRF power handler - soft start (see setting.h for why)
// ---------------------------------------------------------------------------

// Walk the PA up one step at a time instead of jumping straight to target.
// lnaEnable is passed explicitly so receive sensitivity is never dropped.
void nrfRampPALevel(RF24 &radio, uint8_t targetLevel, uint16_t stepMs) {
  if (targetLevel > RF24_PA_MAX) targetLevel = RF24_PA_MAX;
  for (uint8_t level = RF24_PA_MIN; level <= targetLevel; level++) {
    radio.setPALevel(level, true);
    if (level < targetLevel) delay(stepMs);
  }
}

// Key a constant carrier at minimum power first, then ramp. startConstCarrier()
// raises CE itself, so the carrier is already live while we step the PA up.
void nrfStartCarrierRamped(RF24 &radio, uint8_t targetLevel, uint8_t channel,
                           uint16_t stepMs) {
  if (targetLevel > RF24_PA_MAX) targetLevel = RF24_PA_MAX;

  radio.powerUp();                    // startConstCarrier() does NOT power up
  radio.setChannel(channel);
  radio.startConstCarrier(RF24_PA_MIN, channel);

  for (uint8_t level = RF24_PA_MIN + 1; level <= targetLevel; level++) {
    delay(stepMs);
    radio.setPALevel(level, true);
  }
}

void setRadiosNeutralState() {
  RadioA.stopListening();
  RadioA.setAutoAck(false);
  RadioA.setRetries(0, 0);
  RadioA.powerDown(); 
  digitalWrite(NRF_CE_PIN_A, LOW); 

  RadioB.stopListening();
  RadioB.setAutoAck(false);
  RadioB.setRetries(0, 0);
  RadioB.powerDown(); 
  digitalWrite(NRF_CE_PIN_B, LOW); 

  RadioC.stopListening();
  RadioC.setAutoAck(false);
  RadioC.setRetries(0, 0);
  RadioC.powerDown(); 
  digitalWrite(NRF_CE_PIN_C, LOW); 
}

void configureNrf(RF24 &radio) {
  beginRadio(radio);
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
}

void setupRadioA() {
  configureNrf(RadioA);
}

void setupRadioB() {
  configureNrf(RadioB);
}

void setupRadioC() {
  configureNrf(RadioC);
}

void initAllRadios() {
  initSpiBuses();
  setupRadioA();
  setupRadioB();
  setupRadioC();
}

void Str(uint8_t x, uint8_t y, const uint8_t* asciiArray, size_t len) {
  char buf[64]; 
  for (size_t i = 0; i < len && i < sizeof(buf) - 1; i++) {
    buf[i] = (char)asciiArray[i];
  }
  buf[len] = '\0';

  u8g2.drawStr(x, y, buf);
}

void CenteredStr(uint8_t screenWidth, uint8_t y, const uint8_t* asciiArray, size_t len, const uint8_t* font) {
  char buf[64];
  for (size_t i = 0; i < len && i < sizeof(buf) - 1; i++) {
    buf[i] = (char)asciiArray[i];
  }
  buf[len] = '\0';

  u8g2.setFont((const uint8_t*)font);
  int16_t w = u8g2.getUTF8Width(buf);
  u8g2.setCursor((screenWidth - w) / 2, y);
  u8g2.print(buf);
}

void utils() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  Str(7, 15, Line_A, sizeof(Line_A));
  Str(12, 35, Line_B, sizeof(Line_B));
  Str(7, 50, Line_C, sizeof(Line_C));
  uiDrawHint("BACK: menu");
  u8g2.sendBuffer();
}

void conf() {
  u8g2.setBitmapMode(1);
  u8g2.clearBuffer();
  CenteredStr(128, 25, txt_n, sizeof(txt_n), u8g2_font_ncenB14_tr);
  CenteredStr(106, 40, txt_c, sizeof(txt_c), u8g2_font_ncenB08_tr);
  CenteredStr(128, 60, txt_v, sizeof(txt_v), u8g2_font_6x10_tf);
  u8g2.sendBuffer();
  delay(900);
  u8g2.clearBuffer();
  u8g2.drawXBMP(0, 0, 128, 64, cred); 
  u8g2.sendBuffer();
  delay(120);
}

namespace Setting {

// EEPROM addresses live in setting.h so the .ino can read them at boot.

int currentOption = 0;
int totalOptions = 5;   // NeoPixel, Brightness, Boot, Guide, Update Firmware

// Brightness is the only multi-step value here, so OK opens an edit mode on it
// and UP/DOWN then step the value instead of moving the cursor.
bool editing = false;

// Held as a percentage so repeated steps don't drift through the 0-255 mapping,
// and only written to EEPROM when edit mode closes (UP/DOWN auto-repeats).
int brightnessPercent = 100;

void updateFirmware() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 15, "Updating Firmware.");
  u8g2.sendBuffer();

  u8g2.setFont(u8g2_font_5x8_tr);
  if (!SD.begin(SD_CS_PIN)) {
    u8g2.drawStr(0, 30, "[ SD Init Failed ]");
    u8g2.sendBuffer();
    delay(2000);
    return;
  }

  if (!SD.exists(FIRMWARE_FILE)) {
    u8g2.drawStr(0, 30, "[ File Not Found ]");
    u8g2.sendBuffer();
    delay(2000);
    return;
  }

  File firmware = SD.open(FIRMWARE_FILE);
  if (!firmware) {
    u8g2.drawStr(0, 30, "[ Open Failed ]");
    u8g2.sendBuffer();
    delay(2000);
    return;
  }

  if (firmware) {
    u8g2.drawStr(0, 30, "[ Wait a Moment ]");
    u8g2.sendBuffer();
  }

  if (!Update.begin(firmware.size())) {
    u8g2.drawStr(0, 30, "[ Update Init Failed ]");
    u8g2.sendBuffer();
    firmware.close();
    delay(2000);
    return;
  }

  Update.writeStream(firmware);
  if (Update.end(true)) {
    u8g2.drawStr(0, 45, "[ Update Success! ]");
    u8g2.sendBuffer();
    delay(1000);
    ESP.restart();
  } else {
    u8g2.drawStr(0, 45, "[ Update Failed ]");
    u8g2.sendBuffer();
    delay(2000);
  }

  firmware.close();
}

void toggleNeoPixel() {
  neoPixelActive = !neoPixelActive;
  EEPROM.write(EEPROM_ADDR_NEOPIXEL, neoPixelActive);
  EEPROM.commit();
  Serial.print("NeoPixel is now ");
  Serial.println(neoPixelActive ? "Enabled" : "Disabled");
}

// Cycles Off -> WiFi -> Video TX -> ... -> NRF24 -> Off. Written to EEPROM once
// on leaving edit mode, not per step, so holding UP/DOWN can't hammer flash.
void stepFastStart(int delta) {
  int v = (int)fastStartProto + delta;
  while (v < 0) v += FAST_START_COUNT;
  fastStartProto = (uint8_t)(v % FAST_START_COUNT);
  Serial.print("Boot mode: ");
  Serial.println(fastStartNames[fastStartProto]);
}

void saveFastStart() {
  EEPROM.write(EEPROM_ADDR_FASTSTART, fastStartProto);
  EEPROM.commit();
}

void toggleGuide() {
  showGuide = !showGuide;
  EEPROM.write(EEPROM_ADDR_SHOWGUIDE, showGuide ? 1 : 0);
  EEPROM.commit();
  Serial.print("Button guide is now ");
  Serial.println(showGuide ? "On" : "Off");
}

void stepBrightness(int delta) {
  brightnessPercent += delta * 10;
  if (brightnessPercent > 100) brightnessPercent = 0;
  if (brightnessPercent < 0) brightnessPercent = 100;

  oledBrightness = map(brightnessPercent, 0, 100, 0, 255);
  u8g2.setContrast(oledBrightness);

  Serial.print("Brightness set to: ");
  Serial.print(brightnessPercent);
  Serial.println("%");
}

// Called once when edit mode closes, so holding UP/DOWN doesn't hammer flash.
void saveBrightness() {
  EEPROM.write(EEPROM_ADDR_BRIGHTNESS, oledBrightness);
  EEPROM.commit();
}

void handleButtons() {
  UiButton button = uiRead();
  if (button == BTN_NONE) return;

  if (editing) {
    // Rows 1 (brightness) and 2 (boot mode) are the editable ones.
    bool boot = (currentOption == 2);
    switch (button) {
      case BTN_UP:   boot ? stepFastStart(+1) : stepBrightness(+1); break;
      case BTN_DOWN: boot ? stepFastStart(-1) : stepBrightness(-1); break;
      case BTN_OK:
      case BTN_BACK:
        editing = false;
        if (boot) saveFastStart(); else saveBrightness();
        break;
      default: break;
    }
    return;
  }

  switch (button) {
    case BTN_UP:
      currentOption = (currentOption - 1 + totalOptions) % totalOptions;
      break;
    case BTN_DOWN:
      currentOption = (currentOption + 1) % totalOptions;
      break;
    case BTN_OK:
      if (currentOption == 0)      toggleNeoPixel();
      else if (currentOption == 1) editing = true;
      else if (currentOption == 2) editing = true;
      else if (currentOption == 3) toggleGuide();
      else                         updateFirmware();
      break;
    case BTN_BACK:
      uiRequestExit();
      break;
    default:
      break;
  }
}

void displayMenu() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Settings:");

  u8g2.setFont(u8g2_font_5x8_tr);
  // '*' marks a row whose value UP/DOWN is currently editing, '>' just the cursor.
  const char* cursor0 = (currentOption == 0) ? "> NeoPixel: " : "  NeoPixel: ";
  const char* cursor1 = (currentOption != 1) ? "  Brightness: "
                                             : (editing ? "* Brightness: " : "> Brightness: ");
  // Short "Boot:" label leaves room for the full protocol names.
  const char* cursor2 = (currentOption != 2) ? "  Boot: "
                                             : (editing ? "* Boot: " : "> Boot: ");
  const char* cursor3 = (currentOption == 3) ? "> Guide: " : "  Guide: ";
  const char* cursor4 = (currentOption == 4) ? "> Update Firmware" : "  Update Firmware";

  // Baselines chosen so the last row stays clear of the hint strip (y 58..63).
  u8g2.drawStr(0, 21, cursor0);
  u8g2.drawStr(0, 30, cursor1);
  u8g2.drawStr(0, 39, cursor2);
  u8g2.drawStr(0, 48, cursor3);
  u8g2.drawStr(0, 57, cursor4);

  bool editBoot = (editing && currentOption == 2);
  u8g2.setCursor(44, 39);
  if (editBoot) u8g2.print("<");
  u8g2.print(fastStartNames[fastStartProto]);
  if (editBoot) u8g2.print(">");

  u8g2.setCursor(80, 48);
  u8g2.print(showGuide ? "On" : "Off");

  u8g2.setCursor(80, 21);
  u8g2.print(neoPixelActive ? "Enabled" : "Disabled");

  u8g2.setCursor(80, 30);
  if (editing && currentOption == 1) u8g2.print("<");
  u8g2.print(brightnessPercent);
  u8g2.print("%");
  if (editing && currentOption == 1) u8g2.print(">");

  uiDrawHint(editing ? "UP/DN adjust  OK done" : "UP/DN move  OK sel  BACK exit");

  u8g2.sendBuffer();
}

void settingSetup() {
  Serial.begin(115200);

  EEPROM.begin(512);

  neoPixelActive = EEPROM.read(EEPROM_ADDR_NEOPIXEL);
  oledBrightness = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);
  fastStartProto = EEPROM.read(EEPROM_ADDR_FASTSTART);
  if (fastStartProto >= FAST_START_COUNT) fastStartProto = 0;   // blank/garbage EEPROM
  showGuide = (EEPROM.read(EEPROM_ADDR_SHOWGUIDE) != 0);        // 0xFF (blank) -> on

  if (oledBrightness > 255) oledBrightness = 128;
  u8g2.setContrast(oledBrightness);
  brightnessPercent = map(oledBrightness, 0, 255, 0, 100);

  currentOption = 0;
  editing = false;
  uiButtonsBegin();
}

void settingLoop() {
  handleButtons();
  displayMenu();
  }
} 
