/**
 * Wemos D1 Mini Temperature & Humidity Sensor (SHT30 Shield) for Homey
 * Uses the Homeyduino library to communicate with Athom Homey.
 * 
 * Hardware:
 * - Wemos D1 Mini (ESP8266)
 * - Wemos SHT30 Shield (connected via I2C on pins D1/SCL and D2/SDA)
 * 
 * Dependencies (install via Arduino Library Manager):
 * - Homeyduino (by Athom)
 * - WEMOS SHT3x Arduino Library (by WEMOS)
 * 
 * Credentials are kept in a separate file "secrets.h" to prevent them
 * from being committed to GitHub.
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Homey.h>
#include <WEMOS_SHT3X.h>

// Include the local secrets file containing WiFi SSID and password
#include "secrets.h"

// --- Configuration ---
const char* deviceName = "Wemos-SHT30-Sensor";
const unsigned long MEASURE_INTERVAL = 15000; // Measurement interval in milliseconds (15 seconds)

// --- Globals ---
SHT3X sht30(0x44); // I2C address 0x44 (default for SHT30 shield)
unsigned long lastMeasureTime = 0;

// Track last sent values to avoid unnecessary duplicates (optional, but good practice)
float lastTemp = -999.0;
float lastHum = -999.0;

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

void setup() {
  // Start Serial communication for debugging
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Wemos D1 Mini SHT30 Shield Setup ===");

  // Connect to WiFi
  setupWiFi();

  // Initialize Homeyduino
  Serial.println("Initializing Homeyduino...");
  Homey.begin(deviceName);

  // Register device capabilities with Homey
  Homey.addCapability("measure_temperature");
  Homey.addCapability("measure_humidity");
  Serial.println("Capabilities registered: measure_temperature, measure_humidity");

  // Note: I2C pins D1 (SCL) and D2 (SDA) are initialized automatically by Wemos core Wire library,
  // but we can explicitly call Wire.begin() to be safe.
  Wire.begin();
  
  Serial.println("Sensor configuration complete. Starting measurement loop.");
}

void loop() {
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

  // sht30.get() returns 0 on success
  if (sht30.get() == 0) {
    float temp = sht30.cTemp;
    float hum = sht30.humidity;

    Serial.print("Temp: ");
    Serial.print(temp, 1);
    Serial.print(" °C | Hum: ");
    Serial.print(hum, 1);
    Serial.println(" %");

    // Report values to Homey if they differ from the last reported values
    // Or if it's the very first reading
    if (temp != lastTemp) {
      bool success = Homey.setCapabilityValue("measure_temperature", temp);
      if (success) {
        lastTemp = temp;
      } else {
        Serial.println("Warning: Failed to update measure_temperature in Homey");
      }
    }

    if (hum != lastHum) {
      bool success = Homey.setCapabilityValue("measure_humidity", hum);
      if (success) {
        lastHum = hum;
      } else {
        Serial.println("Warning: Failed to update measure_humidity in Homey");
      }
    }
  } else {
    Serial.println("ERROR: Failed to read from SHT30 sensor shield!");
  }
}
