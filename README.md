# Wemos D1 Mini SHT30 Homeyduino Sensor & Diagnostics Node

This project is an advanced environmental sensor and diagnostics node built using a **Wemos D1 Mini (ESP8266)** and an **SHT30 Shield**. It uses the **Homeyduino** library to report raw measurements, psychrometric climate derivations, system/network health diagnostics, and remote maintenance actions to an **Athom Homey** smart home gateway, while serving a sleek, built-in **Web Dashboard** on port 80.

---

## 🌟 Key Features

- **🌐 Built-in Web Interface (Port 80):** Mobile-responsive dashboard accessible via browser (`http://<device-ip>/` or `http://Wemos-SHT30-XXXX.local`). Features a tabbed UI separating environmental sensors from hardware diagnostics.
- **⚡ Live Dynamic Updates:** Uses non-blocking client-side asynchronous polling (`fetch('/api/data')`) every 3 seconds for instant real-time telemetry without page reloads.
- **🌡️ Real-time Environmental Monitoring:** Reads temperature and humidity from the I2C-based SHT30 sensor shield (address `0x45`).
- **🌿 Psycho-Climatic Calculations:** Automatically calculates dew point, dew point spread margin, absolute humidity ($\text{g/m}^3$), heat index ("feels like"), vapor pressure deficit (VPD), mold risk, and freezing risk.
- **🚨 Distinct Functional Alarms in Homey:** Maps each alarm state to a distinct standard Homey capability (*Vannalarm*, *Sabotasjealarm*, *Generell alarm*).
- **📶 Hardware & Network Diagnostics:** Continuously monitors WiFi signal strength (RSSI), internal supply rail voltage ($V_{cc}$), free heap memory, fragmentation, and WiFi reconnect counters.
- **🛠️ Remote Maintenance Actions:** Supports remote reboot and counter reset from both Homey Flows and Web UI buttons.
- **📡 Boot Notifications:** Emits a `device_booted` trigger to Homey on startup containing the reset reason (Power On, Watchdog, Exception).
- **📶 Over-The-Air (OTA) Updates:** Allows wireless firmware flashing directly from the Arduino IDE.

---

## 🖥️ Web Interface Overview

Access the web dashboard in any browser on your local network:
- **`http://<device-ip>/`** or **`http://Wemos-SHT30-XXXX.local`**

### Tab 1: 📊 Sensors & Climate
- **Hero Tiles:** Large gauges for **Temperature (°C)** and **Relative Humidity (%)**.
- **Psychrometric Comfort Metrics:** Dew Point (°C), Dew Point Spread Margin (°C), Absolute Humidity ($\text{g/m}^3$), Heat Index (°C), and VPD (kPa).
- **Status Pills:** Live indicators for Condensation Risk, Mold Risk, Freezing Risk, and SHT30 Hardware Health.

### Tab 2: ⚙️ System Diagnostics
- **Wireless & Network:** WiFi SSID, Signal Strength (RSSI in dBm), IP Address, MAC Address, Reconnect Count.
- **Hardware & Resources:** System Uptime (*Xd Xh Xm Xs*), Internal Supply Rail Voltage ($V_{cc}$), Free Heap RAM (KB), Heap Fragmentation (%), Boot/Reset Reason, I2C Error Counter.
- **Remote Maintenance:** One-click **Reboot ESP8266** and **Reset Counters** buttons.

### REST API Endpoint (`/api/data`)
- `GET /api/data`: Returns full telemetry JSON snapshot.
- `POST /api/reboot`: Triggers software restart.
- `POST /api/reset_stats`: Resets WiFi drop and I2C error counters.

---

## Capabilities Reported to Homey Device Tile

| Capability ID | Homey UI Name (NO / EN) | Unit | Type | Functional Meaning |
| :--- | :--- | :--- | :--- | :--- |
| `measure_temperature` | **Temperatur** (*Temperature*) | °C | Number | Ambient dry-bulb temperature measured by SHT30 |
| `measure_humidity` | **Relativ fuktighet** (*Humidity*) | % | Number | Relative humidity measured by SHT30 |
| `measure_signal_strength` | **Signalstyrke** (*Signal Strength*) | dBm | Number | WiFi RSSI signal strength (e.g. `-62 dBm`) |
| `measure_voltage` | **Spenning** (*Supply Voltage*) | V | Number | ESP8266 internal supply rail voltage ($V_{cc}$) |
| `alarm_water` | **Vannalarm** (*Water Alarm*) | — | Boolean | 💧 **Kondensrisiko** ($T - T_{\text{dew}} \le 2.0^\circ\text{C}$) |
| `alarm_generic` | **Generell alarm** (*Generic Alarm*) | — | Boolean | 🦠 **Muggsopprisiko** ($\text{RH} \ge 70\%$) |
| `alarm_tamper` | **Sabotasjealarm** (*Tamper Alarm*) | — | Boolean | ⚠️ **SHT30 Maskinvarefeil** (I2C read / communication failure) |

---

## Flow Triggers & Tags (Homey Flows)

### Environmental Triggers
- `dew_point`: Real-time dew point temperature (°C)
- `dew_point_spread`: Margin between ambient temperature and dew point ($T - T_{\text{dew}}$ in °C)
- `absolute_humidity`: Actual mass of water vapor ($\text{g/m}^3$)
- `heat_index`: Perceived apparent "feels like" temperature (°C)
- `vpd`: Vapor Pressure Deficit (kPa)
- `freeze_risk`: Boolean state tag (true/false) when $T \le 3.0^\circ\text{C}$
- `frost_alert`: Event trigger emitted when temperature drops into freezing danger zone ($T \le 3.0^\circ\text{C}$, tag: Temp °C)

### Diagnostics & System Triggers
- `device_booted`: Emitted once on boot/reconnect. Tag: `reset_reason` (*Power on*, *Hardware Watchdog*, *Exception*)
- `diagnostics_heap`: Free RAM Heap (KB tag)
- `diagnostics_uptime`: System Uptime (Hours tag)
- `diagnostics_reconnects`: WiFi Reconnect Counter tag

---

## Remote Maintenance Actions (Homey Flow "THEN" Cards)

- **`reboot`**: Remotely reboots the Wemos D1 Mini board.
- **`reset_stats`**: Clears the WiFi reconnect counter and I2C error counters.

---

## Visual LED Status & Diagnostics

| LED Pattern | Timing / Rhythm | Status / Fault Meaning | Action Required |
| :--- | :--- | :--- | :--- |
| **Solid ON** | Continuous lit | **All OK** (WiFi connected, SHT30 sensor healthy) | None (Normal operation) |
| **Fast Blink** | 100ms ON / 100ms OFF | **Connecting / Booting** | Waiting for network connection |
| **Slow Blink** | 500ms ON / 500ms OFF | **WiFi Lost / Disconnected** | Check router / WiFi credentials in `secrets.h` |
| **Double Flash** | `* * ___ * * ___` (2 quick pulses) | **Sensor Fault** (I2C read failure / NaN) | Check SHT30 shield connection (pins D1/D2) |
| **Rapid Strobe** | 50ms ON / 50ms OFF | **OTA Update in Progress** | Do not power off device |

---

## Software Installation & Uploading

1. Open [`wemos-homeyduino-sht30.ino`](./wemos-homeyduino-sht30.ino) in Arduino IDE.
2. Ensure credentials are set in `secrets.h`.
3. Upload via USB or OTA.
4. Navigate to `http://<device-ip>/` in your browser to view the live dashboard.
5. In the Homey app, re-pair or restart the device to refresh capabilities.

---

## License
This project is licensed under the MIT License.
