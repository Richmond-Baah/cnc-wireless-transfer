# CNC Wireless Transfer System - Receiver

ESP32-S3 firmware that receives G-code files wirelessly, stores them on an SD card via SPI, and exposes the card as a USB Mass Storage device to the CNC machine.

## Features
- **Wi-Fi Station mode** — connects to the Transmitter's AP (or a shared LAN)
- **SD Card via SPI** — FATFS filesystem, files stored with original filenames
- **USB Mass Storage** — TinyUSB MSC class exposes the SD card as a standard USB flash drive
- **Strict State Machine** — ensures mutual exclusion between Wi-Fi writes and USB reads
- **MD5 verification** — validates file integrity after transfer
- **LED feedback** — color-coded state indicators

## Hardware Connections (SD Card Pinout)

Connect your generic MicroSD Card SPI module to the ESP32-S3 Super Mini as follows:

| SD Card Module Pin | ESP32-S3 GPIO | Notes |
|--------------------|---------------|----------------------------------|
| **VCC**            | **3.3V**      | Must be 3.3V (Do not use 5V)     |
| **GND**            | **GND**       | Common Ground                    |
| **MOSI**           | **GPIO 11**   | SPI Data Out to SD card          |
| **MISO**           | **GPIO 13**   | SPI Data In from SD card         |
| **SCK / CLK**      | **GPIO 12**   | SPI Clock                        |
| **CS / SS**        | **GPIO 10**   | SPI Chip Select                  |
| *Status LED*       | *GPIO 48*     | *Onboard WS2812 RGB LED (Built-in)*|
| *USB D+/D-*        | *Native*      | *Built-in USB-C Port*            |

> **Important:** The SD card must be formatted as **FAT32** before use. The ESP32-S3 acts as the USB device — the SD card stays physically connected to the ESP32 at all times.

## State Machine

```
IDLE ──────────→ RECEIVING ──────→ FILE_READY
  ↑                  │                 │
  │                  ↓                 ↓
  │               ERROR          USB_ACTIVE
  │                  │                 │
  └──────────────────┴─────────────────┘
```

| State       | LED Color       | Description                                |
|-------------|----------------|--------------------------------------------|
| IDLE        | Off / White    | Ready for incoming files or USB access      |
| RECEIVING   | Blinking Yellow| File transfer in progress, USB blocked      |
| FILE_READY  | Solid Green    | Transfer verified, USB accessible           |
| USB_ACTIVE  | Solid Blue     | PC/CNC machine is reading the USB drive     |
| ERROR       | Solid Red (5s) | Checksum mismatch or write failure          |

### Transition Rules
- `IDLE → RECEIVING`: Valid `/prepare` request received
- `RECEIVING → FILE_READY`: `/finalize` MD5 verified
- `RECEIVING → ERROR`: MD5 mismatch or write failure
- `FILE_READY → USB_ACTIVE`: PC mounts USB
- `USB_ACTIVE → IDLE`: PC unmounts/ejects USB
- `ERROR → IDLE`: Automatic after 5-second LED timeout

## HTTP Endpoints

| Endpoint     | Method | Description                              | Success | Busy  |
|--------------|--------|------------------------------------------|---------|-------|
| `/prepare`   | POST   | Start file reception `{filename,filesize,md5}` | 200 | 503   |
| `/chunk`     | POST   | Write binary chunk to SD card            | 200     | 500   |
| `/finalize`  | POST   | Verify MD5, complete transfer `{md5}`    | 200     | 400   |
| `/status`    | GET    | Get current state `{state:"IDLE"}`       | 200     | —     |

## Default Configuration

| Setting       | Default Value   |
|---------------|-----------------|
| Wi-Fi SSID    | `CNC_WIRELESS`  |
| Wi-Fi Password| `cnc12345`      |

Credentials are stored in NVS flash. They can be dynamically updated from the Transmitter's Web UI.

## Step-by-Step Setup Guide

**Step 1: Format the SD Card**
* Insert a MicroSD card into your computer.
* Format it explicitly as **FAT32** (exFAT or NTFS will not work natively).
* Insert the formatted SD card into the SPI module.

**Step 2: Wire the Hardware**
* Connect the SD Card SPI module to the ESP32-S3 Super Mini using the pinout table above. Double-check that VCC goes to exactly 3.3V.

**Step 3: Build & Flash the Firmware**
* Open your ESP-IDF Command Prompt.
* Navigate to the receiver folder: `cd cnc-wireless-transfer/receiver`
* Set the target platform: `idf.py set-target esp32s3`
* Build and flash the firmware to your board: `idf.py -p COM_PORT flash monitor` (Replace `COM_PORT` with your specific port, e.g., `COM3`).

**Step 4: Connect to the CNC Machine**
* Plug the ESP32-S3's USB-C port directly into the CNC Machine's USB port.
* The device will power on, initialize the SD card, and automatically connect to the Transmitter's Wi-Fi network.
* The LED will slowly pulse white indicating it is `IDLE` and ready.
* The CNC machine will now see the ESP32 as a standard USB Flash Drive!

**Step 5: Receive Files**
* When you upload a file via the Transmitter UI, the Receiver LED will blink yellow.
* Once complete, the LED turns Green, and the new G-code file instantly appears on the CNC machine's screen.
