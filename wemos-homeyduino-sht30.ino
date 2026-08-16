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
 * - Over-The-Air (OTA) wireless flashing support via Arduino IDE
 * - Non-blocking timing loop
 * 
 * Capabilities Reported to Homey Device Card:
 * 1. measure_temperature           - Ambient Temperature (°C)
 * 2. measure_humidity              - Relative Humidity (%)
 * 3. alarm_generic                 - Mold Risk Alert (RH >= 70%)
 * 4. alarm_water                   - Condensation Risk Alert (Temp - DewPoint <= 2.0°C)
 * 
 * Flow Triggers with Tag Values:
 * - dew_point                      - Calculated Dew Point (°C)
 * - dew_point_spread               - Temperature Margin Above Dew Point (°C)
 * - absolute_humidity              - Absolute Humidity (g/m³)
 * - heat_index                     - Heat Index / Apparent "Feels Like" Temp (°C)
 * - vpd                            - Vapor Pressure Deficit (kPa)
 * - freeze_risk                    - Freezing Risk State (boolean: true/false)
 * - frost_alert                    - Frost/Freeze Trigger Event (Temp °C)

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

// Include the local secrets file containing WiFi SSID and password
#include "secrets.h"

// --- Configuration ---
const char* deviceName = "Wemos-SHT30-Sensor";
const unsigned long MEASURE_INTERVAL = 15000; // Measurement interval in milliseconds (15 seconds)
const float CONDENSATION_TEMP_MARGIN = 2.0f;  // Temperature margin in °C (alert if Temp - DewPoint <= 2.0°C)
const float FREEZE_ALERT_TEMP = 3.0f;         // Temperature in °C for early freezing / frost risk alert

// --- Globals ---
Adafruit_SHT31 sht30 = Adafruit_SHT31();
unsigned long lastMeasureTime = 0;
bool sensorHealthy = false;

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
 * The dew point is the temperature at which water vapor condenses into liquid water.
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
 * Measures the actual mass of water vapor in a given volume of air,
 * independent of temperature fluctuations.
 */
float calculateAbsoluteHumidity(float temp, float hum) {
  if (hum <= 0.0f) return 0.0f;
  // Saturated vapor pressure (hPa) using Magnus-Tetens
  float satVaporPressure_hPa = 6.112f * exp((17.67f * temp) / (temp + 243.5f));
  // Actual vapor pressure (hPa)
  float actualVaporPressure_hPa = (hum / 100.0f) * satVaporPressure_hPa;
  // Absolute humidity in g/m³
  return (216.7f * actualVaporPressure_hPa) / (273.15f + temp);
}

/**
 * Calculates the Heat Index ("Feels Like" temperature) in Celsius.
 * Uses the Steadman / Rothfusz formula for temperatures >= 20°C.
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
 * Measures the drying power of the air (crucial for plants/greenhouses).
 */
float calculateVPD(float temp, float hum) {
  float vpsat_kPa = 0.61078f * exp((17.27f * temp) / (temp + 237.3f));
  float vpact_kPa = vpsat_kPa * (hum / 100.0f);
  float vpd = vpsat_kPa - vpact_kPa;
  return (vpd < 0.0f) ? 0.0f : vpd;
}

// Setup WiFi connection
void setupWiFi() {
  Serial.println();
  Serial.print("Connecting to WiFi network: ");
  Serial.println(SECRET_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_SSID, SECRET_PASS);

  // Wait for connection
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    currentState = STATE_CONNECTING_WIFI;
    updateLed();
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 40) { // If it takes more than 20 seconds, reboot the board
      Serial.println("\nWiFi connection failed! Rebooting...");
      ESP.restart();
    }
  }

  Serial.println();
  Serial.println("WiFi successfully connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// Setup Multicast DNS (mDNS) responder
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

// Setup Over-The-Air (OTA) firmware updates
void setupOTA() {
  // Set the OTA hostname (visible in Arduino IDE Network Ports)
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

void setup() {
  // Configure Built-in Status LED (Active-LOW on Wemos D1 Mini)
  pinMode(LED_BUILTIN, OUTPUT);
  currentState = STATE_CONNECTING_WIFI;
  updateLed();

  // Start Serial communication for debugging
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Wemos D1 Mini SHT30 Shield Setup ===");

  // Connect to WiFi
  setupWiFi();

  // Initialize mDNS
  setupMDNS();

  // Initialize OTA update service
  setupOTA();

  // Initialize Homeyduino
  Serial.println("Initializing Homeyduino...");
  Homey.begin(deviceName);

  // Register only standard Athom Homey capabilities (custom IDs cause Homey pairing to fail)
  Homey.addCapability("measure_temperature");
  Homey.addCapability("measure_humidity");
  Homey.addCapability("alarm_generic"); // Mold Risk Alert (>70% RH)
  Homey.addCapability("alarm_water");   // Condensation Risk Alert (Temp approaches Dew Point)
  
  Serial.println("Capabilities registered:");
  Serial.println(" - measure_temperature (Temp °C)");
  Serial.println(" - measure_humidity (Rel Hum %)");
  Serial.println(" - alarm_generic (Mold Risk Alert)");
  Serial.println(" - alarm_water (Condensation Alert)");
  Serial.println("Triggers enabled for Homey Flows:");
  Serial.println(" - dew_point (tag: °C)");
  Serial.println(" - dew_point_spread (tag: °C)");
  Serial.println(" - absolute_humidity (tag: g/m³)");
  Serial.println(" - heat_index (tag: °C)");
  Serial.println(" - vpd (tag: kPa)");
  Serial.println(" - freeze_risk (boolean tag)");
  Serial.println(" - frost_alert (temp tag: °C)");

  // Initialize I2C and SHT30 Sensor
  Wire.begin();
  if (!sht30.begin(0x45)) {
    sensorHealthy = false;
    Serial.println("ERROR: SHT30 sensor shield not found at address 0x45!");
  } else {
    sensorHealthy = true;
    Serial.println("SHT30 sensor successfully initialized.");
  }
  
  Serial.println("Sensor configuration complete. Starting measurement loop.");
}

void loop() {
  // Dynamically evaluate overall system state & update LED diagnostics
  if (currentState != STATE_OTA_UPDATING) {
    if (WiFi.status() != WL_CONNECTED) {
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

  // Always handle incoming OTA updates
  ArduinoOTA.handle();

  // Always call Homey.loop() to process incoming network requests
  Homey.loop();

  // Run non-blocking periodic task to read sensor and report values
  unsigned long currentMillis = millis();
  if (currentMillis - lastMeasureTime >= MEASURE_INTERVAL || lastMeasureTime == 0) {
    lastMeasureTime = currentMillis;
    readAndReportSensor();
  }
}

// Function to read SHT30 sensor and report values to Homey
void readAndReportSensor() {
  Serial.print("[");
  Serial.print(millis() / 1000);
  Serial.print("s] Reading sensor... ");

  float temp = sht30.readTemperature();
  float hum = sht30.readHumidity();

  // Check if readings are valid (not NaN)
  if (!isnan(temp) && !isnan(hum)) {
    sensorHealthy = true;

    // Calculate derived environmental metrics
    float dewPoint = calculateDewPoint(temp, hum);
    float dewPointSpread = temp - dewPoint; // Temperature delta above dew point
    float absHum = calculateAbsoluteHumidity(temp, hum);
    float heatIndex = calculateHeatIndex(temp, hum);
    float vpd = calculateVPD(temp, hum);
    bool moldRisk = (hum >= 70.0f); // High humidity favorable for mold growth
    bool condensationRisk = (dewPointSpread <= CONDENSATION_TEMP_MARGIN); // Temp near dew point
    bool freezeRisk = (temp <= FREEZE_ALERT_TEMP); // Freezing / Frost early warning

    // Print all readings to Serial Monitor
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
    // 1. Temperature (°C)
    if (abs(temp - lastTemp) >= 0.05f || lastTemp == -999.0f) {
      if (Homey.setCapabilityValue("measure_temperature", temp)) {
        lastTemp = temp;
      }
    }

    // 2. Relative Humidity (%)
    if (abs(hum - lastHum) >= 0.1f || lastHum == -999.0f) {
      if (Homey.setCapabilityValue("measure_humidity", hum)) {
        lastHum = hum;
      }
    }

    // 3. Mold Risk Alert (alarm_generic)
    if (lastMoldRisk == -1 || ((lastMoldRisk == 1) != moldRisk)) {
      if (Homey.setCapabilityValue("alarm_generic", moldRisk)) {
        lastMoldRisk = moldRisk ? 1 : 0;
      }
    }

    // 4. Condensation Risk Alert (alarm_water)
    if (lastCondensationRisk == -1 || ((lastCondensationRisk == 1) != condensationRisk)) {
      if (Homey.setCapabilityValue("alarm_water", condensationRisk)) {
        lastCondensationRisk = condensationRisk ? 1 : 0;
      }
    }

    // 5. Freezing / Frost Risk Triggers
    if (lastFreezeRisk == -1 || ((lastFreezeRisk == 1) != freezeRisk)) {
      Homey.trigger("freeze_risk", freezeRisk);
      if (freezeRisk) {
        Homey.trigger("frost_alert", temp);
      }
      lastFreezeRisk = freezeRisk ? 1 : 0;
    }

    // Emit Flow Triggers with tag values for Homey Flows
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
    Serial.println("ERROR: Failed to read from SHT30 sensor shield!");
  }
}
