#!/usr/bin/env python3
"""Wireshark extcap plugin for the nRF52840 ESB / nRF24 sniffer.

Wireshark discovers capture sources by running every executable in its extcap
directory with --extcap-interfaces. This script answers that handshake, then on
--capture opens the dongle's serial port, unwraps its binary frames and writes
a PCAP stream to the fifo Wireshark hands us.

Link type is DLT_USER0 (147); nrf24_esb.lua decodes it.

Install: see ../README.md
"""

import argparse
import struct
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

DLT_USER0 = 147
SYNC = b"\xAB\xCD"
TYPE_PACKET = 0x01
TYPE_TEXT = 0x02

RATES = {"2Mbit": 0, "1Mbit": 1}


# --------------------------------------------------------------- pcap out ---
def pcap_header(out):
    # classic libpcap, microsecond resolution
    out.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 262144, DLT_USER0))


def pcap_record(out, payload):
    now = time.time()
    out.write(struct.pack("<IIII", int(now), int((now % 1) * 1_000_000),
                          len(payload), len(payload)))
    out.write(payload)
    out.flush()


# ------------------------------------------------------------ extcap args ---
def list_interfaces():
    print("extcap {version=1.0}{help=nRF52840 ESB / nRF24 sniffer}")
    print("interface {value=nrf-sniff}{display=NRF-sniff (nRF24 / ESB 2.4GHz)}")


def list_dlts():
    print("dlt {number=%d}{name=USER0}{display=nRF24 ESB}" % DLT_USER0)


def list_config():
    ports = []
    default_port = ""
    if list_ports:
        found = list(list_ports.comports())
        ports = [p.device for p in found]
        # Prefer something that looks like a Nordic dongle over "first COM port",
        # which on Windows is usually a motherboard or Bluetooth serial port.
        # VIDs worth preferring: 0x1915 Nordic (stock/DFU), 0x239A Adafruit -
        # which is what the dongle reports once running this firmware, since it
        # is built against the Adafruit nRF52 core.
        for p in found:
            hay = " ".join(filter(None, (p.description, p.manufacturer,
                                         p.product, p.hwid))).lower()
            if any(k in hay for k in ("nordic", "nrf", "1915", "239a", "adafruit")):
                default_port = p.device
                break
        if not default_port and ports:
            default_port = ports[0]

    print("arg {number=0}{call=--port}{display=Serial port}"
          "{type=selector}{tooltip=Serial port of the nRF52840 dongle}")
    for p in ports:
        print("value {arg=0}{value=%s}{display=%s}%s"
              % (p, p, "{default=true}" if p == default_port else ""))

    print("arg {number=1}{call=--channel}{display=Channel}{type=integer}"
          "{range=0,125}{default=16}{tooltip=2400 + n MHz. nRFBox WLAN Jammer sweeps 6-27}")
    print("arg {number=2}{call=--rate}{display=Data rate}{type=selector}{default=2Mbit}")
    print("value {arg=2}{value=2Mbit}{display=2 Mbit}{default=true}")
    print("value {arg=2}{value=1Mbit}{display=1 Mbit}")

    print("arg {number=3}{call=--address}{display=Pipe address (hex, MSB first)}"
          "{type=string}{default=FFFFFFFFFF}"
          "{tooltip=3-5 bytes. Ignored in promiscuous mode}")
    print("arg {number=4}{call=--promiscuous}{display=Promiscuous (best effort)}"
          "{type=boolflag}{default=false}"
          "{tooltip=No address known. Lossy and prone to false positives}")
    print("arg {number=5}{call=--crc}{display=CRC bytes}{type=selector}{default=0}"
          "{tooltip=nRFBox transmits with CRC DISABLED - use 0 to see its frames}")
    print("value {arg=5}{value=0}{display=0 (off - nRFBox)}{default=true}")
    print("value {arg=5}{value=2}{display=2 (standard ESB)}")
    print("value {arg=5}{value=1}{display=1}")
    print("arg {number=6}{call=--sweep}{display=Sweep all channels}"
          "{type=boolflag}{default=false}"
          "{tooltip=Hop 0-125. Finds activity, but misses packets while hopping}")


# ---------------------------------------------------------------- capture ---
def configure(ser, args):
    """Push settings, then start. Order matters: address before promiscuous."""
    cmds = [
        "C%d" % args.channel,
        "R%d" % RATES.get(args.rate, 0),
        "A%s" % args.address.upper().replace(" ", ""),
        "K%d" % args.crc,
        "P%d" % (1 if args.promiscuous else 0),
        "W%d" % (1 if args.sweep else 0),
        "G",
    ]
    for c in cmds:
        ser.write((c + "\n").encode())
        ser.flush()
        time.sleep(0.05)


def find_dongle():
    """Best guess at the sniffer's port, by USB identity."""
    if not list_ports:
        return ""
    for p in list_ports.comports():
        hay = " ".join(filter(None, (p.description, p.manufacturer,
                                     p.product, p.hwid))).lower()
        if any(k in hay for k in ("nordic", "nrf", "1915", "239a", "adafruit")):
            return p.device
    return ""


def _dumpcap_running():
    """True while a dumpcap process exists (Windows)."""
    try:
        import subprocess
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq dumpcap.exe", "/NH"],
                           capture_output=True, text=True,
                           creationflags=0x08000000)   # no console window
        return "dumpcap" in r.stdout.lower()
    except Exception:  # noqa: BLE001 - if we cannot tell, assume still capturing
        return True


def watch_for_shutdown(stop):
    """Set `stop` once the capture is over.

    Detecting this is awkward. An idle capture never writes to the fifo, so a
    closed pipe cannot be seen from the write side - a zero-length write is
    optimised away without a syscall. stdin never reaches EOF either, because
    the .bat wrapper's cmd.exe sits between dumpcap and us and holds the handle
    open. So track dumpcap itself: it exists for exactly the life of a capture.

    Without this the process lingers holding the serial port, and the next
    capture fails with 'Access is denied'.
    """
    # Grace period - dumpcap may not have appeared yet when we start.
    deadline = time.time() + 10
    while time.time() < deadline and not _dumpcap_running():
        if stop.wait(0.5):
            return

    while not stop.is_set():
        if not _dumpcap_running():
            stop.set()
            return
        stop.wait(1.0)


def capture(args):
    if serial is None:
        sys.stderr.write("pyserial is required: pip install pyserial\n")
        return 1

    # The Wireshark GUI always supplies --port, but tshark -i does not apply
    # config defaults. Fall back to autodetection rather than failing.
    if not args.port:
        args.port = find_dongle()
        if not args.port:
            sys.stderr.write("no serial port given and no dongle found\n")
            return 1
        # Deliberately not logged: Wireshark renders any extcap stderr output
        # as an error banner, so routine chatter would look like a failure.

    try:
        ser = serial.Serial(args.port, 115200, timeout=0.2)
        # TinyUSB CDC discards everything it transmits until the host asserts
        # DTR. Without this the dongle looks completely dead.
        ser.dtr = True
        ser.rts = True
    except Exception as e:  # noqa: BLE001 - surfaced to the Wireshark UI
        sys.stderr.write("cannot open %s: %s\n" % (args.port, e))
        return 1

    # Some boards reset when the port opens; give the firmware a moment.
    time.sleep(0.6)
    ser.reset_input_buffer()
    configure(ser, args)

    # Unbuffered: a buffered flush() with nothing pending makes no syscall,
    # so a closed pipe would never be detected on an idle capture.
    out = open(args.fifo, "wb", buffering=0)
    pcap_header(out)

    stop = threading.Event()
    threading.Thread(target=watch_for_shutdown, args=(stop,), daemon=True).start()

    buf = bytearray()
    last_poke = time.time()
    try:
        while not stop.is_set():
            chunk = ser.read(1024)
            if chunk:
                buf.extend(chunk)

            # With no traffic nothing is ever written to the fifo, so a closed
            # pipe would go unnoticed and this process would linger forever
            # holding the serial port. Flush periodically: once Wireshark has
            # gone away this raises and we shut down cleanly.
            # Belt and braces: if packets *are* flowing, a closed pipe also
            # surfaces as a write error from pcap_record().
            now = time.time()
            if now - last_poke >= 1.0:
                last_poke = now

            # Resynchronise on SYNC rather than trusting stream alignment - the
            # dongle may already have been mid-frame when we attached.
            while True:
                i = buf.find(SYNC)
                if i < 0:
                    del buf[:max(0, len(buf) - 1)]
                    break
                if len(buf) < i + 4:
                    del buf[:i]
                    break
                ftype = buf[i + 2]
                flen = buf[i + 3]
                if len(buf) < i + 4 + flen:
                    del buf[:i]
                    break
                payload = bytes(buf[i + 4:i + 4 + flen])
                del buf[:i + 4 + flen]

                if ftype == TYPE_PACKET and flen >= 8:
                    pcap_record(out, payload)
                elif ftype == TYPE_TEXT and args.verbose:
                    sys.stderr.write("[dongle] %s\n"
                                     % payload.decode("ascii", "replace"))
    except (BrokenPipeError, KeyboardInterrupt, OSError):
        pass          # Wireshark closed the fifo - normal shutdown.
                      # Windows reports the closed pipe as OSError, not
                      # BrokenPipeError, so catch both.
    finally:
        try:
            ser.write(b"X\n")
            ser.close()
        except Exception:  # noqa: BLE001
            pass
        try:
            out.close()
        except Exception:  # noqa: BLE001
            pass
    return 0


def main():
    p = argparse.ArgumentParser(description="nRF24/ESB extcap", add_help=False)
    p.add_argument("--extcap-interfaces", action="store_true")
    p.add_argument("--extcap-dlts", action="store_true")
    p.add_argument("--extcap-config", action="store_true")
    p.add_argument("--extcap-interface")
    p.add_argument("--extcap-version")
    p.add_argument("--extcap-capture-filter")
    p.add_argument("--capture", action="store_true")
    p.add_argument("--fifo")
    p.add_argument("--port", default="")
    p.add_argument("--channel", type=int, default=16)
    p.add_argument("--rate", default="2Mbit")
    p.add_argument("--address", default="FFFFFFFFFF")
    p.add_argument("--promiscuous", action="store_true")
    p.add_argument("--crc", type=int, default=0)
    p.add_argument("--sweep", action="store_true")
    p.add_argument("--verbose", action="store_true",
                   help="echo dongle status lines to stderr")
    p.add_argument("-h", "--help", action="store_true")

    args = p.parse_args()

    if args.help:
        p.print_help()
    elif args.extcap_interfaces:
        list_interfaces()
    elif args.extcap_dlts:
        list_dlts()
    elif args.extcap_config:
        list_config()
    elif args.capture:
        if not args.fifo:
            sys.stderr.write("--fifo is required with --capture\n")
            return 1
        return capture(args)
    else:
        list_interfaces()
    return 0


if __name__ == "__main__":
    sys.exit(main())
