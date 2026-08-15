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
| `measure_dew_point` | Dew Point | °C | Number | Temperature at which air reaches water vapor saturation and condenses |
| `measure_humidity.absolute` | Absolute Humidity | $\text{g/m}^3$ | Number | Physical mass of water vapor per cubic meter of air |
| `measure_temperature.feels_like`| Feels Like | °C | Number | Heat index / perceived apparent temperature |
| `measure_vpd` | Vapor Pressure Deficit | kPa | Number | Difference between saturated and actual vapor pressure (plant growth / drying) |
| `alarm_generic` | Mold Risk Alert | — | Boolean | Triggers when RH $\ge 70\%$, indicating conditions favoring mold |

---

## Smart Home Use Cases & Flow Ideas

1. **Intelligent Bathroom Fan Control (Absolute Humidity)**
   - *Flow*: IF `Absolute Humidity` rises $> 3\,\text{g/m}^3$ above normal room baseline, THEN turn on exhaust fan. Turn off when absolute humidity returns to baseline. (Avoids false triggers caused by winter temperature drops).
2. **Window Condensation Prevention (Dew Point)**
   - *Flow*: IF `Dew Point` is higher than the outside window glass surface temperature, THEN send a notification to ventilate or lower humidifier output.
3. **Plant Care / Greenhouse Climate (VPD)**
   - *Flow*: Maintain optimal VPD between $0.8 - 1.2\,\text{kPa}$ for vegetative growth by modulating misting humidifiers or ventilation fans.
4. **Mold & Mildew Prevention (Mold Risk Alert)**
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
