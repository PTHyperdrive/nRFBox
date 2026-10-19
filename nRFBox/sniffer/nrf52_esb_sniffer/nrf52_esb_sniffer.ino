/* nRF52840 Enhanced ShockBurst / nRF24L01 sniffer
 * ------------------------------------------------
 * Streams captured 2.4GHz proprietary (nRF24-compatible) packets over USB CDC
 * to a Wireshark extcap plugin.
 *
 * Why this works: the nRF52840 RADIO supports MODE = Nrf_1Mbit / Nrf_2Mbit,
 * which are bit-compatible with nRF24L01(+) ShockBurst - same preamble, address
 * matching, packet control field and CRC. So the same silicon that talks BLE
 * can decode an nRF24 link, which is exactly what nRFBox transmits.
 *
 * Two capture modes:
 *   ADDRESS      you supply the pipe address. Proper ESB framing with hardware
 *                address match and CRC validation. Reliable.
 *   PROMISCUOUS  best effort. The nRF52 cannot match fewer than 3 address
 *                bytes, so we match a 0xAA preamble-like pattern with CRC off
 *                and let the host sift candidates. Expect false positives -
 *                this is the same trick the mousejack research used, and it
 *                is inherently lossy. Use ADDRESS mode when you know the pipe.
 *
 * Host protocol (binary, little-endian):
 *   0xAB 0xCD  type(1)  len(1)  payload[len]
 *     type 0x01 = packet   payload = PacketHeader + address + payload bytes
 *     type 0x02 = text     payload = ASCII status/log line
 *
 * Host commands (ASCII, newline terminated):
 *   C<n>       channel 0..125            (2400 + n MHz)
 *   R<0|1>     rate  0 = 2Mbit, 1 = 1Mbit
 *   A<hex>     address, 3..5 bytes, MSB first, e.g. AE7E7E7E7E7
 *   K<0|1|2>   CRC length in bytes. nRFBox transmits with CRC DISABLED, so
 *              use K0 to sniff it - otherwise every frame fails validation.
 *   P<0|1>     promiscuous off/on
 *   W<0|1>     sweep all channels off/on
 *   G / X      start / stop capture
 *   ?          report status
 *   D          reboot into the Nordic bootloader for reflashing
 */

#include <Arduino.h>
// Required by the Adafruit nRF52 core to link the USB CDC stack behind Serial.
// Without it every Adafruit_USBD_CDC symbol comes up undefined at link time.
#include <Adafruit_TinyUSB.h>

// ---------------------------------------------------------------- config ---
static const uint8_t  SYNC0 = 0xAB, SYNC1 = 0xCD;
static const uint8_t  TYPE_PACKET = 0x01, TYPE_TEXT = 0x02;
static const uint8_t  MAX_PAYLOAD = 32;

// Dwell per channel when sweeping. Long enough to catch a burst, short enough
// that a 126-channel sweep still completes about twice a second.
static const uint32_t SWEEP_DWELL_MS = 4;

static uint8_t  cfgChannel     = 76;      // nRFBox default
static uint8_t  cfgRate        = 0;       // 0 = 2Mbit
static uint8_t  cfgAddr[5]     = { 0xE7, 0xE7, 0xE7, 0xE7, 0xE7 };
static uint8_t  cfgAddrLen     = 5;
static uint8_t  cfgCrcBytes    = 2;       // 0 = off, 1 or 2 = CRC length
static bool     cfgPromiscuous = false;
static bool     cfgSweep       = false;
static bool     running        = false;

// RADIO writes here. S1 (PID + NO_ACK) lands in byte 1 because S0LEN=0.
static uint8_t  rxBuf[64] __attribute__((aligned(4)));

// ------------------------------------------------------------ host output ---
static void sendFrame(uint8_t type, const uint8_t* data, uint8_t len) {
  Serial.write(SYNC0);
  Serial.write(SYNC1);
  Serial.write(type);
  Serial.write(len);
  if (len) Serial.write(data, len);
}

static void sendText(const char* s) {
  uint8_t n = strlen(s);
  if (n > 200) n = 200;
  sendFrame(TYPE_TEXT, (const uint8_t*)s, n);
}

// Fixed header ahead of address+payload in every packet frame. Mirrors the
// struct the Lua dissector expects - keep the two in step.
struct __attribute__((packed)) PacketHeader {
  uint8_t channel;
  uint8_t rate;         // 0 = 2Mbit, 1 = 1Mbit
  uint8_t rssi;         // magnitude in dBm; host negates
  uint8_t flags;        // b0 crc_ok, b1 promiscuous
  uint8_t addr_len;
  uint8_t pid;          // 0..3
  uint8_t no_ack;       // 0/1
  uint8_t payload_len;
};

// ------------------------------------------------------------------ radio ---
static void radioDisable() {
  // TASKS_DISABLE on an already-disabled radio produces no EVENTS_DISABLED, so
  // waiting on it would spin forever. That is the state at reset, which is
  // exactly when radioConfigure() first runs. Check STATE first, and never
  // wait unbounded even when a transition is genuinely expected.
  if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) {
    NRF_RADIO->EVENTS_DISABLED = 0;
    return;
  }

  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->TASKS_DISABLE = 1;
  uint32_t t0 = millis();
  while (NRF_RADIO->EVENTS_DISABLED == 0 && (millis() - t0) < 100) { }
  NRF_RADIO->EVENTS_DISABLED = 0;
}

static void radioConfigure() {
  radioDisable();

  NRF_RADIO->MODE = cfgRate ? RADIO_MODE_MODE_Nrf_1Mbit : RADIO_MODE_MODE_Nrf_2Mbit;
  NRF_RADIO->FREQUENCY = cfgChannel;          // 2400 + n MHz
  NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;

  const uint8_t addrLen = cfgPromiscuous ? 3 : cfgAddrLen;

  // ESB framing: S0 = 0 bits, LENGTH = 6 bits, S1 = 3 bits (PID[2] | NO_ACK[1])
  NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                     (6 << RADIO_PCNF0_LFLEN_Pos) |
                     (3 << RADIO_PCNF0_S1LEN_Pos);

  // BALEN counts base bytes only; on-air address = BALEN + 1 prefix byte.
  NRF_RADIO->PCNF1 = (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Big       << RADIO_PCNF1_ENDIAN_Pos)  |
                     (((uint32_t)(addrLen - 1))    << RADIO_PCNF1_BALEN_Pos)   |
                     (0                            << RADIO_PCNF1_STATLEN_Pos) |
                     (((uint32_t)MAX_PAYLOAD)      << RADIO_PCNF1_MAXLEN_Pos);

  // Address bytes are given MSB first. PREFIX takes the first byte, BASE the
  // rest, left-justified into the 32-bit register.
  uint8_t a[5];
  if (cfgPromiscuous) {
    a[0] = 0xAA; a[1] = 0xAA; a[2] = 0xAA;    // preamble-like pattern
  } else {
    memcpy(a, cfgAddr, addrLen);
  }
  uint32_t base = 0;
  for (uint8_t i = 1; i < addrLen; i++) base = (base << 8) | a[i];
  base <<= (8 * (4 - (addrLen - 1)));          // left-justify

  NRF_RADIO->BASE0    = base;
  NRF_RADIO->PREFIX0  = a[0];
  NRF_RADIO->TXADDRESS   = 0;
  NRF_RADIO->RXADDRESSES = 1;                  // logical address 0 only

  // Promiscuous always runs CRC-less; otherwise honour cfgCrcBytes. nRFBox
  // calls setCRCLength(RF24_CRC_DISABLED), so sniffing it needs K0.
  const uint8_t crcLen = cfgPromiscuous ? 0 : cfgCrcBytes;
  if (crcLen == 0) {
    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
  } else {
    // ESB CRC covers the address too. 1-byte uses a different polynomial.
    NRF_RADIO->CRCCNF = ((crcLen == 1 ? RADIO_CRCCNF_LEN_One : RADIO_CRCCNF_LEN_Two)
                                                     << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = (crcLen == 1) ? 0x107 : 0x11021;
    NRF_RADIO->CRCINIT = (crcLen == 1) ? 0xFF  : 0xFFFF;
  }

  NRF_RADIO->PACKETPTR = (uint32_t)rxBuf;
  // Assigning SHORTS here used to clobber the ADDRESS->RSSISTART short set up
  // in setup(), so every captured packet reported 0 dBm. Set both together.
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
                      RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  NRF_RADIO->EVENTS_END = 0;
}

static void radioStartRx() {
  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->TASKS_RXEN = 1;
}

// Non-blocking: emits one frame if a packet landed since the last call.
static void radioPoll() {
  if (NRF_RADIO->EVENTS_END == 0) return;
  NRF_RADIO->EVENTS_END = 0;

  const bool crcOk = (NRF_RADIO->CRCSTATUS == 1);
  const uint8_t len = rxBuf[0] & 0x3F;              // LENGTH field, 6 bits
  const uint8_t s1  = rxBuf[1];                      // PID[1:0] << 1 | NO_ACK

  const bool crcChecked = (!cfgPromiscuous && cfgCrcBytes > 0);
  if (len <= MAX_PAYLOAD && (crcOk || !crcChecked)) {
    PacketHeader h;
    h.channel     = cfgChannel;
    h.rate        = cfgRate;
    h.rssi        = (uint8_t)NRF_RADIO->RSSISAMPLE;  // already a magnitude
    h.flags       = ((crcChecked && crcOk) ? 0x01 : 0x00) |
                    ((cfgPromiscuous || !crcChecked) ? 0x02 : 0x00);
    h.addr_len    = cfgPromiscuous ? 3 : cfgAddrLen;
    h.pid         = (s1 >> 1) & 0x03;
    h.no_ack      = s1 & 0x01;
    h.payload_len = len;

    uint8_t out[sizeof(PacketHeader) + 5 + MAX_PAYLOAD];
    uint8_t n = 0;
    memcpy(out + n, &h, sizeof(h)); n += sizeof(h);

    if (cfgPromiscuous) { out[n++] = 0xAA; out[n++] = 0xAA; out[n++] = 0xAA; }
    else                { memcpy(out + n, cfgAddr, cfgAddrLen); n += cfgAddrLen; }

    memcpy(out + n, rxBuf + 2, len); n += len;       // payload starts after S1
    sendFrame(TYPE_PACKET, out, n);
  }

  radioStartRx();
}

// ---------------------------------------------------------------- commands ---
static uint8_t hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0xFF;
}

static void reportStatus() {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "ch=%u rate=%s addr=%02X%02X%02X%02X%02X len=%u crc=%u promisc=%u sweep=%u run=%u",
           cfgChannel, cfgRate ? "1Mbit" : "2Mbit",
           cfgAddr[0], cfgAddr[1], cfgAddr[2], cfgAddr[3], cfgAddr[4],
           cfgAddrLen, cfgCrcBytes, cfgPromiscuous, cfgSweep, running);
  sendText(buf);
}

static void handleCommand(char* line) {
  switch (line[0]) {
    case 'C': {
      int v = atoi(line + 1);
      if (v >= 0 && v <= 125) { cfgChannel = v; radioConfigure(); if (running) radioStartRx(); }
      break;
    }
    case 'R':
      cfgRate = (line[1] == '1') ? 1 : 0;
      radioConfigure(); if (running) radioStartRx();
      break;
    case 'A': {
      // Hex string, MSB first, 3..5 bytes.
      uint8_t tmp[5], n = 0;
      for (char* p = line + 1; p[0] && p[1] && n < 5; p += 2) {
        uint8_t hi = hexNib(p[0]), lo = hexNib(p[1]);
        if (hi == 0xFF || lo == 0xFF) break;
        tmp[n++] = (hi << 4) | lo;
      }
      if (n >= 3) { memcpy(cfgAddr, tmp, n); cfgAddrLen = n;
                    radioConfigure(); if (running) radioStartRx(); }
      break;
    }
    case 'K': {
      uint8_t v = line[1] - '0';
      if (v <= 2) { cfgCrcBytes = v; radioConfigure(); if (running) radioStartRx(); }
      break;
    }
    case 'P':
      cfgPromiscuous = (line[1] == '1');
      radioConfigure(); if (running) radioStartRx();
      break;
    case 'W':
      cfgSweep = (line[1] == '1');
      break;
    case 'G':
      running = true;  radioConfigure(); radioStartRx(); sendText("capture started");
      break;
    case 'X':
      running = false; radioDisable(); sendText("capture stopped");
      break;
    case '?':
      reportStatus();
      break;
    case 'D':
      // Reboot into the Nordic Open Bootloader so the dongle can be reflashed
      // without reaching for the RESET button. 0xB1 is the magic GPREGRET
      // value the bootloader looks for on startup.
      sendText("rebooting to DFU");
      delay(50);
      NRF_POWER->GPREGRET = 0xB1;
      NVIC_SystemReset();
      break;
    default:
      break;
  }
}

static void pollSerial() {
  static char line[64];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (n) { line[n] = 0; handleCommand(line); n = 0; }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}

// -------------------------------------------------------------------- main ---
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }

  // Do NOT call any sd_* function here. The SoftDevice image only exists in
  // flash to satisfy this build's memory map (app linked above S140); the
  // Arduino core never enables it. With no SoftDevice running, an SVC lands in
  // the core's FreeRTOS handler and wedges the main task - USB keeps
  // enumerating from interrupt context, so the board looks alive but is not.
  // Nothing owns the RADIO, so we can drive it directly.

  // The radio needs the external crystal. USB is already up by this point,
  // which means the HFXO is running - starting it again would wait on an event
  // that has already fired. Only start it if it genuinely is not running, and
  // never spin without a timeout.
  bool hfxoRunning = (NRF_CLOCK->HFCLKSTAT &
                      (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)) ==
                     ((CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos) |
                      (CLOCK_HFCLKSTAT_SRC_Xtal      << CLOCK_HFCLKSTAT_SRC_Pos));
  if (!hfxoRunning) {
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    uint32_t t0 = millis();
    while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0 && (millis() - t0) < 500) { }
  }

  // Sample RSSI automatically at the start of every received address.
  NRF_RADIO->SHORTS |= RADIO_SHORTS_ADDRESS_RSSISTART_Msk;

  radioConfigure();
  sendText("nRF52840 ESB sniffer ready");
}

void loop() {
  pollSerial();

  if (!running) return;

  radioPoll();

  if (cfgSweep) {
    static uint32_t lastHop = 0;
    if (millis() - lastHop >= SWEEP_DWELL_MS) {
      lastHop = millis();
      cfgChannel = (cfgChannel + 1) % 126;
      radioConfigure();
      radioStartRx();
    }
  }
}
