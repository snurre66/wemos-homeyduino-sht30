# Wemos D1 Mini SHT30 Homeyduino Sensor

This project is a temperature and humidity sensor node built using a **Wemos D1 Mini (ESP8266)** and an **SHT30 Shield**. It uses the **Homeyduino** library to seamlessly report measurements to an **Athom Homey** smart home gateway.

## Features
- **Real-time Monitoring:** Reads temperature and humidity from the I2C-based SHT30 sensor shield.
- **Homey Integration:** Connects directly to Homey over WiFi using the Homeyduino protocol.
- **Serial Debugging:** Prints detailed status, connection, and sensor readings to the Serial Monitor (115200 baud).
- **GitHub-Friendly Structure:** Keeps sensitive WiFi credentials private in a git-ignored `secrets.h` file, preventing accidental leaks.
- **Non-blocking Timing:** Uses asynchronous `millis()` timing to avoid disrupting network socket loops.

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
   *(Note: The sketch has already been pre-filled with the credentials you provided in the session workspace, so you can upload directly if they are correct!)*

---

## Uploading the Code
1. Connect your Wemos D1 Mini to your computer via USB.
2. Open [wemos-homeyduino-sht30.ino](./wemos-homeyduino-sht30.ino) in the Arduino IDE.
3. Select the correct **COM Port** under **Tools > Port**.
4. Click **Upload** (the right-arrow icon at the top-left).
5. Once complete, open the **Serial Monitor** (**Tools > Serial Monitor**).
6. Set the baud rate to **115200**. You should see the boot message, WiFi connection sequence, and sensor output:
   ```text
   === Wemos D1 Mini SHT30 Shield Setup ===
   Connecting to WiFi network: TheVeivaags
   ....
   WiFi successfully connected!
   IP Address: 192.168.1.150
   Initializing Homeyduino...
   Capabilities registered: measure_temperature, measure_humidity
   Sensor configuration complete. Starting measurement loop.
   
   [15s] Reading sensor... Temp: 22.4 °C | Hum: 45.2 %
   ```

---

## Pairing with Homey
Once your device is running and connected to WiFi:
1. Ensure your smartphone or computer is on the same WiFi network as the Wemos device.
2. Open the **Homey App**.
3. Tap **Devices** > **+** (Add Device) > search for **Homeyduino**.
4. Select the **Homeyduino** brand and click **Add**.
5. Homey will scan the local network and discover your device as `Wemos-SHT30-Sensor`.
6. Follow the prompt to pair the device. Once added, you will see a new sensor device showing live temperature and humidity values.

---

## Repository Hosting on GitHub
To publish this code to GitHub securely:
1. Create a new repository on GitHub (keep it public or private, as you prefer).
2. Open a terminal in the project directory `C:\Users\thoma\.gemini\antigravity\scratch\wemos-homeyduino-sht30`.
3. Initialize git and commit the files:
   ```bash
   git init
   git add .
   git commit -m "Initial commit: Homeyduino SHT30 sensor sketch"
   ```
4. Link it to your GitHub repository and push:
   ```bash
   git branch -M main
   git remote add origin https://github.com/YOUR_GITHUB_USERNAME/YOUR_REPOSITORY_NAME.git
   git push -u origin main
   ```
5. Notice that `secrets.h` is excluded from git tracking automatically, keeping your credentials safe.

---

## License
This project is licensed under the MIT License.
