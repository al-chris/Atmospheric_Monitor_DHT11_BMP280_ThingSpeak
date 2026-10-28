# IoT Atmospheric Monitor with Alerts

An ESP32 weather and atmosphere monitor. It reads temperature and humidity from a **DHT11** and temperature and barometric pressure from a **BMP280**, uploads everything to **ThingSpeak**, and sounds a **buzzer** and flashes an **LED** when conditions cross your limits. Thresholds, buzzer mute, and display units are all changed remotely through a second ThingSpeak channel, so there's no need to reflash the board.

> Sketch: `Atmospheric_Monitor_DHT11_BMP280_ThingSpeak.ino`

## Features

- DHT11 temperature and humidity, plus BMP280 temperature and pressure, so you can compare the two temperature sensors
- Writes sensor data and an alert code to a ThingSpeak **data channel**
- Reads thresholds, mute, and unit settings from a ThingSpeak **control channel**
- Four alert conditions: high temperature, high humidity, low pressure, and a rapid pressure drop (storm warning)
- Non-blocking buzzer and LED alerts, so sensor reads and uploads are never delayed by a beep
- Switchable units: °C / °F / K and hPa / kPa / inHg / mmHg / psi
- Remote buzzer mute (the LED keeps blinking)
- Per-sensor calibration from the Serial Monitor, with offsets saved to flash
- Automatic WiFi reconnect, and bad sensor reads are never uploaded

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | DOIT ESP32 DevKit V1 or similar |
| DHT11 | Bare 4-pin versions need a 10 kΩ pull-up between DATA and VCC. 3-pin breakout modules already include it. |
| BMP280 module | I2C version. Address 0x76 or 0x77 is detected automatically. |
| Active buzzer | Beeps when its pin is set HIGH. A passive buzzer needs `tone()` instead. |
| LED + 220 Ω resistor | Any colour |
| Breadboard + jumper wires | |

## Wiring

| Component | Pin on component | ESP32 |
|---|---|---|
| DHT11 | VCC | 3.3V |
| | GND | GND |
| | DATA | GPIO4 |
| BMP280 | VCC | 3.3V |
| | GND | GND |
| | SDA | GPIO21 |
| | SCL | GPIO22 |
| Buzzer | + | GPIO25 |
| | − | GND |
| LED | Anode (via 220 Ω) | GPIO26 |
| | Cathode | GND |

Pin numbers can be changed in the `#define` block at the top of the sketch.

## Software setup

### 1. Arduino IDE

1. Install the **esp32 by Espressif** board package (Boards Manager) and select your ESP32 board.
2. Install these libraries (Library Manager):
   - **DHT sensor library** by Adafruit (and **Adafruit Unified Sensor**)
   - **Adafruit BMP280 Library** (and **Adafruit BusIO**)
   - **ThingSpeak** by MathWorks

### 2. ThingSpeak channels

Create two channels at [thingspeak.com](https://thingspeak.com).

**Data channel** (the ESP32 writes to it)

| Field | Contents |
|---|---|
| 1 | DHT11 temperature |
| 2 | DHT11 humidity (%) |
| 3 | BMP280 temperature |
| 4 | BMP280 pressure |
| 5 | Alert code (see below) |

**Control channel** (the ESP32 reads from it)

| Field | Contents |
|---|---|
| 1 | High temperature threshold (in the selected temperature unit) |
| 2 | High humidity threshold (%) |
| 3 | Low pressure threshold (in the selected pressure unit) |
| 4 | Mute buzzer: `1` = mute, `0` or empty = buzzer on |
| 5 | Temperature unit: `0` = °C, `1` = °F, `2` = K |
| 6 | Pressure unit: `0` = hPa, `1` = kPa, `2` = inHg, `3` = mmHg, `4` = psi |

Keep your **Channel ID** and the **Write API Key** for the data channel. For the control channel you need the **Channel ID**, a **Write API Key** (for changing settings from a browser), and a **Read API Key** if the channel is private. A public control channel needs no read key.

### 3. Configure the sketch

Fill in these values near the top of the `.ino` file:

```cpp
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

unsigned long DATA_CHANNEL_ID      = 0;
const char*   DATA_WRITE_API_KEY   = "";

unsigned long CONTROL_CHANNEL_ID   = 0;
const char*   CONTROL_READ_API_KEY = "";   // leave "" if the channel is public
```

Then select your board and port, and upload.

## Using it

Open the Serial Monitor at **115200 baud**. You'll see the readings every 5 seconds, plus a line each time data is uploaded or settings are fetched.

### Changing settings remotely

Open a URL like this in any browser, with your control channel's Write API Key:

```
https://api.thingspeak.com/update?api_key=CONTROL_WRITE_KEY&field1=35&field2=75&field3=1000&field4=0&field5=0&field6=0
```

That means: alert above 35 °C, above 75 % humidity, or below 1000 hPa, buzzer on, metric units. The ESP32 picks up the change within 30 seconds.

Fahrenheit and inHg version:

```
https://api.thingspeak.com/update?api_key=CONTROL_WRITE_KEY&field1=95&field2=75&field3=29.5&field4=0&field5=1&field6=2
```

**Always send all six fields.** Each update creates a new entry, and any field you leave out is saved as empty. An empty unit field is read as `0` (°C / hPa), and empty thresholds are ignored.

Thresholds are always interpreted in the units selected in the same entry. Changing the units without updating the thresholds will reinterpret the old numbers.

To mute the buzzer, send `field4=1` along with the other five fields.

## Alerts

The LED and buzzer pulse together (250 ms on/off) while any alert is active, and stop when everything is back in range.

| Alert | Triggered when | Bit |
|---|---|---|
| High temperature | DHT11 temperature is above the threshold (default 35 °C) | 1 |
| High humidity | Humidity is above the threshold (default 80 %) | 2 |
| Low pressure | Pressure is below the threshold (default 1000 hPa) | 4 |
| Rapid pressure drop | Pressure has fallen by 2 hPa or more over 60 minutes | 8 |

Field 5 on the data channel holds the sum of the active bits. For example, `5` means high temperature plus low pressure, and `0` means all clear. The ThingSpeak status text also lists the active alerts and the units in use, for example `HighTemp LowPress | F, inHg`.

The pressure-drop alert needs a full window of data first. It takes samples once a minute, so it only becomes active 60 minutes after boot. The Serial Monitor shows the progress while it collects. For bench testing, set `TREND_WINDOW_MIN` to `2` and `PRESSURE_DROP_HPA` to `0.3`, then change them back.

## Units

Everything is stored internally in °C and hPa. The selected units affect:

- The Serial Monitor output
- The thresholds you enter
- The values uploaded to the data channel (when `UPLOAD_USES_SELECTED_UNITS` is `true`)

Changing units mid-stream mixes your ThingSpeak chart history, because old data isn't converted. If you want consistent charts, set `UPLOAD_USES_SELECTED_UNITS` to `false` in the sketch. ThingSpeak then always receives °C and hPa, and only the serial output and your thresholds follow the selected units.

The `g_tempUnit` and `g_pressUnit` defaults in the sketch are only used until the first successful read of the control channel. After that, the control channel decides.

## Calibration

Each of the four readings (DHT11 temperature, DHT11 humidity, BMP280 temperature, BMP280 pressure) has its own offset, added to the raw reading. You don't type the offset in. You tell the sketch what the **true** value is, from a trusted reference instrument, and it works out the offset (reference − raw reading). Offsets are saved to the ESP32's flash, so they survive resets and power loss.

Calibrated values are what the alerts, the ThingSpeak uploads, and the serial output use.

### Commands

Open the Serial Monitor at **115200 baud** and set the line ending to **Newline**.

| Command | What it does |
|---|---|
| `cal show` | Shows the raw value, offset, and calibrated value for every sensor |
| `cal dtemp <value>` | Calibrates DHT11 temperature (value in the selected temperature unit) |
| `cal dhum <value>` | Calibrates DHT11 humidity (value in %) |
| `cal btemp <value>` | Calibrates BMP280 temperature (value in the selected temperature unit) |
| `cal press <value>` | Calibrates BMP280 pressure (value in the selected pressure unit) |
| `cal <sensor> reset` | Clears one offset, e.g. `cal press reset` |
| `cal reset` | Clears all offsets |
| `cal help` | Lists the commands |

Example: your reference thermometer reads 24.0 °C while the DHT11 reads 25.5 °C.

```
cal dtemp 24.0
```

The sketch saves an offset of −1.50 °C, and the DHT11 now reports 24.0 °C.

### How to calibrate well

1. Let the board run for 10–15 minutes first, so the sensors settle and warm up.
2. Put the reference instrument right next to the sensor in still air, away from the ESP32 and direct sun.
3. Wait until both readings are stable, then enter the reference value.
4. Check the result with `cal show`.

### Things to know

- **Offset only.** Each sensor gets one fixed correction. That works well for a constant error. It can't fix an error that changes across the range, such as a sensor that reads 2 °C high at 20 °C but 4 °C high at 40 °C. Calibrate near the conditions you care about most.
- **Station pressure.** The BMP280 measures the pressure at your actual altitude (station pressure), not the sea-level pressure quoted in weather reports. Pressure drops by about 12 hPa for every 100 m of height. Calibrate against a station-pressure reference, or convert the sea-level value to your altitude first. Otherwise you'll get a large, wrong offset.
- **Low-pressure threshold.** At higher altitudes, the default low-pressure threshold of 1000 hPa may sit above your normal readings, so the alert would stay on all the time. Set field 3 on the control channel to a value that suits your location.
- **Units.** The reference value is read in your currently selected units, so `cal dtemp 75.2` means 75.2 °F if Fahrenheit is selected. Offsets are always stored and shown in °C, %, and hPa.
- **Safety limits.** Offsets beyond ±10 °C, ±30 % humidity, or ±30 hPa are rejected. This usually means a typo or the wrong units, and the sketch tells you so.
- **Pressure trend restarts.** Changing the pressure offset clears the pressure-trend history, because old samples were taken with the previous offset. The rapid-drop alert is inactive until a full window of data has been collected again.
- **Humidity.** The DHT11 is a low-accuracy sensor (±5 %), so calibrate humidity against a good reference, and expect modest improvement.
- **Charts.** ThingSpeak history isn't corrected. Data uploaded before you calibrated keeps its old values.

## Timing

| Task | Interval |
|---|---|
| Read sensors | 5 s |
| Upload to ThingSpeak | 20 s (free tier minimum is 15 s) |
| Read control channel | 30 s |
| Pressure trend sample | 60 s |

These are the `*_INTERVAL_MS` constants at the top of the sketch.

## Troubleshooting

| Problem | What to check |
|---|---|
| `BMP280 NOT found` | Check SDA/SCL wiring and 3.3 V power. Make sure the module is an I2C one. |
| DHT11 readings stay `nan` | Check the data pin and pull-up resistor, and wait a few seconds after boot. |
| Upload error `404` | The channel ID or write API key is wrong. |
| Upload error `-301` | The ESP32 couldn't reach ThingSpeak. Check WiFi and your internet connection. |
| Upload error `-401` | The point wasn't inserted. Usually you're uploading faster than once every 15 seconds. |
| "Control channel read failed" | Check the control channel ID, and the read key if the channel is private. |
| Settings never change | Make sure you send all six fields, and that thresholds are within sensible ranges (temperature 0–80 °C, humidity 1–100 %, pressure 800–1100 hPa). |
| Calibration commands do nothing | Set the Serial Monitor to 115200 baud with the **Newline** line ending. |
| "Rejected: that would need an offset of..." | The reference differs from the raw reading by more than the limit. Check the reference value and the selected units. |
| "No valid ... reading yet" | The sensor hasn't produced a good reading yet. Wait a few seconds and try again. |
| Low-pressure alert is always on | Your normal station pressure is below the threshold (common at higher altitude). Lower field 3 on the control channel. |
| Buzzer is silent but the LED blinks | The buzzer is muted (control field 4), or you have a passive buzzer and need `tone()`. |
| Buzzer clicks but has no tone | That's a passive buzzer. Switch to an active one, or replace the buzzer `digitalWrite` with `tone()`. |

## Sensor accuracy

| Sensor | Temperature | Other |
|---|---|---|
| DHT11 | ±2 °C | Humidity ±5 % |
| BMP280 | ±1 °C | Pressure ±1 hPa |

The BMP280 temperature may read slightly higher than the DHT11 if it sits near the ESP32, which gives off some heat. Keep the sensors away from the board for more accurate readings.

## Project structure

```
Atmospheric_Monitor_DHT11_BMP280_ThingSpeak/
├── Atmospheric_Monitor_DHT11_BMP280_ThingSpeak.ino
└── README.md
```

## Credits

[al-chris](https://github.com/al-chris)
