/* ____________________________
   This software is licensed under the MIT License:
   https://github.com/cifertech/nrfbox
   ________________________________________ */

#include "config.h"
#include "icon.h"
#include "setting.h"

namespace Analyzer {
  #define NRF24_CONFIG  0x00
  #define NRF24_EN_AA   0x01
  #define NRF24_RF_CH   0x05
  #define NRF24_RF_SETUP 0x06
  #define NRF24_RPD     0x09

  #define N 128
  uint8_t values[N];

  #define CHANNELS 64
  int channel[CHANNELS]; 

  byte getregister(byte r) {
    byte c;
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer(r & 0x1F);
    c = hspiBus.transfer(0);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
    return c;
  }

  void setregister(byte r, byte v) {
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer((r & 0x1F) | 0x20);
    hspiBus.transfer(v);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
  }

  void powerup(void) {
    setregister(NRF24_CONFIG, getregister(NRF24_CONFIG) | 0x02);
    delayMicroseconds(130);
  }

  void powerdown(void) {
    setregister(NRF24_CONFIG, getregister(NRF24_CONFIG) & ~0x02);
  }

  void ENable(void) {
    digitalWrite(NRF_CE_PIN_A, HIGH);
  }

  void DIsable(void) {
    digitalWrite(NRF_CE_PIN_A, LOW);
  }

  void setrx(void) {
    setregister(NRF24_CONFIG, getregister(NRF24_CONFIG) | 0x01);
    ENable();
    delayMicroseconds(100);
  }

  void ScanChannels(void) {
    DIsable();
    // Uncomment if needed: for (int j = 0; j < 10; j++) {
      for (int i = 0; i < CHANNELS; i++) {
        setNeoPixelColour("purple");
        RadioA.setChannel((128 * i) / CHANNELS); 
        setrx();
        delayMicroseconds(40);
        DIsable();
        if (getregister(NRF24_RPD) > 0) channel[i]++;
      }
      setNeoPixelColour("0");
    // }
  }

  void writeRegister(uint8_t reg, uint8_t value) {
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer(reg | 0x20);
    hspiBus.transfer(value);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
  }

  uint8_t readRegister(uint8_t reg) {
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer(reg & 0x1F);
    uint8_t result = hspiBus.transfer(0x00);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
    return result;
  }

  void setChannel(uint8_t channel) {
    writeRegister(NRF24_RF_CH, channel);
  }

  void powerUP() {
    uint8_t config = readRegister(NRF24_CONFIG);
    writeRegister(NRF24_CONFIG, config | 0x02);
    delay(5);
  }

  void powerDOWN() {
    uint8_t config = readRegister(NRF24_CONFIG);
    writeRegister(NRF24_CONFIG, config & ~0x02);
  }

  void startListening() {
    digitalWrite(NRF_CE_PIN_A, HIGH);
  }

  void stopListening() {
    digitalWrite(NRF_CE_PIN_A, LOW);
  }

  bool carrierDetected() {
    return readRegister(NRF24_RPD) & 0x01;
  }

  void analyzerSetup() {
    Serial.begin(115200);

    esp_bt_controller_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    initSpiBuses();
    hspiBus.setDataMode(SPI_MODE0);
    hspiBus.setFrequency(10000000);
    hspiBus.setBitOrder(MSBFIRST);

    digitalWrite(NRF_CSN_PIN_A, HIGH);
    digitalWrite(NRF_CE_PIN_A, LOW);

    beginRadio(RadioA);
    DIsable();

    powerUP();
    writeRegister(NRF24_EN_AA, 0x00);
    // RF_SETUP 0x0F = 2MBPS | RF_PWR 0dBm (max) | LNA on.
    // Was 0x03, which is RF_PWR 0b01 = -12dBm and costs receive sensitivity.
    writeRegister(NRF24_RF_SETUP, 0x0F);

    uiButtonsBegin();
  }

  // The sweep below blocks for a long time, so BACK is sampled as a level
  // inside the inner loops rather than waiting for uiRead() at the top.
  bool backHeld() {
    if (!uiHeld(BTN_BACK)) return false;
    Serial.println("BACK pressed, exiting Analyzer");
    uiRequestExit();
    return true;
  }

  void analyzerLoop() {
    ScanChannels();

    if (backHeld()) return;

    memset(values, 0, sizeof(uint8_t)*N);

    int n = 200;
    while (n--) {
      if (backHeld()) break;
      int i = N;
      while (i--) {
        if (backHeld()) return;
        setChannel(i);
        startListening();
        delayMicroseconds(128);
        stopListening();
        if (carrierDetected()) {
          ++values[i];
        }
      }
    }

    if (backHeld()) return;

    u8g2.clearBuffer();
    int barWidth = SCREEN_WIDTH / N;
    int x = 0;
    for (int i = 0; i < N; ++i) {
      int v = 63 - values[i] * 3;
      if (v < 0) {
        v = 0;
      }
      u8g2.drawVLine(x, v - 10, 64 - v);
      x += barWidth;
    }

    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(0, 64);
    u8g2.print("1...5...10...25..50...80...128");

    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(84, 6, "BACK:exit");

    u8g2.sendBuffer();

    //delay(50);
  }
}

namespace ProtoKill {

  enum OperationMode {WiFi_MODULE, VIDEO_TX_MODULE, RC_MODULE, BLE_MODULE, Bluetooth_MODULE, USB_WIRELESS_MODULE, ZIGBEE_MODULE, NRF24_MODULE};
  OperationMode current_Mode = WiFi_MODULE;

  enum Operation {DEACTIVE_MODE, ACTIVE_MODE};
  Operation current = DEACTIVE_MODE;

  byte channelGroup_1[] = {2, 5, 8, 11};
  byte channelGroup_2[] = {26, 29, 32, 35};
  byte channelGroup_3[] = {80, 83, 86, 89};

  const byte bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
  const byte ble_channels[] = {2, 26, 80};
  const byte WiFi_channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const byte usbWireless_channels[] = {40, 50, 60};
  const byte videoTransmitter_channels[] = {70, 75, 80};
  const byte rc_channels[] = {1, 3, 5, 7};
  const byte zigbee_channels[] = {11, 15, 20, 25};
  const byte nrf24_channels[] = {76, 78, 79};

  void configure_Radio(RF24 &radio, const byte *channels, size_t size) {
    configureNrf(radio);
    radio.printPrettyDetails();
    // Soft-start on the first channel (see nrfStartCarrierRamped), then hop
    // the rest at full power with the PA already biased up.
    if (size > 0) nrfStartCarrierRamped(radio, RF24_PA_MAX, channels[0]);
    for (size_t i = 1; i < size; i++) {
      radio.setChannel(channels[i]);
      radio.startConstCarrier(RF24_PA_MAX, channels[i]);
    }
  }

  void initialize_MultiMode() {
    if (beginRadio(RadioA)) {
      configure_Radio(RadioA, channelGroup_1, sizeof(channelGroup_1));
    }
    if (beginRadio(RadioB)) {
      configure_Radio(RadioB, channelGroup_2, sizeof(channelGroup_2));
    }
    if (beginRadio(RadioC)) {
      configure_Radio(RadioC, channelGroup_3, sizeof(channelGroup_3));
    }
  }

  void initialize_Radios() {
    if (current == ACTIVE_MODE) {
      initialize_MultiMode();
    } else if (current == DEACTIVE_MODE) {
      RadioA.powerDown();
      RadioB.powerDown();
      RadioC.powerDown();
      delay(100);
    }
  }

  void update_OLED() {
    u8g2.clearBuffer();

    // These bitmaps are full-screen 128x64 artwork. They used to be drawn at
    // y=5, which shoved everything down and clipped the bottom five rows off
    // the panel. Origin is (0,0).
    u8g2.drawXBMP(0, 0, 128, 64, bitmap_arrow_left);
    u8g2.drawXBMP(0, 0, 128, 64, bitmap_arrow_right);

    const char* name = "";
    switch (current_Mode) {
      case WiFi_MODULE:         u8g2.drawXBMP(0, 0, 128, 64, bitmap_wifi_jammer);      name = "WiFi";         break;
      case VIDEO_TX_MODULE:     u8g2.drawXBMP(0, 0, 128, 64, bitmap_cctv);             name = "Video TX";     break;
      case RC_MODULE:           u8g2.drawXBMP(0, 0, 128, 64, bitmap_rc);               name = "RC";           break;
      case BLE_MODULE:          u8g2.drawXBMP(0, 0, 128, 64, bitmap_ble_jammer);       name = "BLE";          break;
      case Bluetooth_MODULE:    u8g2.drawXBMP(0, 0, 128, 64, bitmap_bluetooth_jammer); name = "Bluetooth";    break;
      case USB_WIRELESS_MODULE: u8g2.drawXBMP(0, 0, 128, 64, bitmap_usb);              name = "USB Wireless"; break;
      case ZIGBEE_MODULE:       u8g2.drawXBMP(0, 0, 128, 64, bitmap_zigbee);           name = "Zigbee";       break;
      case NRF24_MODULE:        u8g2.drawXBMP(0, 0, 128, 64, bitmap_nrf24);            name = "NRF24";        break;
    }

    // Inverted title bar: protocol on the left, armed state right-aligned.
    // Solid background keeps it legible over whatever artwork is behind it.
    u8g2.drawBox(0, 0, 128, 9);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_5x8_tr);

    char modeBuf[20];
    snprintf(modeBuf, sizeof(modeBuf), "[%s]", name);
    u8g2.drawStr(2, 7, modeBuf);

    char stateBuf[16];
    snprintf(stateBuf, sizeof(stateBuf), "<%s>",
             (current == ACTIVE_MODE) ? "ACTIVE" : "OFF");
    u8g2.drawStr(126 - u8g2.getStrWidth(stateBuf), 7, stateBuf);

    u8g2.setDrawColor(1);

    // Punch a clear strip for the hint instead of drawing it into the artwork.
    u8g2.setDrawColor(0);
    u8g2.drawBox(0, 56, 128, 8);
    u8g2.setDrawColor(1);
    uiDrawHint("UP/DN proto  OK arm  BACK exit");

    u8g2.sendBuffer();
  }

  // UP/DOWN walk the protocol list, OK arms/disarms, BACK leaves the tool.
  void checkMode() {
    switch (uiRead()) {
      case BTN_UP:
        current_Mode = static_cast<OperationMode>((current_Mode == 0) ? 7 : (current_Mode - 1));
        update_OLED();
        break;
      case BTN_DOWN:
        current_Mode = static_cast<OperationMode>((current_Mode + 1) % 8);
        update_OLED();
        break;
      case BTN_OK:
        current = (current == DEACTIVE_MODE) ? ACTIVE_MODE : DEACTIVE_MODE;
        break;
      case BTN_BACK:
        current = DEACTIVE_MODE;
        initialize_Radios();
        uiRequestExit();
        break;
      default:
        break;
    }
  }

  void blackoutSetup() {
    Serial.begin(115200);

    esp_bt_controller_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_wifi_disconnect();

    uiButtonsBegin();

    // Fast startup boots straight in here on its chosen protocol, already
    // armed; otherwise start idle on whatever protocol was last shown and let
    // the user arm it with OK. fastStartProto is 1-based so 0 can mean "off".
    if (fastStartProto != 0) {
      current_Mode = static_cast<OperationMode>(fastStartProto - 1);
      current = ACTIVE_MODE;
    } else {
      current = DEACTIVE_MODE;
    }
    initialize_Radios();
    update_OLED();
  }

  void blackoutLoop() {
    checkMode();
    if (uiExitRequested()) return;

    static Operation lastMode = DEACTIVE_MODE;

    if (current != lastMode) {
      lastMode = current;
      initialize_Radios();
      update_OLED();
    }

    if (current_Mode == BLE_MODULE) {
      int randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
      byte channel = ble_channels[randomIndex];
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == Bluetooth_MODULE) {
      int randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
      byte channel = bluetooth_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == WiFi_MODULE) {
      int randomIndex = random(0, sizeof(WiFi_channels) / sizeof(WiFi_channels[0]));
      byte channel = WiFi_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == USB_WIRELESS_MODULE) {
      int randomIndex = random(0, sizeof(usbWireless_channels) / sizeof(usbWireless_channels[0]));
      byte channel = usbWireless_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == VIDEO_TX_MODULE) {
      int randomIndex = random(0, sizeof(videoTransmitter_channels) / sizeof(videoTransmitter_channels[0]));
      byte channel = videoTransmitter_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == RC_MODULE) {
      int randomIndex = random(0, sizeof(rc_channels) / sizeof(rc_channels[0]));
      byte channel = rc_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == ZIGBEE_MODULE) {
      int randomIndex = random(0, sizeof(zigbee_channels) / sizeof(zigbee_channels[0]));
      byte channel = zigbee_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    } else if (current_Mode == NRF24_MODULE) {
      int randomIndex = random(0, sizeof(nrf24_channels) / sizeof(nrf24_channels[0]));
      byte channel = nrf24_channels[randomIndex]; 
      RadioA.setChannel(channel);
      RadioB.setChannel(channel);
      RadioC.setChannel(channel);
    }
  }
}

namespace Scanner {
  #define CHANNELS 64
  int channel[CHANNELS];

  int line;
  char grey[] = " .:-=+*aRW";

  #define _NRF24_CONFIG   0x00
  #define _NRF24_EN_AA    0x01
  #define _NRF24_RF_CH    0x05
  #define _NRF24_RF_SETUP 0x06
  #define _NRF24_RPD      0x09

  #define EEPROM_ADDRESS_SENSOR_ARRAY 2

  byte sensorArray[129];

  unsigned long lastSaveTime = 0;
  const unsigned long saveInterval = 5000;

  byte getRegister(byte r) {
    byte c;
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer(r & 0x1F);
    c = hspiBus.transfer(0);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
    return c;
  }

  void setRegister(byte r, byte v) {
    digitalWrite(NRF_CSN_PIN_A, LOW);
    hspiBus.transfer((r & 0x1F) | 0x20);
    hspiBus.transfer(v);
    digitalWrite(NRF_CSN_PIN_A, HIGH);
  }

  void powerUp(void) {
    setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x02);
    delayMicroseconds(130);
  }

  void powerDown(void) {
    setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) & ~0x02);
  }

  void enable(void) {
    digitalWrite(NRF_CE_PIN_A, HIGH);
  }

  void disable(void) {
    digitalWrite(NRF_CE_PIN_A, LOW);
  }

  void setRX(void) {
    setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x01);
    enable();
    delayMicroseconds(100);
  }

  // Sampled as a level: the sweep blocks far longer than a button press lasts.
  bool backHeld() {
    if (!uiHeld(BTN_BACK)) return false;
    Serial.println("BACK pressed, exiting Scanner");
    uiRequestExit();
    return true;
  }

  void scanChannels(void) {
    disable();

    memset(channel, 0, sizeof(channel));

    const int samplesPerChannel = 50;

    for (int i = 0; i < CHANNELS; i++) {
      RadioA.setChannel((128 * i) / CHANNELS); // Use RF24 setChannel

      for (int j = 0; j < samplesPerChannel; j++) {
        if (backHeld()) return;  // Exit immediately
        setRX();
        delayMicroseconds(100);
        disable();
        channel[i] += getRegister(_NRF24_RPD);
      }

      channel[i] = (channel[i] * 100) / samplesPerChannel;
    }
  }

  void outputChannels(void) {
    int norm = 0;

    for (int i = 0; i < CHANNELS; i++) {
      if (channel[i] > norm) {
        norm = channel[i];
      }
    }

    byte drawHeight = map(norm, 0, 64, 0, 64);

    for (byte count = 126; count > 0; count--) {
      sensorArray[count] = sensorArray[count - 1];
    }
    sensorArray[0] = drawHeight;

    u8g2.clearBuffer();

    u8g2.drawLine(0, 0, 0, 63);
    u8g2.drawLine(127, 0, 127, 63);

    for (byte count = 0; count < 64; count += 10) {
      u8g2.drawLine(127, count, 122, count);
      u8g2.drawLine(0, count, 5, count);
    }

    for (byte count = 10; count < 127; count += 10) {
      u8g2.drawPixel(count, 0);
      u8g2.drawPixel(count, 63);
    }

    for (byte count = 0; count < 127; count++) {
      u8g2.drawLine(127 - count, 63, 127 - count, 63 - sensorArray[count]);
      setNeoPixelColour("purple");
    }

    setNeoPixelColour("0");

    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(12, 12);
    u8g2.print("[");
    u8g2.print(norm);
    u8g2.print("]");

    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(84, 8, "BACK:exit");

    u8g2.sendBuffer();
  }

  void loadPreviousGraph() {
    EEPROM.begin(128);
    for (byte i = 0; i < 128; i++) {
      sensorArray[i] = EEPROM.read(EEPROM_ADDRESS_SENSOR_ARRAY + i);
    }
    EEPROM.end();
  }

  void saveGraphToEEPROM() {
    EEPROM.begin(128);
    for (byte i = 0; i < 128; i++) {
      EEPROM.write(EEPROM_ADDRESS_SENSOR_ARRAY + i, sensorArray[i]);
    }
    EEPROM.commit();
    EEPROM.end();
  }

  void scannerSetup() {
    Serial.begin(115200);

    esp_bt_controller_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    for (byte count = 0; count <= 128; count++) {
      sensorArray[count] = 0;
    }

    initSpiBuses();
    hspiBus.setDataMode(SPI_MODE0);
    hspiBus.setFrequency(16000000);
    hspiBus.setBitOrder(MSBFIRST);

    beginRadio(RadioA); // Initialize RadioA
    disable();

    powerUp();
    setRegister(_NRF24_EN_AA, 0x0);
    setRegister(_NRF24_RF_SETUP, 0x0F);

    loadPreviousGraph();

    uiButtonsBegin();
  }

  void scannerLoop() {
    if (backHeld()) return;   // before scanning

    scanChannels();

    if (backHeld()) return;   // after scanning

    outputChannels();

    if (backHeld()) return;   // skip the EEPROM save on the way out

    if (millis() - lastSaveTime > saveInterval) {
      saveGraphToEEPROM();
      lastSaveTime = millis();
    }
  }
}

namespace Jammer {

  const int num_channels = 64;
  int value[num_channels];
  int valuesDisplay[32];
  int channels = 1;
  const int num_reps = 50;
  bool jamming = false;
  const byte address[6] = "00001";

  const byte wlanchannels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
  const byte wifi_channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 1, 2, 3, 4, 5, 6, 7};

  byte wifiGroup1[] = {1, 2, 3, 4};
  byte wifiGroup2[] = {5, 6, 7, 8};
  byte wifiGroup3[] = {9, 10, 11, 12};

  // Defaults matter: these are applied by setRadioParameters() the moment the
  // Jammer opens. Index 2 = 2MBPS (widest occupied bandwidth, and matches the
  // global configureNrf setting); index 3 = PA_MAX (full output + LNA).
  uint8_t dataRateIndex = 2;   // 2MBPS
  uint8_t paLevelIndex = 3;    // PA_MAX

  int menuIndex = 0;
  const char* menuItems[] = {"Channel:", "PA Level:", "Data Rate:", "Jamming:"};
  const int menuSize = 4;

  // Rows 0-2 hold a value: OK opens edit mode, then UP/DOWN step it.
  // Row 3 is an action: OK toggles jamming straight away.
  bool editing = false;
  const int actionRow = 3;

  void setRadioParameters() {
    switch (dataRateIndex) {
      case 0: RadioA.setDataRate(RF24_250KBPS); RadioB.setDataRate(RF24_250KBPS); RadioC.setDataRate(RF24_250KBPS); break;
      case 1: RadioA.setDataRate(RF24_1MBPS); RadioB.setDataRate(RF24_1MBPS); RadioC.setDataRate(RF24_1MBPS); break;
      case 2: RadioA.setDataRate(RF24_2MBPS); RadioB.setDataRate(RF24_2MBPS); RadioC.setDataRate(RF24_2MBPS); break;
    }

    // The second argument is lnaEnable (RF_SETUP bit 0). It defaults to true in
    // RF24, but pass it explicitly so a library default change can't silently
    // drop the LNA and cost us receive sensitivity.
    switch (paLevelIndex) {
      case 0: RadioA.setPALevel(RF24_PA_MIN, true); RadioB.setPALevel(RF24_PA_MIN, true); RadioC.setPALevel(RF24_PA_MIN, true); break;
      case 1: RadioA.setPALevel(RF24_PA_LOW, true); RadioB.setPALevel(RF24_PA_LOW, true); RadioC.setPALevel(RF24_PA_LOW, true); break;
      case 2: RadioA.setPALevel(RF24_PA_HIGH, true); RadioB.setPALevel(RF24_PA_HIGH, true); RadioC.setPALevel(RF24_PA_HIGH, true); break;
      case 3: RadioA.setPALevel(RF24_PA_MAX, true); RadioB.setPALevel(RF24_PA_MAX, true); RadioC.setPALevel(RF24_PA_MAX, true); break;
    }
  }

  void radioSetChannel(int channels) {
    RadioA.setChannel(channels);
    RadioB.setChannel(channels);
    RadioC.setChannel(channels);
  }



  void changeChannelNext() {
    channels = (channels % 14) + 1;
    Serial.println("Channel: " + String(channels));
  }

  void changeChannelPrev() {
    channels = (channels - 2 + 14) % 14 + 1;
    Serial.println("Channel: " + String(channels));
  }

  void toggleJamming() {
    jamming = !jamming;
    Serial.println(jamming ? "Jamming started" : "Jamming stopped");
    setNeoPixelColour(jamming ? "red" : "0");
    if (jamming) {
      RadioA.powerUp();
      RadioB.powerUp();
      RadioC.powerUp();
      setRadioParameters();
      // Three PAs coming up together is the worst inrush case on this board,
      // so walk each one up to the selected level rather than stepping there.
      nrfRampPALevel(RadioA, paLevelIndex);
      nrfRampPALevel(RadioB, paLevelIndex);
      nrfRampPALevel(RadioC, paLevelIndex);
    } else {
      RadioA.stopListening();
      RadioA.powerDown();
      RadioB.stopListening();
      RadioB.powerDown();
      RadioC.stopListening();
      RadioC.powerDown();
    }
  }

  void changeDataRateNext() {
    dataRateIndex = (dataRateIndex + 1) % 3;
    setRadioParameters();
    Serial.println("Data rate changed to: " + String(dataRateIndex));
  }

  void changeDataRatePrev() {
    dataRateIndex = (dataRateIndex - 1 + 3) % 3;
    setRadioParameters();
    Serial.println("Data rate changed to: " + String(dataRateIndex));
  }

  void changePALevelNext() {
    paLevelIndex = (paLevelIndex + 1) % 4;
    setRadioParameters();
    Serial.println("PA level changed to: " + String(paLevelIndex));
  }

  void changePALevelPrev() {
    paLevelIndex = (paLevelIndex - 1 + 4) % 4;
    setRadioParameters();
    Serial.println("PA level changed to: " + String(paLevelIndex));
  }

  void stepValueNext() {
    if (menuIndex == 0)      changeChannelNext();
    else if (menuIndex == 1) changePALevelNext();
    else if (menuIndex == 2) changeDataRateNext();
  }

  void stepValuePrev() {
    if (menuIndex == 0)      changeChannelPrev();
    else if (menuIndex == 1) changePALevelPrev();
    else if (menuIndex == 2) changeDataRatePrev();
  }

  // Returns true if anything changed, so callers can redraw.
  bool handleButtons() {
    UiButton button = uiRead();
    if (button == BTN_NONE) return false;

    if (editing) {
      switch (button) {
        case BTN_UP:   stepValuePrev(); break;
        case BTN_DOWN: stepValueNext(); break;
        case BTN_OK:
        case BTN_BACK: editing = false; break;
        default: break;
      }
      return true;
    }

    switch (button) {
      case BTN_UP:
        menuIndex = (menuIndex - 1 + menuSize) % menuSize;
        break;
      case BTN_DOWN:
        menuIndex = (menuIndex + 1) % menuSize;
        break;
      case BTN_OK:
        if (menuIndex == actionRow) toggleJamming();
        else                        editing = true;
        break;
      case BTN_BACK:
        if (jamming) toggleJamming();   // never leave the radios transmitting
        uiRequestExit();
        break;
      default:
        break;
    }
    return true;
  }


  void jammer() {
    int methode = 1;

    if (methode == 1) {
      const char text[] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55,
                            0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
      for (int i = ((channels * 5) + 1); i < ((channels * 5) + 23); i++) {
        // Check buttons to allow immediate deactivation
        handleButtons();
        if (!jamming) break; // Exit loop if jamming is deactivated
        radioSetChannel(i);
        RadioA.write(&text, sizeof(text));
        RadioB.write(&text, sizeof(text));
        RadioC.write(&text, sizeof(text));
      }
    } else if (methode == 2) {
      // Check buttons to allow immediate deactivation
      handleButtons();
      if (!jamming) return; // Exit early if jamming is deactivated
      radioSetChannel(channels);
      int randomIndex = random(0, sizeof(wlanchannels) / sizeof(wlanchannels[0]));
      bool resultA = RadioA.write(&randomIndex, sizeof(randomIndex));
      bool resultB = RadioB.write(&randomIndex, sizeof(randomIndex));
      bool resultC = RadioC.write(&randomIndex, sizeof(randomIndex));
    } else if (methode == 3) {
      for (int i = 0; i < 22; i++) {
        // Check buttons to allow immediate deactivation
        handleButtons();
        if (!jamming) break; // Exit loop if jamming is deactivated
        int channelA = ((channels * 5) + 1) + i;
        int channelB = ((channels * 5) + 1) + i + 1;
        int channelC = ((channels * 5) + 1) + i + 2;

        int randomIndex = random(0, sizeof(wlanchannels) / sizeof(wlanchannels[0]));

        RadioA.setChannel(channelA);
        RadioB.setChannel(channelB);
        RadioC.setChannel(channelC);

        RadioA.write(&randomIndex, sizeof(randomIndex));
        RadioB.write(&randomIndex, sizeof(randomIndex));
        RadioC.write(&randomIndex, sizeof(randomIndex));
      }
    }
  }
  

  void updateDisplay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_profont11_tf);

    u8g2.drawHLine(0, 0, 4); u8g2.drawVLine(0, 0, 4);
    u8g2.drawHLine(124, 0, 4); u8g2.drawVLine(127, 0, 4);
    u8g2.drawHLine(0, 63, 4); u8g2.drawVLine(0, 60, 4);
    u8g2.drawHLine(124, 63, 4); u8g2.drawVLine(127, 60, 4);

    int xshift = 5;
    u8g2.setFont(u8g2_font_profont11_tf);
    for (int i = 0; i < menuSize; i++) {
      int y = (i == 0) ? 10 : (i == 1) ? 24 : (i == 2) ? 38 : 52;
      bool onRow = (menuIndex == i);
      if (onRow) {
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0 + xshift, y - 1, editing ? "*" : ">");
        u8g2.setFont(u8g2_font_profont11_tf);
        u8g2.drawStr(8 + xshift, y, menuItems[i]);
      } else {
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(8 + xshift, y, menuItems[i]);
      }

      u8g2.setCursor(75 + xshift, y);
      u8g2.setFont(u8g2_font_5x7_tf);
      bool editRow = onRow && editing;
      if (editRow) u8g2.print("<");
      if (i == 0) {
        u8g2.print("[");
        u8g2.print(channels);
        u8g2.print("]");
      } else if (i == 1) {
        switch (paLevelIndex) {
          case 0: u8g2.print("MIN"); break;
          case 1: u8g2.print("LOW"); break;
          case 2: u8g2.print("HIGH"); break;
          case 3: u8g2.print("MAX"); break;
        }
      } else if (i == 2) {
        switch (dataRateIndex) {
          case 0: u8g2.print("250KBPS"); break;
          case 1: u8g2.print("1MBPS"); break;
          case 2: u8g2.print("2MBPS"); break;
        }
      } else {
        u8g2.print(jamming ? "Active" : "disable");
      }
      if (editRow) u8g2.print(">");
    }

    uiDrawHint(editing ? "UP/DN adjust  OK done"
                       : (menuIndex == actionRow ? "UP/DN move  OK toggle  BACK exit"
                                                 : "UP/DN move  OK edit  BACK exit"));
    u8g2.sendBuffer();
  }

  void configure(RF24 &radio) {
    beginRadio(radio);
    radio.openWritingPipe(0xFFFFFFFFFF);
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.printPrettyDetails();
  }

  void jammerSetup() {
    Serial.begin(115200);

    esp_bt_controller_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    uiButtonsBegin();

    initSpiBuses();

    configure(RadioA);
    configure(RadioB);
    configure(RadioC);

    setRadioParameters();

    menuIndex = 0;
    editing = false;

    u8g2.begin();
    updateDisplay();
  }

  void jammerLoop() {
    handleButtons();
    if (uiExitRequested()) return;

    updateDisplay();

    if (jamming) {
      jammer();
    }

    delay(50);
  }
}
