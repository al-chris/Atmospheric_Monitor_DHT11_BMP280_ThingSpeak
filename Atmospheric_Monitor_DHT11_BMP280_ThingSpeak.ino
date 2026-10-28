/*
  =====================================================================
  IoT Atmospheric Monitor with Alerts
  DHT11 + BMP280 + ThingSpeak (write AND read) + Buzzer + LED
  Board: ESP32 (DOIT ESP32 DEVKIT V1 or similar)
  =====================================================================
  SENSORS / OUTPUTS:
    - DHT11 Temperature & Humidity ................ GPIO4  (Data)
    - BMP280 Pressure & Temperature (I2C) ......... SDA = GPIO21
                                                    SCL = GPIO22
    - Active buzzer ............................... GPIO25 (+ to pin, - to GND)
    - Alert LED (with 220 ohm resistor) ........... GPIO26 (anode via resistor, cathode to GND)

  WIRING NOTES:
    DHT11  : VCC -> 3.3V, GND -> GND, DATA -> GPIO4
             (bare 4-pin DHT11 needs a 10k pull-up between DATA and VCC;
              3-pin breakout modules already have it)
    BMP280 : VCC -> 3.3V, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
             I2C address is 0x76 or 0x77 depending on the module;
             this sketch tries both automatically.
    Buzzer : an ACTIVE buzzer (has its own oscillator) works directly
             with digitalWrite. A PASSIVE buzzer needs tone() instead.

  THINGSPEAK - TWO CHANNELS:
    1) DATA channel (ESP32 WRITES to it):
         Field 1 = DHT11 Temperature (C)
         Field 2 = DHT11 Humidity (%)
         Field 3 = BMP280 Temperature (C)
         Field 4 = BMP280 Pressure (hPa)
         Field 5 = Alert code (0 = OK, bitmask - see ALERT_* below)

    2) CONTROL channel (ESP32 READS from it, you change values remotely):
         Field 1 = High temperature threshold   (in the selected temp unit)
         Field 2 = High humidity threshold (%)
         Field 3 = Low pressure threshold       (in the selected pressure unit)
         Field 4 = Mute buzzer (1 = mute, 0 or empty = buzzer on)
         Field 5 = Temperature unit: 0 = C (default), 1 = F, 2 = K
         Field 6 = Pressure unit:    0 = hPa (default), 1 = kPa, 2 = inHg,
                                     3 = mmHg, 4 = psi
       Change values from any browser, e.g. Fahrenheit + inHg:
         https://api.thingspeak.com/update?api_key=CONTROL_WRITE_KEY&field1=95&field2=75&field3=29.5&field4=0&field5=1&field6=2
       Metric (C + hPa):
         https://api.thingspeak.com/update?api_key=CONTROL_WRITE_KEY&field1=35&field2=75&field3=1000&field4=0&field5=0&field6=0
       (always send ALL six fields in one request, because each update
        is a new entry and unsent fields are saved as empty; thresholds
        are always interpreted in the units chosen in the same entry)

  UNITS:
    Internally everything is stored in C and hPa. The selected units are
    applied to the serial output, the thresholds you enter, and (by
    default) the values uploaded to the data channel.

  CALIBRATION (Serial Monitor, 115200 baud, line ending = "Newline"):
    Each sensor has an offset that is added to its raw reading. Tell the
    sketch what the TRUE value is (from a trusted reference thermometer,
    hygrometer or barometer) and it works out and saves the offset.
    Offsets are stored in flash and survive resets and power loss.
      cal show                 show raw value, offset and calibrated value
      cal dtemp <value>        DHT11 temperature  (value in selected temp unit)
      cal dhum  <value>        DHT11 humidity     (value in %)
      cal btemp <value>        BMP280 temperature (value in selected temp unit)
      cal press <value>        BMP280 pressure    (value in selected pressure unit)
      cal <sensor> reset       clear one offset
      cal reset                clear all offsets
      cal help                 list commands
    NOTE: the BMP280 measures STATION pressure (at your actual altitude).
    Calibrate it against a station-pressure reference, not the sea-level
    pressure quoted in weather reports, unless you correct for altitude.

  LIBRARIES (Arduino IDE Library Manager):
    - "DHT sensor library" by Adafruit  (+ "Adafruit Unified Sensor")
    - "Adafruit BMP280 Library"         (+ "Adafruit BusIO")
    - "ThingSpeak" by MathWorks
  Board support: "esp32" by Espressif in Boards Manager.
  =====================================================================
*/

#include <Wire.h>
#include <WiFi.h>
#include <DHT.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>      // flash storage for calibration offsets (built into the ESP32 core)
#include <HTTPClient.h>       // POSTs readings to the FastAPI server (built into the ESP32 core)
#include <WiFiClientSecure.h>
#include "ThingSpeak.h"
#include "secrets.h"      // WiFi password and API keys (copy secrets.h.example to secrets.h)

// ---------------------------------------------------------------------
// USER CONFIGURATION - EDIT THESE VALUES
// ---------------------------------------------------------------------

// WiFi credentials, API keys and the server URL live in secrets.h (not shared)

// ---- DATA channel (write) ----
unsigned long DATA_CHANNEL_ID      = 3536464;     // <-- your data channel ID

// ---- CONTROL channel (read) ----
unsigned long CONTROL_CHANNEL_ID   = 3536467;     // <-- your control channel ID

// FastAPI server: readings are POSTed to API_BASE_URL (set in secrets.h) in addition to ThingSpeak

// ---- Pins ----
#define DHT_PIN      4
#define DHT_TYPE     DHT11
#define I2C_SDA      21
#define I2C_SCL      22
#define BUZZER_PIN   25
#define LED_PIN      26

// ---- Units ----
enum TempUnit  { TEMP_C = 0, TEMP_F = 1, TEMP_K = 2 };
enum PressUnit { PRESS_HPA = 0, PRESS_KPA = 1, PRESS_INHG = 2, PRESS_MMHG = 3, PRESS_PSI = 4 };

// Startup units, used until the first successful read of the control channel
TempUnit  g_tempUnit  = TEMP_C;
PressUnit g_pressUnit = PRESS_HPA;

// true  = ThingSpeak data fields are uploaded in the selected units
// false = ThingSpeak always receives C and hPa (keeps chart history consistent)
#define UPLOAD_USES_SELECTED_UNITS  true

// ---- Default thresholds (stored in C / hPa; the control channel overrides them) ----
float g_tempThreshold     = 35.0;    // C   - alert when DHT11 temp is above this
float g_humThreshold      = 80.0;    // %   - alert when humidity is above this
float g_pressLowThreshold = 1000.0;  // hPa - alert when pressure is below this
bool  g_muted             = false;   // buzzer mute (LED still works)

// ---- Rapid pressure-drop detection (storm warning) ----
#define TREND_WINDOW_MIN        60     // compare current pressure with N minutes ago
#define PRESSURE_DROP_HPA       2.0    // alert if pressure fell by this much in the window
// For quick bench testing, try TREND_WINDOW_MIN 2 and PRESSURE_DROP_HPA 0.3

// ---- Alert code bits (written to ThingSpeak field 5) ----
#define ALERT_HIGH_TEMP   1
#define ALERT_HIGH_HUM    2
#define ALERT_LOW_PRESS   4
#define ALERT_PRESS_DROP  8

// ---- Timing ----
#define SENSOR_READ_INTERVAL_MS    5000     // read sensors every 5 s
#define CLOUD_UPLOAD_INTERVAL_MS   20000    // ThingSpeak free tier min = 15 s
#define CONTROL_READ_INTERVAL_MS   30000    // check control channel every 30 s
#define TREND_SAMPLE_INTERVAL_MS   60000    // one pressure sample per minute
#define BEEP_INTERVAL_MS           250      // beep/blink speed during an alert

// ---------------------------------------------------------------------
// GLOBALS
// ---------------------------------------------------------------------
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_BMP280 bmp;              // I2C
WiFiClient client;

bool bmpOk = false;
Preferences prefs;

// Raw (uncalibrated) sensor readings
float r_dhtTemp  = NAN;           // C
float r_humidity = NAN;           // %
float r_bmpTemp  = NAN;           // C
float r_pressure = NAN;           // hPa

// Calibration offsets (added to the raw reading), loaded from flash
float off_dhtTemp  = 0;           // C
float off_humidity = 0;           // % RH
float off_bmpTemp  = 0;           // C
float off_pressure = 0;           // hPa

// Calibrated readings (these are used for alerts, uploads and display)
float g_dhtTemp  = NAN;           // C
float g_humidity = NAN;           // %
float g_bmpTemp  = NAN;           // C
float g_pressure = NAN;           // hPa
float g_pressDrop = 0;            // hPa fallen over the trend window (positive = falling)
int   g_alertCode = 0;

float pressHistory[TREND_WINDOW_MIN];
int   pressIdx   = 0;
int   pressCount = 0;

unsigned long lastSensorRead   = 0;
unsigned long lastCloudUpload  = 0;
unsigned long lastControlRead  = 0;
unsigned long lastTrendSample  = 0;

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  loadCalibration();

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  bmpOk = bmp.begin(0x76) || bmp.begin(0x77);
  if (bmpOk) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,    // temperature
                    Adafruit_BMP280::SAMPLING_X16,   // pressure
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("BMP280 found.");
  } else {
    Serial.println("BMP280 NOT found - check wiring/address (0x76 / 0x77).");
  }

  connectWiFi();
  ThingSpeak.begin(client);

  // Startup beep so you know the buzzer + LED work
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Take the first readings and fetch remote settings straight away
  readSensors();
  evaluateAlerts();
  readControlChannel();
  lastSensorRead  = millis();
  lastControlRead = millis();
  lastCloudUpload = millis();
}

// =====================================================================
void loop() {
  unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    readSensors();
    evaluateAlerts();
    printSerialDebug();
  }

  if (now - lastTrendSample >= TREND_SAMPLE_INTERVAL_MS) {
    lastTrendSample = now;
    updatePressureTrend();
  }

  if (now - lastControlRead >= CONTROL_READ_INTERVAL_MS) {
    lastControlRead = now;
    readControlChannel();
  }

  if (now - lastCloudUpload >= CLOUD_UPLOAD_INTERVAL_MS) {
    lastCloudUpload = now;
    uploadToThingSpeak();
    uploadToApi();
  }

  updateAlarmOutputs();   // non-blocking buzzer + LED pattern
  handleSerialCommands(); // calibration commands from the Serial Monitor

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
}

// =====================================================================
// WIFI
// =====================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection failed - will retry in loop().");
  }
}

// =====================================================================
// UNIT CONVERSION (internal values are always C and hPa)
// =====================================================================
float cToDisp(float c) {
  switch (g_tempUnit) {
    case TEMP_F: return c * 9.0 / 5.0 + 32.0;
    case TEMP_K: return c + 273.15;
    default:     return c;
  }
}

float dispToC(float v) {
  switch (g_tempUnit) {
    case TEMP_F: return (v - 32.0) * 5.0 / 9.0;
    case TEMP_K: return v - 273.15;
    default:     return v;
  }
}

// Pressure conversions are linear, so these also work for differences (drop)
float hpaToDisp(float hpa) {
  switch (g_pressUnit) {
    case PRESS_KPA:  return hpa * 0.1;
    case PRESS_INHG: return hpa * 0.0295300;
    case PRESS_MMHG: return hpa * 0.750062;
    case PRESS_PSI:  return hpa * 0.0145038;
    default:         return hpa;
  }
}

float dispToHpa(float v) {
  switch (g_pressUnit) {
    case PRESS_KPA:  return v * 10.0;
    case PRESS_INHG: return v / 0.0295300;
    case PRESS_MMHG: return v / 0.750062;
    case PRESS_PSI:  return v / 0.0145038;
    default:         return v;
  }
}

const char* tempUnitLabel() {
  switch (g_tempUnit) {
    case TEMP_F: return "F";
    case TEMP_K: return "K";
    default:     return "C";
  }
}

const char* pressUnitLabel() {
  switch (g_pressUnit) {
    case PRESS_KPA:  return "kPa";
    case PRESS_INHG: return "inHg";
    case PRESS_MMHG: return "mmHg";
    case PRESS_PSI:  return "psi";
    default:         return "hPa";
  }
}

// Sensible number of decimals for each pressure unit
int pressDecimals() {
  switch (g_pressUnit) {
    case PRESS_KPA:  return 2;
    case PRESS_INHG: return 2;
    case PRESS_PSI:  return 3;
    default:         return 1;   // hPa, mmHg
  }
}

// Value actually sent to ThingSpeak for the data fields
float uploadTemp(float c)    { return UPLOAD_USES_SELECTED_UNITS ? cToDisp(c)    : c;   }
float uploadPress(float hpa) { return UPLOAD_USES_SELECTED_UNITS ? hpaToDisp(hpa) : hpa; }

// =====================================================================
// SENSORS
// =====================================================================
void readSensors() {
  // DHT11: temperature (+/-2 C) and humidity (+/-5 %)
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) r_dhtTemp = t;
  if (!isnan(h)) r_humidity = h;

  // BMP280: temperature (+/-1 C) and pressure (+/-1 hPa)
  if (bmpOk) {
    float bt = bmp.readTemperature();
    float bp = bmp.readPressure() / 100.0;   // Pa -> hPa
    if (!isnan(bt)) r_bmpTemp = bt;
    if (!isnan(bp) && bp > 300 && bp < 1100) r_pressure = bp;
  }

  applyCalibration();
}

// =====================================================================
// CALIBRATION
// =====================================================================

// calibrated value = raw value + offset
void applyCalibration() {
  if (!isnan(r_dhtTemp))  g_dhtTemp  = r_dhtTemp + off_dhtTemp;
  if (!isnan(r_humidity)) g_humidity = constrain(r_humidity + off_humidity, 0.0, 100.0);
  if (!isnan(r_bmpTemp))  g_bmpTemp  = r_bmpTemp + off_bmpTemp;
  if (!isnan(r_pressure)) g_pressure = r_pressure + off_pressure;
}

void loadCalibration() {
  prefs.begin("calib", false);
  off_dhtTemp  = prefs.getFloat("dT", 0.0);
  off_humidity = prefs.getFloat("dH", 0.0);
  off_bmpTemp  = prefs.getFloat("bT", 0.0);
  off_pressure = prefs.getFloat("bP", 0.0);
  prefs.end();
}

void saveCalibration() {
  prefs.begin("calib", false);
  prefs.putFloat("dT", off_dhtTemp);
  prefs.putFloat("dH", off_humidity);
  prefs.putFloat("bT", off_bmpTemp);
  prefs.putFloat("bP", off_pressure);
  prefs.end();
}

// Old pressure samples were taken with the previous offset, so the
// trend history is no longer comparable and must start again.
void resetPressureTrend() {
  pressIdx    = 0;
  pressCount  = 0;
  g_pressDrop = 0;
}

void printCalibration() {
  Serial.println("--------------- Calibration ---------------");
  Serial.println("(raw and calibrated in selected units, offsets in C / % / hPa)");

  Serial.print("dtemp : raw "); Serial.print(cToDisp(r_dhtTemp), 2);
  Serial.print(" "); Serial.print(tempUnitLabel());
  Serial.print(" | offset "); Serial.print(off_dhtTemp, 2);
  Serial.print(" C | calibrated "); Serial.print(cToDisp(g_dhtTemp), 2);
  Serial.print(" "); Serial.println(tempUnitLabel());

  Serial.print("dhum  : raw "); Serial.print(r_humidity, 1);
  Serial.print(" % | offset "); Serial.print(off_humidity, 1);
  Serial.print(" % | calibrated "); Serial.print(g_humidity, 1);
  Serial.println(" %");

  Serial.print("btemp : raw "); Serial.print(cToDisp(r_bmpTemp), 2);
  Serial.print(" "); Serial.print(tempUnitLabel());
  Serial.print(" | offset "); Serial.print(off_bmpTemp, 2);
  Serial.print(" C | calibrated "); Serial.print(cToDisp(g_bmpTemp), 2);
  Serial.print(" "); Serial.println(tempUnitLabel());

  Serial.print("press : raw "); Serial.print(hpaToDisp(r_pressure), pressDecimals());
  Serial.print(" "); Serial.print(pressUnitLabel());
  Serial.print(" | offset "); Serial.print(off_pressure, 2);
  Serial.print(" hPa | calibrated "); Serial.print(hpaToDisp(g_pressure), pressDecimals());
  Serial.print(" "); Serial.println(pressUnitLabel());
  Serial.println("-------------------------------------------");
}

void printCalibrationHelp() {
  Serial.println("Calibration commands (enter the TRUE value from a reference instrument):");
  Serial.println("  cal show              show raw, offset and calibrated values");
  Serial.println("  cal dtemp <value>     DHT11 temperature  (selected temp unit)");
  Serial.println("  cal dhum  <value>     DHT11 humidity     (%)");
  Serial.println("  cal btemp <value>     BMP280 temperature (selected temp unit)");
  Serial.println("  cal press <value>     BMP280 pressure    (selected pressure unit)");
  Serial.println("  cal <sensor> reset    clear one offset");
  Serial.println("  cal reset             clear all offsets");
}

// Reads typed characters without blocking and runs a command per line.
void handleSerialCommands() {
  static String line = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length() > 0) processCommand(line);
      line = "";
    } else if (line.length() < 60) {
      line += c;
    }
  }
}

void processCommand(String cmd) {
  cmd.toLowerCase();

  // Split "cal <sensor> <argument>"
  int s1 = cmd.indexOf(' ');
  String verb = (s1 < 0) ? cmd : cmd.substring(0, s1);
  String rest = (s1 < 0) ? "" : cmd.substring(s1 + 1);
  rest.trim();

  if (verb != "cal") {
    Serial.println("Unknown command. Type: cal help");
    return;
  }
  if (rest.length() == 0 || rest == "help") { printCalibrationHelp(); return; }
  if (rest == "show")                       { printCalibration();     return; }

  if (rest == "reset") {
    off_dhtTemp = off_humidity = off_bmpTemp = off_pressure = 0;
    saveCalibration();
    resetPressureTrend();
    applyCalibration();
    evaluateAlerts();
    Serial.println("All calibration offsets cleared.");
    return;
  }

  int s2 = rest.indexOf(' ');
  String sensor = (s2 < 0) ? rest : rest.substring(0, s2);
  String arg    = (s2 < 0) ? "" : rest.substring(s2 + 1);
  arg.trim();

  // Pick the sensor being calibrated
  float* offset;
  float  raw;
  float  maxOffset;          // sanity limit, catches typos and wrong units
  const char* label;
  const char* offsetUnit;
  int kind;                  // 0 = temperature, 1 = humidity, 2 = pressure

  if (sensor == "dtemp") {
    offset = &off_dhtTemp;  raw = r_dhtTemp;  maxOffset = 10.0; label = "DHT11 temperature";  offsetUnit = "C";   kind = 0;
  } else if (sensor == "dhum") {
    offset = &off_humidity; raw = r_humidity; maxOffset = 30.0; label = "DHT11 humidity";     offsetUnit = "%";   kind = 1;
  } else if (sensor == "btemp") {
    offset = &off_bmpTemp;  raw = r_bmpTemp;  maxOffset = 10.0; label = "BMP280 temperature"; offsetUnit = "C";   kind = 0;
  } else if (sensor == "press") {
    offset = &off_pressure; raw = r_pressure; maxOffset = 30.0; label = "BMP280 pressure";    offsetUnit = "hPa"; kind = 2;
  } else {
    Serial.println("Unknown sensor. Use: dtemp, dhum, btemp or press (type: cal help)");
    return;
  }

  // "cal <sensor> reset"
  if (arg == "reset") {
    *offset = 0;
    saveCalibration();
    if (kind == 2) resetPressureTrend();
    applyCalibration();
    evaluateAlerts();
    Serial.print(label); Serial.println(" offset cleared.");
    return;
  }

  // "cal <sensor> <reference value>"
  char* endPtr = NULL;
  double ref = strtod(arg.c_str(), &endPtr);
  if (arg.length() == 0 || endPtr == arg.c_str() || *endPtr != '\0') {
    Serial.println("Please give a number, e.g.  cal dtemp 24.5");
    return;
  }
  if (isnan(raw)) {
    Serial.print("No valid "); Serial.print(label); Serial.println(" reading yet - try again in a few seconds.");
    return;
  }

  // Convert the reference from the selected units to C / % / hPa
  float refMetric;
  if (kind == 0)      refMetric = dispToC((float)ref);
  else if (kind == 2) refMetric = dispToHpa((float)ref);
  else                refMetric = (float)ref;

  float newOffset = refMetric - raw;
  if (fabs(newOffset) > maxOffset) {
    Serial.print("Rejected: that would need an offset of ");
    Serial.print(newOffset, 2); Serial.print(" "); Serial.print(offsetUnit);
    Serial.print(" (limit +/-"); Serial.print(maxOffset, 0);
    Serial.println("). Check the reference value and the selected units.");
    return;
  }

  *offset = newOffset;
  saveCalibration();
  if (kind == 2) resetPressureTrend();
  applyCalibration();
  evaluateAlerts();

  Serial.print(label); Serial.print(" calibrated. Offset = ");
  Serial.print(newOffset, 2); Serial.print(" "); Serial.print(offsetUnit);
  Serial.println(" (saved to flash).");
  printCalibration();
}

// Stores one pressure sample per minute and works out how much it has
// fallen over the last TREND_WINDOW_MIN minutes.
void updatePressureTrend() {
  if (isnan(g_pressure)) return;

  if (pressCount == TREND_WINDOW_MIN) {
    // pressHistory[pressIdx] is the oldest sample (about N minutes ago)
    g_pressDrop = pressHistory[pressIdx] - g_pressure;
  }
  pressHistory[pressIdx] = g_pressure;
  pressIdx = (pressIdx + 1) % TREND_WINDOW_MIN;
  if (pressCount < TREND_WINDOW_MIN) pressCount++;
}

// =====================================================================
// ALERT LOGIC
// =====================================================================
void evaluateAlerts() {
  g_alertCode = 0;

  if (!isnan(g_dhtTemp)  && g_dhtTemp  > g_tempThreshold)     g_alertCode |= ALERT_HIGH_TEMP;
  if (!isnan(g_humidity) && g_humidity > g_humThreshold)      g_alertCode |= ALERT_HIGH_HUM;
  if (!isnan(g_pressure) && g_pressure < g_pressLowThreshold) g_alertCode |= ALERT_LOW_PRESS;
  if (pressCount == TREND_WINDOW_MIN && g_pressDrop >= PRESSURE_DROP_HPA) g_alertCode |= ALERT_PRESS_DROP;
}

String alertText() {
  if (g_alertCode == 0) return "OK";
  String s = "";
  if (g_alertCode & ALERT_HIGH_TEMP)  s += "HighTemp ";
  if (g_alertCode & ALERT_HIGH_HUM)   s += "HighHum ";
  if (g_alertCode & ALERT_LOW_PRESS)  s += "LowPress ";
  if (g_alertCode & ALERT_PRESS_DROP) s += "PressDrop ";
  s.trim();
  return s;
}

// Non-blocking: LED and buzzer pulse together while any alert is active.
// The LED always blinks; the buzzer stays silent if muted remotely.
void updateAlarmOutputs() {
  static unsigned long lastToggle = 0;
  static bool on = false;

  if (g_alertCode == 0) {
    on = false;
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  if (millis() - lastToggle >= BEEP_INTERVAL_MS) {
    lastToggle = millis();
    on = !on;
  }
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, (on && !g_muted) ? HIGH : LOW);
}

// =====================================================================
// THINGSPEAK - READ (remote settings from the control channel)
// =====================================================================
void readControlChannel() {
  if (WiFi.status() != WL_CONNECTED) return;

  const char* key = (strlen(CONTROL_READ_API_KEY) > 0) ? CONTROL_READ_API_KEY : NULL;

  // Units first, so the thresholds below are interpreted in the right units
  float tUnitRaw = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 5, key);
  if (ThingSpeak.getLastReadStatus() != 200) {
    Serial.print("Control channel read failed. Status: ");
    Serial.println(ThingSpeak.getLastReadStatus());
    return;                                   // keep the current settings
  }
  float pUnitRaw = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 6, key);

  // Empty fields read as 0, which maps to the metric defaults (C / hPa)
  int tu = (int)(tUnitRaw + 0.5);
  int pu = (int)(pUnitRaw + 0.5);
  if (tu >= TEMP_C  && tu <= TEMP_K)      g_tempUnit  = (TempUnit)tu;
  if (pu >= PRESS_HPA && pu <= PRESS_PSI) g_pressUnit = (PressUnit)pu;

  float tHigh = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 1, key);
  float hHigh = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 2, key);
  float pLow  = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 3, key);
  float mute  = ThingSpeak.readFloatField(CONTROL_CHANNEL_ID, 4, key);

  // Empty fields come back as 0, so skip those, then convert to C / hPa
  // and only accept sensible values.
  if (tHigh > 0) {
    float c = dispToC(tHigh);
    if (c > 0 && c < 80) g_tempThreshold = c;
  }
  if (hHigh > 0 && hHigh <= 100) g_humThreshold = hHigh;
  if (pLow > 0) {
    float hpa = dispToHpa(pLow);
    if (hpa > 800 && hpa < 1100) g_pressLowThreshold = hpa;
  }
  g_muted = (mute >= 0.5);

  Serial.println("Control settings updated from ThingSpeak:");
  Serial.print("  Units: "); Serial.print(tempUnitLabel());
  Serial.print(" / ");       Serial.println(pressUnitLabel());
  Serial.print("  Temp > ");  Serial.print(cToDisp(g_tempThreshold), 1);
  Serial.print(" ");          Serial.println(tempUnitLabel());
  Serial.print("  Hum  > ");  Serial.print(g_humThreshold, 0);  Serial.println(" %");
  Serial.print("  Press < "); Serial.print(hpaToDisp(g_pressLowThreshold), pressDecimals());
  Serial.print(" ");          Serial.println(pressUnitLabel());
  Serial.print("  Muted: ");  Serial.println(g_muted ? "YES" : "no");
}

// =====================================================================
// THINGSPEAK - WRITE (sensor data to the data channel)
// =====================================================================
void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping upload - WiFi not connected.");
    return;
  }

  // Only set fields that have a valid reading, so a failed sensor
  // doesn't push NaN/garbage into your charts.
  if (!isnan(g_dhtTemp))  ThingSpeak.setField(1, uploadTemp(g_dhtTemp));
  if (!isnan(g_humidity)) ThingSpeak.setField(2, g_humidity);
  if (!isnan(g_bmpTemp))  ThingSpeak.setField(3, uploadTemp(g_bmpTemp));
  if (!isnan(g_pressure)) ThingSpeak.setField(4, uploadPress(g_pressure));
  ThingSpeak.setField(5, g_alertCode);

  // Status shows the alert plus the units the fields were sent in
  String status = alertText() + " | ";
  status += UPLOAD_USES_SELECTED_UNITS ? tempUnitLabel() : "C";
  status += ", ";
  status += UPLOAD_USES_SELECTED_UNITS ? pressUnitLabel() : "hPa";
  ThingSpeak.setStatus(status);

  int httpCode = ThingSpeak.writeFields(DATA_CHANNEL_ID, DATA_WRITE_API_KEY);

  if (httpCode == 200) {
    Serial.println("ThingSpeak update successful.");
  } else {
    Serial.print("ThingSpeak update failed. HTTP error code: ");
    Serial.println(httpCode);
  }
}

// =====================================================================
// FASTAPI SERVER - WRITE (same readings, sent as JSON; server adds the timestamp)
// =====================================================================
void uploadToApi() {
  if (strlen(API_BASE_URL) == 0 || WiFi.status() != WL_CONNECTED) return;
  // The API stores one temperature, pressure and humidity per reading, so
  // wait until all three sensors have produced a valid value.
  if (isnan(g_dhtTemp) || isnan(g_humidity) || isnan(g_pressure)) return;

  // Always C / hPa / %, whatever units are selected for ThingSpeak
  String body = "{\"temperature_c\":" + String(g_dhtTemp, 2) +
                ",\"pressure_hpa\":" + String(g_pressure, 2) +
                ",\"humidity_pct\":" + String(g_humidity, 2) + "}";

  String url = String(API_BASE_URL) + "/api/readings";
  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;         // separate from the ThingSpeak client
  bool begun;
  if (url.startsWith("https://")) {
    secure.setInsecure();   // skips certificate validation; load the server's CA cert for production
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) {
    Serial.println("API upload failed - bad URL.");
    return;
  }
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) http.addHeader("X-API-Key", API_KEY);

  int code = http.POST(body);
  if (code == 201) {
    Serial.println("API update successful.");
  } else {
    Serial.print("API update failed. HTTP code: ");
    Serial.println(code);
  }
  http.end();
}

// =====================================================================
// SERIAL DEBUG
// =====================================================================
void printSerialDebug() {
  Serial.println("---------------------------------------");
  Serial.print("DHT11 Temp    : "); Serial.print(cToDisp(g_dhtTemp), 1);
  Serial.print(" "); Serial.println(tempUnitLabel());
  Serial.print("DHT11 Humidity: "); Serial.print(g_humidity, 0);  Serial.println(" %");
  Serial.print("BMP280 Temp   : "); Serial.print(cToDisp(g_bmpTemp), 1);
  Serial.print(" "); Serial.println(tempUnitLabel());
  Serial.print("BMP280 Press  : "); Serial.print(hpaToDisp(g_pressure), pressDecimals());
  Serial.print(" "); Serial.println(pressUnitLabel());
  if (pressCount == TREND_WINDOW_MIN) {
    Serial.print("Pressure drop : "); Serial.print(hpaToDisp(g_pressDrop), pressDecimals());
    Serial.print(" "); Serial.print(pressUnitLabel());
    Serial.print(" over "); Serial.print(TREND_WINDOW_MIN); Serial.println(" min");
  } else {
    Serial.print("Pressure trend: collecting ("); Serial.print(pressCount);
    Serial.print("/"); Serial.print(TREND_WINDOW_MIN); Serial.println(" min)");
  }
  Serial.print("Alert         : "); Serial.println(alertText());
}
