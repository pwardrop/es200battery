#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <math.h>

// ============================================================
// ESP32-C6 ES200 / ES90 battery monitor + Wi-Fi provisioning
//
// HARDWARE WIRING
//   Battery BLUE  -> ESP32 TX pin (BAT_TX_PIN)
//   Battery GREEN -> ESP32 RX pin (BAT_RX_PIN)
//   
//
// IMPORTANT
//   Add an EXTERNAL pull-up resistor from GREEN to 3.3V.
//   Start with 1k ohm.
//   INPUT_PULLUP is enabled on BAT_RX_PIN 3.3v, 
//
// OBSERVED SERIAL FRAMES
//   Normal mode:   3A 16 20 02 ...
//   Charging mode: 3A 16 20 0F ...
//
// UNLOCK / WAKE COMMAND
//   3A 13 01 16 79 @ 9600 baud
//
// ============================================================

// -----------------------------
// Device names
// -----------------------------
const char* DEVICE_NAME = "es200battery";
const char* MDNS_NAME   = "es200battery";

// Optional AP password
const char* AP_PASSWORD = nullptr;

// -----------------------------
// UART pins
// -----------------------------
// Change only if your board wiring changed.
static const int BAT_RX_PIN = 6;    // ESP32 RX <- battery GREEN
static const int BAT_TX_PIN = 7;    // ESP32 TX -> battery BLUE

// -----------------------------
// Networking
// -----------------------------
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress apNetmask(255, 255, 255, 0);

bool wifiConnected = false;
bool portalMode = false;
String savedSSID = "";
String apSSID = DEVICE_NAME;

// -----------------------------
// Battery UART
// -----------------------------
HardwareSerial BatterySerial(1);
const uint8_t wakeCmd[] = {0x3A, 0x13, 0x01, 0x16, 0x79};

unsigned long lastWakeMs = 0;
const unsigned long wakeIntervalMs = 5000;

// -----------------------------
// Counters / uptime
// -----------------------------
unsigned long rxCount = 0;
unsigned long txCount = 0;
unsigned long wakeCount = 0;
unsigned long bootMs = 0;

// -----------------------------
// Frame capture
// -----------------------------
uint8_t frameBuf[128];
size_t frameLen = 0;
unsigned long lastByteMs = 0;
const unsigned long frameGapMs = 100;

// -----------------------------
// Small frame history
// -----------------------------
struct FrameRecord {
  String hex = "";
  size_t len = 0;
  unsigned long ms = 0;
};

FrameRecord txFrames[2];
FrameRecord rxFrames[2];

void pushFrame(FrameRecord history[2], const String &hex, size_t len) {
  history[0] = history[1];
  history[1].hex = hex;
  history[1].len = len;
  history[1].ms = millis();
}

// -----------------------------
// Battery status
// -----------------------------
struct BatteryStatus {
  bool valid = false;
  bool frameLooksValid = false;
  bool voltageValid = false;
  bool socValid = false;
  bool currentValid = false;
  bool tempValid = false;
  bool chargingDetected = false;

  unsigned long lastUpdateMs = 0;

  float packVoltage = NAN;
  float packCurrent = NAN;
  float stateOfCharge = NAN;
  float temperatureC = NAN;
  float minTempC = NAN;
  float maxTempC = NAN;
  float lowCellVoltage = NAN;
  float highCellVoltage = NAN;

  float statusCode = NAN;
  float packStateCode = NAN;
  float cycleLikeCounter = NAN;
  float mosfetOrModeCode = NAN;
  float trailingByteCode = NAN;

  uint16_t rawVoltage = 0;
  uint16_t rawCurrent = 0;
  uint8_t rawSoc = 0;
  uint16_t rawLowCell = 0;
  uint16_t rawHighCell = 0;

  String operatingMode = "Unknown";
  String lastFrameHex = "";
  String decodeNote = "No decoded frame yet";
  String candidateNote = "";
  size_t lastFrameLen = 0;
};

BatteryStatus batteryStatus;

// ============================================================
// Forward declarations
// ============================================================
void setupPortalRoutes();
void setupMainRoutes();
void processCompletedFrame();
void readBatterySerial();
void tryDecodeFrame(const uint8_t *data, size_t len);

// ============================================================
// Helpers
// ============================================================

String byteToHex(uint8_t b) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%02X", b);
  return String(buf);
}

String wordToHex(uint16_t w) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04X", w);
  return String(buf);
}

String bytesToHexString(const uint8_t *data, size_t len) {
  String s;
  s.reserve(len * 3);
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) s += "0";
    s += String(data[i], HEX);
    if (i + 1 < len) s += " ";
  }
  s.toUpperCase();
  return s;
}

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 32);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 32);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  return out;
}

String fmtFloat(float v, int digits = 2, const char* unit = "") {
  if (isnan(v)) return String("--");
  String s = String(v, digits);
  s += unit;
  return s;
}

String commonStyle() {
  String css;
  css += "body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:20px;}";
  css += "h1{font-size:22px;margin-top:0;} h2{font-size:18px;}";
  css += ".card{background:#1b1b1b;border:1px solid #333;border-radius:10px;padding:16px;margin-bottom:16px;}";
  css += "pre{background:#000;color:#0f0;padding:10px;border-radius:8px;display:block;overflow:auto;white-space:pre-wrap;}";
  css += "a{color:#7fc8ff;} .muted{color:#aaa;} .warn{color:#ffd166;}";
  css += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  css += "input,select{width:100%;padding:10px;border-radius:8px;border:1px solid #444;background:#0f0f0f;color:#eee;box-sizing:border-box;}";
  css += "button{padding:10px 14px;border-radius:8px;border:1px solid #444;background:#2d6cdf;color:#fff;cursor:pointer;}";
  css += "label{display:block;margin:10px 0 6px;}";
  return css;
}

uint16_t be16(const uint8_t *data, size_t idx) {
  return ((uint16_t)data[idx] << 8) | data[idx + 1];
}

bool isIpAddress(const String &str) {
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!(c == '.' || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

String timeAgo(unsigned long thenMs) {
  if (thenMs == 0) return "--";
  unsigned long d = (millis() - thenMs) / 1000;
  return String(d) + " s ago";
}

String frameAge(unsigned long thenMs) {
  if (thenMs == 0) return "--";
  return String((millis() - thenMs) / 1000) + " s ago";
}

// ============================================================
// Preferences
// ============================================================

void loadCredentials(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
}

void saveCredentials(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearCredentials() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ============================================================
// Battery UART / wake
// ============================================================

void sendWakeCommand() {
  BatterySerial.write(wakeCmd, sizeof(wakeCmd));
  BatterySerial.flush();

  txCount += sizeof(wakeCmd);
  wakeCount++;

  String line = bytesToHexString(wakeCmd, sizeof(wakeCmd));
  pushFrame(txFrames, line, sizeof(wakeCmd));

  Serial.print("[TX] ");
  Serial.println(line);
}

void tryDecodeFrame(const uint8_t *data, size_t len) {
  batteryStatus.lastFrameHex = bytesToHexString(data, len);
  batteryStatus.lastFrameLen = len;
  batteryStatus.lastUpdateMs = millis();

  batteryStatus.valid = false;
  batteryStatus.frameLooksValid = false;
  batteryStatus.voltageValid = false;
  batteryStatus.socValid = false;
  batteryStatus.currentValid = false;
  batteryStatus.tempValid = false;
  batteryStatus.chargingDetected = false;
  batteryStatus.operatingMode = "Unknown";

  batteryStatus.packVoltage = NAN;
  batteryStatus.packCurrent = NAN;
  batteryStatus.stateOfCharge = NAN;
  batteryStatus.temperatureC = NAN;
  batteryStatus.minTempC = NAN;
  batteryStatus.maxTempC = NAN;
  batteryStatus.lowCellVoltage = NAN;
  batteryStatus.highCellVoltage = NAN;

  batteryStatus.statusCode = NAN;
  batteryStatus.packStateCode = NAN;
  batteryStatus.cycleLikeCounter = NAN;
  batteryStatus.mosfetOrModeCode = NAN;
  batteryStatus.trailingByteCode = NAN;

  batteryStatus.rawVoltage = 0;
  batteryStatus.rawCurrent = 0;
  batteryStatus.rawSoc = 0;
  batteryStatus.rawLowCell = 0;
  batteryStatus.rawHighCell = 0;

  batteryStatus.decodeNote = "";
  batteryStatus.candidateNote = "";

  if (len != 36) {
    batteryStatus.decodeNote = "Unexpected frame length; expected 36 bytes";
    return;
  }

  if (!(data[0] == 0x3A && data[1] == 0x16 && data[2] == 0x20)) {
    batteryStatus.decodeNote = "Header does not match expected 3A 16 20";
    return;
  }

  batteryStatus.frameLooksValid = true;

  uint8_t modeByte = data[3];
  batteryStatus.statusCode = data[1];
  batteryStatus.packStateCode = modeByte;
  batteryStatus.cycleLikeCounter = be16(data, 5);
  batteryStatus.mosfetOrModeCode = data[11];
  batteryStatus.trailingByteCode = data[35];

  uint16_t candVoltage = be16(data, 32);
  uint8_t candSoc = data[34];

  batteryStatus.rawVoltage = candVoltage;
  batteryStatus.rawSoc = candSoc;

  float candVoltageV = candVoltage / 100.0f;
  if (candVoltageV >= 20.0f && candVoltageV <= 60.0f) {
    batteryStatus.packVoltage = candVoltageV;
    batteryStatus.voltageValid = true;
  }

  if (candSoc <= 100) {
    batteryStatus.stateOfCharge = candSoc;
    batteryStatus.socValid = true;
  }

  uint8_t t1 = data[7];
  uint8_t t2 = data[8];
  uint8_t t3 = data[9];
  uint8_t t4 = data[10];
  uint8_t tMin = min(min(t1, t2), min(t3, t4));
  uint8_t tMax = max(max(t1, t2), max(t3, t4));

  batteryStatus.minTempC = (float)tMin;
  batteryStatus.maxTempC = (float)tMax;
  batteryStatus.temperatureC = (float)tMax;
  batteryStatus.tempValid = true;

  uint16_t rawCurrentBE2526 = be16(data, 25);
  batteryStatus.rawCurrent = rawCurrentBE2526;

  float calibratedCurrent = (60415.0f - rawCurrentBE2526) / 13446.0f;

  if (fabsf(calibratedCurrent) < 0.12f) {
    calibratedCurrent = 0.0f;
  }

  if (calibratedCurrent >= -10.0f && calibratedCurrent <= 20.0f) {
    batteryStatus.packCurrent = calibratedCurrent;
    batteryStatus.currentValid = true;
  } else {
    batteryStatus.packCurrent = NAN;
    batteryStatus.currentValid = false;
  }

  batteryStatus.chargingDetected = (modeByte == 0x0F);

  if (!batteryStatus.frameLooksValid) {
    batteryStatus.operatingMode = "Unknown";
  } else if (batteryStatus.chargingDetected) {
    batteryStatus.operatingMode = "Charging";
  } else if (batteryStatus.currentValid) {
    if (fabsf(batteryStatus.packCurrent) < 0.12f) {
      batteryStatus.operatingMode = "Idle";
    } else if (batteryStatus.packCurrent > 0.0f) {
      batteryStatus.operatingMode = "Discharging";
    } else {
      batteryStatus.operatingMode = "Idle";
    }
  } else {
    batteryStatus.operatingMode = "Unknown";
  }

  String modeLabel = "unknown";
  if (modeByte == 0x02) modeLabel = "normal/discharge";
  else if (modeByte == 0x0F) modeLabel = "charging";

  batteryStatus.decodeNote =
    "Mode=" + batteryStatus.operatingMode +
    "; byte[3]=0x" + byteToHex(modeByte) +
    " (" + modeLabel + "); voltage/SOC use stable offsets; current uses provisional BE[25..26] calibration";

  batteryStatus.candidateNote =
    String("Header=") + byteToHex(data[0]) + " " + byteToHex(data[1]) + " " + byteToHex(data[2]) +
    ", Mode[3]=0x" + byteToHex(modeByte) +
    ", V[32..33]=0x" + wordToHex(candVoltage) +
    ", SOC[34]=0x" + byteToHex(candSoc) +
    ", Tail[35]=0x" + byteToHex(data[35]) +
    ", T[7..10]=" + byteToHex(data[7]) + " " + byteToHex(data[8]) + " " + byteToHex(data[9]) + " " + byteToHex(data[10]) +
    "\nCurrent raw BE[25..26] = 0x" + wordToHex(rawCurrentBE2526) +
    "\nCurrent formula = (60415 - rawBE2526) / 13446.0" +
    "\nCalibrated current = " + String(calibratedCurrent, 2) + " A" +
    "\nDerived mode = " + batteryStatus.operatingMode;

  batteryStatus.valid =
    batteryStatus.voltageValid ||
    batteryStatus.socValid ||
    batteryStatus.currentValid ||
    batteryStatus.tempValid;
}

void processCompletedFrame() {
  if (frameLen == 0) return;

  String rxHex = bytesToHexString(frameBuf, frameLen);
  pushFrame(rxFrames, rxHex, frameLen);

  tryDecodeFrame(frameBuf, frameLen);
  frameLen = 0;
}

void readBatterySerial() {
  while (BatterySerial.available()) {
    int readVal = BatterySerial.read();
    if (readVal < 0) break;

    uint8_t b = (uint8_t)readVal;
    rxCount++;
    lastByteMs = millis();

    if (frameLen < sizeof(frameBuf)) {
      frameBuf[frameLen++] = b;
    } else {
      processCompletedFrame();
      frameBuf[0] = b;
      frameLen = 1;
    }

    Serial.print("RX 0x");
    Serial.println(byteToHex(b));
  }

  if (frameLen > 0 && (millis() - lastByteMs) > frameGapMs) {
    processCompletedFrame();
  }
}

// ============================================================
// Wi-Fi / captive portal
// ============================================================

bool connectToSavedWiFi(unsigned long timeoutMs = 20000) {
  String pass;
  loadCredentials(savedSSID, pass);

  if (savedSSID.isEmpty()) {
    Serial.println("No saved Wi-Fi credentials.");
    return false;
  }

  Serial.print("Trying saved Wi-Fi SSID: ");
  Serial.println(savedSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    portalMode = false;
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Saved Wi-Fi connect failed.");
  WiFi.disconnect(true, true);
  wifiConnected = false;
  portalMode = false;
  return false;
}

void startAccessPoint() {
  WiFi.disconnect(true, true);
  delay(200);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, apNetmask);

  bool ok;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    ok = WiFi.softAP(apSSID.c_str(), AP_PASSWORD);
  } else {
    ok = WiFi.softAP(apSSID.c_str());
  }

  portalMode = true;
  wifiConnected = false;

  Serial.print("AP mode start: ");
  Serial.println(ok ? "OK" : "FAILED");
  Serial.print("Connect to SSID: ");
  Serial.println(apSSID);
  Serial.print("Portal IP: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("Captive DNS server started.");
}

void startMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS started: http://");
    Serial.print(MDNS_NAME);
    Serial.println(".local/");
  } else {
    Serial.println("Error starting mDNS");
  }
}

String buildWifiOptions() {
  String options;
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    options = "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      options += "<option value=\"" + htmlEscape(s) + "\">" + htmlEscape(s);
      options += " (";
      options += String(WiFi.RSSI(i));
      options += " dBm)</option>";
    }
  }
  return options;
}

void redirectToPortal() {
  String url = String("http://") + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "");
}

// ============================================================
// Portal handlers
// ============================================================

void handlePortalRoot() {
  String html;
  html.reserve(9000);

  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Configure Wi-Fi</title><style>";
  html += commonStyle();
  html += "</style></head><body>";

  html += "<h1>Configure Wi-Fi for es200battery</h1>";
  html += "<div class='card'>";
  html += "<p>Connect this device to your Wi-Fi. After saving, it will reboot and stop broadcasting its setup network once it connects successfully.</p>";
  html += "<p><b>Setup SSID:</b> " + htmlEscape(apSSID) + "<br>";
  html += "<b>Portal IP:</b> " + WiFi.softAPIP().toString() + "</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<form method='POST' action='/save'>";
  html += "<label for='ssid_select'>Nearby networks</label>";
  html += "<select id='ssid_select' onchange='document.getElementById(\"ssid\").value=this.value;'>";
  html += buildWifiOptions();
  html += "</select>";

  html += "<label for='ssid'>Wi-Fi SSID</label>";
  html += "<input id='ssid' name='ssid' type='text' placeholder='Enter Wi-Fi name'>";

  html += "<label for='pass'>Wi-Fi password</label>";
  html += "<input id='pass' name='pass' type='password' placeholder='Enter Wi-Fi password'>";

  html += "<div style='margin-top:14px;display:flex;gap:10px;flex-wrap:wrap;'>";
  html += "<button type='submit'>Save and reboot</button>";
  html += "<a href='/scan'>Rescan networks</a>";
  html += "</div>";
  html += "</form></div>";

  html += "<div class='card'><a href='/clearwifi'>Clear saved Wi-Fi</a></div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handlePortalSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing SSID");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }

  saveCredentials(ssid, pass);

  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
  html += commonStyle();
  html += "</style></head><body>";
  html += "<div class='card'><h1>Saved</h1><p>Credentials saved for <b>" + htmlEscape(ssid) + "</b>.</p>";
  html += "<p>The ESP32 will reboot now and try to connect. If successful, the setup network will disappear.</p></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);

  delay(1500);
  ESP.restart();
}

void handlePortalScan() {
  handlePortalRoot();
}

void handlePortalClearWifi() {
  clearCredentials();
  server.send(200, "text/plain", "Saved Wi-Fi credentials cleared. Reboot to re-enter setup mode.");
}

void handleGenerate204() { redirectToPortal(); }
void handleGen204()      { redirectToPortal(); }
void handleHotspot()     { redirectToPortal(); }
void handleNcsi()        { redirectToPortal(); }
void handleConnectTest() { redirectToPortal(); }
void handleAppleTest()   { redirectToPortal(); }

void handlePortalNotFound() {
  String host = server.hostHeader();

  if (!isIpAddress(host) && host != (String(MDNS_NAME) + ".local")) {
    redirectToPortal();
    return;
  }

  handlePortalRoot();
}

// ============================================================
// Main app handlers
// ============================================================

void handleMainRoot() {
  String html;
  html.reserve(14000);

  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "<title>ES200 Battery Monitor</title><style>";
  html += commonStyle();
  html += "</style></head><body>";

  html += "<h1>ESP32-C6 ES200 Battery Monitor</h1>";

  html += "<div class='card grid'>";
  html += "<div><b>Connected SSID</b><br>" + htmlEscape(savedSSID) + "</div>";
  html += "<div><b>IP Address</b><br>" + WiFi.localIP().toString() + "</div>";
  html += "<div><b>mDNS Hostname</b><br>" + String(MDNS_NAME) + ".local</div>";
  html += "<div><b>Open URL</b><br>http://" + String(MDNS_NAME) + ".local/</div>";

  unsigned long uptime_seconds = (millis() - bootMs) / 1000;
  unsigned long years = uptime_seconds/31536000;
  unsigned long months = (uptime_seconds%31536000)/2592000;
  unsigned long days = (uptime_seconds%2592000)/86400;
  unsigned long hours = (uptime_seconds%86400)/3600;
  unsigned long minutes = (uptime_seconds%3600)/60;
  unsigned long seconds = uptime_seconds%60;


  html += "<div><b>Uptime</b><br>" + String(years) + "y " + String(months) + "mo " + String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s</div>";
  html += "<div><b>RSSI</b><br>" + String(WiFi.RSSI()) + " dBm</div>";
  html += "<div><b>Wake commands sent</b><br>" + String(wakeCount) + "</div>";
  html += "<div><b>Battery RX bytes</b><br>" + String(rxCount) + "</div>";
  html += "<div><b>Battery TX bytes</b><br>" + String(txCount) + "</div>";
  html += "<div><b>RX pin level now</b><br>" + String(digitalRead(BAT_RX_PIN)) + "</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<b>Wiring notes</b><br>";
  html += "<span class='warn'>Current mapping: RX=GPIO " + String(BAT_RX_PIN) + ", TX=GPIO " + String(BAT_TX_PIN) + ". Use external pull-up on green reply line to 3.3V, typically a 1k ohm resistor.</span>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<b>Decoded battery status</b><br>";
  html += "<span class='muted'>Provisional decode from observed 36-byte frame</span>";
  html += "<div class='grid' style='margin-top:12px;'>";
  html += "<div><b>Mode</b><br>" + htmlEscape(batteryStatus.operatingMode) + "</div>";
  html += "<div><b>Voltage</b><br>" + fmtFloat(batteryStatus.packVoltage, 2, " V") + "</div>";
  html += "<div><b>Current</b><br>" + fmtFloat(batteryStatus.packCurrent, 2, " A") + "</div>";
  html += "<div><b>State of charge</b><br>" + fmtFloat(batteryStatus.stateOfCharge, 0, " %") + "</div>";
  html += "<div><b>Temperature</b><br>" + fmtFloat(batteryStatus.temperatureC, 1, " C") + "</div>";
  html += "<div><b>Min temp</b><br>" + fmtFloat(batteryStatus.minTempC, 1, " C") + "</div>";
  html += "<div><b>Max temp</b><br>" + fmtFloat(batteryStatus.maxTempC, 1, " C") + "</div>";
  html += "<div><b>Charging detected</b><br>" + String(batteryStatus.chargingDetected ? "Yes" : "No") + "</div>";
  html += "<div><b>Last update</b><br>" + timeAgo(batteryStatus.lastUpdateMs) + "</div>";
  html += "<div><b>Last frame length</b><br>" + String(batteryStatus.lastFrameLen) + "</div>";
  html += "<div><b>Frame shape</b><br>" + String(batteryStatus.frameLooksValid ? "Valid header family" : "Not valid yet") + "</div>";
  html += "<div><b>Decode status</b><br>" + htmlEscape(batteryStatus.decodeNote) + "</div>";
  html += "</div>";
  html += "</div>";

  html += "<div class='card'><b>Guessed / placeholder fields</b><div class='grid' style='margin-top:12px;'>";
  html += "<div><b>Status code</b><br>" + String((int)batteryStatus.statusCode) + "</div>";
  html += "<div><b>Pack state code</b><br>" + String((int)batteryStatus.packStateCode) + "</div>";
  html += "<div><b>Cycle-like counter</b><br>" + String((int)batteryStatus.cycleLikeCounter) + "</div>";
  html += "<div><b>MOSFET / mode code</b><br>" + String((int)batteryStatus.mosfetOrModeCode) + "</div>";
  html += "<div><b>Trailing byte</b><br>" + String((int)batteryStatus.trailingByteCode) + "</div>";
  html += "</div></div>";

  html += "<div class='card'><b>Candidate fields</b><pre>";
  html += "Raw voltage candidate [32..33]: 0x" + wordToHex(batteryStatus.rawVoltage) + "\n";
  html += "Raw SOC candidate [34]:        0x" + byteToHex(batteryStatus.rawSoc) + "\n";
  html += "Raw current candidate:         0x" + wordToHex(batteryStatus.rawCurrent) + "\n";
  html += htmlEscape(batteryStatus.candidateNote);
  html += "</pre></div>";

  html += "<div class='card'><b>Latest decoded RX frame</b><pre>";
  html += htmlEscape(batteryStatus.lastFrameHex);
  html += "</pre></div>";

  html += "<div class='card'><b>Last two TX frames</b><pre>";
  html += "Older: " + htmlEscape(txFrames[0].hex) + "\n";
  html += "Len: " + String(txFrames[0].len) + " bytes, Age: " + frameAge(txFrames[0].ms) + "\n\n";
  html += "Newest: " + htmlEscape(txFrames[1].hex) + "\n";
  html += "Len: " + String(txFrames[1].len) + " bytes, Age: " + frameAge(txFrames[1].ms);
  html += "</pre></div>";

  html += "<div class='card'><b>Last two RX frames</b><pre>";
  html += "Older: " + htmlEscape(rxFrames[0].hex) + "\n";
  html += "Len: " + String(rxFrames[0].len) + " bytes, Age: " + frameAge(rxFrames[0].ms) + "\n\n";
  html += "Newest: " + htmlEscape(rxFrames[1].hex) + "\n";
  html += "Len: " + String(rxFrames[1].len) + " bytes, Age: " + frameAge(rxFrames[1].ms);
  html += "</pre></div>";

  html += "<div class='card'><a href='/sendWake'>Send wake now</a> | <a href='/clear'>Clear frame history</a> | <a href='/resetwifi'>Reset Wi-Fi and reboot</a> | <a href='/json'>JSON</a></div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSendWake() {
  sendWakeCommand();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Wake sent");
}

void handleClear() {
  txFrames[0] = FrameRecord();
  txFrames[1] = FrameRecord();
  rxFrames[0] = FrameRecord();
  rxFrames[1] = FrameRecord();
  batteryStatus = BatteryStatus();
  rxCount = 0;
  txCount = 0;
  wakeCount = 0;
  frameLen = 0;

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Frame history cleared");
}

void handleJson() {
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"mdns\":\"" + String(MDNS_NAME) + ".local\",";
  json += "\"ssid\":\"" + jsonEscape(savedSSID) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_sec\":" + String((millis() - bootMs) / 1000) + ",";
  json += "\"rx_count\":" + String(rxCount) + ",";
  json += "\"tx_count\":" + String(txCount) + ",";
  json += "\"wake_count\":" + String(wakeCount) + ",";
  json += "\"rx_pin_level\":" + String(digitalRead(BAT_RX_PIN)) + ",";
  json += "\"tx_frames\":[";
  json += "{\"hex\":\"" + jsonEscape(txFrames[0].hex) + "\",\"len\":" + String(txFrames[0].len) + ",\"age\":\"" + jsonEscape(frameAge(txFrames[0].ms)) + "\"},";
  json += "{\"hex\":\"" + jsonEscape(txFrames[1].hex) + "\",\"len\":" + String(txFrames[1].len) + ",\"age\":\"" + jsonEscape(frameAge(txFrames[1].ms)) + "\"}";
  json += "],";
  json += "\"rx_frames\":[";
  json += "{\"hex\":\"" + jsonEscape(rxFrames[0].hex) + "\",\"len\":" + String(rxFrames[0].len) + ",\"age\":\"" + jsonEscape(frameAge(rxFrames[0].ms)) + "\"},";
  json += "{\"hex\":\"" + jsonEscape(rxFrames[1].hex) + "\",\"len\":" + String(rxFrames[1].len) + ",\"age\":\"" + jsonEscape(frameAge(rxFrames[1].ms)) + "\"}";
  json += "],";
  json += "\"decoded\":{";
  json += "\"valid\":" + String(batteryStatus.valid ? "true" : "false") + ",";
  json += "\"mode\":\"" + jsonEscape(batteryStatus.operatingMode) + "\",";
  json += "\"charging_detected\":" + String(batteryStatus.chargingDetected ? "true" : "false") + ",";
  json += "\"voltage\":" + String(isnan(batteryStatus.packVoltage) ? 0.0 : batteryStatus.packVoltage, 2) + ",";
  json += "\"current\":" + String(isnan(batteryStatus.packCurrent) ? 0.0 : batteryStatus.packCurrent, 2) + ",";
  json += "\"soc\":" + String(isnan(batteryStatus.stateOfCharge) ? 0.0 : batteryStatus.stateOfCharge, 1) + ",";
  json += "\"temp_c\":" + String(isnan(batteryStatus.temperatureC) ? 0.0 : batteryStatus.temperatureC, 1) + ",";
  json += "\"last_frame_len\":" + String(batteryStatus.lastFrameLen) + ",";
  json += "\"note\":\"" + jsonEscape(batteryStatus.decodeNote) + "\",";
  json += "\"last_frame_hex\":\"" + jsonEscape(batteryStatus.lastFrameHex) + "\"";
  json += "}";
  json += "}";
  server.send(200, "application/json", json);
}

void handleResetWiFi() {
  clearCredentials();

  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
  html += commonStyle();
  html += "</style></head><body>";
  html += "<div class='card'><h1>Wi-Fi reset</h1><p>Saved credentials cleared. Rebooting into setup mode.</p></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);

  delay(1500);
  ESP.restart();
}

// ============================================================
// Route setup
// ============================================================

void setupPortalRoutes() {
  server.on("/", HTTP_GET, handlePortalRoot);
  server.on("/scan", HTTP_GET, handlePortalScan);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.on("/clearwifi", HTTP_GET, handlePortalClearWifi);

  server.on("/generate_204", HTTP_GET, handleGenerate204);
  server.on("/gen_204", HTTP_GET, handleGen204);
  server.on("/hotspot-detect.html", HTTP_GET, handleAppleTest);
  server.on("/library/test/success.html", HTTP_GET, handleAppleTest);
  server.on("/connecttest.txt", HTTP_GET, handleConnectTest);
  server.on("/ncsi.txt", HTTP_GET, handleNcsi);
  server.on("/fwlink", HTTP_GET, handleNcsi);

  server.onNotFound(handlePortalNotFound);
}

void setupMainRoutes() {
  server.on("/", HTTP_GET, handleMainRoot);
  server.on("/sendWake", HTTP_GET, handleSendWake);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/json", HTTP_GET, handleJson);
  server.on("/resetwifi", HTTP_GET, handleResetWiFi);
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
  bootMs = millis();

  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-C6 ES200 battery monitor starting...");

  pinMode(BAT_RX_PIN, INPUT_PULLUP);
  pinMode(BAT_TX_PIN, OUTPUT);
  digitalWrite(BAT_TX_PIN, HIGH);

  BatterySerial.begin(9600, SERIAL_8N1, BAT_RX_PIN, BAT_TX_PIN);
  delay(200);

  bool connected = connectToSavedWiFi(20000);

  if (connected) {
    startMDNS();
    setupMainRoutes();
  } else {
    startAccessPoint();
    setupPortalRoutes();
  }

  server.begin();

  sendWakeCommand();
  lastWakeMs = millis();
}

void loop() {
  server.handleClient();

  if (portalMode) {
    dnsServer.processNextRequest();
  }

  readBatterySerial();

  if (millis() - lastWakeMs >= wakeIntervalMs) {
    sendWakeCommand();
    lastWakeMs = millis();
  }

  if (!portalMode && WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped, rebooting to retry / fallback to AP.");
    delay(1000);
    ESP.restart();
  }

  delay(10);
}
