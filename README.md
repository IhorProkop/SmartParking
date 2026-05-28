# Smart Parking System on ESP32

An educational smart parking project based on ESP32 DevKit. The board controls
a barrier, validates RFID access cards, tracks occupancy of three parking
spots via ultrasonic sensors, and sends events to a local Python logger that
stores them in SQLite.

## Components

- ESP32 DevKit (Wi-Fi Access Point)
- OLED display SH1106 128x64 (I2C, U8g2 library)
- RFID reader RC522 (SPI)
- Servo motor for the barrier (manual PWM, no `ESP32Servo` library)
- Active buzzer
- 3x ultrasonic sensors HC-SR04

## Main features

- Wi-Fi Access Point `SmartParking_AP` with web interfaces:
  - `http://192.168.4.1/` - main dashboard (P1/P2/P3 cards, barrier status, latest RFID scan)
  - `http://192.168.4.1/debug` - diagnostic panel (raw/filtered/lastGood for sensors, free heap, last HTTP code)
  - `http://192.168.4.1/data` - JSON API for the frontend
- RFID card validation, barrier control, audible buzzer feedback
- Stable spot occupancy detection: median filter across 3 measurements,
  15/22 cm hysteresis, debounce confirmation across two consecutive
  measurements, protection against missing echo responses
- Non-blocking event logging: the physical reaction (OLED, buzzer, servo)
  happens instantly, while the HTTP POST to the Python logger is queued
  as a pending log and dispatched from `loop()` with retries every
  4 seconds when the logger is offline
- Local Python logger built on the standard library, SQLite database,
  dashboard with statistics, filters, UID search, CSV export, and the
  ability to clear the table

## Pin layout

| Component       | Signal | GPIO |
|-|-|-|
| OLED SH1106     | SDA    | 21   |
| OLED SH1106     | SCL    | 22   |
| RC522           | SS     | 5    |
| RC522           | RST    | 27   |
| RC522           | SCK    | 18   |
| RC522           | MOSI   | 23   |
| RC522           | MISO   | 19   |
| Buzzer          | OUT    | 25   |
| Servo (barrier) | PWM    | 13   |
| HC-SR04 #1      | TRIG   | 14   |
| HC-SR04 #1      | ECHO   | 34   |
| HC-SR04 #2      | TRIG   | 16   |
| HC-SR04 #2      | ECHO   | 35   |
| HC-SR04 #3      | TRIG   | 17   |
| HC-SR04 #3      | ECHO   | 36   |

**Important notes on power and voltage levels:**

- RC522 must be powered **only from 3.3 V**. Connecting 5 V to VCC will damage the module.
- The HC-SR04 echo output is 5 V, and ESP32 GPIOs are not 5 V tolerant.
  Always add a voltage divider on the ECHO lines (for example 1k + 2k)
  to bring the signal down to 3.3 V.
- GPIO 34, 35, 36 on ESP32 are input-only and are used for ECHO.

## Flashing the ESP32 (PlatformIO)

The project targets PlatformIO Core or PlatformIO IDE in VS Code.

```sh
# Build
pio run -e esp32dev

# Flash (replace COM3 with your port)
pio run -e esp32dev -t upload -upload-port COM3

# Serial Monitor
pio device monitor -p COM3 -b 115200
```

After flashing, the serial output will show:

```
Smart Parking system started
WiFi Access Point started
IP address: 192.168.4.1
Web server started
RFID OK
System ready
```

## Running the local logger

The logger is written using only the Python 3 standard library, with no
external dependencies.

```sh
cd local_logger
python server.py
```

You should see output similar to:

```
Smart Parking local logger
DB file : .../local_logger/smart_parking.db
Listen  : http://0.0.0.0:5000
Dashboard on this PC: http://localhost:5000
```

Before flashing, set the IP address of the laptop on the `SmartParking_AP`
network in the `LOG_SERVER_URL` constant in `src/main.cpp`:

```cpp
const char* LOG_SERVER_URL = "http://192.168.4.2:5000/api/log";
```

You can find the IP with `ipconfig` under "Wireless LAN adapter Wi-Fi"
after connecting the laptop to `SmartParking_AP`. It is usually `192.168.4.2`.

## Web interfaces

| URL                                  | Description                                          |
|-|-|
| `http://192.168.4.1/`                | main parking dashboard                               |
| `http://192.168.4.1/debug`           | technical panel with raw sensor values               |
| `http://192.168.4.1/data`            | JSON API for dynamic dashboard updates               |
| `http://localhost:5000/`             | local logger dashboard (statistics, filters)         |
| `http://localhost:5000/api/logs`     | JSON list of events                                  |
| `http://localhost:5000/api/stats`    | aggregated statistics                                |
| `http://localhost:5000/download.csv` | export the full table to CSV                         |
| `http://localhost:5000/api/log`      | accepts POST from ESP32                              |
| `http://localhost:5000/api/clear`    | POST to clear the table (also exposed on dashboard)  |
| `http://localhost:5000/api/health`   | logger health check                                  |

## SQLite logging

The local logger creates a SQLite database at `local_logger/smart_parking.db`
with the `access_logs` table:

```sql
CREATE TABLE access_logs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at      TEXT NOT NULL,
    uid             TEXT NOT NULL,
    access_status   TEXT NOT NULL,
    message         TEXT,
    free_spots      INTEGER,
    p1_status       TEXT, p1_distance INTEGER,
    p2_status       TEXT, p2_distance INTEGER,
    p3_status       TEXT, p3_distance INTEGER,
    raw_json        TEXT
);
```

Each RFID event on the ESP32 builds a snapshot of the parking state and is
POSTed to `/api/log`. If the logger is offline, the ESP32 keeps the record
in a pending queue and retries every 4 seconds - the RFID, buzzer, and servo
logic is not slowed down by this.

The `smart_parking.db` file is not committed to the repository (see `.gitignore`).

## Project structure

```
SmartParking/
- platformio.ini          PlatformIO configuration
- src/
  - main.cpp              ESP32 firmware
- local_logger/
  - server.py             Python logger with dashboard
  - smart_parking.db      local database (not tracked in Git)
- .gitignore
- README.md
```
