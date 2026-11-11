# Smart Patient Bed-Exit Alarm Node

An IoT-enabled bed occupancy and exit monitoring alarm system designed for ESP32 microcontrollers. This project integrates an HX711 weight sensor interface with non-blocking load-cell data acquisition, local physical notification controls, and native integration with the **ESP32 UDP Parameter Server Framework (`ParamServer v1.1.0`)**.

The node operates within a multi-supervisor environment (such as ESP32 smart clocks, mobile applications, or centralized monitoring systems), maintaining isolated status flags and delivering targeted audio/text alerts across the network without race conditions.

---

## Key Features

* **Non-Blocking Scale Engine:** Utilizes asynchronous data checks (`scale.dataWaitingAsync()` / `scale.updateAsync()`) to eliminate loop blocking and ensure network updates remain responsive.
* **Distributed Multi-Supervisor State Tracking:** Tracks separate read/unread alarm flags for each supervisor node (IP address) on the network.
* **Persistent Baseline Configuration:** Automatically captures, restores, and manages hardware raw tare offsets and calibration parameters via `LittleFS` storage.
* **Over-The-Air (OTA) Updates:** Includes built-in `ArduinoOTA` support for wireless firmware deployments.
* **Automatic Network Self-Healing:** Monitors Wi-Fi connectivity and handles background re-association without interrupting physical sampling loops.

---

## Hardware Requirements

* **Microcontroller:** ESP32 (e.g., ESP32-WROOM-32 / ESP32-DevKitC)
* **Load Cell ADC Module:** HX711 module paired with bed-frame or bed-post load cells
* **Local Indicator:** 5V/3.3V Active Buzzer or Relay module (connected to `GPIO 2`)

### Pin Configuration

| Component Pin | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **HX711 DOUT** | `GPIO 4` | Load Cell Data Output |
| **HX711 SCK** | `GPIO 5` | Load Cell Serial Clock |
| **Buzzer / Relay** | `GPIO 2` | Local Physical Alarm Output |

---

## Software Dependencies & PlatformIO Setup

This project is configured for compilation using **VSCode + PlatformIO**.

### `platformio.ini` Configuration

Ensure your `platformio.ini` includes the following dependencies:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps =
    olakal/HX711_ADC @ ^1.2.12
    https://github.com/jim-mckeown/ESP32-UDP-Parameter-Server-Class.git
```

---

## System Architecture & State Machine

```
                        +----------------------------+
                        |  HX711 Load Cell Sensors   |
                        +--------------+-------------+
                                       |
                                       v
                      +--------------------------------+
                      |     ESP32 Bed-Scale Node       |
                      |  - Non-blocking scale sampling |
                      |  - State tracking per IP       |
                      +-------+----------------+-------+
                              |                |
             Critical Alerts  |                |  Soft Alerts
             ("msg dd01")     |                |  ("alert ...")
                              v                v
                  +--------------------------------+
                  |    Supervisor Network Nodes    |
                  |  - Smart Clocks                |
                  |  - Linux / Python Servers      |
                  |  - Mobile Applications         |
                  +--------------------------------+
```

1. **Patient In Bed:** Weight meets or exceeds `min_weight`. Status flag updates to `1` (Occupied) and dispatches a plain-text soft notification (`alert Patient in bed`) to all registered supervisors.
2. **Patient Bed-Exit (Critical Event):** Weight drops below `min_weight`. Dispatches a critical audio trigger (`msg dd01`) to all supervisors on port `8888`, locks all supervisor status flags to `2` (Critical), and activates the local physical alarm pin (`GPIO 2`) for the configured `alarm_duration`.

---

## UDP Command Dictionary (Port 8888)

The node listens for incoming UDP packets via `ParamServer`. 

### Dynamic Parameter Control
| Command | Example | Description |
| :--- | :--- | :--- |
| `<param> ?` | `cal ?` | Returns the current value of a parameter. |
| `<param> <value>` | `min_weight 1.50` | Updates a parameter and commits to LittleFS if persistent. |

#### Registered Parameters

* `device_number`: Unit ID (`00`–`99`). Default is `0` (Unconfigured).
* `cal`: HX711 Calibration factor (float).
* `min_weight`: Minimum occupancy threshold weight in kg (float).
* `alarm_dur`: Local alarm pin pulse duration in milliseconds (int).
* `tare_offset`: Saved raw hardware zero-reference offset (int).
* `current_weight`: Live filtered weight dataset (Read-Only).

### Network & Polling Management

| Command | Response | Description |
| :--- | :--- | :--- |
| `status ?` | `OK status <0|1|2>` | Returns the specific unread status flag for the requesting IP and clears it back to `0`. |
| `status 0` | `OK status 0` | Forces explicit clearing of the status flag for the requesting IP. |
| `add_poll <ID> <IP>` | `OK add_poll <IP>` | Registers or updates a supervisor node IP in the matrix and saves it to LittleFS. |
| `list_poll` | `OK list_poll <IP1,IP2,...>` | Returns a CSV-formatted list of all active supervisor IPs tracked by the node. |

---

## First-Time Configuration Sequence

1. **Power On:** Upon boot, if `tare_offset` is `0`, the scale automatically zeros itself assuming an empty bed, saving the baseline raw offset to LittleFS.
2. **Set Device Number:** Configure the unit's unique device ID over UDP:
   ```bash
   echo -n "device_number 1" | nc -u -w1 <ESP32_IP> 8888
   ```
3. **Calibrate Weight:** Place a known weight on the scale and adjust the calibration factor over UDP using `cal <value>` until `current_weight ?` reads accurately.