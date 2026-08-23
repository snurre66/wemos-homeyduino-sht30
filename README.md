# Wemos D1 Mini SHT30 Homeyduino Sensor & Diagnostics Node

This project is an advanced environmental sensor and diagnostics node built using a **Wemos D1 Mini (ESP8266)** and an **SHT30 Shield**. It uses the **Homeyduino** library to seamlessly report raw measurements, psychrometric climate derivations, system/network health diagnostics, and remote maintenance actions to an **Athom Homey** smart home gateway.

## Features
- **Real-time Environmental Monitoring:** Reads temperature and humidity from the I2C-based SHT30 sensor shield (address `0x45`).
- **Psycho-Climatic Calculations:** Automatically calculates dew point, absolute humidity, heat index ("feels like"), vapor pressure deficit (VPD), mold risk, and freezing risk.
- **Hardware & Network Diagnostics:** Continuously monitors WiFi signal strength (RSSI), internal supply rail voltage ($V_{cc}$), free heap memory, fragmentation, and WiFi reconnect counters.
- **Remote Maintenance Actions:** Supports Homey Flow "THEN" cards to remotely reboot the ESP8266 or reset diagnostic counters.
- **Boot Notifications:** Emits a `device_booted` trigger to Homey on startup containing the reset reason (Power On, Watchdog, Exception).
- **Homey Integration:** Connects directly to Homey over WiFi using the Homeyduino protocol.
- **Serial Debugging:** Prints clear, structured tree diagnostics to the Serial Monitor (115200 baud).
- **Over-The-Air (OTA) Updates:** Allows wireless firmware flashing directly from the Arduino IDE.
- **GitHub-Friendly Structure:** Keeps sensitive WiFi credentials private in a git-ignored `secrets.h` file.
- **Non-blocking Timing:** Uses asynchronous `millis()` timing loops (15s sensor / 60s diagnostics) to maintain network responsiveness.

---

## Capabilities Reported to Homey Device Tile

| Capability ID | Display Name | Unit | Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| `measure_temperature` | Temperature | °C | Number | Ambient dry-bulb temperature measured by SHT30 |
| `measure_humidity` | Relative Humidity | % | Number | Relative humidity measured by SHT30 |
| `measure_signal_strength` | Signal Strength | dBm | Number | WiFi RSSI signal strength (e.g. `-65 dBm`) |
| `measure_voltage` | Supply Voltage ($V_{cc}$) | V | Number | ESP8266 internal supply rail voltage (measured via `ADC_MODE(ADC_VCC)`) |
| `alarm_generic.mold` | Mold Risk Alert | — | Boolean | Triggers when RH $\ge 70\%$, indicating conditions favoring mold |
| `alarm_water` | Condensation Risk Alert | — | Boolean | Triggers when $T - T_{\text{dew}} \le 2.0^\circ\text{C}$ (air near saturation) |
| `alarm_generic.freeze` | Freezing / Frost Alert | — | Boolean | Triggers when $T \le 3.0^\circ\text{C}$ (frost / pipe freeze warning) |
| `alarm_generic.sensor` | Sensor Fault Alert | — | Boolean | Triggers when SHT30 I2C communication fails or reads invalid data |

---

## Flow Triggers & Tags (Homey Flows)

### Environmental Triggers
- `dew_point`: Real-time dew point temperature (°C)
- `dew_point_spread`: Margin between ambient temperature and dew point ($T - T_{\text{dew}}$ in °C)
- `absolute_humidity`: Actual mass of water vapor ($\text{g/m}^3$)
- `heat_index`: Perceived apparent "feels like" temperature (°C)
- `vpd`: Vapor Pressure Deficit (kPa)
- `freeze_risk`: Boolean state tag (true/false) when $T \le 3.0^\circ\text{C}$
- `frost_alert`: Event trigger emitted when temperature drops into freezing danger zone ($T \le 3.0^\circ\text{C}$)

### Diagnostics & System Triggers
- `device_booted`: Emitted once on boot/reconnect. Tag: `reset_reason` (e.g., *Power on*, *Hardware Watchdog*, *Exception*)
- `diagnostics_heap`: Free RAM Heap (KB tag)
- `diagnostics_uptime`: System Uptime (Hours tag)
- `diagnostics_reconnects`: WiFi Reconnect Counter tag

---

## Remote Maintenance Actions (Homey Flow "THEN" Cards)

You can trigger these actions directly from any Homey Flow "THEN" card:
- **`reboot`**: Remotely reboots the Wemos D1 Mini board.
- **`reset_stats`**: Clears the WiFi reconnect counter and I2C error counters.

---

## Visual LED Status & Diagnostics

The Wemos D1 Mini built-in LED indicates real-time operational status:

| LED Pattern | Timing / Rhythm | Status / Fault Meaning | Action Required |
| :--- | :--- | :--- | :--- |
| **Solid ON** | Continuous lit | **All OK** (WiFi connected, SHT30 sensor healthy) | None (Normal operation) |
| **Fast Blink** | 100ms ON / 100ms OFF | **Connecting / Booting** | Waiting for network connection |
| **Slow Blink** | 500ms ON / 500ms OFF | **WiFi Lost / Disconnected** | Check router / WiFi credentials in `secrets.h` |
| **Double Flash** | `* * ___ * * ___` (2 quick pulses) | **Sensor Fault** (I2C read failure / NaN) | Check SHT30 shield connection (pins D1/D2) |
| **Rapid Strobe** | 50ms ON / 50ms OFF | **OTA Update in Progress** | Do not power off device |

---

## Smart Home Use Cases & Flow Ideas

1. **Device Reboot & Health Monitoring**
   - *Flow*: IF `alarm_generic.sensor` turns ON, THEN send push notification *"SHT30 sensor fault on Wemos"*, wait 1 minute, and execute action `reboot`.
2. **Boot Notification & Power Outage Detection**
   - *Flow*: WHEN `device_booted` triggers, THEN send notification *"Wemos Sensor restarted. Reason: {{reset_reason}}"*.
3. **Intelligent Bathroom Fan Control (Absolute Humidity)**
   - *Flow*: IF `Absolute Humidity` rises $> 3\,\text{g/m}^3$ above room baseline, THEN turn on exhaust fan.
4. **Window Condensation Prevention (Dew Point / Condensation Alert)**
   - *Flow*: IF `Condensation Risk Alert` turns ON, THEN send notification to ventilate.
5. **Frost & Pipe Burst Protection (Freeze Risk / Frost Alert)**
   - *Flow*: IF `Frost Alert` triggers, THEN activate pipe heating cable / frost guard heater.

---

## Hardware Requirements
1. **Wemos D1 Mini** (or compatible ESP8266 board)
2. **Wemos SHT30 Shield** (connected to `D1` / `SCL` and `D2` / `SDA`)
3. Micro-USB cable (for programming and power)

---

## Software Installation & Configuration

### 1. Arduino IDE Setup
1. Open Arduino IDE > **File > Preferences**.
2. In **Additional Boards Manager URLs**, paste:
   ```text
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
3. Go to **Tools > Board > Boards Manager...**, search for `esp8266` and install.
4. Select board: **Tools > Board > ESP8266 Boards > LOLIN(WEMOS) D1 R2 & mini**.

### 2. Required Libraries
Install via **Tools > Manage Libraries...**:
- **Homeyduino** (by Athom)
- **Adafruit SHT31 Library** (by Adafruit) — choose **"Install All"** dependencies.

### 3. WiFi Credentials Configuration
1. Make a copy of `secrets.h.example` named `secrets.h`:
   ```bash
   cp secrets.h.example secrets.h
   ```
2. Edit `secrets.h` with your SSID and password:
   ```cpp
   #define SECRET_SSID "YOUR_WIFI_SSID"
   #define SECRET_PASS "YOUR_WIFI_PASSWORD"
   ```

---

## Uploading Firmware

### Initial Upload (via USB)
1. Connect Wemos D1 Mini via USB.
2. Select COM Port under **Tools > Port**.
3. Click **Upload**.
4. Open **Tools > Serial Monitor** (`115200` baud) to verify startup.

### Subsequent Uploads (Wireless Over-The-Air / OTA)
1. In Arduino IDE on the same WiFi network, go to **Tools > Port**.
2. Select network port: `Wemos-SHT30-XXXX at 192.168.x.x`.
3. Click **Upload** to flash wirelessly over WiFi.

---

## Pairing / Updating in Homey
1. Open Homey App > **Devices** > **+** > search for **Homeyduino**.
2. Select your sensor (e.g., `Wemos-SHT30-1A2B`).
3. If previously paired, restart the device or trigger a capability sync in Homey so all 8 capabilities and actions become active.

---

## License
This project is licensed under the MIT License.
