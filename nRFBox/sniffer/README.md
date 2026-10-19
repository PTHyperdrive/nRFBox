# NRF-sniff — nRF24 / ESB 2.4 GHz sniffer for Wireshark

Captures Nordic **Enhanced ShockBurst / nRF24L01** traffic — the proprietary
2.4 GHz protocol nRFBox uses — into Wireshark, live.

This is **not** a BLE sniffer. For Bluetooth LE use Nordic's official
[nRF Sniffer for Bluetooth LE](https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE);
it does connection following and decryption and is far better than anything
hand-rolled. Nothing off-the-shelf sniffs nRF24, which is why this exists.

## How it works

The nRF52840 radio supports `MODE = Nrf_1Mbit` / `Nrf_2Mbit`, which are
bit-compatible with nRF24L01(+) — same preamble, address matching, packet
control field and CRC. So the dongle can decode an nRF24 link directly in
hardware, no bit-banging.

```
nRF52840 dongle ──USB CDC──> NRF-sniff.py (extcap) ──pcap──> Wireshark
   RADIO in ESB mode          framing + PCAP            nrf24_esb.lua
```

## Install status

Already installed on this machine and verified:

| Piece | Location |
|---|---|
| extcap plugin | `%APPDATA%\Wireshark\extcap\NRF-sniff.py` |
| Windows wrapper | `%APPDATA%\Wireshark\extcap\NRF-sniff.bat` |
| dissector | `%APPDATA%\Wireshark\plugins\nrf24_esb.lua` |

Wireshark lists it as **`NRF-sniff (nRF24 / ESB 2.4GHz)`**. The `.bat` wrapper
exists because Windows extcap entries must be executables; it hands off to the
Store Python (3.12.10) that has `pyserial` 3.5.

## Flashing the dongle

Firmware: `nrf52_esb_sniffer/nrf52_esb_sniffer.ino`, built with the Adafruit
nRF52 core (`arduino-cli core install adafruit:nrf52 --additional-urls
https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`).

```
arduino-cli compile --fqbn adafruit:nrf52:pca10056 sniffer/nrf52_esb_sniffer
```

Builds at 6% flash / 3% RAM. Pick the FQBN matching your board:

| Board | FQBN |
|---|---|
| Nordic nRF52840 **DK** (PCA10056) | `adafruit:nrf52:pca10056` |
| Raytac nRF52840 dongle | `adafruit:nrf52:mdbt50qrx` |

The Nordic **dongle** (PCA10059) has no onboard debugger. Either flash the
generated `.hex` with **nRF Connect for Desktop → Programmer** over its USB
bootloader, or use a DK, which is far less fiddly.

## Using it

1. Plug in the dongle, open Wireshark.
2. Pick **NRF-sniff** in the interface list; click the gear to configure.
3. Set:
   - **Serial port** — auto-defaults to the Nordic device (VID `0x1915`)
   - **Channel** — 0–125, i.e. 2400 + n MHz. nRFBox defaults to **76**
   - **Data rate** — nRFBox's `configureNrf()` uses **2 Mbit**
   - **Pipe address** — nRFBox transmits on `E7E7E7E7E7` unless changed
4. Start capture.

### Capture modes

**Address mode** (default, reliable). You supply the pipe address; the radio
address-matches in hardware and validates CRC. Use this whenever you know the
address.

**Promiscuous** (best effort). The nRF52 cannot match fewer than 3 address
bytes, so it matches a `0xAA` preamble-like pattern with CRC disabled and lets
you sift the results. Expect false positives — same inherent limitation as the
mousejack research. Packets are marked `[promisc, unverified]` so they are never
mistaken for CRC-checked frames.

**Sweep** hops all 126 channels at ~4 ms dwell. Good for finding where activity
is; it will miss packets while hopping, so pin the channel once you know it.

## Wireshark filters

```
nrf24esb                      all captured packets
nrf24esb.crc_ok == false      corrupt or mis-decoded
nrf24esb.channel == 76        one channel
nrf24esb.payload_len == 0     ACKs
nrf24esb.rssi > -60           strong signals only
nrf24esb.address == e7:e7:e7:e7:e7
```

## Frame format

The firmware's `PacketHeader` and the Lua dissector must stay in step:

| Offset | Field | Notes |
|---|---|---|
| 0 | channel | 2400 + n MHz |
| 1 | rate | 0 = 2 Mbit, 1 = 1 Mbit |
| 2 | rssi | magnitude; dissector negates |
| 3 | flags | b0 crc_ok, b1 promiscuous |
| 4 | addr_len | 3–5 |
| 5 | pid | 0–3 |
| 6 | no_ack | 0/1 |
| 7 | payload_len | 0–32 |
| 8 | address | addr_len bytes, MSB first |
| 8+addr_len | payload | payload_len bytes |

Link type is `DLT_USER0` (147).

## Serial protocol

Commands, newline terminated — useful for driving the dongle without Wireshark:

```
C<n>     channel 0..125          R<0|1>   rate 0=2Mbit 1=1Mbit
A<hex>   address, 3..5 bytes     P<0|1>   promiscuous
W<0|1>   sweep                   G / X    start / stop
?        status
```

## What was verified

- Firmware compiles for `adafruit:nrf52:pca10056` — 56 KB flash, 8.9 KB RAM
- extcap handshake (`--extcap-interfaces`, `--extcap-dlts`, `--extcap-config`)
- Port enumeration through the `.bat` → Store Python → pyserial chain
- `tshark -D` lists NRF-sniff
- Dissector auto-loads and correctly decodes synthetic captures: channel→MHz,
  addresses, PID, negated RSSI, ACK detection, CRC and promiscuous flagging,
  full 32-byte payloads
- Frame resynchronisation recovers from mid-stream attachment

**Not yet verified against real over-the-air traffic** — that needs the dongle
flashed and an nRFBox transmitting.
