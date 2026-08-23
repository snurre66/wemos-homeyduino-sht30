/**
 * Wemos D1 Mini Temperature & Humidity Sensor (SHT30 Shield) for Homey
 * Uses the Homeyduino library to communicate with Athom Homey.
 * 
 * Hardware:
 * - Wemos D1 Mini (ESP8266)
 * - Wemos SHT30 Shield (connected via I2C on pins D1/SCL and D2/SDA)
 * 
 * Features:
 * - Real-time temperature & humidity monitoring with psychrometric derivations
 * - Embedded Web Server (Port 80) with live mobile-responsive dashboard & diagnostics
 * - Comprehensive hardware, network, and memory diagnostics
 * - Distinct, standard Homey alarm capabilities with functional names
 * - Remote maintenance actions triggered from Homey Flows & Web UI (Reboot, Reset Stats)
 * - Over-The-Air (OTA) wireless flashing support via Arduino IDE
 * - Non-blocking timing loop
 * 
 * Capabilities Reported to Homey Device Card:
 * 1. measure_temperature           - Ambient Temperature (°C)
 * 2. measure_humidity              - Relative Humidity (%)
 * 3. measure_signal_strength       - WiFi Signal Strength RSSI (dBm)
 * 4. measure_voltage               - Internal Supply Rail Voltage Vcc (V)
 * 5. alarm_water                   - Vannalarm: Condensation Risk Alert (Temp - DewPoint <= 2.0°C)
 * 6. alarm_generic                 - Generell alarm: Mold Risk Alert (RH >= 70%)
 * 7. alarm_tamper                  - Sabotasjealarm: SHT30 I2C Sensor Hardware Fault Alert
 * 
 * Flow Triggers with Tag Values:
 * - dew_point                      - Calculated Dew Point (°C)
 * - dew_point_spread               - Temperature Margin Above Dew Point (°C)
 * - absolute_humidity              - Absolute Humidity (g/m³)
 * - heat_index                     - Heat Index / Apparent "Feels Like" Temp (°C)
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
 * 
 * Dependencies (install via Arduino Library Manager):
 * - Homeyduino (by Athom)
 * - Adafruit SHT31 Library (by Adafruit)
 * - ArduinoOTA (included in ESP8266 core)
 * 
 * Credentials are kept in a separate file "secrets.h" to prevent them
 * from being committed to GitHub.
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Homey.h>
#include <Adafruit_SHT31.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <math.h>

// Enable internal VCC rail measurement on ESP8266 ADC
ADC_MODE(ADC_VCC);

// Include the local secrets file containing WiFi SSID and password
#include "secrets.h"

// --- Configuration Constants ---
const char* BASE_DEVICE_NAME = "Wemos-SHT30";
char deviceName[32];                                // Unique hostname populated at boot (e.g. "Wemos-SHT30-1A2B")
const unsigned long MEASURE_INTERVAL = 15000;       // Sensor measurement interval in ms (15 seconds)
const unsigned long DIAGNOSTIC_INTERVAL = 60000;    // Diagnostic reporting interval in ms (60 seconds)
const float CONDENSATION_TEMP_MARGIN = 2.0f;        // Temperature margin in °C (alert if Temp - DewPoint <= 2.0°C)
const float FREEZE_ALERT_TEMP = 3.0f;               // Temperature in °C for early freezing / frost risk alert

// --- Web Server on Port 80 ---
ESP8266WebServer server(80);

// --- Globals ---
Adafruit_SHT31 sht30 = Adafruit_SHT31();
unsigned long lastMeasureTime = 0;
unsigned long lastDiagnosticTime = 0;
bool sensorHealthy = false;
bool wasConnected = false;
bool bootTriggerSent = false;

// Diagnostic Statistics & Counters
uint32_t wifiReconnectCount = 0;
uint32_t i2cErrorCount = 0;

// Current Sensor & Climate Readings (shared with Web Server)
float currentTemp = 0.0f;
float currentHum = 0.0f;
float currentDewPoint = 0.0f;
float currentDewPointSpread = 0.0f;
float currentAbsHum = 0.0f;
float currentHeatIndex = 0.0f;
float currentVpd = 0.0f;
bool currentMoldRisk = false;
bool currentCondensationRisk = false;
bool currentFreezeRisk = false;

// --- System State & LED Diagnostic Codes ---
enum SystemState {
  STATE_OK,                 // Solid ON: Normal operation (WiFi connected & sensor healthy)
  STATE_CONNECTING_WIFI,    // Fast Blink (100ms ON / 100ms OFF): Boot / Connecting to network
  STATE_WIFI_LOST,          // Slow Blink (500ms ON / 500ms OFF): Disconnected from WiFi
  STATE_SENSOR_FAULT,       // Double Flash (* * ___): SHT30 I2C communication failure
  STATE_OTA_UPDATING        // Rapid Strobe (50ms ON / 50ms OFF): Flashing firmware over OTA
};

SystemState currentState = STATE_CONNECTING_WIFI;

// Track last sent values to avoid unnecessary network transmissions
float lastTemp = -999.0f;
float lastHum = -999.0f;
float lastDewPoint = -999.0f;
float lastDewPointSpread = -999.0f;
float lastAbsHum = -999.0f;
float lastHeatIndex = -999.0f;
float lastVpd = -999.0f;
int lastMoldRisk = -1;         // -1 = uninitialized, 0 = false, 1 = true
int lastCondensationRisk = -1; // -1 = uninitialized, 0 = false, 1 = true
int lastFreezeRisk = -1;       // -1 = uninitialized, 0 = false, 1 = true
int lastSensorFault = -1;      // -1 = uninitialized, 0 = false, 1 = true

// Diagnostics cache
int lastRssi = 999;
float lastVcc = -999.0f;
uint32_t lastFreeHeap = 0;

// --- Web UI HTML Template (Stored in PROGMEM flash) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>%DEVICE_NAME% Dashboard</title>
  <style>
    :root {
      --bg: #0f172a; --card: #1e293b; --card-border: #334155;
      --text: #f8fafc; --muted: #94a3b8; --accent: #38bdf8;
      --success: #10b981; --warning: #f59e0b; --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; }
    .container { max-width: 800px; margin: 0 auto; }
    header { display: flex; justify-content: space-between; align-items: center; padding: 12px 0 20px; border-bottom: 1px solid var(--card-border); margin-bottom: 20px; }
    .title-group h1 { font-size: 1.25rem; font-weight: 700; color: var(--text); display: flex; align-items: center; gap: 8px; }
    .title-group span { font-size: 0.8rem; color: var(--muted); }
    .status-badge { display: flex; align-items: center; gap: 6px; font-size: 0.75rem; background: #064e3b; color: #6ee7b7; padding: 4px 10px; border-radius: 999px; font-weight: 600; }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; background: #10b981; animation: pulse 2s infinite; }
    @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.4; } 100% { opacity: 1; } }
    
    .nav-tabs { display: flex; gap: 8px; margin-bottom: 20px; background: #0f172a; padding: 4px; border-radius: 10px; border: 1px solid var(--card-border); }
    .tab-btn { flex: 1; padding: 10px 16px; background: transparent; border: none; color: var(--muted); font-size: 0.9rem; font-weight: 600; border-radius: 8px; cursor: pointer; transition: all 0.2s; }
    .tab-btn.active { background: var(--card); color: var(--accent); box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    
    .tab-pane { display: none; }
    .tab-pane.active { display: block; }
    
    .hero-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px; }
    @media(max-width: 500px) { .hero-grid { grid-template-columns: 1fr; } }
    .hero-card { background: var(--card); border: 1px solid var(--card-border); border-radius: 14px; padding: 20px; text-align: center; }
    .hero-label { font-size: 0.85rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-weight: 600; margin-bottom: 8px; }
    .hero-value { font-size: 2.75rem; font-weight: 800; color: var(--text); line-height: 1; margin-bottom: 8px; }
    .hero-unit { font-size: 1.25rem; font-weight: 500; color: var(--muted); }
    .hero-sub { font-size: 0.8rem; color: var(--accent); }
    
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; margin-bottom: 16px; }
    .card { background: var(--card); border: 1px solid var(--card-border); border-radius: 12px; padding: 14px 16px; }
    .card-label { font-size: 0.75rem; color: var(--muted); font-weight: 600; margin-bottom: 4px; text-transform: uppercase; }
    .card-value { font-size: 1.15rem; font-weight: 700; color: var(--text); }
    
    .section-title { font-size: 0.95rem; font-weight: 700; color: var(--accent); margin: 20px 0 12px; text-transform: uppercase; letter-spacing: 0.05em; }
    
    .alarms-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 10px; margin-bottom: 20px; }
    .alarm-pill { display: flex; align-items: center; justify-content: space-between; padding: 10px 14px; border-radius: 10px; background: var(--card); border: 1px solid var(--card-border); font-size: 0.85rem; font-weight: 600; }
    .alarm-pill.ok { border-color: rgba(16,185,129,0.3); color: #6ee7b7; }
    .alarm-pill.alert { background: rgba(239,68,68,0.15); border-color: var(--danger); color: #fca5a5; }
    
    .btn-group { display: flex; gap: 12px; margin-top: 20px; }
    .btn { flex: 1; padding: 12px; font-size: 0.9rem; font-weight: 600; border-radius: 10px; border: 1px solid var(--card-border); background: var(--card); color: var(--text); cursor: pointer; transition: 0.2s; }
    .btn:hover { background: #334155; }
    .btn-danger { background: rgba(239,68,68,0.15); border-color: rgba(239,68,68,0.4); color: #fca5a5; }
    .btn-danger:hover { background: rgba(239,68,68,0.3); }
    
    footer { text-align: center; margin-top: 30px; font-size: 0.75rem; color: var(--muted); padding-top: 12px; border-top: 1px solid var(--card-border); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="title-group">
        <h1>🌡️ <span id="devName" style="color:var(--text);font-size:1.25rem;">%DEVICE_NAME%</span></h1>
        <span id="devSub">Homeyduino SHT30 Node</span>
      </div>
      <div class="status-badge" id="netBadge">
        <div class="status-dot"></div>
        <span id="netStatus">Online</span>
      </div>
    </header>

    <div class="nav-tabs">
      <button class="tab-btn active" onclick="switchTab('sensors')">📊 Sensors & Climate</button>
      <button class="tab-btn" onclick="switchTab('diagnostics')">⚙️ System Diagnostics</button>
    </div>

    <!-- TAB 1: SENSORS & CLIMATE -->
    <div id="tab-sensors" class="tab-pane active">
      <div class="hero-grid">
        <div class="hero-card">
          <div class="hero-label">Temperature</div>
          <div class="hero-value"><span id="temp">--</span><span class="hero-unit">°C</span></div>
          <div class="hero-sub">Feels like: <span id="heatIndex">--</span>°C</div>
        </div>
        <div class="hero-card">
          <div class="hero-label">Relative Humidity</div>
          <div class="hero-value"><span id="hum">--</span><span class="hero-unit">%</span></div>
          <div class="hero-sub">Abs Hum: <span id="absHum">--</span> g/m³</div>
        </div>
      </div>

      <div class="section-title">🌿 Psychrometric Metrics</div>
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

      <div class="section-title">🚨 Environmental Risk Alerts</div>
      <div class="alarms-grid">
        <div class="alarm-pill ok" id="alarmCondensation">
          <span>💧 Condensation</span>
          <span id="txtCondensation">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmMold">
          <span>🦠 Mold Risk</span>
          <span id="txtMold">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmFreeze">
          <span>❄️ Freezing Risk</span>
          <span id="txtFreeze">OK</span>
        </div>
        <div class="alarm-pill ok" id="alarmSensor">
          <span>⚠️ SHT30 Hardware</span>
          <span id="txtSensor">Healthy</span>
        </div>
      </div>
    </div>

    <!-- TAB 2: SYSTEM DIAGNOSTICS -->
    <div id="tab-diagnostics" class="tab-pane">
      <div class="section-title">📶 Wireless & Network</div>
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
          <div class="card-label">MAC Address</div>
          <div class="card-value" id="netMac">--</div>
        </div>
        <div class="card">
          <div class="card-label">WiFi Reconnect Count</div>
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
          <div class="card-label">Free Heap RAM</div>
          <div class="card-value"><span id="freeHeap">--</span> KB</div>
        </div>
        <div class="card">
          <div class="card-label">Heap Fragmentation</div>
          <div class="card-value"><span id="heapFrag">--</span>%</div>
        </div>
        <div class="card">
          <div class="card-label">Boot / Reset Reason</div>
          <div class="card-value" id="resetReason">--</div>
        </div>
        <div class="card">
          <div class="card-label">I2C Read Error Count</div>
          <div class="card-value" id="i2cErrors">0</div>
        </div>
      </div>

      <div class="section-title">🛠️ Remote Maintenance Actions</div>
      <div class="btn-group">
        <button class="btn btn-danger" onclick="rebootDevice()">🔄 Reboot ESP8266</button>
        <button class="btn" onclick="resetStats()">🧹 Reset Counters</button>
      </div>
    </div>

    <footer>
      Wemos D1 Mini SHT30 Node &bull; Athom Homey Homeyduino &bull; <a href="/api/data" target="_blank" style="color:var(--accent);text-decoration:none;">JSON API (/api/data)</a>
    </footer>
  </div>

  <script>
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

    function setPill(elemId, txtId, isAlert, alertText, okText) {
      const el = document.getElementById(elemId);
      const txt = document.getElementById(txtId);
      if(isAlert) {
        el.className = 'alarm-pill alert';
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
        
        // Hero Values
        document.getElementById('temp').textContent = d.temp.toFixed(1);
        document.getElementById('hum').textContent = d.hum.toFixed(1);
        document.getElementById('heatIndex').textContent = d.heat_index.toFixed(1);
        document.getElementById('absHum').textContent = d.abs_hum.toFixed(2);
        
        // Psychrometric
        document.getElementById('dewPoint').textContent = d.dew_point.toFixed(1);
        document.getElementById('dewSpread').textContent = d.dew_point_spread.toFixed(1);
        document.getElementById('vpd').textContent = d.vpd.toFixed(2);

        // Alarms
        setPill('alarmCondensation', 'txtCondensation', d.condensation_risk, 'RISK ALERT', 'OK');
        setPill('alarmMold', 'txtMold', d.mold_risk, 'HIGH RH (>70%)', 'OK');
        setPill('alarmFreeze', 'txtFreeze', d.freeze_risk, 'FREEZE ALERT', 'OK');
        setPill('alarmSensor', 'txtSensor', !d.sensor_healthy, 'FAULT', 'Healthy');

        // Diagnostics
        document.getElementById('wifiSsid').textContent = d.ssid;
        document.getElementById('wifiRssi').textContent = d.rssi;
        document.getElementById('netIp').textContent = d.ip;
        document.getElementById('netMac').textContent = d.mac;
        document.getElementById('reconnectCount').textContent = d.wifi_reconnects;
        document.getElementById('uptime').textContent = d.uptime_str;
        document.getElementById('vcc').textContent = d.vcc.toFixed(2);
        document.getElementById('freeHeap').textContent = (d.free_heap / 1024).toFixed(1);
        document.getElementById('heapFrag').textContent = d.heap_frag;
        document.getElementById('resetReason').textContent = d.reset_reason;
        document.getElementById('i2cErrors').textContent = d.i2c_errors;
      } catch(e) {
        console.error('Fetch error:', e);
      }
    }

    async function rebootDevice() {
      if(confirm('Are you sure you want to reboot the ESP8266?')) {
        await fetch('/api/reboot', { method: 'POST' });
        alert('Reboot command sent! Device will restart now.');
        setTimeout(() => location.reload(), 6000);
      }
    }

    async function resetStats() {
      if(confirm('Reset WiFi reconnect and I2C error counters?')) {
        await fetch('/api/reset_stats', { method: 'POST' });
        fetchData();
      }
    }

    // Initial fetch & 3-second live refresh interval
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

/**
 * Serves the primary HTML Dashboard
 */
void handleRoot() {
  String html = FPSTR(INDEX_HTML);
  html.replace("%DEVICE_NAME%", deviceName);
  server.send(200, "text/html", html);
}

/**
 * JSON API Endpoint (/api/data)
 */
void handleApiData() {
  unsigned long nowSec = millis() / 1000;
  float vcc = ESP.getVcc() / 1000.0f;

  String json = "{";
  json += "\"device\":\"" + String(deviceName) + "\",";
  json += "\"uptime_sec\":" + String(nowSec) + ",";
  json += "\"uptime_str\":\"" + formatUptime(nowSec) + "\",";
  json += "\"temp\":" + String(currentTemp, 1) + ",";
  json += "\"hum\":" + String(currentHum, 1) + ",";
  json += "\"dew_point\":" + String(currentDewPoint, 1) + ",";
  json += "\"dew_point_spread\":" + String(currentDewPointSpread, 1) + ",";
  json += "\"abs_hum\":" + String(currentAbsHum, 2) + ",";
  json += "\"heat_index\":" + String(currentHeatIndex, 1) + ",";
  json += "\"vpd\":" + String(currentVpd, 2) + ",";
  json += "\"mold_risk\":" + String(currentMoldRisk ? "true" : "false") + ",";
  json += "\"condensation_risk\":" + String(currentCondensationRisk ? "true" : "false") + ",";
  json += "\"freeze_risk\":" + String(currentFreezeRisk ? "true" : "false") + ",";
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

/**
 * Web API Remote Reboot Endpoint (/api/reboot)
 */
void handleApiReboot() {
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  Serial.println("\n[Web Server] Remote reboot requested from web browser!");
  delay(500);
  ESP.restart();
}

/**
 * Web API Reset Counters Endpoint (/api/reset_stats)
 */
void handleApiResetStats() {
  wifiReconnectCount = 0;
  i2cErrorCount = 0;
  Serial.println("\n[Web Server] Diagnostic counters reset from web browser!");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// --- LED Diagnostic Controller ---

void setLedHardware(bool on) {
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

void updateLed() {
  unsigned long now = millis();

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

// --- Psychrometric Calculation Helpers ---

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

float calculateHeatIndex(float temp, float hum) {
  if (temp < 20.0f) {
    return temp;
  }
  float T_f = (temp * 9.0f / 5.0f) + 32.0f;
  float R = hum;
  float hi_f = 0.5f * (T_f + 61.0f + ((T_f - 68.0f) * 1.2f) + (R * 0.094f));
  if (hi_f >= 80.0f) {
    hi_f = -42.379f + 2.04901523f * T_f + 10.14333127f * R
           - 0.22475541f * T_f * R - 0.00683783f * T_f * T_f
           - 0.05481717f * R * R + 0.00122874f * T_f * T_f * R
           + 0.00085282f * T_f * R * R - 0.00000199f * T_f * T_f * R * R;
    if ((R < 13.0f) && (T_f >= 80.0f) && (T_f <= 112.0f)) {
      hi_f -= ((13.0f - R) / 4.0f) * sqrt((17.0f - abs(T_f - 95.0f)) / 17.0f);
    } else if ((R > 85.0f) && (T_f >= 80.0f) && (T_f <= 87.0f)) {
      hi_f += ((R - 85.0f) / 10.0f) * ((87.0f - T_f) / 5.0f);
    }
  }
  return (hi_f - 32.0f) * 5.0f / 9.0f;
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
  uint32_t chipId = ESP.getChipId();
  snprintf(deviceName, sizeof(deviceName), "%s-%04X", BASE_DEVICE_NAME, (uint16_t)(chipId & 0xFFFF));

  pinMode(LED_BUILTIN, OUTPUT);
  currentState = STATE_CONNECTING_WIFI;
  updateLed();

  Serial.begin(115200);
  delay(100);
  Serial.println("\n=============================================");
  Serial.println("   Wemos D1 Mini SHT30 Environment & Diagnostics Node   ");
  Serial.println("=============================================");
  Serial.print("Device Hostname / ID: ");
  Serial.println(deviceName);
  Serial.print("Reset Reason:         ");
  Serial.println(ESP.getResetReason());
  Serial.print("Core / SDK Version:   ");
  Serial.println(ESP.getCoreVersion() + " / " + ESP.getSdkVersion());
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
  Homey.addCapability("measure_temperature");       // Temperature (°C)
  Homey.addCapability("measure_humidity");          // Relative Humidity (%)
  Homey.addCapability("measure_signal_strength");   // WiFi Signal Strength (dBm)
  Homey.addCapability("measure_voltage");           // ESP8266 Internal Vcc Rail (V)
  Homey.addCapability("alarm_water");               // Homey UI: "Vannalarm" -> Condensation Risk
  Homey.addCapability("alarm_generic");             // Homey UI: "Generell alarm" -> Mold Risk
  Homey.addCapability("alarm_tamper");              // Homey UI: "Sabotasjealarm" -> SHT30 Hardware/I2C Fault

  // Register Homey Flow Actions ("THEN" cards)
  Homey.addAction("reboot", handleRemoteReboot);
  Homey.addAction("reset_stats", handleResetStats);
  
  Serial.println("Capabilities registered on Homey Device Tile:");
  Serial.println(" - measure_temperature    (Ambient Temp °C)");
  Serial.println(" - measure_humidity       (Rel Humidity %)");
  Serial.println(" - measure_signal_strength(WiFi RSSI dBm)");
  Serial.println(" - measure_voltage        (ESP8266 Vcc V)");
  Serial.println(" - alarm_water            (Vannalarm: Condensation Risk)");
  Serial.println(" - alarm_generic          (Generell alarm: Mold Risk)");
  Serial.println(" - alarm_tamper           (Sabotasjealarm: SHT30 Sensor Fault)");

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
  Serial.println("Initialization complete. Measurement, diagnostic & web loops running.\n");
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
 * Reads SHT30 sensor and reports environmental readings to Homey & Web UI
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

    // Update global state variables for Web UI
    currentTemp = temp;
    currentHum = hum;
    currentDewPoint = calculateDewPoint(temp, hum);
    currentDewPointSpread = temp - currentDewPoint;
    currentAbsHum = calculateAbsoluteHumidity(temp, hum);
    currentHeatIndex = calculateHeatIndex(temp, hum);
    currentVpd = calculateVPD(temp, hum);
    currentMoldRisk = (hum >= 70.0f);
    currentCondensationRisk = (currentDewPointSpread <= CONDENSATION_TEMP_MARGIN);
    currentFreezeRisk = (temp <= FREEZE_ALERT_TEMP);

    // Print to Serial Monitor
    Serial.println();
    Serial.print("  ├─ Temp:             "); Serial.print(currentTemp, 1); Serial.println(" °C");
    Serial.print("  ├─ Rel Humidity:     "); Serial.print(currentHum, 1); Serial.println(" %");
    Serial.print("  ├─ Dew Point:        "); Serial.print(currentDewPoint, 1); Serial.println(" °C");
    Serial.print("  ├─ Dew Point Spread: "); Serial.print(currentDewPointSpread, 1); Serial.println(" °C");
    Serial.print("  ├─ Abs Humidity:     "); Serial.print(currentAbsHum, 2); Serial.println(" g/m³");
    Serial.print("  ├─ Feels Like:       "); Serial.print(currentHeatIndex, 1); Serial.println(" °C");
    Serial.print("  ├─ VPD:              "); Serial.print(currentVpd, 2); Serial.println(" kPa");
    Serial.print("  ├─ Mold Risk:        "); Serial.println(currentMoldRisk ? "ALERT (>70% RH)" : "OK");
    Serial.print("  ├─ Condensation:     "); Serial.println(currentCondensationRisk ? "ALERT (Temp <= DewPoint + 2°C)" : "OK");
    Serial.print("  └─ Freezing Risk:    "); Serial.println(currentFreezeRisk ? "ALERT (Temp <= 3°C)" : "OK");

    // Report standard capabilities to Homey
    if (abs(temp - lastTemp) >= 0.05f || lastTemp == -999.0f) {
      if (Homey.setCapabilityValue("measure_temperature", temp)) {
        lastTemp = temp;
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

    if (lastFreezeRisk == -1 || ((lastFreezeRisk == 1) != currentFreezeRisk)) {
      lastFreezeRisk = currentFreezeRisk ? 1 : 0;
      Homey.trigger("freeze_risk", currentFreezeRisk);
      if (currentFreezeRisk) {
        Homey.trigger("frost_alert", temp);
      }
    }

    if (lastSensorFault != 0) {
      if (Homey.setCapabilityValue("alarm_tamper", false)) {
        lastSensorFault = 0;
      }
    }

    // Emit Flow Triggers with tag values
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

    if (abs(currentHeatIndex - lastHeatIndex) >= 0.1f || lastHeatIndex == -999.0f) {
      Homey.trigger("heat_index", currentHeatIndex);
      lastHeatIndex = currentHeatIndex;
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
  Serial.print("  ├─ WiFi Signal (RSSI):   "); Serial.print(currentRssi); Serial.println(" dBm");
  Serial.print("  ├─ Supply Rail (Vcc):    "); Serial.print(currentVcc, 2); Serial.println(" V");
  Serial.print("  ├─ System Uptime:        "); Serial.print(uptimeHours, 2); Serial.println(" hours");
  Serial.print("  ├─ Free Heap RAM:        "); Serial.print(freeHeap / 1024.0f, 1); Serial.print(" KB (Frag: "); Serial.print(heapFrag); Serial.println("%)");
  Serial.print("  ├─ WiFi Reconnect Count: "); Serial.println(wifiReconnectCount);
  Serial.print("  └─ I2C Error Count:      "); Serial.println(i2cErrorCount);
  Serial.println("--------------------------------------\n");
}
