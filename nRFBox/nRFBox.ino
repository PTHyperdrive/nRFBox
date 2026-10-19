/* ____________________________
   This software is licensed under the MIT License:
   https://github.com/cifertech/nrfbox
   ________________________________________ */

#include "icon.h"
#include "setting.h"
#include "config.h"

extern uint8_t oledBrightness;

const int NUM_ITEMS = 12;
const int MAX_ITEM_LENGTH = 20;

const unsigned char* bitmap_icons[NUM_ITEMS] = {
  bitmap_icon_scanner, bitmap_icon_analyzer, bitmap_icon_jammer, bitmap_icon_kill,
  bitmap_icon_ble_jammer, bitmap_icon_spoofer, bitmap_icon_apple, bitmap_icon_ble,
  bitmap_icon_wifi, bitmap_icon_wifi_jammer, bitmap_icon_about, 
  bitmap_icon_setting
};

char menu_items[NUM_ITEMS][MAX_ITEM_LENGTH] = {  
  "Scanner", "Analyzer", "WLAN Jammer", "Proto Kill", "BLE Jammer",
  "BLE Spoofer", "Sour Apple", "BLE Scan", "WiFi Scan", 
  "Deauther", "About", "Setting"
};

void (*menu_functions[NUM_ITEMS])() = {
  Scanner::scannerSetup, Analyzer::analyzerSetup, Jammer::jammerSetup,
  ProtoKill::blackoutSetup, BleJammer::blejammerSetup, Spoofer::spooferSetup,
  SourApple::sourappleSetup, BleScan::blescanSetup, WifiScan::wifiscanSetup, Deauther::deautherSetup,
  utils, Setting::settingSetup
};

void (*menu_loop_functions[NUM_ITEMS])() = {
  Scanner::scannerLoop, Analyzer::analyzerLoop, Jammer::jammerLoop,
  ProtoKill::blackoutLoop, BleJammer::blejammerLoop, Spoofer::spooferLoop,
  SourApple::sourappleLoop, BleScan::blescanLoop, WifiScan::wifiscanLoop, Deauther::deautherLoop,
  nullptr, Setting::settingLoop
};

int item_selected = 0;
int current_screen = 0;

void drawMenu() {
  u8g2.clearBuffer();
  if (current_screen != 0) return;

  u8g2.setFont(u8g2_font_5x7_tf); 
  u8g2.drawBox(0, 0, 128, 8); 
  u8g2.setDrawColor(0); 
  char versionStr[16];
  for (size_t i = 0; i < sizeof(txt_v); i++) {
    versionStr[i] = (char)txt_v[i];
  }
  versionStr[sizeof(txt_v)] = '\0';

  u8g2.setDrawColor(0);

  Str(2, 7, txt_n, sizeof(txt_n));
  int version_width = u8g2.getUTF8Width(versionStr);
  Str(128 - version_width - 2, 7, txt_v, sizeof(txt_v));
  u8g2.setDrawColor(1); 
  u8g2.drawHLine(0, 8, 128); 

  const int icons_per_row = 3;
  const int icons_per_col = 2;
  const int max_display_items = icons_per_row * icons_per_col; 

  int selected_col = item_selected % icons_per_row;
  int selected_row = (item_selected / icons_per_row) % icons_per_col;
  if (item_selected == 12) {
    selected_row = 1; 
  }
  int highlight_x = 13 + selected_col * 40;
  int highlight_y = 14 + selected_row * 24;

  int start_row = (item_selected / icons_per_row) - selected_row;
  if (start_row < 0) start_row = 0;
  int total_rows = (NUM_ITEMS + icons_per_row - 1) / icons_per_row;
  if (start_row > total_rows - icons_per_col) start_row = total_rows - icons_per_col;
  if (start_row < 0) start_row = 0; 
  int start_item = start_row * icons_per_row;
  int end_item = min(NUM_ITEMS, start_item + max_display_items);

  for (int i = start_item; i < end_item; i++) {
    int idx = i - start_item; 
    int row = idx / icons_per_row;
    int col = idx % icons_per_row;
    int x_pos = 13 + col * 40;
    int y_pos = 14 + row * 24;
    u8g2.drawXBMP(x_pos, y_pos, 16, 16, bitmap_icons[i]);
  }

  u8g2.drawRFrame(highlight_x - 3, highlight_y - 3, 22, 22, 3); 
  u8g2.setDrawColor(0);
  u8g2.drawRFrame(highlight_x - 2, highlight_y - 2, 22, 22, 3); 
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_5x8_tf);
  int name_width = u8g2.getUTF8Width(menu_items[item_selected]);
  int name_x = (128 - name_width) / 2;
  u8g2.drawStr(name_x, 64, menu_items[item_selected]);

  u8g2.setFont(u8g2_font_4x6_tf);
  char pos[10];
  snprintf(pos, sizeof(pos), "%d/%d", item_selected + 1, NUM_ITEMS);
  u8g2.drawStr(0, 64, pos);

  u8g2.drawFrame(124, 18, 4, 38); 
  int bar_height = 38 / total_rows;
  u8g2.drawBox(124, 18 + (bar_height * start_row), 4, bar_height); 

  if (start_row > 0) {
    u8g2.drawStr(124, 15, "."); 
  }
  if (start_row < total_rows - 1) {
    u8g2.drawStr(124, 64, "."); 
  }
  u8g2.sendBuffer();

  setRadiosNeutralState();
}

void openSelectedTool(bool showLoading) {
  current_screen = 1;

  for (int cycle = 0; showLoading && cycle < 1; cycle++) {
    for (int i = 0; i < 3; i++) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(30, 32, "Loading");

      String dots = "";
      for (int j = 0; j <= i; j++) {
        dots += ".";
        setNeoPixelColour("white");
      }
      setNeoPixelColour("0");

      u8g2.drawStr(73, 32, dots.c_str());

      u8g2.sendBuffer();
      delay(70);
    }
  }

  // Don't let the press that opened the tool leak into the tool's first frame.
  uiFlush();
  uiClearExit();

  menu_functions[item_selected]();

  while (current_screen == 1) {
    if (menu_loop_functions[item_selected]) {
      menu_loop_functions[item_selected]();
    } else {
      // Static screens (About) have no loop of their own; BACK still leaves.
      if (uiRead() == BTN_BACK) uiRequestExit();
    }

    if (uiExitRequested()) {
      current_screen = 0;
    }
  }

  uiFlush();
  uiClearExit();
  drawMenu();
}

// Index of "Proto Kill" in menu_items - where fast startup lands.
const int PROTO_KILL_ITEM = 3;

void setup() {
  Serial.begin(115200);
  neopixelSetup();
  initAllRadios();
  EEPROM.begin(512);

  // Brightness is restored before the first frame either way, so fast startup
  // never flashes at the wrong contrast.
  oledBrightness = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);
  fastStartProto = EEPROM.read(EEPROM_ADDR_FASTSTART);
  if (fastStartProto >= FAST_START_COUNT) fastStartProto = 0;   // blank/garbage EEPROM
  showGuide      = (EEPROM.read(EEPROM_ADDR_SHOWGUIDE) != 0);   // 0xFF (blank) -> on

  u8g2.begin();
  u8g2.setContrast(oledBrightness);
  uiButtonsBegin();

  if (fastStartProto != 0) {
    // Straight into Proto Kill on the chosen protocol, already armed
    // (see ProtoKill::blackoutSetup). Splash and loading animation are
    // skipped; BACK still returns to the normal menu.
    item_selected = PROTO_KILL_ITEM;
    openSelectedTool(false);
  } else {
    conf();
    drawMenu();
  }
}

void loop() {
  if (current_screen != 0) return;

  switch (uiRead()) {
    case BTN_UP:
      item_selected = (item_selected - 1 + NUM_ITEMS) % NUM_ITEMS;
      drawMenu();
      break;

    case BTN_DOWN:
      item_selected = (item_selected + 1) % NUM_ITEMS;
      drawMenu();
      break;

    case BTN_OK:
      openSelectedTool(true);
      break;

    case BTN_BACK:
      // Already at the top level; nothing to go back to.
      break;

    default:
      break;
  }
}
