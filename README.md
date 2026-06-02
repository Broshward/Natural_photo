# Autonomous low-power timelapse camera on ESP32-S3 for natural videos

An automated, ultra-low-power greenhouse monitoring camera system built from scratch using pure C and **ESP-IDF v6.x**. The system wakes up periodically, captures photos, buffers them on an SD card in case of connectivity issues, and bulk-transfers the queue via TCP sockets to a remote server.

## Architectural Features
* **Dynamic Sleep Timing:** The microelectronic wake-up and transfer cycle auto-calculates execution overhead. If a bulk transmission takes 3 minutes, the deep sleep duration shrinks by 3 minutes to guarantee exact 10-minute snapshot intervals.
* **Non-Stop History Flushing:** When a network link is established, the MCU stream-transfers the backed-up photos without going to sleep. If a new scheduled snapshot time arrives mid-transfer, it pauses for a second, takes a fresh photo, commits it to the SD card, and resumes the transfer stream.
* **Crash-Proof Indexing:** The last successfully transmitted file index is persistently cached into a micro-text file on the SD card (`last_sent.txt`). This prevents queue duplicates or loss of progress even after panic reboots, watchdog triggers, or total power loss.
* **Low-Cost Buffer Power Architecture:** Designed to work with a standard 18650 Li-ion battery coupled with a cheap TP4056 charger and an MT3608 boost converter. Hardware components are split sequentially in code to prevent concurrent Wi-Fi modem and SD card peak currents from sagging the LDO regulator voltage.

## Hardware Specifications
* **MCU Board:** ESP32-S3 WROOM N16R8 (Freenove V1695 / Dual Type-C revision).
* **RAM:** 8MB Octal SPI PSRAM (Required for high-resolution DMA frame allocation).
* **Camera Sensor:** OmniVision **OV5640** (DVP 24-pin flat cable with manual focus ring).
* **Storage:** MicroSD card formatted in FAT32 (1-bit SDMMC bus wiring mode).
* **Status Indication:** On-board WS2812B RGB LED on GPIO48 (Hard-locked to ground during deep sleep to prevent battery drain).

## Repository Structure
```text
.
├── .gitignore               # Excludes large build objects and local sdkconfigs
├── CMakeLists.txt           # Top-level project CMake configuration
├── README.md                # Project documentation
└── main/
    ├── CMakeLists.txt       # Main component build manifest
    ├── idf_component.yml    # Auto-managed toolchain dependencies
    └── main.c               # Core firmware implementation in pure C
```

## How to Build and Flash

1. Clone this repository to your Linux build workstation:
   ```bash
   git clone --recursive https://github.com
   cd YOUR_REPO_NAME
   ```

2. Export the ESP-IDF toolchain environment path (v5.x branch required):
   ```bash
   . /opt/esp-idf/export.sh
   ```

3. Set the compilation target platform architecture to ESP32-S3:
   ```bash
   idf.py set-target esp32s3
   ```

4. Open the visual configuration menu to force **Octal Mode PSRAM** instantiation:
   ```bash
   idf.py menuconfig
   ```
   * Navigate to: `Component config` -> `ESP32S3-Specific`.
   * Enable `Support for external, SPI-connected RAM`.
   * Under `SPI RAM config`, verify that `Type of SmartMemory/PSRAM connected` is explicitly set to **`Octal Mode PSRAM`** and clock speed is clocked to **`80MHz`**.
   * Set allocation strategy to `Make RAM allocatable using heap_caps_malloc()`.
   * Save (`S`) and Exit (`Q`).

5. Compile the binary image, flash it via the USB-UART interface, and open the console monitor:
   ```bash
   idf.py build flash monitor
   ```
   *Note: The built-in ESP-IDF Component Manager will automatically parse `idf_component.yml` and fetch the certified version of `espressif/esp32-camera` directly into your local workspace on the first run.*

## Mobile Remote App (Flutter / Dart)

The repository contains a native Android mobile control unit located in the `greenhouse_control_flutter/` directory. It operates as a synchronous, blocking TCP single-thread server that handles dynamic storage indexing, battery telemetry rendering, secure hardware alerts logging, and direct over-the-air firmware binaries uploading.

### Mobile App Hardware Permissions
The app runs safely under Android 11-14 (Scoped Storage compliant) without requesting global disk monitoring tokens.
* **Firmware updates storage:** `/storage/emulated/0/Download/firmware.bin`
* **Greenhouse Photo Archive:** Isolated inside application sandboxed environment: `Android/data/com.example.greenhouse_control_flutter/files/greenhouse_archive/` (accessible via advanced file managers like Cx File Explorer).

### How to Build the Android APK

1. Ensure the Flutter SDK is installed and verified on your Linux development system:
   ```bash
   flutter doctor
   ```

2. Navigate to the mobile app component directory:
   ```bash
   cd greenhouse_control_flutter
   ```

3. Download the necessary high-speed image processing packages and providers:
   ```bash
   flutter pub get
   ```

4. Connect your Android smartphone via USB with "USB Debugging" toggled on, and boot a live debug session:
   ```bash
   flutter run
   ```

5. Compile a high-performance standalone release binary package (`.apk`) for deployment:
   ```bash
   flutter build apk --release
   ```
   *The compiled production package will be successfully generated at:* `build/app/outputs/flutter-apk/app-release.apk`

