# nRFBox — prebuilt binaries

Board: **ESP32 Dev Module** (`esp32:esp32:esp32`), esp32 Arduino core **3.3.11**, 4 MB flash.
Built from the 4-button UI / dual-SPI source in the parent folder.

## Which variant?

| Folder | App size | OTA slot | SD "Update Firmware" |
|---|---|---|---|
| `min_spiffs/` | 1,803,218 B (91% of 1.9 MB) | yes (app0 + app1) | **works** |
| `huge_app/`   | 1,803,218 B (57% of 3 MB)   | no (app0 only)    | cannot work |

Use **`min_spiffs`** unless you don't care about the SD firmware-update feature.
`huge_app` has no second app partition, so `Update.begin()` has nowhere to write.

## Flashing

`esptool.exe` lives at:
`%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe`

Replace `COM5` with your port.

### Option A — single merged image (simplest, erases the whole chip)

    esptool --chip esp32 --port COM5 --baud 921600 write-flash -z 0x0 min_spiffs/nRFBox.ino.merged.bin

`merged.bin` is a full 4 MB image containing bootloader + partition table +
boot_app0 + application. Flashing it wipes NVS, so saved NeoPixel/brightness
settings reset to defaults.

### Option B — individual images (keeps NVS / saved settings)

    esptool --chip esp32 --port COM5 --baud 921600 write-flash -z ^
      0x1000  min_spiffs/nRFBox.ino.bootloader.bin ^
      0x8000  min_spiffs/nRFBox.ino.partitions.bin ^
      0xe000  boot_app0.bin ^
      0x10000 min_spiffs/nRFBox.ino.bin

(On bash/PowerShell put it all on one line instead of using `^`.)

### Option C — app only (fastest re-flash, partition layout unchanged)

    esptool --chip esp32 --port COM5 --baud 921600 write-flash -z 0x10000 min_spiffs/nRFBox.ino.bin

Only valid if the board already has the matching partition table flashed.

## Rebuilding from source

    arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
      --output-dir build/min_spiffs <sketch-folder>

The sketch folder must be named `nRFBox` to match `nRFBox.ino`.
In the Arduino IDE the equivalent is **Tools > Partition Scheme > Minimal SPIFFS
(1.9MB APP with OTA/190KB SPIFFS)**; the default scheme overflows at 137%.

## Button wiring for this firmware

    UP   GPIO 26      DOWN  GPIO 32
    OK   GPIO 33      BACK  GPIO 25
