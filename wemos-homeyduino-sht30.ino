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
 * - Comprehensive hardware, network, and memory diagnostics
 * - Remote maintenance actions triggered from Homey Flows (Reboot, Reset Stats)
 * - Over-The-Air (OTA) wireless flashing support via Arduino IDE
 * - Non-blocking timing loop
 * 
 * Capabilities Reported to Homey Device Card:
 * 1. measure_temperature           - Ambient Temperature (°C)
 * 2. measure_humidity              - Relative Humidity (%)
 * 3. measure_signal_strength       - WiFi Signal Strength RSSI (dBm)
 * 4. measure_voltage               - Internal Supply Rail Voltage Vcc (V)
 * 5. alarm_generic.mold            - Mold Risk Alert (RH >= 70%)
 * 6. alarm_water                   - Condensation Risk Alert (Temp - DewPoint <= 2.0°C)
 * 7. alarm_generic.freeze          - Freezing / Frost Risk Alert (Temp <= 3.0°C)
 * 8. alarm_generic.sensor          - SHT30 I2C Sensor Hardware Fault Alert
 * 
 * Flow Triggers with Tag Values:
 * - dew_point                      - Calculated Dew Point (°C)
 * - dew_point_spread               - Temperature Margin Above Dew Point (°C)
 * - absolute_humidity              - Absolute Humidity (g/m³)
 * - heat_index                     - Heat Index / Apparent "Feels Like" Temp (°C)
 * - vpd                            - Vapor Pressure Deficit (kPa)
 * - freeze_risk                    - Freezing Risk State (boolean: true/false)
 * - frost_alert                    - Frost/Freeze Trigger Event (Temp °C)
 * - device_booted                  - Emitted on boot/reconnect (Reset Reason tag)
 * - diagnostics_heap               - Free RAM Heap (KB tag)
 * - diagnostics_uptime             - System Uptime (Hours tag)
 * - diagnostics_reconnects         - WiFi Reconnect Counter tag
 * 
 * Actions Callable from Homey Flows ("THEN" cards):
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

// --- LED Diagnostic Controller ---

/**
 * Controls the physical built-in LED.
 * On Wemos D1 Mini / ESP8266, the LED is Active-LOW:
 *   LOW  = LED ON
 *   HIGH = LED OFF
 */
void setLedHardware(bool on) {
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

/**
 * Non-blocking LED update routine called continuously in loop().
 * Generates distinct blink/strobe patterns using millis().
 */
void updateLed() {
  unsigned long now = millis();

  switch (currentState) {
    case STATE_OK:
      // Solid ON: Everything connected and operating normally
      setLedHardware(true);
      break;

    case STATE_CONNECTING_WIFI:
      // Fast Blink: 100ms ON / 100ms OFF
      setLedHardware((now / 100) % 2 == 0);
      break;

    case STATE_WIFI_LOST:
      // Slow Blink: 500ms ON / 500ms OFF
      setLedHardware((now / 500) % 2 == 0);
      break;

    case STATE_SENSOR_FAULT:
      // Double Flash Pattern: [ON 100ms, OFF 100ms, ON 100ms, OFF 700ms] (1 second cycle)
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
      // Rapid Strobe: 50ms ON / 50ms OFF
      setLedHardware((now / 50) % 2 == 0);
      break;
  }
}

// --- Psychrometric Calculation Helpers ---

/**
 * Calculates the Dew Point temperature in Celsius using the Magnus-Tetens approximation.
 */
float calculateDewPoint(float temp, float hum) {
  if (hum <= 0.0f) return temp;
  const float a = 17.27f;
  const float b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + log(hum / 100.0f);
  return (b * alpha) / (a - alpha);
}

/**
 * Calculates Absolute Humidity in grams per cubic meter (g/m³).
 */
float calculateAbsoluteHumidity(float temp, float hum) {
  if (hum <= 0.0f) return 0.0f;
  float satVaporPressure_hPa = 6.112f * exp((17.67f * temp) / (temp + 243.5f));
  float actualVaporPressure_hPa = (hum / 100.0f) * satVaporPressure_hPa;
  return (216.7f * actualVaporPressure_hPa) / (273.15f + temp);
}

/**
 * Calculates the Heat Index ("Feels Like" temperature) in Celsius.
 */
float calculateHeatIndex(float temp, float hum) {
  if (temp < 20.0f) {
    return temp; // Below 20°C, heat index effect is negligible
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

/**
 * Calculates Vapor Pressure Deficit (VPD) in kilopascals (kPa).
 */
float calculateVPD(float temp, float hum) {
  float vpsat_kPa = 0.61078f * exp((17.27f * temp) / (temp + 237.3f));
  float vpact_kPa = vpsat_kPa * (hum / 100.0f);
  float vpd = vpsat_kPa - vpact_kPa;
  return (vpd < 0.0f) ? 0.0f : vpd;
}

// --- Homey Remote Actions ("THEN" Flow Cards) ---

/**
 * Remotely reboots the ESP8266 from a Homey Flow.
 */
void handleRemoteReboot() {
  Serial.println("\n[Homey Action] >>> Remote Reboot Triggered from Homey! <<<");
  delay(200);
  ESP.restart();
}

/**
 * Resets the WiFi reconnect and I2C error statistics counters.
 */
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
    if (attempts > 40) { // If connecting takes > 20s, reboot
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
    Serial.println("  └─ Status: Discoverable on local network as " + String(deviceName) + ".local");
  } else {
    Serial.println("FAILED (Error initializing mDNS responder)");
  }
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
  // Generate unique device name using the ESP8266 hardware Chip ID
  uint32_t chipId = ESP.getChipId();
  snprintf(deviceName, sizeof(deviceName), "%s-%04X", BASE_DEVICE_NAME, (uint16_t)(chipId & 0xFFFF));

  // Configure Built-in Status LED (Active-LOW on Wemos D1 Mini)
  pinMode(LED_BUILTIN, OUTPUT);
  currentState = STATE_CONNECTING_WIFI;
  updateLed();

  // Start Serial communication for debugging
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

  // Initialize OTA update service
  setupOTA();

  // Initialize Homeyduino
  Serial.println("Initializing Homeyduino...");
  Homey.begin(deviceName);

  // Register Homey capabilities
  Homey.addCapability("measure_temperature");       // Ambient Temp (°C)
  Homey.addCapability("measure_humidity");          // Relative Humidity (%)
  Homey.addCapability("measure_signal_strength");   // WiFi Signal Strength (dBm)
  Homey.addCapability("measure_voltage");           // ESP8266 Internal Vcc Rail (V)
  Homey.addCapability("alarm_generic.mold");        // Mold Risk Alert (RH >= 70%)
  Homey.addCapability("alarm_water");               // Condensation Risk Alert
  Homey.addCapability("alarm_generic.freeze");      // Freezing / Frost Risk Alert
  Homey.addCapability("alarm_generic.sensor");      // Sensor Hardware Fault Alert

  // Register Homey Flow Actions ("THEN" cards)
  Homey.addAction("reboot", handleRemoteReboot);
  Homey.addAction("reset_stats", handleResetStats);
  
  Serial.println("Capabilities registered on Homey Device Tile:");
  Serial.println(" - measure_temperature (Temp °C)");
  Serial.println(" - measure_humidity (Rel Hum %)");
  Serial.println(" - measure_signal_strength (WiFi RSSI dBm)");
  Serial.println(" - measure_voltage (ESP8266 Vcc V)");
  Serial.println(" - alarm_generic.mold (Mold Risk)");
  Serial.println(" - alarm_water (Condensation Risk)");
  Serial.println(" - alarm_generic.freeze (Freeze Risk)");
  Serial.println(" - alarm_generic.sensor (Sensor Fault)");
  Serial.println("Actions registered for Homey Flows:");
  Serial.println(" - reboot (Remote ESP8266 restart)");
  Serial.println(" - reset_stats (Clear error/reconnect counters)");

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
  
  Serial.println("Initialization complete. Starting measurement and diagnostic loops.\n");
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
 * Reads SHT30 sensor and reports environmental readings to Homey
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

    // Calculate derived environmental metrics
    float dewPoint = calculateDewPoint(temp, hum);
    float dewPointSpread = temp - dewPoint;
    float absHum = calculateAbsoluteHumidity(temp, hum);
    float heatIndex = calculateHeatIndex(temp, hum);
    float vpd = calculateVPD(temp, hum);
    bool moldRisk = (hum >= 70.0f);
    bool condensationRisk = (dewPointSpread <= CONDENSATION_TEMP_MARGIN);
    bool freezeRisk = (temp <= FREEZE_ALERT_TEMP);

    // Print to Serial Monitor
    Serial.println();
    Serial.print("  ├─ Temp:             "); Serial.print(temp, 1); Serial.println(" °C");
    Serial.print("  ├─ Rel Humidity:     "); Serial.print(hum, 1); Serial.println(" %");
    Serial.print("  ├─ Dew Point:        "); Serial.print(dewPoint, 1); Serial.println(" °C");
    Serial.print("  ├─ Dew Point Spread: "); Serial.print(dewPointSpread, 1); Serial.println(" °C");
    Serial.print("  ├─ Abs Humidity:     "); Serial.print(absHum, 2); Serial.println(" g/m³");
    Serial.print("  ├─ Feels Like:       "); Serial.print(heatIndex, 1); Serial.println(" °C");
    Serial.print("  ├─ VPD:              "); Serial.print(vpd, 2); Serial.println(" kPa");
    Serial.print("  ├─ Mold Risk:        "); Serial.println(moldRisk ? "ALERT (>70% RH)" : "OK");
    Serial.print("  ├─ Condensation:     "); Serial.println(condensationRisk ? "ALERT (Temp <= DewPoint + 2°C)" : "OK");
    Serial.print("  └─ Freezing Risk:    "); Serial.println(freezeRisk ? "ALERT (Temp <= 3°C)" : "OK");

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

    if (lastMoldRisk == -1 || ((lastMoldRisk == 1) != moldRisk)) {
      if (Homey.setCapabilityValue("alarm_generic.mold", moldRisk)) {
        lastMoldRisk = moldRisk ? 1 : 0;
      }
    }

    if (lastCondensationRisk == -1 || ((lastCondensationRisk == 1) != condensationRisk)) {
      if (Homey.setCapabilityValue("alarm_water", condensationRisk)) {
        lastCondensationRisk = condensationRisk ? 1 : 0;
      }
    }

    if (lastFreezeRisk == -1 || ((lastFreezeRisk == 1) != freezeRisk)) {
      if (Homey.setCapabilityValue("alarm_generic.freeze", freezeRisk)) {
        lastFreezeRisk = freezeRisk ? 1 : 0;
      }
      Homey.trigger("freeze_risk", freezeRisk);
      if (freezeRisk) {
        Homey.trigger("frost_alert", temp);
      }
    }

    // Reset sensor fault alarm if it was previously set
    if (lastSensorFault != 0) {
      if (Homey.setCapabilityValue("alarm_generic.sensor", false)) {
        lastSensorFault = 0;
      }
    }

    // Emit Flow Triggers with tag values
    if (abs(dewPoint - lastDewPoint) >= 0.1f || lastDewPoint == -999.0f) {
      Homey.trigger("dew_point", dewPoint);
      lastDewPoint = dewPoint;
    }

    if (abs(dewPointSpread - lastDewPointSpread) >= 0.1f || lastDewPointSpread == -999.0f) {
      Homey.trigger("dew_point_spread", dewPointSpread);
      lastDewPointSpread = dewPointSpread;
    }

    if (abs(absHum - lastAbsHum) >= 0.1f || lastAbsHum == -999.0f) {
      Homey.trigger("absolute_humidity", absHum);
      lastAbsHum = absHum;
    }

    if (abs(heatIndex - lastHeatIndex) >= 0.1f || lastHeatIndex == -999.0f) {
      Homey.trigger("heat_index", heatIndex);
      lastHeatIndex = heatIndex;
    }

    if (abs(vpd - lastVpd) >= 0.05f || lastVpd == -999.0f) {
      Homey.trigger("vpd", vpd);
      lastVpd = vpd;
    }
  } else {
    sensorHealthy = false;
    i2cErrorCount++;
    Serial.println("ERROR: Failed to read from SHT30 sensor shield!");

    if (lastSensorFault != 1) {
      if (Homey.setCapabilityValue("alarm_generic.sensor", true)) {
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
