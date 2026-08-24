# Wemos D1 Mini Wine Cabinet Climate & Diagnostics Node (SHT30 Shield)

An advanced environmental, wine preservation, and diagnostics node built for wine storage cabinets, cellars, and coolers using a **Wemos D1 Mini (ESP8266)** and an **SHT30 Shield**. 

It communicates natively with **Athom Homey** via the **Homeyduino** library to report ambient climate, simulated bottle core temperature, cork dryness protection, psychrometric derivations, and system health diagnostics, while serving a sleek, built-in **Web Dashboard** on port 80.

---

## 🍷 Key Wine Storage & Climate Features

- **🌐 Embedded Wine Dashboard (Port 80):** Mobile-responsive dark-mode web interface accessible via browser (`http://<device-ip>/` or `http://<device-name>.local`). Features tabbed navigation separating wine telemetry from system diagnostics and controls.
- **⚡ Non-Blocking Live Telemetry:** Uses non-blocking client-side asynchronous polling (`fetch('/api/data')`) every 3 seconds for instant real-time telemetry.
- **🍾 Simulated Wine Core Temperature:** Implements a PT1 first-order low-pass thermal filter ($\tau \approx 3.5\text{h}$) modeling a standard 750ml wine bottle to filter out short-term air fluctuations caused by door openings.
- **🪵 Cork Dryness & Preservation Alarm:** Continuous relative humidity monitoring with alerts when $\text{RH} < 50\%$ to prevent cork drying, shrinkage, and wine oxidation.
- **🌡️ Wine Temperature Limit Alarms:** Lower warning limit ($< 8^\circ\text{C}$) and upper warning limit ($> 16^\circ\text{C}$) to protect wine structure.
- **🚪 Rapid Temperature Rise / Door Open Alert:** Detects rapid thermal spikes ($\Delta T > 1.5^\circ\text{C}$ over a 5-minute rolling window) indicating an open door or compressor failure.
- **💧 Condensation & Mold Prevention:** Automatically computes dew point, dew point spread margin, absolute humidity ($\text{g/m}^3$), and vapor pressure deficit (VPD) using peer-reviewed Magnus-Tetens formulas.
- **🌙 Stealth Mode:** Disables onboard diagnostic LEDs for seamless placement inside glass-door wine cabinets. Can be toggled from Web UI or Homey flows.
- **🏷️ Persistent Device Naming & mDNS:** Set custom names dynamically via Web UI (saved in EEPROM flash) or `secrets.h` with automatic mDNS hostname sanitization (`[a-z0-9-]`).
- **📶 Hardware & Network Diagnostics:** Continuously monitors WiFi RSSI, supply rail voltage ($V_{cc}$), free heap RAM, fragmentation, uptime, and I2C/WiFi reconnect statistics.
- **🛠️ Remote Maintenance Actions:** Supports remote reboot, stats reset, and stealth mode toggle from Homey Flows and Web UI.
- **📡 Boot Notifications:** Emits a `device_booted` trigger to Homey on startup containing the reset reason (Power On, Watchdog, Exception).
- **📶 Over-The-Air (OTA) Updates:** Allows wireless firmware flashing directly from the Arduino IDE.

---

## 🖥️ Web Interface Overview

Access the web dashboard in any browser on your local network:
- **`http://<device-ip>/`** or **`http://<device-name>.local`**

### Tab 1: 📊 Wine Climate & Telemetry
- **Hero Tiles:** Large gauges for **Simulated Wine Core Temp (°C)**, **Ambient Air Temp (°C)**, **Relative Humidity (%)**, and **Absolute Humidity ($\text{g/m}^3$)**.
- **Wine Cabinet Alarms:** Live status pills for Cork Protection, Temperature Range (8–16°C), Rapid Rise / Open Door, Condensation Risk, Mold Control, and SHT30 Hardware Health.
- **Psychrometric Telemetry:** Dew Point (°C), Dew Point Margin Spread (°C), and Vapor Pressure Deficit (VPD in kPa).

### Tab 2: ⚙️ System Diagnostics & Control
- **Device Configuration:** Edit and save custom device name to EEPROM with automatic reboot and mDNS update.
- **Hardware Controls:** One-click toggles for **Stealth Mode (LED Off)**, **Clear Counters**, and **Reboot Node**.
- **Wireless & Network:** WiFi SSID, Signal Strength (RSSI in dBm), IP Address, and Reconnect Counter.
- **Hardware & Resources:** System Uptime (*Xd Xh Xm Xs*), Internal Supply Rail Voltage ($V_{cc}$), Free Heap RAM (KB), and I2C Read Error Counter.

### REST API Endpoints
- `GET /api/data`: Returns full telemetry and diagnostics JSON snapshot.
- `POST /api/set_name`: Updates custom device name in EEPROM (`name=<new_name>`) and reboots.
- `POST /api/reset_name`: Clears EEPROM custom name and reverts to default name.
- `POST /api/toggle_stealth`: Toggles onboard LED stealth mode.
- `POST /api/reboot`: Triggers software restart.
- `POST /api/reset_stats`: Resets WiFi drop and I2C error counters.

---

## Capabilities Reported to Homey Device Tile

| Capability ID | Homey UI Name (NO / EN) | Unit | Type | Functional Meaning |
| :--- | :--- | :--- | :--- | :--- |
| `measure_temperature` | **Temperatur** (*Air Temperature*) | °C | Number | Ambient dry-bulb air temperature measured by SHT30 |
| `measure_temperature.liquid` | **Vintemperatur** (*Wine Core Temp*) | °C | Number | Simulated liquid core temperature of a 750ml bottle |
| `measure_humidity` | **Relativ fuktighet** (*Humidity*) | % | Number | Relative humidity measured by SHT30 |
| `measure_signal_strength` | **Signalstyrke** (*Signal Strength*) | dBm | Number | WiFi RSSI signal strength (e.g. `-62 dBm`) |
| `measure_voltage` | **Spenning** (*Supply Voltage*) | V | Number | ESP8266 internal supply rail voltage ($V_{cc}$) |
| `alarm_water` | **Vannalarm** (*Water Alarm*) | — | Boolean | 💧 **Kondensrisiko** ($T - T_{\text{dew}} \le 2.0^\circ\text{C}$) |
| `alarm_generic` | **Generell alarm** (*Generic Alarm*) | — | Boolean | 🦠 **Muggsopprisiko** ($\text{RH} \ge 75\%$) |
| `alarm_tamper` | **Sabotasjealarm** (*Tamper Alarm*) | — | Boolean | ⚠️ **SHT30 Maskinvarefeil** (I2C read / communication failure) |

---

## Flow Triggers & Tags (Homey Flows)

### Wine & Environmental Triggers ("WHEN" Cards)
- `cork_dry_risk`: Boolean state tag (`true`/`false`) when $\text{RH} < 50.0\%$
- `cork_dry_alert`: Emitted when humidity drops below cork preservation threshold (tag: `% RH`)
- `wine_temp_low`: Alert when temperature drops below $8.0^\circ\text{C}$ (tag: Temp °C)
- `wine_temp_high`: Alert when temperature exceeds $16.0^\circ\text{C}$ (tag: Temp °C)
- `rapid_temp_rise`: Alert emitted when temperature rises $\Delta T > 1.5^\circ\text{C}$ in 5 min (tag: $\Delta T$ °C)
- `liquid_temp`: Simulated bottle core temperature change (tag: Temp °C)
- `wine_status`: Text summary tag (*"Optimal wine climate"*, *"Warning: Dry air"*, etc.)
- `dew_point`: Real-time dew point temperature (°C)
- `dew_point_spread`: Margin between ambient temperature and dew point ($T - T_{\text{dew}}$ in °C)
- `absolute_humidity`: Actual mass of water vapor ($\text{g/m}^3$)
- `vpd`: Vapor Pressure Deficit (kPa)
- `freeze_risk`: Boolean state tag (`true`/`false`) when $T \le 3.0^\circ\text{C}$
- `frost_alert`: Frost warning event when $T \le 3.0^\circ\text{C}$ (tag: Temp °C)

### Diagnostics & System Triggers
- `device_booted`: Emitted once on boot/reconnect. Tag: `reset_reason` (*Power on*, *Hardware Watchdog*, *Exception*)
- `diagnostics_heap`: Free RAM Heap (KB tag)
- `diagnostics_uptime`: System Uptime (Hours tag)
- `diagnostics_reconnects`: WiFi Reconnect Counter tag

---

## Remote Maintenance Actions (Homey Flow "THEN" Cards)

- **`reboot`**: Remotely reboots the Wemos D1 Mini board.
- **`reset_stats`**: Clears the WiFi reconnect counter and I2C error counters.
- **`set_stealth_mode`**: Sets stealth LED mode (`"true"`/`"false"` or `"1"`/`"0"`).
- **`toggle_stealth_mode`**: Toggles stealth LED mode on/off.

---

## Visual LED Status & Diagnostics

| LED Pattern | Timing / Rhythm | Status / Fault Meaning | Stealth Mode Behavior |
| :--- | :--- | :--- | :--- |
| **Solid ON** | Continuous lit | **All OK** (WiFi connected, SHT30 healthy) | **Disabled (LED OFF)** in Stealth Mode |
| **Fast Blink** | 100ms ON / 100ms OFF | **Connecting / Booting** | Active (Visual Diagnostic) |
| **Slow Blink** | 500ms ON / 500ms OFF | **WiFi Lost / Disconnected** | Active (Visual Diagnostic) |
| **Double Flash** | `* * ___ * * ___` (2 quick pulses) | **Sensor Fault** (I2C read failure / NaN) | Active (Visual Diagnostic) |
| **Rapid Strobe** | 50ms ON / 50ms OFF | **OTA Update in Progress** | Active (Visual Diagnostic) |

---

## Setup & Configuration

1. Copy `secrets.h.example` to `secrets.h`:
   ```c
   #define SECRET_SSID "Your_WiFi_SSID"
   #define SECRET_PASS "Your_WiFi_Password"
   #define DEVICE_NAME "WineCabinet-SHT30" // Optional default name
   ```
2. Open [`wemos-homeyduino-sht30.ino`](./wemos-homeyduino-sht30.ino) in Arduino IDE.
3. Install required libraries via Library Manager:
   - **Homeyduino** by Athom
   - **Adafruit SHT31 Library** by Adafruit
4. Select board **LOLIN(WEMOS) D1 R2 & mini** and flash via USB or OTA.
5. Access `http://winecabinet-sht30.local` (or IP) in browser to customize settings.
6. In Athom Homey, pair using the **Homeyduino** app.

---

## License
This project is licensed under the MIT License.
