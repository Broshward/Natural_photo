# Autonomous Low-Power 5MP Timelapse Camera on ESP32-S3

An automated, ultra-low-power greenhouse and wildlife monitoring camera system built from scratch using pure C and **ESP-IDF v6.x**. The system wakes up periodically, captures native 5-megapixel JPEG frames, filters out dark/night shots via on-the-fly prescaled decoding, buffers valid photos on a MicroSD card, and syncs the archive via TCP sockets to a companion Flutter mobile/desktop app.

## Architectural & Software Features

* **Native 5MP JPEG Pipeline:** Captures high-resolution `2560x1920` hardware-compressed JPEG frames directly into 8MB Octal PSRAM, eliminating heavy raw YUV422 transfers and external color conversion overhead.
* **Smart Night/Darkness Filtering (`esp_jpeg`):** Uses prescaled software JPEG decoding (`1/8` scale into internal RAM) to calculate true average pixel luminance (`Y` channel), preventing high-gain sensor noise in the dark from wasting SD storage and battery.
* **Timestamped File Naming & Auto-Sync:** Files are stored using the `YYYYMMDD_HHMMSS_INDEX.jpg` pattern. When time is synchronized via the mobile app, the firmware automatically renames the freshly captured pre-sync frame and updates file modification timestamps (`mtime`) on the smartphone for accurate gallery sorting.
* **Persistent NVS Configuration:** Snapshot intervals (in seconds, from `5s` up to `7 days`) are stored in the ESP32-S3 Non-Volatile Storage (NVS), surviving deep sleep, resets, and battery swaps.
* **Hardware Mode Jumper (`GPIO 42` + `GPIO 2`):** Supports instant hardware switching between standalone wildlife mode (button-triggered wakeup) and continuous greenhouse mode (automatic capture + Wi-Fi sync on every wakeup) without reflashing.
* **Dynamic Sleep Timing & Safe SD Handling:** Auto-calculates execution and transfer overhead to maintain exact snapshot intervals. The SD card is mounted strictly for I/O and immediately unmounted before deep sleep to prevent FAT32 corruption.
* **Offline-Ready Build Architecture:** Core dependencies (`esp32-camera` and `esp_jpeg`) are vendored locally in `components/`, allowing instant compilation without external registry access (`IDF_COMPONENT_MANAGER=0`).

## Operating Modes

| Mode | Trigger Condition | Behavior |
| :--- | :--- | :--- |
| **Mode 0 (Wildlife Timer)** | Timer wakeup (No jumper) | Captures photo, checks brightness, saves to SD, enters Deep Sleep. Wi-Fi stays off to maximize battery life. |
| **Mode 1 (Live Preview)** | Short press `GPIO 0` (No jumper) | Captures a live frame and streams it directly to the app (`live_preview.jpg`) without saving to SD. Used for manual lens focusing. |
| **Mode 2 (Archive Sync)** | Long press `GPIO 0` (No jumper) | Skips capture, connects to Wi-Fi, and bulk-transfers all missing photos from SD to the smartphone. |
| **Mode 3 (Greenhouse)** | Hardware jumper on `GPIO 42`–`GPIO 2` | Captures photo, saves to SD, connects to Wi-Fi, syncs new frames, executes queued remote commands, and sleeps. |

## Hardware Specifications

* **MCU Board:** ESP32-S3 WROOM N16R8 (Freenove V1695 / Dual Type-C revision).
* **RAM:** 8MB Octal SPI PSRAM (80MHz).
* **Camera Sensor:** OmniVision **OV5640** (24-pin DVP interface with manual focus lens).
* **Storage:** MicroSD card formatted in FAT32 (1-bit SDMMC bus: `CLK=39`, `CMD=38`, `D0=40`).
* **Mode Jumper:** Standard 2.54mm jumper across `GPIO 42` (driven low during check) and `GPIO 2` (internal pull-up), completely floating during deep sleep to prevent current leakage.
* **Power Architecture:** Single 18650 Li-ion cell with TP4056 charger and MT3608 boost converter, with sequential peripheral initialization to prevent LDO brownouts.

## Repository Structure

```text
.
├── .gitignore                   # Excludes build artifacts, caches, and unused platforms
├── CMakeLists.txt               # Top-level ESP-IDF project configuration
├── README.md                    # Project documentation
├── components/                  # Vendored local components for offline builds
│   ├── esp32-camera/            # OV5640 DVP camera driver
│   └── esp_jpeg/                # Software JPEG decoder for brightness analysis
├── main/
│   ├── CMakeLists.txt           # Main component manifest
│   └── main.c                   # Core firmware implementation in pure C
└── greenhouse_control_flutter/  # Cross-platform Flutter control app (Android & Linux)
    ├── lib/main.dart            # TCP socket server, binary protocol parser & UI
    ├── android/                 # Android Gradle configs and permissions
    └── linux/                   # Linux desktop build files
```

## How to Build and Flash Firmware

1. Clone the repository and enter the project directory:
   ```bash
   git clone https://github.com
   cd Natural_photo
   ```

2. Export the ESP-IDF toolchain environment (v6.x) and disable online component checks:
   ```bash
   . /opt/esp-idf/export.sh
   export IDF_COMPONENT_MANAGER=0
   ```

3. Configure target and partition table (required for OTA support):
   ```bash
   idf.py set-target esp32s3
   idf.py menuconfig
   ```
   * **Octal PSRAM:** `Component config` -> `ESP32S3-Specific` -> `Support for external, SPI-connected RAM` -> Set Type to **`Octal Mode PSRAM`** (`80MHz`).
   * **OTA Partitions:** `Partition Table` -> `Partition Table Layout` -> Select **`Factory app, two OTA definitions`**.
   * Save (`S`) and Exit (`Q`).

4. Erase flash (first time only), build, and flash:
   ```bash
   idf.py erase-flash
   idf.py -p /dev/ttyACM0 build flash monitor
   ```

## Mobile & Desktop Control App (Flutter)

Located in `greenhouse_control_flutter/`, the app acts as a TCP server receiving telemetry and photos whenever the ESP32-S3 wakes up. It features a built-in **Command Terminal** with delayed queue execution (commands are queued while the board sleeps and executed sequentially by priority upon connection).

### Supported Terminal Commands
* `SET_TIME` — Synchronizes the ESP32-S3 system clock with the smartphone's Unix timestamp and renames the current session's photo.
* `INT:<seconds>` — Sets a new wakeup/snapshot interval (`5` to `604800` seconds) and commits it to NVS flash.
* `FORMAT_SD` — Formats the MicroSD card on the board and resets the file index counter.
* `OTA` — Uploads a new firmware binary located at `/storage/emulated/0/Download/firmware.bin` over Wi-Fi and triggers a safe reboot.

### Building Lightweight Android APK (~17 MB)
1. Navigate to the Flutter directory and fetch dependencies:
   ```bash
   cd greenhouse_control_flutter
   flutter pub get
   ```
2. Build a split-ABI release APK with icon tree-shaking and install via ADB:
   ```bash
   flutter build apk --release --split-per-abi --tree-shake-icons
   adb install -r build/app/outputs/flutter-apk/app-arm64-v8a-release.apk
   ```
*Downloaded photos are saved to:* `Android/data/com.example.greenhouse_control_flutter/files/greenhouse_archive/`
