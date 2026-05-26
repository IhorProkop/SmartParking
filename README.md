# Smart Parking System on ESP32

Навчальний проект розумної автостоянки на базі ESP32 DevKit. Плата керує
шлагбаумом, перевіряє RFID-картки доступу, відстежує зайнятість трьох
паркомісць за допомогою ультразвукових датчиків і надсилає події на
локальний Python-логер, який зберігає їх у SQLite.

## Компоненти

- ESP32 DevKit (Wi-Fi Access Point)
- OLED дисплей SH1106 128×64 (I2C, бібліотека U8g2)
- RFID-зчитувач RC522 (SPI)
- Сервопривід шлагбаума (керування через ручний PWM, без бібліотеки `ESP32Servo`)
- Активний buzzer
- 3× ультразвукові датчики HC-SR04

## Основні функції

- Wi-Fi Access Point `SmartParking_AP` з web-інтерфейсами:
  - `http://192.168.4.1/` — головний dashboard (картки P1/P2/P3, статус шлагбаума, останній RFID-скан)
  - `http://192.168.4.1/debug` — діагностична панель (raw/filtered/lastGood для сенсорів, free heap, last HTTP code)
  - `http://192.168.4.1/data` — JSON API для frontend
- Перевірка RFID-картки, керування шлагбаумом, звуковий feedback на buzzer
- Стабільне визначення зайнятості місць: median-фільтр з 3 вимірювань,
  гістерезис 15/22 см, debounce-підтвердження двома підряд вимірюваннями,
  захист від відсутніх echo-відповідей
- Неблокуюче логування подій: фізична реакція системи (OLED, buzzer, серво)
  відбувається миттєво, а HTTP-POST до Python-логера ставиться в pending-чергу
  і відправляється з `loop()` з повторами кожні 4 секунди при недоступному логері
- Локальний Python-логер на стандартних бібліотеках, SQLite-база, dashboard
  зі статистикою, фільтрами, пошуком по UID, CSV-експортом і очищенням таблиці

## Схема пінів

| Компонент       | Сигнал | GPIO |
|-----------------|--------|------|
| OLED SH1106     | SDA    | 21   |
| OLED SH1106     | SCL    | 22   |
| RC522           | SS     | 5    |
| RC522           | RST    | 27   |
| RC522           | SCK    | 18   |
| RC522           | MOSI   | 23   |
| RC522           | MISO   | 19   |
| Buzzer          | OUT    | 25   |
| Servo (шлагбаум)| PWM    | 13   |
| HC-SR04 #1      | TRIG   | 14   |
| HC-SR04 #1      | ECHO   | 34   |
| HC-SR04 #2      | TRIG   | 16   |
| HC-SR04 #2      | ECHO   | 35   |
| HC-SR04 #3      | TRIG   | 17   |
| HC-SR04 #3      | ECHO   | 36   |

**Важливо щодо живлення і рівнів:**

- RC522 живиться **тільки від 3.3 В**. Підключення 5 В на VCC виведе модуль з ладу.
- Echo-вихід HC-SR04 видає 5 В, а GPIO ESP32 не толерантні до 5 В.
  На лініях ECHO обов'язково ставити дільник напруги (наприклад 1 кΩ + 2 кΩ),
  щоб привести сигнал до 3.3 В.
- GPIO 34, 35, 36 на ESP32 — input-only, тому використовуються саме для ECHO.

## Як прошити ESP32 (PlatformIO)

Проект розрахований на PlatformIO Core або PlatformIO IDE у VS Code.

```sh
# Збірка
pio run -e esp32dev

# Прошивка (заміни COM-порт на свій)
pio run -e esp32dev -t upload --upload-port COM3

# Serial Monitor
pio device monitor -p COM3 -b 115200
```

Після прошивки в Serial з'явиться:

```
Smart Parking system started
WiFi Access Point started
IP address: 192.168.4.1
Web server started
RFID OK
System ready
```

## Як запустити локальний логер

Логер написаний на стандартних бібліотеках Python 3, без зовнішніх залежностей.

```sh
cd local_logger
python server.py
```

Має з'явитися вивід:

```
Smart Parking local logger
DB file : .../local_logger/smart_parking.db
Listen  : http://0.0.0.0:5000
Dashboard on this PC: http://localhost:5000
```

Перед прошивкою в `src/main.cpp` потрібно вказати IP-адресу комп'ютера в
мережі `SmartParking_AP` у константі `LOG_SERVER_URL`:

```cpp
const char* LOG_SERVER_URL = "http://192.168.4.2:5000/api/log";
```

IP можна дізнатися командою `ipconfig` у блоці "Wireless LAN adapter Wi-Fi"
після підключення ноутбука до `SmartParking_AP`. Зазвичай це `192.168.4.2`.

## Web-інтерфейси

| URL                              | Опис                                            |
|----------------------------------|-------------------------------------------------|
| `http://192.168.4.1/`            | головний dashboard паркінгу                     |
| `http://192.168.4.1/debug`       | технічна панель з raw-значеннями сенсорів       |
| `http://192.168.4.1/data`        | JSON API для динамічного оновлення dashboard    |
| `http://localhost:5000/`         | dashboard локального логера (статистика, фільтри)|
| `http://localhost:5000/api/logs` | список подій у JSON                             |
| `http://localhost:5000/api/stats`| агрегована статистика                           |
| `http://localhost:5000/download.csv` | експорт усієї таблиці у CSV                 |
| `http://localhost:5000/api/log`  | приймає POST з ESP32                            |
| `http://localhost:5000/api/clear`| POST для очищення таблиці (видно на dashboard)  |
| `http://localhost:5000/api/health`| перевірка стану логера                         |

## SQLite logging

Локальний логер створює SQLite-базу `local_logger/smart_parking.db` з
таблицею `access_logs`:

```sql
CREATE TABLE access_logs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at      TEXT NOT NULL,
    uid             TEXT NOT NULL,
    access_status   TEXT NOT NULL,    -- GRANTED / DENIED
    message         TEXT,
    free_spots      INTEGER,
    p1_status       TEXT, p1_distance INTEGER,
    p2_status       TEXT, p2_distance INTEGER,
    p3_status       TEXT, p3_distance INTEGER,
    raw_json        TEXT
);
```

Кожна RFID-подія на ESP32 формує snapshot стану паркінгу і відправляється
POST'ом на `/api/log`. Якщо логер недоступний, ESP32 тримає запис у
pending-черзі і повторює спробу кожні 4 секунди — RFID-, buzzer- і
servo-логіка від цього не сповільнюється.

Файл `smart_parking.db` не комітиться в репозиторій (див. `.gitignore`).

## Структура проекту

```
SmartParking/
├── platformio.ini          конфігурація PlatformIO
├── src/
│   └── main.cpp            прошивка ESP32
├── local_logger/
│   ├── server.py           Python-логер з dashboard
│   └── smart_parking.db    локальна база (не у Git)
├── .gitignore
└── README.md
```
