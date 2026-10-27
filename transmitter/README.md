# CNC Wireless Transfer System - Transmitter

ESP32-S3 firmware that hosts a secure web interface for uploading G-code files and wirelessly transfers them to the Receiver unit.

## Features
- **SoftAP or Station mode** — configurable via the web UI Settings panel or NVS
- **Secure login** with session token authentication
- **Chunked file transfer** — streams data directly to the receiver without buffering the full file in RAM
- **MD5 integrity verification** — computed on-the-fly during upload and verified on the receiver
- **LED feedback** — visual indicators for idle, transferring, success, and error states
- **Receiver status polling** — real-time state display in the web UI

## Hardware Connections

| Function    | GPIO Pin | Notes |
|-------------|----------|-------|
| Status LED  | GPIO 48  | Onboard WS2812 RGB LED |

> **Note:** The ESP32-S3 uses its native USB port for flashing only. No external USB connections are needed on the transmitter.

## Default Credentials

| Setting       | Default Value   |
|---------------|-----------------|
| Wi-Fi SSID    | `CNC_WIRELESS`  |
| Wi-Fi Password| `cnc12345`      |
| Web Username  | `admin`         |
| Web Password  | `admin123`      |

All credentials can be changed from the **Settings** panel in the web UI after login.

## Step-by-Step Setup Guide

**Step 1: Build & Flash the Firmware**
* Open your ESP-IDF Command Prompt.
* Navigate to the transmitter folder: `cd cnc-wireless-transfer/transmitter`
* Set the target platform: `idf.py set-target esp32s3`
* Build the firmware (which also packages the HTML files into SPIFFS): `idf.py build`
* Flash to your board: `idf.py -p COM_PORT flash monitor` (Replace `COM_PORT` with your port).

**Step 2: Connect to the Transmitter Wi-Fi**
* Once powered, the ESP32-S3 broadcasts a Wi-Fi Access Point (SoftAP).
* On your PC or phone, connect to the Wi-Fi network: **`CNC_WIRELESS`**
* Enter the default password: **`cnc12345`**

**Step 3: Access the Web Interface**
* Open a Chrome, Firefox, or Edge browser.
* Navigate to exactly: `http://192.168.4.1/`
* You should see the secure login portal.
* Login using the default web credentials:
  * Username: **`admin`**
  * Password: **`admin123`**

**Step 4: Configure Settings (Optional)**
* Click the **Settings** button in the web UI.
* **To change Wi-Fi mode**: Under Wi-Fi Settings, select `Station (Client)` if you want the Transmitter to connect to your existing shop/house Wi-Fi router. Enter your router's SSID and Password. 
* **To change Security**: Under Auth Settings, change your Web Username / Password.
* Note: If you stay in AP mode, the Receiver unit will automatically connect to it and be ready immediately.

## Transfer Protocol

The transmitter acts as a proxy between the browser and the receiver:

1. **`POST /prepare`** → sends `{ filename, filesize, md5 }` to receiver
2. **`POST /chunk`** → streams 8KB raw binary chunks with `X-Chunk-Index` header
3. **`POST /finalize`** → sends `{ md5 }` for receiver-side verification

## Web UI Endpoints

| Endpoint       | Method | Auth | Description                       |
|----------------|--------|------|-----------------------------------|
| `/`            | GET    | No   | Redirects to `/ui/<crc>`          |
| `/ui/<crc>`    | GET    | No   | Web UI, one gzipped page (login + app), cached 1 year |
| `/login`       | POST   | No   | Authenticate, returns session token |
| `/upload`      | POST   | Yes  | File upload + transfer to receiver |
| `/status`      | GET    | Yes  | Get receiver state                |
| `/config`      | GET    | Yes  | Get current Wi-Fi configuration   |
| `/config`      | POST   | Yes  | Update Wi-Fi config + restart     |
