# Wemos D1 Mini SHT30 Homeyduino Sensor

This project is an advanced environmental sensor node built using a **Wemos D1 Mini (ESP8266)** and an **SHT30 Shield**. It uses the **Homeyduino** library to seamlessly report both raw measurements and psychrometric derived metrics to an **Athom Homey** smart home gateway.

## Features
- **Real-time Monitoring:** Reads temperature and humidity from the I2C-based SHT30 sensor shield (address `0x45`).
- **Psycho-Climatic Calculations:** Automatically calculates dew point, absolute humidity, heat index ("feels like"), vapor pressure deficit (VPD), and mold risk.
- **Homey Integration:** Connects directly to Homey over WiFi using the Homeyduino protocol.
- **Serial Debugging:** Prints clear, structured tree diagnostics to the Serial Monitor (115200 baud).
- **GitHub-Friendly Structure:** Keeps sensitive WiFi credentials private in a git-ignored `secrets.h` file, preventing accidental leaks.
- **Non-blocking Timing:** Uses asynchronous `millis()` timing to avoid disrupting network socket loops.

---

## Capabilities Reported to Homey

| Capability ID | Display Name | Unit | Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| `measure_temperature` | Temperature | °C | Number | Ambient dry-bulb temperature measured by SHT30 |
| `measure_humidity` | Relative Humidity | % | Number | Relative humidity measured by SHT30 |
| `alarm_generic` | Mold Risk Alert | — | Boolean | Triggers when RH $\ge 70\%$, indicating conditions favoring mold |
| `alarm_water` | Condensation Risk Alert | — | Boolean | Triggers when $T - T_{\text{dew}} \le 2.0^\circ\text{C}$ (air near saturation) |
| `alarm_generic.freeze` | Freezing / Frost Alert | — | Boolean | Triggers when $T \le 3.0^\circ\text{C}$ (frost / pipe freeze warning) |

### Flow Triggers (Homey Flows)
- `dew_point`: Real-time dew point temperature (°C)
- `dew_point_spread`: Margin between ambient temperature and dew point ($T - T_{\text{dew}}$ in °C)
- `absolute_humidity`: Actual mass of water vapor ($\text{g/m}^3$)
- `heat_index`: Perceived apparent "feels like" temperature (°C)
- `vpd`: Vapor Pressure Deficit (kPa)
- `freeze_risk`: Boolean state tag (true/false) when $T \le 3.0^\circ\text{C}$
- `frost_alert`: Event trigger emitted when temperature drops into freezing danger zone ($T \le 3.0^\circ\text{C}$)

---

## Visual LED Status & Diagnostics

The Wemos D1 Mini built-in LED indicates real-time operational status without needing serial output:

| LED Pattern | Timing / Rhythm | Status / Fault Meaning | Action Required |
| :--- | :--- | :--- | :--- |
| **Solid ON** | Continuous lit | **All OK** (WiFi connected, SHT30 sensor healthy) | None (Normal operation) |
| **Fast Blink** | 100ms ON / 100ms OFF | **Connecting / Booting** | Waiting for network connection |
| **Slow Blink** | 500ms ON / 500ms OFF | **WiFi Lost / Disconnected** | Check router / WiFi credentials in `secrets.h` |
| **Double Flash** | `* * ___ * * ___` (2 quick pulses) | **Sensor Fault** (I2C read failure / NaN) | Check SHT30 shield connection (pins D1/D2) |
| **Rapid Strobe** | 50ms ON / 50ms OFF | **OTA Update in Progress** | Do not power off device |

---

## Smart Home Use Cases & Flow Ideas

1. **Intelligent Bathroom Fan Control (Absolute Humidity)**
   - *Flow*: IF `Absolute Humidity` rises $> 3\,\text{g/m}^3$ above normal room baseline, THEN turn on exhaust fan. Turn off when absolute humidity returns to baseline. (Avoids false triggers caused by winter temperature drops).
2. **Window Condensation Prevention (Dew Point / Condensation Alert)**
   - *Flow*: IF `Condensation Risk Alert` turns ON (or `Dew Point Spread` $\le 2.0^\circ\text{C}$), THEN send a notification to ventilate or lower humidifier output.
3. **Frost & Pipe Burst Protection (Freeze Risk / Frost Alert)**
   - *Flow*: IF `Frost Alert` triggers (or `Freeze Risk` is TRUE), THEN activate pipe heating cable / frost guard heater in cabin or garage and send urgent push notification.
4. **Plant Care / Greenhouse Climate (VPD)**
   - *Flow*: Maintain optimal VPD between $0.8 - 1.2\,\text{kPa}$ for vegetative growth by modulating misting humidifiers or ventilation fans.
5. **Mold & Mildew Prevention (Mold Risk Alert)**
   - *Flow*: IF `Mold Risk Alert` turns ON for $> 2$ hours, THEN turn on dehumidifier and alert homeowner.

---

## Hardware Requirements
1. **Wemos D1 Mini** (or compatible ESP8266 board)
2. **Wemos SHT30 Shield** (or any SHT30 sensor connected to `D1` / `SCL` and `D2` / `SDA`)
3. Micro-USB cable (for programming and power)

---

## Software Installation & Configuration

### 1. Arduino IDE Setup
If you haven't already configured the Arduino IDE for ESP8266:
1. Open the Arduino IDE.
2. Go to **File > Preferences**.
3. In **Additional Boards Manager URLs**, paste:
   ```text
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
4. Go to **Tools > Board > Boards Manager...**, search for `esp8266` by *ESP8266 Community*, and install the library.
5. Select your board under **Tools > Board > ESP8266 Boards > LOLIN(WEMOS) D1 R2 & mini**.

### 2. Required Libraries
Install the following libraries via **Tools > Manage Libraries...**:
- **Homeyduino** (by Athom)
- **Adafruit SHT31 Library** (by Adafruit) — When prompted to install dependencies (such as *Adafruit BusIO*), choose **"Install All"**.

### 3. WiFi Credentials Configuration
This repository keeps WiFi credentials secure by separating them into `secrets.h`, which is listed in `.gitignore` so that it is never committed to GitHub.

To configure your credentials:
1. Make a copy of `secrets.h.example` and name it `secrets.h` in the same directory:
   ```bash
   cp secrets.h.example secrets.h
   ```
2. Open `secrets.h` and edit your WiFi network name (SSID) and password:
   ```cpp
   #define SECRET_SSID "YOUR_WIFI_SSID"
   #define SECRET_PASS "YOUR_WIFI_PASSWORD"
   ```

---

## Uploading Firmware

### Initial Upload (via USB)
1. Connect your Wemos D1 Mini to your computer via USB.
2. Open [wemos-homeyduino-sht30.ino](./wemos-homeyduino-sht30.ino) in the Arduino IDE.
3. Select your board under **Tools > Board > ESP8266 Boards > LOLIN(WEMOS) D1 R2 & mini**.
4. Select the correct **COM Port** under **Tools > Port**.
5. Click **Upload** (the right-arrow icon at the top-left).
6. Once complete, open the **Serial Monitor** (**Tools > Serial Monitor** at `115200` baud) to verify WiFi and sensor initialization.

---

### Subsequent Uploads (Wireless Over-The-Air / OTA)
Once the sketch is uploaded over USB once and connected to your WiFi:
1. Open the Arduino IDE on the same WiFi network.
2. Go to **Tools > Port**.
3. Under **Network Ports**, you will see:
   ```text
   Wemos-SHT30-Sensor at 192.168.x.x
   ```
4. Select that network port and click **Upload**!
5. The firmware will flash wirelessly over WiFi without needing to plug the device into your PC.

---

## Pairing / Updating in Homey
- **If adding as a new device**:
  1. Open the **Homey App** > **Devices** > **+** > search for **Homeyduino**.
  2. Select **Homeyduino** and choose `Wemos-SHT30-Sensor`.
- **If already paired**:
  - Restart the device or trigger a capability sync in the Homeyduino app on Homey so Homey retrieves the newly registered capabilities.

---

## License
This project is licensed under the MIT License.
