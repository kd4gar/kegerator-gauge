# kegerator-gauge

An ESP32 drives a real 12V automotive gauge to show **CO2 keg pressure** on a kegerator. It also
serves a phone-friendly web page with the pressure and keg / fridge-air temperatures.

The gauge is an electronic oil-temperature gauge (terminals **S / I / GND**, dial 130–300). The
ESP32 fakes the gauge's temperature sender, so the needle can be put anywhere on the dial from
code.

![Wiring](wiring-diagram.svg)

## Status

| Part | State |
|---|---|
| Gauge driven from the ESP32 (PWM + MOSFET), dial calibrated | Done |
| 0–60 psi pressure sensor read and calibrated | Done (span to be re-done with the CO2 regulator) |
| 12V supply + 12V→5V buck converter | Done |
| Home Wi-Fi, `http://kegerator.local`, over-the-air updates | Done |
| Web display page (pressure dial, temps) + password-protected config page | Done |
| 2× DS18B20 temperature probes (keg + fridge air) | Planned. The page shows "no probe" until then |
| Needle driven from pressure (0 psi→140, 12→210, 30→260, 60→300 on the dial) | Planned |

## Hardware

- **ESP32 dev board** (CP2102, USB-C) on a screw-terminal breakout
- **12V electronic gauge** with S / I / GND terminals and a 2-wire 12V backlight
- **IRLZ44N** logic-level N-MOSFET, **22Ω 1W**, **47µF 25V**, **220Ω**, **10k**
- **0–60 psi pressure transducer**, 5V, 0.5–4.5V output, 1/8" NPT (red +5V, black GND, green signal)
- **10k + 18k** voltage divider for the sensor signal (4.5V → ~2.9V for the ESP32)
- **12V 5A supply** and **12V→5V 5A buck converter**
- Planned: 2× **DS18B20** waterproof probes + **4.7k** pull-up

### Pins

| Pin | Use |
|---|---|
| GPIO18 (P18) | Gauge PWM → MOSFET gate |
| GPIO34 (P34) | Pressure sensor, through the 10k/18k divider (ADC1, works with Wi-Fi on) |
| GPIO4 (P4) | DS18B20 1-Wire bus (planned) |

### How the gauge is driven

The gauge expects a thermistor between **S** and ground: lower resistance moves the needle up. A
MOSFET switches S to ground through 22Ω with 5 kHz PWM. More duty means lower average
resistance, which moves the needle up. The 47µF capacitor on S smooths the PWM. The response is
non-linear (the top of the dial needs much more duty per degree), so a measured calibration
table (`cal[]`) maps dial values to duty.

**Never connect S directly to an ESP32 pin.** It can sit near 12V.

### Wiring notes

- All grounds are common: 12V supply −, buck converter, ESP32, gauge, MOSFET source and sensor.
- Divider order matters: **10k from the sensor's green wire to P34, 18k from P34 to GND**.
- Set the buck converter to 5.0V **before** connecting it.
- Don't power the ESP32 from USB and the buck converter at the same time. It isn't yet known
  whether this board isolates USB 5V from the 5V pin.
- `esp32_gauge_wiring.png` is the original proof-of-concept wiring (gauge only, USB power).
  `wiring-diagram.svg` is the current wiring.

## Software

Arduino IDE 2 with the **esp32 by Espressif** core (3.x). Board: **ESP32 Dev Module**, Partition
Scheme **Default 4MB with spiffs** (it has room for OTA). No extra libraries are needed. Wi-Fi,
WebServer, mDNS, ArduinoOTA, HTTPUpdateServer and Preferences all come with the core.

| File | What it is |
|---|---|
| `PhysicalGauge/PhysicalGauge.ino` | The sketch |
| `PhysicalGauge/web_page.h` | The web pages (display, config, Wi-Fi setup) |
| `ota-upload.bat` | Compile and upload over Wi-Fi (Windows) |
| `wiring-diagram.svg` | Current wiring |

### First-time Wi-Fi setup

1. Upload once over USB.
2. With no home network saved, the ESP32 starts its own network **Kegerator-Setup**.
3. Join it from a phone and open `http://192.168.4.1/wifi`. Choose your 2.4 GHz network and
   enter its password.
4. It reboots and joins your network. Then open **http://kegerator.local**.

Home Wi-Fi credentials are stored in the ESP32's flash, **never in the source code**. If the
home network is unreachable for 20 seconds, the setup network comes back on. The gauge keeps
working without Wi-Fi.

### Web pages

| Page | Access | Contents |
|---|---|---|
| `/` | Open | CO2 pressure dial (0–60 psi, green 10–14 psi serving band), keg + air temps |
| `/config` | Login | Live sensor readings, zero / span calibration, system info |
| `/wifi` | Login | Change the home network |
| `/update` | Login | Upload a firmware `.bin` |
| `/data` | Open | All live values as JSON |

The login is user `OTA_USER` with password `OTA_PASS`, both set near the top of the sketch.

### Updating over Wi-Fi (OTA)

Double-click **`ota-upload.bat`**. It compiles the sketch, then uploads it with `espota` straight
to the IP address in the batch file. If that fails, it posts the `.bin` to the board's `/update`
page instead. Set `ESP32_IP` and `OTA_PASS` at the top of the batch file.

The Arduino IDE's "Network ports" menu may not list the board on PCs with extra virtual network
adapters. The batch file avoids that by going straight to the IP address.

### Pressure calibration

Do this on the final power supply, because the sensor's output follows its 5V supply.

1. **Zero:** sensor open to air → **Set zero** on `/config` (or `z` in Serial Monitor).
2. **Span:** apply a known pressure, enter the reference gauge's reading → **Set span** (or `k30`).

Both are saved in flash and survive reboots and updates. Readings far outside the sensor's
spec are rejected, so a wiring fault can't be saved as a calibration.

### Serial commands (115200 baud, Newline)

| Command | Action |
|---|---|
| `180` | Put the needle on 180 (dial units, from `cal[]`) |
| `d512` | Raw PWM duty 0–1023 (for dial calibration) |
| `s` / `w` | Fast sweep / slow calibration sweep (Enter stops it) |
| `p` | Toggle live pressure readout |
| `z` / `k30` / `r` | Zero / span at 30 psi / clear saved pressure calibration |
| `wifi` / `wififorget` | Wi-Fi status / erase saved network and reboot |
| `?` | Current state and calibration |
