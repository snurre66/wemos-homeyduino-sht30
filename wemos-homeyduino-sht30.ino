/**
 * Wemos D1 Mini Wine Cabinet Climate & Diagnostics Node (SHT30 Shield) for Homey
 * Specially engineered for wine storage, cellar monitoring, and cork preservation.
 * Uses the Homeyduino library to communicate with Athom Homey.
 * 
 * Hardware:
 * - Wemos D1 Mini (ESP8266)
 * - Wemos SHT30 Shield (I2C on pins D1/SCL and D2/SDA, default addr 0x45)
 * 
 * Key Wine Storage & Preservation Features:
 * - Real-time air temperature & relative humidity monitoring (SHT30)
 * - Simulated Liquid Core Temperature (PT1 thermal low-pass filter modeling a 750ml bottle)
 * - Cork Dryness Alert (RH < 50.0% alert to prevent cork shrinkage and wine oxidation)
 * - Wine Cellar Temperature Range Alarms (Low < 8.0°C, High > 16.0°C)
 * - Rapid Temperature Rise / Open Door / Cooling Failure Alert (ΔT > 1.5°C over 5 min)
 * - Condensation & Mold Risk psychrometric derivations (Magnus-Tetens)
 * - Stealth Mode: Configurable dark mode disabling onboard LEDs inside glass-door cabinets
 * - Persistent Device Naming: Set custom name via Web UI (saved in EEPROM) or secrets.h
 * - Embedded Web Server (Port 80) with live mobile-responsive wine cellar dashboard
 * - Over-The-Air (OTA) wireless firmware update support
 * - Non-blocking timing architecture
 * 
 * Device Naming Priority & mDNS Hostname Rules:
 * 1. Custom name stored in EEPROM Flash (set dynamically via Web UI System tab)
 * 2. DEVICE_NAME defined in secrets.h (e.g. #define DEVICE_NAME "Vinskap-SHT30")
 * 3. Default hardware fallback: Wemos-SHT30-XXXX (based on ESP8266 Chip ID)
 * 
 * ⚠️ Important mDNS / Hostname Constraints:
 * - RFC 1035 / RFC 6762 standards require hostnames to contain ONLY letters (A-Z, a-z),
 *   numbers (0-9), and hyphens (-).
 * - Underscores (_) and spaces are strictly forbidden in mDNS hostnames and cause .local
 *   lookups (e.g. http://vinskap_sht30.local) to fail in standard web browsers and Windows.
 * - The firmware includes automatic sanitization (sanitizeDeviceName) which transparently
 *   converts underscores, spaces, dots, and slashes into hyphens (-) upon boot and save.
 * 
 * Capabilities Reported to Homey Device Tile:
 * 1. measure_temperature           - Ambient Air Temperature (°C)
 * 2. measure_temperature.liquid    - Simulated Wine Bottle Core Temperature (°C)
 * 3. measure_humidity              - Relative Humidity (%)
 * 4. measure_signal_strength       - WiFi Signal Strength RSSI (dBm)
 * 5. measure_voltage               - Internal Supply Rail Voltage Vcc (V)
 * 6. alarm_water                   - Vannalarm: Condensation Risk Alert (Temp - DewPoint <= 2.0°C)
 * 7. alarm_generic                 - Generell alarm: Mold Risk Alert (RH >= 75.0%)
 * 8. alarm_tamper                  - Sabotasjealarm: SHT30 I2C Sensor Hardware Fault Alert
 * 
 * Flow Triggers with Tag Values ("WHEN" cards):
 * - cork_dry_risk                  - Cork Dryness Risk State (boolean tag: true/false)
 * - cork_dry_alert                 - Cork Dryness Event (Humidity tag: % RH)
 * - wine_temp_low                  - Wine Temp Below 8°C Alert (Temp tag: °C)
 * - wine_temp_high                 - Wine Temp Above 16°C Alert (Temp tag: °C)
 * - liquid_temp                    - Calculated Wine Core Temp (Temp tag: °C)
 * - rapid_temp_rise                - Rapid Temp Rise / Open Door Alert (Delta Temp tag: °C)
 * - wine_status                    - Textual Climate Summary tag ("Optimalt", "Tørr luft", etc.)
 * - dew_point                      - Calculated Dew Point (°C)
 * - dew_point_spread               - Temperature Margin Above Dew Point (°C)
 * - absolute_humidity              - Absolute Humidity (g/m³)
 * - vpd                            - Vapor Pressure Deficit (kPa)
 * - freeze_risk                    - Freezing Risk State (boolean tag: true/false)
 * - frost_alert                    - Frost/Freeze Trigger Event (Temp tag: °C)
 * - device_booted                  - Emitted on boot/reconnect (Reset Reason tag)
 * - diagnostics_heap               - Free RAM Heap (KB tag)
 * - diagnostics_uptime             - System Uptime (Hours tag)
 * - diagnostics_reconnects         - WiFi Reconnect Counter tag
 * 
 * Actions Callable from Homey Flows ("THEN" cards) & Web UI:
 * - reboot                         - Remotely reboots the Wemos D1 Mini
 * - reset_stats                    - Clears WiFi reconnect & I2C error counters
 * - set_stealth_mode               - Sets stealth LED mode ("true"/"false" or "1"/"0")
 * - toggle_stealth_mode            - Toggles stealth mode on/off
 * 
 * Dependencies (install via Arduino Library Manager):
 * - Homeyduino (by Athom)
 * - Adafruit SHT31 Library (by Adafruit)
 * - ArduinoOTA (included in ESP8266 core)
 * 
 * Credentials are kept in a separate file "secrets.h".
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Homey.h>
#include <Adafruit_SHT31.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <math.h>

// Enable internal VCC rail measurement on ESP8266 ADC
ADC_MODE(ADC_VCC);

// Include the local secrets file containing WiFi SSID, password and optional DEVICE_NAME
#include "secrets.h"

// --- EEPROM Memory Layout ---
#define EEPROM_SIZE 64
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_MAGIC_BYTE 0x5A                      // Indicates valid custom name stored in EEPROM
#define EEPROM_NAME_ADDR 1
#define MAX_NAME_LEN 31

// --- Configuration & Wine Storage Constants ---
const char* BASE_DEVICE_NAME = "Wemos-SHT30";
char deviceName[32];                                // Active hostname populated at boot
const unsigned long MEASURE_INTERVAL = 15000;       // Sensor measurement interval in ms (15 seconds)
const unsigned long DIAGNOSTIC_INTERVAL = 60000;    // Diagnostic reporting interval in ms (60 seconds)

// Wine Preservation Thresholds
const float WINE_TEMP_MIN = 8.0f;                   // Lower warning limit in °C (Optimal is 10-14°C)
const float WINE_TEMP_MAX = 16.0f;                  // Upper warning limit in °C (Avoid > 18°C)
const float CORK_DRY_RH_THRESHOLD = 50.0f;          // Critical cork dryness threshold in % RH
const float MOLD_RH_THRESHOLD = 75.0f;              // Mold growth threshold in % RH
const float CONDENSATION_TEMP_MARGIN = 2.0f;        // Margin above dew point in °C
const float FREEZE_ALERT_TEMP = 3.0f;               // Critical freeze protection in °C
const float RAPID_TEMP_RISE_THRESHOLD = 1.5f;       // Rapid temp rise alert threshold in °C (5 min window)

// Thermal Model: 750ml Wine Bottle Thermal Time Constant (tau ≈ 3.5 hours = 12600 seconds)
const float THERMAL_TIME_CONSTANT_SEC = 12600.0f;

// --- Web Server on Port 80 ---
ESP8266WebServer server(80);

// --- Globals ---
Adafruit_SHT31 sht30 = Adafruit_SHT31();
unsigned long lastMeasureTime = 0;
unsigned long lastDiagnosticTime = 0;
bool sensorHealthy = false;
bool wasConnected = false;
bool bootTriggerSent = false;
bool stealthMode = false;                           // When true, disables status LED during normal operation
bool customNameFromEeprom = false;

// Diagnostic Statistics & Counters
uint32_t wifiReconnectCount = 0;
uint32_t i2cErrorCount = 0;

// Current Sensor, Wine & Climate Readings
float currentTemp = 0.0f;
float currentLiquidTemp = 0.0f;
float currentHum = 0.0f;
float currentDewPoint = 0.0f;
float currentDewPointSpread = 0.0f;
float currentAbsHum = 0.0f;
float currentVpd = 0.0f;
bool currentCorkDryRisk = false;
bool currentMoldRisk = false;
bool currentCondensationRisk = false;
bool currentFreezeRisk = false;
bool currentWineTempLow = false;
bool currentWineTempHigh = false;
bool currentRapidTempRise = false;
String currentWineStatus = "Starting...";

// Ring buffer for rapid temperature rise detection (5 minutes = 20 samples at 15s interval)
const int TEMP_HISTORY_SIZE = 20;
float tempHistory[TEMP_HISTORY_SIZE];
int tempHistoryIndex = 0;
bool tempHistoryFilled = false;

// --- System State & LED Diagnostic Codes ---
enum SystemState {
  STATE_OK,                 // Solid ON (or OFF if stealth mode active)
  STATE_CONNECTING_WIFI,    // Fast Blink (100ms ON / 100ms OFF)
  STATE_WIFI_LOST,          // Slow Blink (500ms ON / 500ms OFF)
  STATE_SENSOR_FAULT,       // Double Flash (* * ___)
  STATE_OTA_UPDATING        // Rapid Strobe (50ms ON / 50ms OFF)
};

SystemState currentState = STATE_CONNECTING_WIFI;

// Track last sent values to minimize redundant network transmissions
float lastTemp = -999.0f;
float lastLiquidTemp = -999.0f;
float lastHum = -999.0f;
float lastDewPoint = -999.0f;
float lastDewPointSpread = -999.0f;
float lastAbsHum = -999.0f;
float lastVpd = -999.0f;
int lastCorkDryRisk = -1;
int lastMoldRisk = -1;
int lastCondensationRisk = -1;
int lastFreezeRisk = -1;
int lastWineTempLow = -1;
int lastWineTempHigh = -1;
int lastRapidTempRise = -1;
int lastSensorFault = -1;
String lastWineStatus = "";

// Diagnostics cache
int lastRssi = 999;
float lastVcc = -999.0f;
uint32_t lastFreeHeap = 0;

// --- EEPROM Device Name Management & mDNS Sanitization ---

/**
 * Sanitizes a raw device name to comply with standard mDNS/DNS hostnames:
 * - Allows alphanumeric characters [A-Za-z0-9]
 * - Automatically converts underscores (_), spaces, dots (.), and slashes (/) to hyphens (-)
 * - Removes consecutive and trailing hyphens
 */
String sanitizeDeviceName(const String& raw) {
  String clean = "";
  for (unsigned int i = 0; i < raw.length() && clean.length() < MAX_NAME_LEN; i++) {
    char c = raw.charAt(i);
    if (isalnum(c)) {
      clean += c;
    } else if (c == '_' || c == ' ' || c == '.' || c == '/' || c == '-') {
      // Avoid leading hyphens and duplicate consecutive hyphens
      if (clean.length() > 0 && clean.charAt(clean.length() - 1) != '-') {
        clean += '-';
      }
    }
  }
  // Trim trailing hyphens
  while (clean.endsWith("-")) {
    clean.remove(clean.length() - 1);
  }
  return clean;
}

void initDeviceName() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);

  if (magic == EEPROM_MAGIC_BYTE) {
    char storedName[32];
    for (int i = 0; i < MAX_NAME_LEN; i++) {
      storedName[i] = (char)EEPROM.read(EEPROM_NAME_ADDR + i);
      if (storedName[i] == '\0') break;
    }
    storedName[MAX_NAME_LEN] = '\0';

    String sanitizedStored = sanitizeDeviceName(String(storedName));
    if (sanitizedStored.length() > 0) {
      strncpy(deviceName, sanitizedStored.c_str(), sizeof(deviceName) - 1);
      deviceName[sizeof(deviceName) - 1] = '\0';
      customNameFromEeprom = true;
      Serial.println("[Name Manager] Loaded custom name from EEPROM: " + String(deviceName));
      return;
    }
  }

  // 2. Priority: Defined in secrets.h (auto-sanitized)
#ifdef DEVICE_NAME
  String sanitizedSecret = sanitizeDeviceName(String(DEVICE_NAME));
  if (sanitizedSecret.length() > 0) {
    strncpy(deviceName, sanitizedSecret.c_str(), sizeof(deviceName) - 1);
    deviceName[sizeof(deviceName) - 1] = '\0';
    Serial.println("[Name Manager] Using sanitized name from secrets.h: " + String(deviceName));
    return;
  }
#endif

  // 3. Fallback: Auto-generated from Chip ID
  uint32_t chipId = ESP.getChipId();
  snprintf(deviceName, sizeof(deviceName), "%s-%04X", BASE_DEVICE_NAME, (uint16_t)(chipId & 0xFFFF));
  Serial.println("[Name Manager] Using auto-generated fallback name: " + String(deviceName));
}

void saveCustomDeviceName(const String& newName) {
  String cleanName = sanitizeDeviceName(newName);

  if (cleanName.length() == 0) return;

  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_BYTE);
  for (unsigned int i = 0; i < cleanName.length(); i++) {
    EEPROM.write(EEPROM_NAME_ADDR + i, cleanName.charAt(i));
  }
  EEPROM.write(EEPROM_NAME_ADDR + cleanName.length(), '\0');
  EEPROM.commit();
  Serial.println("[Name Manager] Saved sanitized custom name to EEPROM: " + cleanName);
}

void resetDeviceNameEeprom() {
  EEPROM.write(EEPROM_MAGIC_ADDR, 0xFF);
  EEPROM.commit();
  Serial.println("[Name Manager] Reset custom name in EEPROM to default.");
}

// --- Web UI HTML Template (PROGMEM flash) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>%DEVICE_NAME% &bull; Wine Cellar Dashboard</title>
  <style>
    :root {
      --bg: #090d16; --card: #151e2e; --card-border: #24344d;
      --text: #f8fafc; --muted: #94a3b8; --accent: #e11d48; --accent-glow: rgba(225,29,72,0.25);
      --wine: #be123c; --wine-bg: rgba(190,18,60,0.15);
      --success: #10b981; --warning: #f59e0b; --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; }
    .container { max-width: 820px; margin: 0 auto; }
    
    header { display: flex; justify-content: space-between; align-items: center; padding: 14px 0 20px; border-bottom: 1px solid var(--card-border); margin-bottom: 20px; }
    .title-group h1 { font-size: 1.35rem; font-weight: 700; color: var(--text); display: flex; align-items: center; gap: 8px; }
    .title-group span { font-size: 0.82rem; color: var(--muted); }
    .status-badge { display: flex; align-items: center; gap: 6px; font-size: 0.75rem; background: #064e3b; color: #6ee7b7; padding: 5px 12px; border-radius: 999px; font-weight: 600; }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; background: #10b981; animation: pulse 2s infinite; }
    @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.4; } 100% { opacity: 1; } }
    
    .wine-banner { background: var(--wine-bg); border: 1px solid rgba(225,29,72,0.4); border-radius: 12px; padding: 12px 18px; margin-bottom: 18px; display: flex; justify-content: space-between; align-items: center; }
    .wine-banner-text { font-size: 0.95rem; font-weight: 600; color: #fecdd3; display: flex; align-items: center; gap: 8px; }
    .wine-tag { background: #881337; color: #ffe4e6; font-size: 0.75rem; padding: 4px 10px; border-radius: 6px; font-weight: 700; text-transform: uppercase; }

    .nav-tabs { display: flex; gap: 8px; margin-bottom: 20px; background: #0c121e; padding: 4px; border-radius: 10px; border: 1px solid var(--card-border); }
    .tab-btn { flex: 1; padding: 10px 16px; background: transparent; border: none; color: var(--muted); font-size: 0.9rem; font-weight: 600; border-radius: 8px; cursor: pointer; transition: all 0.2s; }
    .tab-btn.active { background: var(--card); color: #fda4af; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    
    .tab-pane { display: none; }
    .tab-pane.active { display: block; }
    
    .hero-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px; }
    @media(max-width: 580px) { .hero-grid { grid-template-columns: 1fr; } }
    .hero-card { background: var(--card); border: 1px solid var(--card-border); border-radius: 14px; padding: 20px; text-align: center; position: relative; overflow: hidden; }
    .hero-card.primary { border-color: rgba(225,29,72,0.4); box-shadow: 0 0 20px rgba(190,18,60,0.1); }
    .hero-label { font-size: 0.85rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-weight: 600; margin-bottom: 6px; }
    .hero-value { font-size: 2.85rem; font-weight: 800; color: var(--text); line-height: 1.1; margin-bottom: 6px; }
    .hero-unit { font-size: 1.35rem; font-weight: 500; color: var(--muted); }
    .hero-sub { font-size: 0.85rem; color: #38bdf8; font-weight: 500; }
    
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; margin-bottom: 18px; }
    .card { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 14px 16px; }
    .card-label { font-size: 0.75rem; color: var(--muted); font-weight: 600; margin-bottom: 4px; text-transform: uppercase; }
    .card-value { font-size: 1.15rem; font-weight: 700; color: var(--text); }
    
    .section-title { font-size: 0.92rem; font-weight: 700; color: #f43f5e; margin: 20px 0 12px; text-transform: uppercase; letter-spacing: 0.05em; }
    
    .alarms-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 10px; margin-bottom: 20px; }
    .alarm-pill { display: flex; align-items: center; justify-content: space-between; padding: 10px 14px; border-radius: 10px; background: var(--card); border: 1px solid var(--card-border); font-size: 0.85rem; font-weight: 600; }
    .alarm-pill.ok { border-color: rgba(16,185,129,0.3); color: #6ee7b7; }
    .alarm-pill.alert { background: rgba(239,68,68,0.18); border-color: var(--danger); color: #fca5a5; }
    .alarm-pill.warn { background: rgba(245,158,11,0.18); border-color: var(--warning); color: #fcd34d; }
    
    .config-box { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 18px; margin-bottom: 20px; }
    .input-row { display: flex; gap: 10px; margin-top: 10px; flex-wrap: wrap; }
    .input-field { flex: 1; min-width: 200px; padding: 12px 14px; background: #0c121e; border: 1px solid var(--card-border); border-radius: 8px; color: var(--text); font-size: 0.95rem; }
    .input-field:focus { outline: none; border-color: var(--accent); }
    
    .btn-group { display: flex; gap: 12px; margin-top: 16px; flex-wrap: wrap; }
    .btn { flex: 1; min-width: 150px; padding: 12px 16px; font-size: 0.9rem; font-weight: 600; border-radius: 10px; border: 1px solid var(--card-border); background: var(--card); color: var(--text); cursor: pointer; transition: 0.2s; }
    .btn:hover { background: #24344d; }
    .btn-primary { background: #be123c; border-color: #e11d48; color: #fff; }
    .btn-primary:hover { background: #9f1239; }
    .btn-stealth { border-color: rgba(225,29,72,0.4); color: #fda4af; }
    .btn-stealth.active { background: #881337; color: #fff; }
    .btn-danger { background: rgba(239,68,68,0.15); border-color: rgba(239,68,68,0.4); color: #fca5a5; }
    .btn-danger:hover { background: rgba(239,68,68,0.3); }
    
    footer { text-align: center; margin-top: 30px; font-size: 0.75rem; color: var(--muted); padding-top: 12px; border-top: 1px solid var(--card-border); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="title-group">
        <h1>🍷 <span id="devName">%DEVICE_NAME%</span></h1>
        <span id="devSub">Vestfrost VKG Wine Cabinet Sensor &bull; Homey Pro</span>
      </div>
      <div class="status-badge" id="netBadge">
        <div class="status-dot"></div>
        <span id="netStatus">Online</span>
      </div>
    </header>

    <div class="wine-banner">
      <div class="wine-banner-text">
        <span>🍷 Wine Climate:</span>
        <strong id="wineStatusTxt">Calculating...</strong>
      </div>
      <span class="wine-tag" id="wineTagStatus">Optimal</span>
    </div>

    <div class="nav-tabs">
      <button class="tab-btn active" onclick="switchTab('sensors')">📊 Wine Climate & Telemetry</button>
      <button class="tab-btn" onclick="switchTab('diagnostics')">⚙️ System & Diagnostics</button>
    </div>

    <!-- TAB 1: SENSORS & WINE CLIMATE -->
    <div id="tab-sensors" class="tab-pane active">
      <div class="hero-grid">
        <div class="hero-card primary">
          <div class="hero-label">Simulated Wine Core Temperature</div>
          <div class="hero-value"><span id="liquidTemp">--</span><span class="hero-unit">°C</span></div>
          <div class="hero-sub">Air Temperature: <span id="airTemp">--</span> °C</div>
        </div>
        <div class="hero-card">
          <div class="hero-label">Relative Humidity</div>
          <div class="hero-value"><span id="hum">--</span><span class="hero-unit">%</span></div>
          <div class="hero-sub">Absolute: <span id="absHum">--</span> g/m³</div>
        </div>
      </div>

      <div class="section-title">🚨 Wine Cabinet Alarms & Protection</div>
      <div class="alarms-grid">
        <div class="alarm-pill ok" id="alarmCork">
          <span>🪵 Cork Protection</span>
          <span id="txtCork">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmTempRange">
          <span>🌡️ Temp Range (8-16°C)</span>
          <span id="txtTempRange">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmDoor">
          <span>🚪 Rapid Rise / Door</span>
          <span id="txtDoor">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmCondensation">
          <span>💧 Condensation Risk</span>
          <span id="txtCondensation">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmMold">
          <span>🦠 Mold Control</span>
          <span id="txtMold">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmSensor">
          <span>⚠️ SHT30 Hardware</span>
          <span id="txtSensor">Healthy</span>
        </div>
      </div>

      <div class="section-title">🌿 Psychrometric Telemetry</div>
      <div class="grid">
        <div class="card">
          <div class="card-label">Dew Point</div>
          <div class="card-value"><span id="dewPoint">--</span> °C</div>
        </div>
        <div class="card">
          <div class="card-label">Dew Point Margin (Spread)</div>
          <div class="card-value"><span id="dewSpread">--</span> °C</div>
        </div>
        <div class="card">
          <div class="card-label">Vapor Pressure Deficit (VPD)</div>
          <div class="card-value"><span id="vpd">--</span> kPa</div>
        </div>
      </div>
    </div>

    <!-- TAB 2: SYSTEM DIAGNOSTICS & CONTROL -->
    <div id="tab-diagnostics" class="tab-pane">
      
      <div class="section-title">🏷️ Device Name & Configuration</div>
      <div class="config-box">
        <div class="card-label">Custom Device Name (Stored in EEPROM Flash)</div>
        <div style="font-size:0.8rem;color:var(--muted);margin-bottom:8px;">Changing the device name updates the mDNS hostname (<span id="txtMdns">name.local</span>). Pair with Homey after setting the desired name.</div>
        <div class="input-row">
          <input type="text" id="inputDevName" class="input-field" maxlength="30" placeholder="e.g. WineCellar, WineCabinet...">
          <button class="btn btn-primary" onclick="saveCustomName()">💾 Save & Reboot</button>
          <button class="btn" onclick="resetDefaultName()">🔄 Reset Name</button>
        </div>
      </div>

      <div class="section-title">🛠️ Controls & Modes</div>
      <div class="btn-group">
        <button class="btn btn-stealth" id="btnStealth" onclick="toggleStealth()">🌙 Stealth Mode (LED Off)</button>
        <button class="btn" onclick="resetStats()">🧹 Clear Counters</button>
        <button class="btn btn-danger" onclick="rebootDevice()">🔄 Reboot Node</button>
      </div>

      <div class="section-title">📶 Wireless Network & Signal</div>
      <div class="grid">
        <div class="card">
          <div class="card-label">WiFi Network (SSID)</div>
          <div class="card-value" id="wifiSsid">--</div>
        </div>
        <div class="card">
          <div class="card-label">Signal Strength (RSSI)</div>
          <div class="card-value"><span id="wifiRssi">--</span> dBm</div>
        </div>
        <div class="card">
          <div class="card-label">IP Address</div>
          <div class="card-value" id="netIp">--</div>
        </div>
        <div class="card">
          <div class="card-label">WiFi Reconnects</div>
          <div class="card-value" id="reconnectCount">0</div>
        </div>
      </div>

      <div class="section-title">⚡ Hardware & Resources</div>
      <div class="grid">
        <div class="card">
          <div class="card-label">System Uptime</div>
          <div class="card-value" id="uptime">--</div>
        </div>
        <div class="card">
          <div class="card-label">Supply Rail Voltage (Vcc)</div>
          <div class="card-value"><span id="vcc">--</span> V</div>
        </div>
        <div class="card">
          <div class="card-label">Free RAM (Heap)</div>
          <div class="card-value"><span id="freeHeap">--</span> KB</div>
        </div>
        <div class="card">
          <div class="card-label">I2C Read Errors</div>
          <div class="card-value" id="i2cErrors">0</div>
        </div>
      </div>
    </div>

    <footer>
      Wemos D1 Mini Wine Node &bull; Athom Homey Homeyduino &bull; <a href="/api/data" target="_blank" style="color:#fda4af;text-decoration:none;">JSON API (/api/data)</a>
    </footer>
  </div>

  <script>
    let currentLoadedName = '';

    function switchTab(tabId) {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
      if(tabId === 'sensors') {
        document.querySelectorAll('.tab-btn')[0].classList.add('active');
        document.getElementById('tab-sensors').classList.add('active');
      } else {
        document.querySelectorAll('.tab-btn')[1].classList.add('active');
        document.getElementById('tab-diagnostics').classList.add('active');
      }
    }

    function setPill(elemId, txtId, isAlert, alertText, okText, isWarn) {
      const el = document.getElementById(elemId);
      const txt = document.getElementById(txtId);
      if(isAlert) {
        el.className = isWarn ? 'alarm-pill warn' : 'alarm-pill alert';
        txt.textContent = alertText;
      } else {
        el.className = 'alarm-pill ok';
        txt.textContent = okText;
      }
    }

    async function fetchData() {
      try {
        const res = await fetch('/api/data');
        if(!res.ok) return;
        const d = await res.json();

        // Header & Name
        document.getElementById('devName').textContent = d.device;
        document.getElementById('txtMdns').textContent = d.device.toLowerCase() + '.local';
        if (!currentLoadedName) {
          document.getElementById('inputDevName').value = d.device;
          currentLoadedName = d.device;
        }

        document.getElementById('wineStatusTxt').textContent = d.wine_status;
        document.getElementById('wineTagStatus').textContent = d.cork_dry_risk ? 'DRY AIR' : (d.wine_temp_low || d.wine_temp_high ? 'TEMP ALERT' : 'OPTIMAL');

        // Hero Values
        document.getElementById('liquidTemp').textContent = d.liquid_temp.toFixed(1);
        document.getElementById('airTemp').textContent = d.temp.toFixed(1);
        document.getElementById('hum').textContent = d.hum.toFixed(1);
        document.getElementById('absHum').textContent = d.abs_hum.toFixed(2);
        
        // Psychrometric
        document.getElementById('dewPoint').textContent = d.dew_point.toFixed(1);
        document.getElementById('dewSpread').textContent = d.dew_point_spread.toFixed(1);
        document.getElementById('vpd').textContent = d.vpd.toFixed(2);

        // Alarms
        setPill('alarmCork', 'txtCork', d.cork_dry_risk, 'DRY AIR (<50%)', 'Safe', true);
        setPill('alarmTempRange', 'txtTempRange', (d.wine_temp_low || d.wine_temp_high), d.wine_temp_high ? 'TOO WARM (>16°C)' : 'TOO COLD (<8°C)', 'OK (8-16°C)', false);
        setPill('alarmDoor', 'txtDoor', d.rapid_temp_rise, 'TEMP SPIKE (>1.5°C)', 'Stable', false);
        setPill('alarmCondensation', 'txtCondensation', d.condensation_risk, 'CONDENSATION', 'OK', false);
        setPill('alarmMold', 'txtMold', d.mold_risk, 'HIGH RH (>75%)', 'OK', false);
        setPill('alarmSensor', 'txtSensor', !d.sensor_healthy, 'I2C FAULT', 'Healthy', false);

        // Diagnostics
        document.getElementById('wifiSsid').textContent = d.ssid;
        document.getElementById('wifiRssi').textContent = d.rssi;
        document.getElementById('netIp').textContent = d.ip;
        document.getElementById('reconnectCount').textContent = d.wifi_reconnects;
        document.getElementById('uptime').textContent = d.uptime_str;
        document.getElementById('vcc').textContent = d.vcc.toFixed(2);
        document.getElementById('freeHeap').textContent = (d.free_heap / 1024).toFixed(1);
        document.getElementById('i2cErrors').textContent = d.i2c_errors;

        // Stealth button state
        const sBtn = document.getElementById('btnStealth');
        if(d.stealth_mode) {
          sBtn.className = 'btn btn-stealth active';
          sBtn.textContent = '🌙 Stealth Mode: ON (LED Off)';
        } else {
          sBtn.className = 'btn btn-stealth';
          sBtn.textContent = '💡 Stealth Mode: OFF (LED On)';
        }
      } catch(e) {
        console.error('Fetch error:', e);
      }
    }

    async function saveCustomName() {
      const name = document.getElementById('inputDevName').value.trim();
      if (!name) {
        alert('Please enter a valid device name!');
        return;
      }
      if (confirm(`Save new device name "${name}" and reboot Wemos?`)) {
        await fetch('/api/set_name', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'name=' + encodeURIComponent(name)
        });
        alert(`Name "${name}" saved! Device is rebooting now.`);
        setTimeout(() => location.reload(), 6000);
      }
    }

    async function resetDefaultName() {
      if (confirm('Reset device name to default and reboot?')) {
        await fetch('/api/reset_name', { method: 'POST' });
        alert('Device name reset! Rebooting now.');
        setTimeout(() => location.reload(), 6000);
      }
    }

    async function toggleStealth() {
      await fetch('/api/toggle_stealth', { method: 'POST' });
      fetchData();
    }

    async function rebootDevice() {
      if(confirm('Are you sure you want to reboot the Wemos?')) {
        await fetch('/api/reboot', { method: 'POST' });
        alert('Reboot command sent!');
        setTimeout(() => location.reload(), 6000);
      }
    }

    async function resetStats() {
      if(confirm('Reset diagnostic counters?')) {
        await fetch('/api/reset_stats', { method: 'POST' });
        fetchData();
      }
    }

    fetchData();
    setInterval(fetchData, 3000);
  </script>
</body>
</html>
)rawliteral";

// --- Helper: Format Uptime as String ---
String formatUptime(unsigned long totalSeconds) {
  unsigned long days = totalSeconds / 86400;
  unsigned int hours = (totalSeconds % 86400) / 3600;
  unsigned int minutes = (totalSeconds % 3600) / 60;
  unsigned int seconds = totalSeconds % 60;

  char buf[32];
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lud %uh %um %us", days, hours, minutes, seconds);
  } else if (hours > 0) {
    snprintf(buf, sizeof(buf), "%uh %um %us", hours, minutes, seconds);
  } else {
    snprintf(buf, sizeof(buf), "%um %us", minutes, seconds);
  }
  return String(buf);
}

// --- Web Server Route Handlers ---

void handleRoot() {
  String html = FPSTR(INDEX_HTML);
  html.replace("%DEVICE_NAME%", deviceName);
  server.send(200, "text/html", html);
}

void handleApiData() {
  unsigned long nowSec = millis() / 1000;
  float vcc = ESP.getVcc() / 1000.0f;

  String json = "{";
  json += "\"device\":\"" + String(deviceName) + "\",";
  json += "\"custom_name\":" + String(customNameFromEeprom ? "true" : "false") + ",";
  json += "\"uptime_sec\":" + String(nowSec) + ",";
  json += "\"uptime_str\":\"" + formatUptime(nowSec) + "\",";
  json += "\"temp\":" + String(currentTemp, 1) + ",";
  json += "\"liquid_temp\":" + String(currentLiquidTemp, 1) + ",";
  json += "\"hum\":" + String(currentHum, 1) + ",";
  json += "\"dew_point\":" + String(currentDewPoint, 1) + ",";
  json += "\"dew_point_spread\":" + String(currentDewPointSpread, 1) + ",";
  json += "\"abs_hum\":" + String(currentAbsHum, 2) + ",";
  json += "\"vpd\":" + String(currentVpd, 2) + ",";
  json += "\"wine_status\":\"" + currentWineStatus + "\",";
  json += "\"cork_dry_risk\":" + String(currentCorkDryRisk ? "true" : "false") + ",";
  json += "\"wine_temp_low\":" + String(currentWineTempLow ? "true" : "false") + ",";
  json += "\"wine_temp_high\":" + String(currentWineTempHigh ? "true" : "false") + ",";
  json += "\"rapid_temp_rise\":" + String(currentRapidTempRise ? "true" : "false") + ",";
  json += "\"mold_risk\":" + String(currentMoldRisk ? "true" : "false") + ",";
  json += "\"condensation_risk\":" + String(currentCondensationRisk ? "true" : "false") + ",";
  json += "\"freeze_risk\":" + String(currentFreezeRisk ? "true" : "false") + ",";
  json += "\"stealth_mode\":" + String(stealthMode ? "true" : "false") + ",";
  json += "\"sensor_healthy\":" + String(sensorHealthy ? "true" : "false") + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"vcc\":" + String(vcc, 2) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"heap_frag\":" + String(ESP.getHeapFragmentation()) + ",";
  json += "\"wifi_reconnects\":" + String(wifiReconnectCount) + ",";
  json += "\"i2c_errors\":" + String(i2cErrorCount) + ",";
  json += "\"reset_reason\":\"" + ESP.getResetReason() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"ssid\":\"" + String(SECRET_SSID) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleApiSetName() {
  if (server.hasArg("name")) {
    String newName = server.arg("name");
    saveCustomDeviceName(newName);
    server.send(200, "application/json", "{\"status\":\"ok\",\"rebooting\":true}");
    Serial.println("\n[Web Server] Custom device name updated! Rebooting...");
    delay(500);
    ESP.restart();
  } else {
    server.send(400, "application/json", "{\"error\":\"Missing 'name' argument\"}");
  }
}

void handleApiResetName() {
  resetDeviceNameEeprom();
  server.send(200, "application/json", "{\"status\":\"ok\",\"rebooting\":true}");
  Serial.println("\n[Web Server] Device name reset to default! Rebooting...");
  delay(500);
  ESP.restart();
}

void handleApiToggleStealth() {
  stealthMode = !stealthMode;
  Serial.println("[Web Server] Stealth Mode toggled: " + String(stealthMode ? "ON" : "OFF"));
  server.send(200, "application/json", "{\"stealth_mode\":" + String(stealthMode ? "true" : "false") + "}");
}

void handleApiReboot() {
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  Serial.println("\n[Web Server] Remote reboot requested from web browser!");
  delay(500);
  ESP.restart();
}

void handleApiResetStats() {
  wifiReconnectCount = 0;
  i2cErrorCount = 0;
  Serial.println("\n[Web Server] Diagnostic counters reset from web browser!");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// --- LED Diagnostic Controller ---

void setLedHardware(bool on) {
  // Built-in LED on ESP8266 is active LOW
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

void updateLed() {
  unsigned long now = millis();

  // In Stealth Mode, suppress LED during normal OK operation
  if (stealthMode && currentState == STATE_OK) {
    setLedHardware(false);
    return;
  }

  switch (currentState) {
    case STATE_OK:
      setLedHardware(true);
      break;

    case STATE_CONNECTING_WIFI:
      setLedHardware((now / 100) % 2 == 0);
      break;

    case STATE_WIFI_LOST:
      setLedHardware((now / 500) % 2 == 0);
      break;

    case STATE_SENSOR_FAULT:
      {
        unsigned long phase = now % 1000;
        if (phase < 100 || (phase >= 200 && phase < 300)) {
          setLedHardware(true);
        } else {
          setLedHardware(false);
        }
      }
      break;

    case STATE_OTA_UPDATING:
      setLedHardware((now / 50) % 2 == 0);
      break;
  }
}

// --- Psychrometric & Thermal Calculation Helpers ---

float calculateDewPoint(float temp, float hum) {
  if (hum <= 0.0f) return temp;
  const float a = 17.27f;
  const float b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + log(hum / 100.0f);
  return (b * alpha) / (a - alpha);
}

float calculateAbsoluteHumidity(float temp, float hum) {
  if (hum <= 0.0f) return 0.0f;
  float satVaporPressure_hPa = 6.112f * exp((17.67f * temp) / (temp + 243.5f));
  float actualVaporPressure_hPa = (hum / 100.0f) * satVaporPressure_hPa;
  return (216.7f * actualVaporPressure_hPa) / (273.15f + temp);
}

float calculateVPD(float temp, float hum) {
  float vpsat_kPa = 0.61078f * exp((17.27f * temp) / (temp + 237.3f));
  float vpact_kPa = vpsat_kPa * (hum / 100.0f);
  float vpd = vpsat_kPa - vpact_kPa;
  return (vpd < 0.0f) ? 0.0f : vpd;
}

// --- Homey Remote Actions ("THEN" Flow Cards) ---

void handleRemoteReboot() {
  Serial.println("\n[Homey Action] >>> Remote Reboot Triggered from Homey! <<<");
  delay(200);
  ESP.restart();
}

void handleResetStats() {
  Serial.println("\n[Homey Action] >>> Diagnostic Counters Reset from Homey! <<<");
  wifiReconnectCount = 0;
  i2cErrorCount = 0;
}

void handleSetStealthMode() {
  String val = Homey.value;
  val.toLowerCase();
  if (val == "true" || val == "1" || val == "on" || val == "yes" || val == "ja") {
    stealthMode = true;
  } else {
    stealthMode = false;
  }
  Serial.println("\n[Homey Action] >>> Set Stealth Mode: " + String(stealthMode ? "ON" : "OFF") + " <<<");
}

void handleToggleStealthMode() {
  stealthMode = !stealthMode;
  Serial.println("\n[Homey Action] >>> Toggled Stealth Mode: " + String(stealthMode ? "ON" : "OFF") + " <<<");
}

// --- Network & Service Initializations ---

void setupWiFi() {
  Serial.println();
  Serial.print("Connecting to WiFi network: ");
  Serial.println(SECRET_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_SSID, SECRET_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    currentState = STATE_CONNECTING_WIFI;
    updateLed();
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 40) {
      Serial.println("\nWiFi connection failed! Rebooting...");
      ESP.restart();
    }
  }

  wasConnected = true;
  Serial.println();
  Serial.println("WiFi successfully connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Signal Strength (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void setupMDNS() {
  Serial.print("Starting mDNS responder... ");
  if (MDNS.begin(deviceName)) {
    Serial.println("OK");
    Serial.print("  ├─ Hostname: ");
    Serial.print(deviceName);
    Serial.println(".local");
    Serial.print("  ├─ IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("  └─ Web UI URL: http://" + String(deviceName) + ".local");
  } else {
    Serial.println("FAILED (Error initializing mDNS responder)");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/set_name", HTTP_POST, handleApiSetName);
  server.on("/api/reset_name", HTTP_POST, handleApiResetName);
  server.on("/api/toggle_stealth", HTTP_POST, handleApiToggleStealth);
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/reset_stats", HTTP_POST, handleApiResetStats);
  server.begin();
  Serial.println("HTTP Web Server started on port 80 (http://" + WiFi.localIP().toString() + "/)");
}

void setupOTA() {
  ArduinoOTA.setHostname(deviceName);

  #ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
  #endif

  ArduinoOTA.onStart([]() {
    currentState = STATE_OTA_UPDATING;
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("\n[OTA] Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete! Rebooting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    updateLed();
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA service ready (Port: 8266, Hostname: " + String(deviceName) + ")");
}

// Forward declarations
void readAndReportSensor();
void reportDiagnostics();

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  currentState = STATE_CONNECTING_WIFI;
  updateLed();

  Serial.begin(115200);
  delay(100);

  // Initialize and load device name (EEPROM -> secrets.h -> ChipID fallback)
  initDeviceName();

  Serial.println("\n=============================================");
  Serial.println("   Wemos D1 Mini SHT30 Wine Climate & Diagnostics Node   ");
  Serial.println("=============================================");
  Serial.print("Device Hostname / ID: ");
  Serial.println(deviceName);
  Serial.print("Reset Reason:         ");
  Serial.println(ESP.getResetReason());
  Serial.print("Free Heap at Boot:    ");
  Serial.print(ESP.getFreeHeap() / 1024.0f, 1);
  Serial.println(" KB");

  // Connect to WiFi
  setupWiFi();

  // Initialize mDNS
  setupMDNS();

  // Initialize Embedded Web Server
  setupWebServer();

  // Initialize OTA update service
  setupOTA();

  // Initialize Homeyduino
  Serial.println("Initializing Homeyduino...");
  Homey.begin(deviceName);

  // Register Homey capabilities
  Homey.addCapability("measure_temperature");           // Ambient Air Temperature (°C)
  Homey.addCapability("measure_temperature.liquid");    // Simulated Wine Bottle Core Temperature (°C)
  Homey.addCapability("measure_humidity");              // Relative Humidity (%)
  Homey.addCapability("measure_signal_strength");       // WiFi Signal Strength (dBm)
  Homey.addCapability("measure_voltage");               // ESP8266 Internal Vcc Rail (V)
  Homey.addCapability("alarm_water");                   // Homey UI: "Vannalarm" -> Condensation Risk
  Homey.addCapability("alarm_generic");                 // Homey UI: "Generell alarm" -> Mold Risk
  Homey.addCapability("alarm_tamper");                  // Homey UI: "Sabotasjealarm" -> SHT30 Hardware/I2C Fault

  // Register Homey Flow Actions ("THEN" cards)
  Homey.addAction("reboot", handleRemoteReboot);
  Homey.addAction("reset_stats", handleResetStats);
  Homey.addAction("set_stealth_mode", handleSetStealthMode);
  Homey.addAction("toggle_stealth_mode", handleToggleStealthMode);
  
  Serial.println("Capabilities registered on Homey Device Tile:");
  Serial.println(" - measure_temperature        (Air Temp °C)");
  Serial.println(" - measure_temperature.liquid (Wine Bottle Core Temp °C)");
  Serial.println(" - measure_humidity           (Rel Humidity %)");
  Serial.println(" - measure_signal_strength    (WiFi RSSI dBm)");
  Serial.println(" - measure_voltage            (ESP8266 Vcc V)");
  Serial.println(" - alarm_water                (Condensation Risk)");
  Serial.println(" - alarm_generic              (Mold Risk)");
  Serial.println(" - alarm_tamper               (SHT30 Hardware Fault)");

  // Initialize I2C and SHT30 Sensor
  Wire.begin();
  if (!sht30.begin(0x45)) {
    sensorHealthy = false;
    i2cErrorCount++;
    Serial.println("ERROR: SHT30 sensor shield not found at I2C address 0x45!");
  } else {
    sensorHealthy = true;
    Serial.println("SHT30 sensor successfully initialized.");
  }
  
  // Read sensor once immediately on boot
  readAndReportSensor();
  Serial.println("Initialization complete. Wine monitoring loops running.\n");
}

void loop() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // Track WiFi disconnects / reconnects
  if (wasConnected && !isConnected) {
    wasConnected = false;
    currentState = STATE_WIFI_LOST;
  } else if (!wasConnected && isConnected) {
    wasConnected = true;
    wifiReconnectCount++;
    Serial.println("[WiFi] Reconnected to network! Total reconnects: " + String(wifiReconnectCount));
  }

  // Evaluate LED system state
  if (currentState != STATE_OTA_UPDATING) {
    if (!isConnected) {
      currentState = STATE_WIFI_LOST;
    } else if (!sensorHealthy) {
      currentState = STATE_SENSOR_FAULT;
    } else {
      currentState = STATE_OK;
    }
  }
  updateLed();

  // Handle incoming HTTP Web Requests
  server.handleClient();

  // Keep mDNS service updated
  MDNS.update();

  // Handle incoming OTA updates
  ArduinoOTA.handle();

  // Process incoming Homeyduino network requests
  Homey.loop();

  // Send single on-boot flow trigger once Homey connection is active
  if (isConnected && !bootTriggerSent) {
    String resetInfo = ESP.getResetReason();
    Homey.trigger("device_booted", resetInfo);
    bootTriggerSent = true;
    Serial.println("[Homey] Emitted 'device_booted' trigger with reason: " + resetInfo);
  }

  unsigned long currentMillis = millis();

  // Periodic sensor measurement loop (every 15 seconds)
  if (currentMillis - lastMeasureTime >= MEASURE_INTERVAL || lastMeasureTime == 0) {
    lastMeasureTime = currentMillis;
    readAndReportSensor();
  }

  // Periodic system diagnostic loop (every 60 seconds)
  if (currentMillis - lastDiagnosticTime >= DIAGNOSTIC_INTERVAL || lastDiagnosticTime == 0) {
    lastDiagnosticTime = currentMillis;
    reportDiagnostics();
  }
}

/**
 * Reads SHT30 sensor and reports environmental & wine cellar readings to Homey & Web UI
 */
void readAndReportSensor() {
  Serial.print("[");
  Serial.print(millis() / 1000);
  Serial.print("s] Reading SHT30 sensor... ");

  float temp = sht30.readTemperature();
  float hum = sht30.readHumidity();

  // Check if readings are valid
  if (!isnan(temp) && !isnan(hum)) {
    sensorHealthy = true;

    // First run initialization of liquid temperature
    if (lastLiquidTemp == -999.0f) {
      currentLiquidTemp = temp;
    } else {
      // PT1 Low-pass thermal filter simulating 750ml bottle core temp
      // dt = 15s, tau = 12600s => alpha = dt / (tau + dt)
      float dt = (float)MEASURE_INTERVAL / 1000.0f;
      float alpha = dt / (THERMAL_TIME_CONSTANT_SEC + dt);
      currentLiquidTemp += alpha * (temp - currentLiquidTemp);
    }

    // Update global state variables
    currentTemp = temp;
    currentHum = hum;
    currentDewPoint = calculateDewPoint(temp, hum);
    currentDewPointSpread = temp - currentDewPoint;
    currentAbsHum = calculateAbsoluteHumidity(temp, hum);
    currentVpd = calculateVPD(temp, hum);

    // Wine preservation risk evaluations
    currentCorkDryRisk = (hum < CORK_DRY_RH_THRESHOLD);
    currentMoldRisk = (hum >= MOLD_RH_THRESHOLD);
    currentCondensationRisk = (currentDewPointSpread <= CONDENSATION_TEMP_MARGIN);
    currentFreezeRisk = (temp <= FREEZE_ALERT_TEMP);
    currentWineTempLow = (temp < WINE_TEMP_MIN);
    currentWineTempHigh = (temp > WINE_TEMP_MAX);

    // Rapid temperature rise detection (Ring buffer over last 5 minutes)
    tempHistory[tempHistoryIndex] = temp;
    tempHistoryIndex = (tempHistoryIndex + 1) % TEMP_HISTORY_SIZE;
    if (tempHistoryIndex == 0) tempHistoryFilled = true;

    float deltaTemp = 0.0f;
    if (tempHistoryFilled) {
      // Oldest value is currently at tempHistoryIndex
      float oldestTemp = tempHistory[tempHistoryIndex];
      deltaTemp = temp - oldestTemp;
      currentRapidTempRise = (deltaTemp >= RAPID_TEMP_RISE_THRESHOLD);
    } else {
      currentRapidTempRise = false;
    }

    // Determine comprehensive wine climate status
    if (currentRapidTempRise) {
      currentWineStatus = "Warning: Rapid temperature rise (Door open / Cooling failure)";
    } else if (currentCorkDryRisk) {
      currentWineStatus = "Warning: Dry air (Cork dryness risk < 50%)";
    } else if (currentWineTempHigh) {
      currentWineStatus = "Warning: High temperature (> 16°C)";
    } else if (currentWineTempLow) {
      currentWineStatus = "Warning: Low temperature (< 8°C)";
    } else if (currentMoldRisk) {
      currentWineStatus = "Warning: High humidity (Mold risk > 75%)";
    } else {
      currentWineStatus = "Optimal wine climate (Corks and wine safe)";
    }

    // Print to Serial Monitor
    Serial.println();
    Serial.print("  ├─ Air Temperature:      "); Serial.print(currentTemp, 1); Serial.println(" °C");
    Serial.print("  ├─ Wine Core Temp:       "); Serial.print(currentLiquidTemp, 1); Serial.println(" °C");
    Serial.print("  ├─ Relative Humidity:    "); Serial.print(currentHum, 1); Serial.println(" %");
    Serial.print("  ├─ Dew Point:            "); Serial.print(currentDewPoint, 1); Serial.println(" °C");
    Serial.print("  ├─ Absolute Humidity:    "); Serial.print(currentAbsHum, 2); Serial.println(" g/m³");
    Serial.print("  ├─ VPD:                  "); Serial.print(currentVpd, 2); Serial.println(" kPa");
    Serial.print("  ├─ Cork Protection:      "); Serial.println(currentCorkDryRisk ? "RISK: DRY AIR (<50%)" : "OK (Safe)");
    Serial.print("  ├─ Temp Range Status:    "); Serial.println((currentWineTempLow || currentWineTempHigh) ? "DEVIATION" : "OK (8-16°C)");
    Serial.print("  ├─ Door / Temp Spike:    "); Serial.println(currentRapidTempRise ? "ALERT (>1.5°C rise)" : "Stable");
    Serial.print("  └─ Wine Climate Summary: "); Serial.println(currentWineStatus);

    // 1. Report Standard Capabilities to Homey
    if (abs(temp - lastTemp) >= 0.05f || lastTemp == -999.0f) {
      if (Homey.setCapabilityValue("measure_temperature", temp)) {
        lastTemp = temp;
      }
    }

    if (abs(currentLiquidTemp - lastLiquidTemp) >= 0.05f || lastLiquidTemp == -999.0f) {
      if (Homey.setCapabilityValue("measure_temperature.liquid", currentLiquidTemp)) {
        lastLiquidTemp = currentLiquidTemp;
      }
    }

    if (abs(hum - lastHum) >= 0.1f || lastHum == -999.0f) {
      if (Homey.setCapabilityValue("measure_humidity", hum)) {
        lastHum = hum;
      }
    }

    if (lastCondensationRisk == -1 || ((lastCondensationRisk == 1) != currentCondensationRisk)) {
      if (Homey.setCapabilityValue("alarm_water", currentCondensationRisk)) {
        lastCondensationRisk = currentCondensationRisk ? 1 : 0;
      }
    }

    if (lastMoldRisk == -1 || ((lastMoldRisk == 1) != currentMoldRisk)) {
      if (Homey.setCapabilityValue("alarm_generic", currentMoldRisk)) {
        lastMoldRisk = currentMoldRisk ? 1 : 0;
      }
    }

    if (lastSensorFault != 0) {
      if (Homey.setCapabilityValue("alarm_tamper", false)) {
        lastSensorFault = 0;
      }
    }

    // 2. Emit Wine-Specific Flow Triggers with Local Tokens / Values
    
    // Cork Dryness Triggers
    if (lastCorkDryRisk == -1 || ((lastCorkDryRisk == 1) != currentCorkDryRisk)) {
      lastCorkDryRisk = currentCorkDryRisk ? 1 : 0;
      Homey.trigger("cork_dry_risk", currentCorkDryRisk);
      if (currentCorkDryRisk) {
        Homey.trigger("cork_dry_alert", hum); // Emits current RH as token
      }
    }

    // Wine Temperature Limit Triggers
    if (lastWineTempLow == -1 || ((lastWineTempLow == 1) != currentWineTempLow)) {
      lastWineTempLow = currentWineTempLow ? 1 : 0;
      if (currentWineTempLow) {
        Homey.trigger("wine_temp_low", temp); // Emits temp as token
      }
    }

    if (lastWineTempHigh == -1 || ((lastWineTempHigh == 1) != currentWineTempHigh)) {
      lastWineTempHigh = currentWineTempHigh ? 1 : 0;
      if (currentWineTempHigh) {
        Homey.trigger("wine_temp_high", temp); // Emits temp as token
      }
    }

    // Rapid Temperature Rise / Door Alert Trigger
    if (lastRapidTempRise == -1 || ((lastRapidTempRise == 1) != currentRapidTempRise)) {
      lastRapidTempRise = currentRapidTempRise ? 1 : 0;
      if (currentRapidTempRise) {
        Homey.trigger("rapid_temp_rise", deltaTemp); // Emits deltaTemp as token
      }
    }

    // Freezing Risk Trigger
    if (lastFreezeRisk == -1 || ((lastFreezeRisk == 1) != currentFreezeRisk)) {
      lastFreezeRisk = currentFreezeRisk ? 1 : 0;
      Homey.trigger("freeze_risk", currentFreezeRisk);
      if (currentFreezeRisk) {
        Homey.trigger("frost_alert", temp);
      }
    }

    // Periodic Metric Triggers (Emitted when changed significantly)
    if (abs(currentLiquidTemp - lastLiquidTemp) >= 0.1f) {
      Homey.trigger("liquid_temp", currentLiquidTemp);
    }

    if (currentWineStatus != lastWineStatus) {
      Homey.trigger("wine_status", currentWineStatus);
      lastWineStatus = currentWineStatus;
    }

    if (abs(currentDewPoint - lastDewPoint) >= 0.1f || lastDewPoint == -999.0f) {
      Homey.trigger("dew_point", currentDewPoint);
      lastDewPoint = currentDewPoint;
    }

    if (abs(currentDewPointSpread - lastDewPointSpread) >= 0.1f || lastDewPointSpread == -999.0f) {
      Homey.trigger("dew_point_spread", currentDewPointSpread);
      lastDewPointSpread = currentDewPointSpread;
    }

    if (abs(currentAbsHum - lastAbsHum) >= 0.1f || lastAbsHum == -999.0f) {
      Homey.trigger("absolute_humidity", currentAbsHum);
      lastAbsHum = currentAbsHum;
    }

    if (abs(currentVpd - lastVpd) >= 0.05f || lastVpd == -999.0f) {
      Homey.trigger("vpd", currentVpd);
      lastVpd = currentVpd;
    }

  } else {
    sensorHealthy = false;
    i2cErrorCount++;
    Serial.println("ERROR: Failed to read from SHT30 sensor shield!");

    if (lastSensorFault != 1) {
      if (Homey.setCapabilityValue("alarm_tamper", true)) {
        lastSensorFault = 1;
      }
    }
  }
}

/**
 * Gathers and reports hardware, network, and memory diagnostics to Homey
 */
void reportDiagnostics() {
  if (WiFi.status() != WL_CONNECTED) return;

  // 1. WiFi RSSI (Signal Strength)
  int currentRssi = WiFi.RSSI();
  if (abs(currentRssi - lastRssi) >= 2 || lastRssi == 999) {
    if (Homey.setCapabilityValue("measure_signal_strength", (float)currentRssi)) {
      lastRssi = currentRssi;
    }
  }

  // 2. Internal Vcc Rail Voltage (V)
  float currentVcc = ESP.getVcc() / 1000.0f;
  if (abs(currentVcc - lastVcc) >= 0.03f || lastVcc == -999.0f) {
    if (Homey.setCapabilityValue("measure_voltage", currentVcc)) {
      lastVcc = currentVcc;
    }
  }

  // 3. System Uptime & Heap Statistics
  uint32_t freeHeap = ESP.getFreeHeap();
  uint8_t heapFrag = ESP.getHeapFragmentation();
  float uptimeHours = (float)millis() / 3600000.0f;

  // Emit flow triggers for deep diagnostic metrics
  Homey.trigger("diagnostics_heap", (float)(freeHeap / 1024.0f));
  Homey.trigger("diagnostics_uptime", uptimeHours);
  Homey.trigger("diagnostics_reconnects", (int)wifiReconnectCount);

  // Serial Diagnostics summary
  Serial.println("\n--- [System & Network Diagnostics] ---");
  Serial.print("  ├─ Device Name:          "); Serial.println(deviceName);
  Serial.print("  ├─ WiFi Signal (RSSI):   "); Serial.print(currentRssi); Serial.println(" dBm");
  Serial.print("  ├─ Supply Rail (Vcc):    "); Serial.print(currentVcc, 2); Serial.println(" V");
  Serial.print("  ├─ System Uptime:        "); Serial.print(uptimeHours, 2); Serial.println(" hours");
  Serial.print("  ├─ Free RAM (Heap):      "); Serial.print(freeHeap / 1024.0f, 1); Serial.print(" KB (Frag: "); Serial.print(heapFrag); Serial.println("%)");
  Serial.print("  ├─ WiFi Reconnects:      "); Serial.println(wifiReconnectCount);
  Serial.print("  ├─ I2C Read Errors:      "); Serial.println(i2cErrorCount);
  Serial.print("  └─ Stealth Mode (LED):   "); Serial.println(stealthMode ? "ON (LED Off)" : "OFF (LED On)");
  Serial.println("--------------------------------------\n");
}
