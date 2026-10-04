/*
  PhysicalGauge - drive a 12V automotive oil temperature gauge from an ESP32.

  The gauge expects a thermistor sender between S and ground (high resistance = cold,
  low resistance = hot). We fake the sender with a PWM-switched logic-level MOSFET:
  more duty -> lower average resistance -> needle reads hotter.

  Gauge terminals
    I   -> +12V (ignition / supply)
    GND -> ground (common with ESP32)
    S   -> sender input (goes to the MOSFET circuit below)

  Wiring
    12V supply + ----------------------------- gauge I
    12V supply + -- buck converter 12V->5V --- ESP32 5V/VIN
    12V supply - ------+---------------------- gauge GND
                       +---------------------- ESP32 GND
                       +---------------------- MOSFET Source

    gauge S --[22 ohm, 1W]-- MOSFET Drain      (IRLZ44N / AO3400, must be LOGIC-LEVEL)
    ESP32 GPIO PWM_PIN --[220 ohm]-- MOSFET Gate
    MOSFET Gate --[10k]-- GND                  (keeps gauge "cold" while ESP32 boots)
    gauge S --[10-47uF, >=25V]-- GND           (smooths PWM; put it on the S side of
                                                the 22 ohm so the resistor limits the
                                                discharge current, NOT across the FET)

    NEVER connect gauge S directly to an ESP32 pin - it can sit near 12V.

  Serial Monitor (115200, newline line ending) commands
    180     -> show 180 degrees (interpolated from cal[])
    d512    -> set raw PWM duty (0..1023) - use this to calibrate
    s       -> sweep full range (fast)
    w       -> slow calibration sweep, prints duty; Enter stops (w500 = 500 ms/step)
    p       -> toggle pressure monitor (prints pin mV, sensor V, psi twice a second)
    z       -> pressure zero calibration (sensor open to air)
    k30     -> pressure span calibration at a known 30 psi
    r       -> clear saved pressure calibration (back to code defaults)
    t       -> print temperature probes (IDs, readings, keg / air assignment)
    wifi    -> print Wi-Fi status
    wififorget -> erase saved home Wi-Fi and reboot into setup mode
    ?       -> print current state

  Wi-Fi
    First boot (or home Wi-Fi unreachable for 20 s): the ESP32 starts its own
    network "Kegerator-Setup". Join it from a phone, browse to
    http://192.168.4.1/wifi and pick your home network. Credentials are stored
    in the ESP32's flash, not in this file.
    Once connected: http://kegerator.local (or the IP printed at boot).
    The gauge never waits on Wi-Fi - it keeps working if the router is down.

  OTA (over-the-air) updates
    Easiest: double-click ota-upload.bat (project folder). It compiles, then
    posts the .bin to http://<ip>/update - only an outgoing web request from
    the PC, so no firewall or network discovery involved.
    Or by hand: browse to http://kegerator.local/update (user OTA_USER,
    password OTA_PASS) and choose build\PhysicalGauge.ino.bin.
    Arduino IDE network ports (ArduinoOTA) also stay enabled, but need the
    IDE's discovery and an inbound connection to the PC to work.
    Partition Scheme must include OTA (the default "Default 4MB with spiffs"
    does; "Huge APP (No OTA)" does not).

  Calibrating
    Use d### to find the duty that puts the needle on each dial marking,
    then enter those pairs in cal[] below (keep them sorted by value).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPUpdateServer.h>
#include <Preferences.h>
#include <OneWire.h>             // library: OneWire (Paul Stoffregen)
#include <DallasTemperature.h>   // library: DallasTemperature (Miles Burton)
#include "web_page.h"

// ---------- Configuration ----------
// Wi-Fi: joins your home network (set up from a phone, see header). If it can't,
// it opens the setup network below. Setup password must be at least 8 characters.
const bool  WIFI_ENABLED = true;
const char* HOSTNAME     = "kegerator";        // http://kegerator.local + OTA port name
const char* SETUP_SSID   = "Kegerator-Setup";
const char* SETUP_PASS   = "kegpressure";      // change this if you like
const char* OTA_PASS     = "kegerator-ota";    // OTA + /update page password; change it
const char* OTA_USER     = "admin";            // user name for the /update page
const unsigned long WIFI_TIMEOUT_MS = 20000;   // no home Wi-Fi this long -> setup network

const int PWM_PIN  = 18;    // change to suit your board
const int PWM_FREQ = 5000;   // Hz; try 1k-20k if the needle buzzes or jitters
const int PWM_BITS = 10;     // duty range 0..1023
const int DUTY_MAX = (1 << PWM_BITS) - 1;

// Needle slew: max duty change per update, so moves look like a real gauge
const int  SLEW_STEP       = 8;
const unsigned long SLEW_MS = 10;

// Slow calibration sweep ("w" command): duty step and default time per step
const int SLOW_STEP    = 4;
const int SLOW_STEP_MS = 250;   // ~64 s for the full range

// Calibration helper: on boot, count down then run the slow sweep automatically
// (instead of the fast sweep). Set false when you're done calibrating.
const bool AUTO_SLOW_SWEEP     = false;
const int  START_DELAY_SECONDS = 3;

// Set to true to drive the needle from the sensor (psi -> dial mapping still TODO)
const bool USE_SENSOR = false;
const unsigned long SENSOR_MS = 500;

// Pressure sensor (see readSensor()). Datasheet values until calibrated.
const int   PRESSURE_PIN     = 34;
const float DIVIDER          = 1.566;         // measured: 0.465V green / 0.297V P34 (nominal 28/18 = 1.556)
const float SENSOR_V_ZERO    = 0.4695;        // sensor volts at 0 psi (open air, USB power)
const float SENSOR_V_PER_PSI = 0.07071;       // compressor at 30 psi, corrected for zero (datasheet 0.0667)
const int   ADC_SAMPLES      = 64;            // averaged per reading to cut noise
const unsigned long PRINT_MS = 500;           // "p" monitor print interval
// DS18B20 temperature probes: all on one 1-Wire bus, 4.7k pull-up P4 -> 3V3.
// Avoid GPIO12 (P12): pulled up at boot it selects the wrong flash voltage and the ESP32 will not boot.
// Which probe is "keg" and which is "air" is chosen on /config (saved by probe ID).
const int ONEWIRE_PIN          = 4;         // P4 (GPIO4)
const int MAX_PROBES           = 4;
const unsigned long TEMP_MS    = 2000;        // read all probes this often
const unsigned long CONVERT_MS = 800;         // 12-bit conversion takes up to 750 ms
const unsigned long RESCAN_MS  = 30000;       // look for missing probes this often

const unsigned long SMOOTH_MS = 100;          // pressure sampled this often for display/gauge
const float SMOOTH_ALPHA      = 0.2;          // smoothing: ~0.5 s to settle after a change
const int steveTest = 42;

// Calibration: {gauge value (deg F), PWM duty}. Dial reads 130 (min) to 300 (max).
// Measured with the "w" slow sweep + d### fine-tuning. 130 = needle at rest
// (duty 0, not measured). Non-linear: the top of the scale needs much more duty
// per degree. Above d896 the needle is pinned at 300.
struct CalPoint { float value; int duty; };
CalPoint cal[] = {
  { 130,   0 },
  { 140,  96 },
  { 160, 132 },
  { 180, 184 },
  { 200, 232 },
  { 210, 300 },
  { 220, 348 },
  { 240, 420 },
  { 260, 568 },
  { 300, 896 },
};
const int CAL_N = sizeof(cal) / sizeof(cal[0]);

// ---------- State ----------
int targetDuty  = 0;
int currentDuty = 0;
unsigned long lastSlew   = 0;
unsigned long lastSensor = 0;
unsigned long lastPrint  = 0;
bool  monitorPressure = false;
float sensorVZero     = SENSOR_V_ZERO;      // runtime copies, changed by z / k###
float sensorVPerPsi   = SENSOR_V_PER_PSI;   // (loaded from flash at boot if saved)
float smoothV         = NAN;                // smoothed sensor volts (display + gauge)
unsigned long lastSmooth = 0;
float kegTempF        = NAN;                // assigned probe temps, NAN = none / offline
float airTempF        = NAN;

OneWire           oneWire(ONEWIRE_PIN);
DallasTemperature probes(&oneWire);
DeviceAddress probeAddr[MAX_PROBES];        // probes found on the bus
float         probeF[MAX_PROBES];           // their latest readings (NAN = bad read)
int           probeCount = 0;
DeviceAddress kegAddr, airAddr;             // assignments, loaded from flash
bool          kegSet = false, airSet = false;
bool          convertPending = false;
unsigned long lastTempMs = 0, convertStartMs = 0, lastScanMs = 0;

WebServer   server(80);
HTTPUpdateServer httpUpdater;      // browser/curl firmware upload at /update
Preferences prefs;

String wifiSsid, wifiPass;          // home network, loaded from flash
bool   setupApOn     = false;
bool   otaStarted    = false;
bool   wifiConnected = false;
unsigned long wifiLostMs = 0;       // when we last started waiting for a connection

// ---------- PWM ----------
void pwmBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(0, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PWM_PIN, 0);
#endif
}

void pwmWrite(int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PWM_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ---------- Calibration lookup ----------
int dutyFor(float v) {
  if (v <= cal[0].value) return cal[0].duty;
  for (int i = 1; i < CAL_N; i++) {
    if (v <= cal[i].value) {
      float t = (v - cal[i - 1].value) / (cal[i].value - cal[i - 1].value);
      return cal[i - 1].duty + t * (cal[i].duty - cal[i - 1].duty);
    }
  }
  return cal[CAL_N - 1].duty;
}

void showValue(float v) {
  targetDuty = constrain(dutyFor(v), 0, DUTY_MAX);
  Serial.printf("value=%.1f -> duty=%d\n", v, targetDuty);
}

void setRawDuty(int d) {
  targetDuty  = constrain(d, 0, DUTY_MAX);
  currentDuty = targetDuty;   // raw mode skips slew so calibration is immediate
  pwmWrite(currentDuty);
  Serial.printf("duty=%d\n", currentDuty);
}

// ---------- Pressure sensor ----------
// 0-60 psi, 5V, 0.5-4.5V output. Green wire --[10k]-- P34 --[18k]-- GND,
// so 4.5V at the sensor is 2.89V at the pin. GPIO34 is ADC1 (works with WiFi on).
//
// SENSOR_V_ZERO and SENSOR_V_PER_PSI are the defaults. Calibrate (serial or web page):
//   z     -> with the sensor open to air (0 psi), records the zero voltage
//   k30   -> with a known 30 psi applied, records the span
// Both are saved to flash and override the defaults at boot ("r" clears them).
// They also print a line you can paste in here as a backup.
float readSensorVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) mv += analogReadMilliVolts(PRESSURE_PIN);
  return (mv / (float)ADC_SAMPLES) * DIVIDER / 1000.0;
}

// Returns psi, or NAN if the voltage is outside what a working sensor can output
// (unplugged, broken wire, shorted).
float voltsToPsi(float v) {
  if (v < 0.2 || v > 4.9) return NAN;
  return (v - sensorVZero) / sensorVPerPsi;
}

float readSensor() {
  return voltsToPsi(readSensorVolts());
}

void printPressure() {
  float v   = readSensorVolts();
  float psi = voltsToPsi(v);          // same sample as v, so the line is consistent
  Serial.printf("pin=%.0fmV  sensor=%.3fV  psi=%.2f\n",
                v * 1000.0 / DIVIDER, v, psi);
}

// ---------- Pressure calibration (saved in flash) ----------
void loadCal() {
  prefs.begin("gauge", true);
  sensorVZero   = prefs.getFloat("vzero", SENSOR_V_ZERO);
  sensorVPerPsi = prefs.getFloat("vpp",   SENSOR_V_PER_PSI);
  prefs.end();
  Serial.printf("pressure cal: zero=%.4f V  span=%.5f V/psi\n", sensorVZero, sensorVPerPsi);
}

void saveCal() {
  prefs.begin("gauge", false);
  prefs.putFloat("vzero", sensorVZero);
  prefs.putFloat("vpp",   sensorVPerPsi);
  prefs.end();
}

void resetCal() {
  prefs.begin("gauge", false);
  prefs.clear();
  prefs.end();
  sensorVZero   = SENSOR_V_ZERO;
  sensorVPerPsi = SENSOR_V_PER_PSI;
  Serial.println("pressure cal cleared, using code defaults");
}

// Both return a message for serial / the web page.
String calZero() {
  float v = readSensorVolts();
  // Sensor spec is 0.5V at 0 psi; anything far off means a wiring fault, not a zero.
  if (v < 0.3 || v > 0.7) {
    char err[96];
    snprintf(err, sizeof(err), "zero rejected: %.3fV is not near 0.5V - check wiring", v);
    return err;
  }
  sensorVZero = v;
  saveCal();
  char msg[96];
  snprintf(msg, sizeof(msg), "zero saved: const float SENSOR_V_ZERO = %.4f;", sensorVZero);
  return msg;
}

String calSpan(float knownPsi) {
  if (knownPsi <= 0) return "usage: k<psi>, e.g. k30";
  float rise = readSensorVolts() - sensorVZero;
  if (rise < 0.05) return "no pressure detected - span not changed";
  // Datasheet span is 0.0667 V/psi; allow half to double that for loading/tolerance.
  float vpp = rise / knownPsi;
  if (vpp < 0.033 || vpp > 0.133) {
    char err[96];
    snprintf(err, sizeof(err), "span rejected: %.5f V/psi is implausible - check wiring/psi", vpp);
    return err;
  }
  sensorVPerPsi = vpp;
  saveCal();
  char msg[96];
  snprintf(msg, sizeof(msg), "span saved: const float SENSOR_V_PER_PSI = %.5f;", sensorVPerPsi);
  return msg;
}

// Number as JSON, or null when there is no valid value.
String jnum(float f, int decimals) {
  return isnan(f) ? String("null") : String(f, decimals);
}

// ---------- Temperature probes (DS18B20) ----------
String addrToStr(const DeviceAddress a) {
  char buf[17];
  for (int i = 0; i < 8; i++) sprintf(buf + i * 2, "%02X", a[i]);
  return String(buf);
}

bool strToAddr(const String& s, DeviceAddress a) {
  if (s.length() != 16) return false;
  for (int i = 0; i < 8; i++) a[i] = strtoul(s.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  return true;
}

bool sameAddr(const DeviceAddress a, const DeviceAddress b) {
  return memcmp(a, b, 8) == 0;
}

const char* roleOf(const DeviceAddress a) {
  if (kegSet && sameAddr(a, kegAddr)) return "keg";
  if (airSet && sameAddr(a, airAddr)) return "air";
  return "none";
}

void loadProbeRoles() {
  prefs.begin("temps", true);
  kegSet = prefs.getBytes("keg", kegAddr, 8) == 8;
  airSet = prefs.getBytes("air", airAddr, 8) == 8;
  prefs.end();
}

// Find every DS18B20 on the bus (family code 0x28, valid CRC).
void scanProbes() {
  DeviceAddress a;
  probeCount = 0;
  oneWire.reset_search();
  while (probeCount < MAX_PROBES && oneWire.search(a)) {
    if (a[0] != 0x28 || OneWire::crc8(a, 7) != a[7]) continue;
    bool dup = false;
    for (int i = 0; i < probeCount; i++) dup |= sameAddr(probeAddr[i], a);
    if (dup) continue;
    memcpy(probeAddr[probeCount], a, 8);
    probeF[probeCount] = NAN;
    probeCount++;
  }
  // Only after the search: setResolution talks on the bus, and without the 'true'
  // (skip global recalculation) it runs its own search and derails ours.
  for (int i = 0; i < probeCount; i++) probes.setResolution(probeAddr[i], 12, true);
  lastScanMs = millis();
  Serial.printf("temperature probes found: %d\n", probeCount);
  for (int i = 0; i < probeCount; i++)
    Serial.printf("  %s  %s\n", addrToStr(probeAddr[i]).c_str(), roleOf(probeAddr[i]));
}

void setupProbes() {
  probes.begin();
  probes.setWaitForConversion(false);   // never block: start a conversion, read it later
  loadProbeRoles();
  scanProbes();
}

// Latest reading for an assigned role, or NAN if unassigned / not found / bad read.
float tempForRole(bool set, const DeviceAddress role) {
  if (!set) return NAN;
  for (int i = 0; i < probeCount; i++)
    if (sameAddr(probeAddr[i], role)) return probeF[i];
  return NAN;
}

// Called every loop: start a conversion, then read it CONVERT_MS later. Never blocks.
void tempLoop() {
  unsigned long now = millis();
  if (!convertPending) {
    if (now - lastTempMs < TEMP_MS) return;
    bool missing = probeCount < 2 || isnan(kegTempF) || isnan(airTempF);
    if (missing && now - lastScanMs > RESCAN_MS) scanProbes();
    if (probeCount == 0) { lastTempMs = now; return; }
    probes.requestTemperatures();
    convertPending = true;
    convertStartMs = now;
    return;
  }
  if (now - convertStartMs < CONVERT_MS) return;
  for (int i = 0; i < probeCount; i++) {
    float c = probes.getTempC(probeAddr[i]);
    // -127 = no answer / CRC error, 85.0 = power-on value (no conversion happened)
    probeF[i] = (c == DEVICE_DISCONNECTED_C || c == 85.0) ? NAN : c * 9.0 / 5.0 + 32.0;
  }
  kegTempF = tempForRole(kegSet, kegAddr);
  airTempF = tempForRole(airSet, airAddr);
  convertPending = false;
  lastTempMs = now;
}

// role: "keg", "air" or "none". Assigning a probe to one role removes it from the other.
String assignProbe(const String& id, const String& role) {
  DeviceAddress a;
  if (!strToAddr(id, a)) return "bad probe id";
  if (role != "keg" && role != "air" && role != "none") return "bad role";
  prefs.begin("temps", false);
  if (kegSet && sameAddr(kegAddr, a)) { prefs.remove("keg"); kegSet = false; }
  if (airSet && sameAddr(airAddr, a)) { prefs.remove("air"); airSet = false; }
  if (role == "keg")      { memcpy(kegAddr, a, 8); prefs.putBytes("keg", a, 8); kegSet = true; }
  else if (role == "air") { memcpy(airAddr, a, 8); prefs.putBytes("air", a, 8); airSet = true; }
  prefs.end();
  kegTempF = tempForRole(kegSet, kegAddr);
  airTempF = tempForRole(airSet, airAddr);
  return "probe " + id + (role == "none" ? String(" unassigned") : " set as " + role);
}

void printTemps() {
  Serial.printf("keg=%s F  air=%s F\n", jnum(kegTempF, 1).c_str(), jnum(airTempF, 1).c_str());
  for (int i = 0; i < probeCount; i++)
    Serial.printf("  %s  %s F  %s\n", addrToStr(probeAddr[i]).c_str(),
                  jnum(probeF[i], 1).c_str(), roleOf(probeAddr[i]));
}

// ---------- Wi-Fi credentials (saved in flash) ----------
void loadWifiCreds() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
}

void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void forgetWifi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  Serial.println("home Wi-Fi forgotten, rebooting into setup mode");
  delay(500);
  ESP.restart();
}

// ---------- Setup network (fallback access point) ----------
void startSetupAp() {
  if (setupApOn) return;
  WiFi.mode(WIFI_AP_STA);           // keep trying home Wi-Fi in the background
  WiFi.softAP(SETUP_SSID, SETUP_PASS);
  setupApOn = true;
  Serial.printf("setup network on: join \"%s\", browse to http://%s/wifi\n",
                SETUP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopSetupAp() {
  if (!setupApOn) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  setupApOn = false;
  Serial.println("setup network off");
}

// ---------- OTA updates ----------
void startOta() {
  if (otaStarted) return;
  ArduinoOTA.setHostname(HOSTNAME);  // also starts mDNS: http://kegerator.local
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([] {
    currentDuty = targetDuty = 0;    // park the needle so the update is visible
    pwmWrite(0);
    Serial.println("OTA update starting");
  });
  ArduinoOTA.onEnd([] { Serial.println("OTA update done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
  otaStarted = true;
}

// ---------- Web pages ----------
String htmlEscape(const String& s) {
  String out;
  for (char c : s) {
    if      (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else               out += c;
  }
  return out;
}

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software (update / reboot)";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    case ESP_RST_EXT:      return "reset button";
    default:               return "other";
  }
}

void handleData() {
  float v   = isnan(smoothV) ? readSensorVolts() : smoothV;
  float psi = voltsToPsi(v);
  String ssid = wifiConnected ? WiFi.SSID() : String("not connected");
  ssid.replace("\\", "\\\\");       // JSON-escape; the page shows it as plain text
  ssid.replace("\"", "\\\"");
  String ip = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String plist;                     // {"id":"28..","f":36.5,"role":"keg"},...
  for (int i = 0; i < probeCount; i++) {
    if (i) plist += ",";
    plist += "{\"id\":\"" + addrToStr(probeAddr[i]) + "\",\"f\":" + jnum(probeF[i], 1) +
             ",\"role\":\"" + roleOf(probeAddr[i]) + "\"}";
  }
  char json[1024];
  snprintf(json, sizeof(json),
           "{\"mv\":%.1f,\"v\":%.4f,\"psi\":%s,\"zero\":%.4f,\"vpp\":%.5f,"
           "\"kegF\":%s,\"airF\":%s,\"kegSet\":%s,\"airSet\":%s,\"probes\":[%s],"
           "\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,"
           "\"heap\":%u,\"reset\":\"%s\",\"build\":\"%s %s\"}",
           v * 1000.0 / DIVIDER, v, jnum(psi, 2).c_str(), sensorVZero, sensorVPerPsi,
           jnum(kegTempF, 1).c_str(), jnum(airTempF, 1).c_str(),
           kegSet ? "true" : "false", airSet ? "true" : "false", plist.c_str(),
           ssid.c_str(), wifiConnected ? WiFi.RSSI() : 0, ip.c_str(), millis() / 1000,
           ESP.getFreeHeap(), resetReasonText(), __DATE__, __TIME__);
  server.send(200, "application/json", json);
}

// Stable, minimal JSON for other programs: {"psi":12.34,"kegF":37.0,"airF":38.0}.
// Kept separate from /data (which changes with the web pages). null = no valid reading.
void handleApi() {
  float psi = voltsToPsi(isnan(smoothV) ? readSensorVolts() : smoothV);
  String json = "{\"psi\":" + jnum(psi, 2) + ",\"kegF\":" + jnum(kegTempF, 1) +
                ",\"airF\":" + jnum(airTempF, 1) + "}";
  server.send(200, "application/json", json);
}

// Config pages and actions use the same login as /update.
bool requireLogin() {
  if (server.authenticate(OTA_USER, OTA_PASS)) return true;
  server.requestAuthentication();
  return false;
}

// Wi-Fi setup page: scans nearby networks and offers them as suggestions.
void handleWifiPage() {
  if (!requireLogin()) return;
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n; i++) {
    String s = htmlEscape(WiFi.SSID(i));
    if (s.length() == 0 || opts.indexOf("\"" + s + "\"") >= 0) continue;   // hidden / duplicate
    opts += "<option value=\"" + s + "\">" + s + " (" + WiFi.RSSI(i) + " dBm)</option>";
  }
  WiFi.scanDelete();
  String page = FPSTR(WIFI_HTML);
  page.replace("%OPTIONS%", opts);
  page.replace("%CURRENT%", htmlEscape(wifiSsid.length() ? wifiSsid : String("none")));
  server.send(200, "text/html", page);
}

void handleWifiSave() {
  if (!requireLogin()) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "Network name is required");
    return;
  }
  saveWifiCreds(ssid, pass);
  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width'>"
              "<body style='font-family:sans-serif;background:#111;color:#eee;padding:16px'>"
              "<p>Saved. Rebooting to join <b>" + htmlEscape(ssid) + "</b>.</p>"
              "<p>Switch your phone back to your home Wi-Fi, then open "
              "<a style='color:#6af' href='http://" + String(HOSTNAME) + ".local'>http://" +
              String(HOSTNAME) + ".local</a></p></body>");
  Serial.printf("home Wi-Fi saved: \"%s\", rebooting\n", ssid.c_str());
  delay(1000);                      // let the response reach the phone
  ESP.restart();
}

void setupRoutes() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", DISPLAY_HTML); });
  server.on("/config", HTTP_GET, [] {
    if (requireLogin()) server.send_P(200, "text/html", CONFIG_HTML);
  });
  server.on("/data", HTTP_GET, handleData);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/zero", HTTP_POST, [] {
    if (!requireLogin()) return;
    String m = calZero();
    Serial.println(m);
    server.send(200, "text/plain", m);
  });
  server.on("/span", HTTP_POST, [] {
    if (!requireLogin()) return;
    String m = calSpan(server.arg("psi").toFloat());
    Serial.println(m);
    server.send(200, "text/plain", m);
  });
  server.on("/probe", HTTP_POST, [] {
    if (!requireLogin()) return;
    String m = assignProbe(server.arg("id"), server.arg("role"));
    Serial.println(m);
    server.send(200, "text/plain", m);
  });
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  httpUpdater.setup(&server, "/update", OTA_USER, OTA_PASS);
  server.begin();
}

// ---------- Network start + upkeep ----------
void startNetwork() {
  WiFi.setHostname(HOSTNAME);
  loadWifiCreds();
  if (wifiSsid.length() == 0) {
    Serial.println("no home Wi-Fi saved");
    startSetupAp();
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);           // mains powered: no modem sleep = snappy web page + OTA
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("joining \"%s\"...\n", wifiSsid.c_str());
  }
  wifiLostMs = millis();
  setupRoutes();
}

// Called every loop. Never blocks, so the gauge keeps running without Wi-Fi.
void networkLoop() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wifiConnected) {
    Serial.printf("Wi-Fi connected to \"%s\": http://%s.local  (http://%s)  %d dBm\n",
                  WiFi.SSID().c_str(), HOSTNAME, WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    startOta();
    stopSetupAp();
  } else if (!connected && wifiConnected) {
    Serial.println("Wi-Fi lost, reconnecting...");
    wifiLostMs = millis();
  }
  wifiConnected = connected;

  if (!connected && !setupApOn && millis() - wifiLostMs > WIFI_TIMEOUT_MS) {
    Serial.println("home Wi-Fi not reachable");
    startSetupAp();
  }

  // Belt and braces: auto-reconnect may give up if the router was off at boot.
  static unsigned long lastRetry = 0;
  if (!connected && wifiSsid.length() && millis() - lastRetry > 60000) {
    lastRetry = millis();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }

  if (otaStarted) ArduinoOTA.handle();
  server.handleClient();
}

void printWifiStatus() {
  if (wifiConnected) {
    Serial.printf("Wi-Fi: \"%s\"  http://%s.local  (http://%s)  %d dBm\n",
                  WiFi.SSID().c_str(), HOSTNAME, WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.printf("Wi-Fi: not connected (saved network: \"%s\")\n",
                  wifiSsid.length() ? wifiSsid.c_str() : "none");
  }
  if (setupApOn) Serial.printf("setup network \"%s\" is on: http://%s/wifi\n",
                               SETUP_SSID, WiFi.softAPIP().toString().c_str());
}

// ---------- Sweep ----------
void sweep() {
  Serial.println("sweep");
  for (int d = 0; d <= DUTY_MAX; d += SLEW_STEP)  { pwmWrite(d); delay(SLEW_MS); }
  for (int d = DUTY_MAX; d >= 0; d -= SLEW_STEP)  { pwmWrite(d); delay(SLEW_MS); }
  pwmWrite(currentDuty);
}

// Slow upward sweep for calibration: prints each duty so you can note where the
// needle hits each dial marking. Send any line (just press Enter) to stop; the
// needle stays at the stop point so you can fine-tune with d###.
void slowSweep(int stepMs) {
  if (stepMs <= 0) stepMs = SLOW_STEP_MS;
  Serial.printf("slow sweep: +%d duty every %d ms. Press Enter to stop.\n",
                SLOW_STEP, stepMs);
  int d = 0;
  pwmWrite(0);
  delay(1500);                       // let the needle settle at the bottom first
  for (; d <= DUTY_MAX; d += SLOW_STEP) {
    pwmWrite(d);
    Serial.printf("d%d\n", d);
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)stepMs) {
      if (Serial.available()) {
        while (Serial.available()) Serial.read();
        Serial.printf("stopped at d%d\n", d);
        currentDuty = targetDuty = d;
        return;
      }
    }
  }
  currentDuty = targetDuty = DUTY_MAX;
  pwmWrite(DUTY_MAX);
  Serial.println("slow sweep done (holding max)");
}

// ---------- Serial commands ----------
void handleSerial() {
  if (!Serial.available()) return;
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() == 0) return;
  Serial.printf("> %s\n", s.c_str());   // echo so you can see input arrived

  if (s == "s") {
    sweep();
  } else if (s.startsWith("w")) {
    slowSweep(s.substring(1).toInt());
  } else if (s == "p") {
    monitorPressure = !monitorPressure;
    Serial.printf("pressure monitor %s\n", monitorPressure ? "on" : "off");
  } else if (s == "z") {
    Serial.println(calZero());
  } else if (s.startsWith("k")) {
    Serial.println(calSpan(s.substring(1).toFloat()));
  } else if (s == "r") {
    resetCal();
  } else if (s == "t") {
    printTemps();
  } else if (s == "wifi") {
    printWifiStatus();
  } else if (s == "wififorget") {
    forgetWifi();
  } else if (s == "?") {
    Serial.printf("current=%d target=%d sensor=%s\n",
                  currentDuty, targetDuty, USE_SENSOR ? "on" : "off");
    Serial.printf("pressure cal: zero=%.4f V  span=%.5f V/psi\n", sensorVZero, sensorVPerPsi);
  } else if (s.startsWith("d")) {
    setRawDuty(s.substring(1).toInt());
  } else if (isDigit(s[0]) || s[0] == '-' || s[0] == '.') {
    showValue(s.toFloat());
  } else {
    Serial.println("unknown command. Use <value>, d<duty>, s, w[ms], p, z, k<psi>, r, t, wifi, wififorget, ?");
  }
}

// ---------- Main ----------
void setup() {
  Serial.begin(115200);
  analogSetPinAttenuation(PRESSURE_PIN, ADC_11db);   // ~0-3.1V input range
  loadCal();
  setupProbes();
  if (WIFI_ENABLED) startNetwork();
  pwmBegin();
  pwmWrite(0);
  delay(500);
  if (AUTO_SLOW_SWEEP) {
    for (int i = START_DELAY_SECONDS; i > 0; i--) {
      Serial.printf("slow sweep starts in %d...\n", i);
      delay(1000);
    }
    while (Serial.available()) Serial.read();   // ignore keys pressed during countdown
    slowSweep(SLOW_STEP_MS);
  } else {
    sweep();
  }
  Serial.println("PhysicalGauge ready. Commands: <value>, d<duty>, s, w[ms], p, z, k<psi>, r, t, wifi, wififorget, ?");
}

void loop() {
  handleSerial();
  if (WIFI_ENABLED) networkLoop();

  unsigned long now = millis();

  tempLoop();

  if (now - lastSmooth >= SMOOTH_MS) {
    lastSmooth = now;
    float v = readSensorVolts();
    smoothV = isnan(smoothV) ? v : smoothV + SMOOTH_ALPHA * (v - smoothV);
  }

  if (USE_SENSOR && now - lastSensor >= SENSOR_MS) {
    lastSensor = now;
    float v = readSensor();
    if (!isnan(v)) targetDuty = constrain(dutyFor(v), 0, DUTY_MAX);
  }

  if (monitorPressure && now - lastPrint >= PRINT_MS) {
    lastPrint = now;
    printPressure();
  }

  if (now - lastSlew >= SLEW_MS && currentDuty != targetDuty) {
    lastSlew = now;
    int delta = constrain(targetDuty - currentDuty, -SLEW_STEP, SLEW_STEP);
    currentDuty += delta;
    pwmWrite(currentDuty);
  }
}
