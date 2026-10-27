# ESP32-C6 ES200 / ES90 Battery Monitor

An Arduino-based wake/keep-alive controller and local Wi-Fi status monitor for compatible ES200 / ES90 scooter batteries. Ideas and information were inspired by material shared by [JAG35](https://jag35.com/blogs/jehu-garcia-battery-university/the-curious-case-of-jehus-sleeping-scooter-batteries-how-to-wake-them-up).

The ESP32-C6 periodically sends the battery's observed wake command over UART, captures the battery's replies, and serves a browser dashboard with raw frames and provisional decoded telemetry. Wi-Fi credentials are configured through a setup portal rather than compiled into the sketch.

> **Experimental:** UART wake/response operation has been observed in this project, but the telemetry protocol is not fully verified. In particular, the current calculation is an empirical placeholder. Do not use this firmware as a battery protection device, charger controller, calibrated meter, or unattended safety system.

## Why use this project?

Some compatible scooter battery packs need communication from their original controller to remain enabled. This project provides a small, standalone way to send the observed keep-alive message without retaining the entire scooter controller.

It is useful for:

- Bench testing a compatible pack while keeping its communication interface active.
- Viewing battery responses from a phone or computer on the local network.
- Separating UART/wiring problems from telemetry decoding problems.
- Capturing raw frames to investigate an undocumented battery protocol.
- Exploring second-life battery projects, provided the surrounding electrical system has suitable independent protection.

Sending a wake command does not prove that a pack is healthy, safe to charge, or suitable for a particular load. It does not repair damaged cells or replace the battery's BMS.

## Features

- ESP32-C6 hardware UART at 9600 baud, 8 data bits, no parity, 1 stop bit.
- Wake command sent after startup networking completes and approximately every five seconds afterward.
- Separate histories for the last two transmitted frames and last two received frames.
- RX/TX byte counters, wake counter, frame ages, and RX pin level.
- Local browser dashboard that refreshes every three seconds.
- Provisional voltage, current, state-of-charge, temperature, and operating-mode decoding.
- Raw candidate values and decode notes for protocol investigation.
- Wi-Fi setup access point and captive portal.
- Saved Wi-Fi credentials using ESP32 Preferences.
- mDNS hostname: `http://es200battery.local/`.
- HTTP JSON endpoint for local scripts and experimental integrations.

No cloud account is required. There is no built-in MQTT, Home Assistant discovery, OTA updater, or persistent telemetry database.

## Screenshot of battery monitoring dashboard
<img width="1504" height="931" alt="image" src="https://github.com/user-attachments/assets/c0b0122f-7deb-4ff8-a355-0313e8a57fd3" />


## Compatibility and project status

The sketch targets an ESP32-C6 development board and the observed ES200 / ES90 UART frame family. Compatibility with every pack revision, connector, or firmware version is not established.

The baseline pin mapping is GPIO 6 for RX and GPIO 7 for TX. Earlier development versions used other pins; use the constants in the current sketch, not an older wiring screenshot.

The exact ESP32-C6 board model and Arduino core version used for a release should be recorded when testing it. Board-specific pin availability and USB arrangements vary.

## Safety first

Lithium battery packs can deliver hazardous fault current. An enabled pack can energize its output unexpectedly.

- Disconnect loads and charging equipment before changing wiring.
- Verify connector pinout and signal levels on your actual pack. Wire colors below describe the project harness, not a universal standard.
- Never connect battery pack voltage to an ESP32 GPIO, its 3.3 V pin, or its USB connector.
- Use 3.3 V-compatible UART signals. If the battery interface is not compatible, use a suitable interface circuit; a pull-up resistor is not a voltage converter.
- Pull the reply line up to the ESP32's regulated 3.3 V rail, not to pack voltage or 5 V.
- Use a suitable fuse, insulated connections, strain relief, and an enclosure for any load-bearing installation.
- Use only a charger appropriate for the battery chemistry, voltage, and pack requirements.
- Do not bypass the BMS or use this project to force operation of a damaged pack.
- Consider ground paths through a USB-connected computer, charger, and test equipment before connecting them together.

For initial firmware loading, power the board from USB with the battery interface disconnected. Reconnect only after verifying the wiring and voltage compatibility.

## Hardware required

| Item | Purpose |
| --- | --- |
| ESP32-C6 development board [Amazon link](https://www.amazon.com/dp/B0GVQZT6FD?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_2)| Runs the firmware and Wi-Fi dashboard |
| USB data cable | Programming and initial board power |
| Compatible ES200 / ES90 battery | UART communication target |
| 1 kΩ resistor | External reply-line pull-up for the observed setup |
| Suitable mating connector and insulated wiring | Battery communication harness |
| Computer with Arduino IDE | Compile and upload firmware |
| Local 2.4 GHz Wi-Fi network | Normal dashboard access |
| Multimeter; optional logic analyzer | Verify electrical levels and diagnose communication |
| Down Buck Converter Power Module 4.5-50V to 3-40V [Amazon link](https://www.amazon.com/dp/B0DCJM5M5P?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_1) | To convert the battery packs 30V-42V down to a stable 3.3V |

The ESP32 must have its own suitable regulated power supply. This README does not specify a pack-powered regulator design.

## Wiring

The names RX and TX below are from the ESP32's perspective.

| Observed battery harness | ESP32-C6 connection | Direction |
| --- | --- | --- |
| GREEN reply line | GPIO 6 (`BAT_RX_PIN`) | Battery → ESP32 |
| BLUE command line | GPIO 7 (`BAT_TX_PIN`) | ESP32 → battery |
| BLACK signal ground | GND | Common signal reference |
| External pull-up | Between GREEN/GPIO 6 and 3.3 V | Typically 1k ohm resistor will work. |

```text
ESP32-C6                            Battery communication harness

GPIO 7  (TX) ---------------------- BLUE  (battery command input)
GPIO 6  (RX) ---------------------- GREEN (battery reply output)
                  |
                 [1 kΩ]
                  |
                 3.3 V

GND ------------------------------- BLACK (verified signal ground)
```

The resistor connects from the reply line to 3.3 V; it is not placed in series with the UART wire. Verify that this arrangement suits your battery interface before using it.

Current sketch configuration:

```cpp
static const int BAT_RX_PIN = 6;
static const int BAT_TX_PIN = 7;

HardwareSerial BatterySerial(1);

pinMode(BAT_RX_PIN, INPUT_PULLUP);
pinMode(BAT_TX_PIN, OUTPUT);
digitalWrite(BAT_TX_PIN, HIGH);
BatterySerial.begin(9600, SERIAL_8N1, BAT_RX_PIN, BAT_TX_PIN);
```

These numbers refer to GPIO numbers, not physical header positions. Check your board's pinout; a label such as `D6` is not necessarily GPIO 6. The sketch enables an internal pull-up as well, but the observed hardware setup uses the external resistor.

Image of wiring from battery cable to esp32 board via down buck module:
<img width="1244" height="766" alt="image" src="https://github.com/user-attachments/assets/e1eb9220-bb14-4d74-809e-14f75b69bbae" />


## Install and upload with Arduino IDE

### 1. Install Arduino IDE

Install Arduino IDE 2.x from the official Arduino software page:

https://www.arduino.cc/en/software

### 2. Install ESP32 board support

1. Open Arduino IDE settings/preferences.
2. Add the following stable package index to “Additional Boards Manager URLs” if needed:

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Open Boards Manager.
4. Search for `esp32`.
5. Install `esp32` by Espressif Systems, using a current stable release with ESP32-C6 support.

Official installation instructions:

https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html

### 3. Open the sketch

Download or clone this repository and open its `.ino` file in Arduino IDE.

If starting from a text copy of the source, create a sketch named `es200battery`, replace the default contents with the full source, and save it as:

```text
es200battery/
└── es200battery.ino
```

The sketch folder and primary `.ino` filename should match. Use plain-text source, not an RTF document.

A suggested repository layout is:

```text
README.md
es200battery/
└── es200battery.ino
```

This is a suggested naming convention, not a requirement that an existing repository already uses these filenames.

### 4. Check dependencies

The sketch uses:

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <math.h>
```

These headers come from the ESP32 Arduino platform/toolchain. No additional third-party library installation is required for this baseline. If `WiFi.h` or another ESP32 platform header is missing, check the installed board package and selected target first.

### 5. Select the board and USB port

1. Connect the ESP32-C6 with a USB data cable.
2. Select the exact board profile if available.
3. If there is no matching profile and the board vendor permits the generic target, select `ESP32C6 Dev Module`.
4. Select the port that appears when connecting the board.
5. Leave flash size, partition scheme, and other settings at the board vendor's defaults unless the hardware requires otherwise.

Do not select the classic `ESP32 Dev Module` profile for an ESP32-C6.

For boards using the C6's native USB connector, enable `USB CDC On Boot` when available so `Serial` diagnostics can appear through native USB. Choose the native/internal USB upload option if the board profile exposes one and its documentation calls for it. Boards using a USB-to-UART bridge may need different settings or a bridge driver. Menu options are board/core dependent.

The battery UART is a separate `HardwareSerial(1)` interface; its baud rate is not changed by Serial Monitor settings.

### 6. Verify and upload

1. Keep the battery wiring disconnected during the first upload.
2. Click Verify to compile.
3. Click Upload.
4. Wait until the IDE reports successful completion.
5. Reset the board if it does not start automatically.

If the uploader cannot connect, use the board's download-mode procedure. A common sequence is to hold BOOT, press and release RESET, release BOOT, then select the newly enumerated port and upload. Button names and timing vary by board.

### 7. Open Serial Monitor

Set Serial Monitor to 115200 baud. Startup output includes networking progress, setup AP information or the assigned LAN IP, and UART diagnostics.

The two serial configurations are:

| Interface | Setting |
| --- | --- |
| Debug `Serial` | 115200 baud |
| Battery UART | 9600 baud, 8N1 |

After verifying firmware startup, power down and attach the verified battery communication harness. Restore power and observe the counters and raw frames.

## First-time Wi-Fi setup

1. Power on the ESP32-C6.
2. If there are no saved credentials, or the saved network cannot be reached during startup, it starts a setup network named `es200battery`.
3. Join that network with a phone or computer.
4. If the captive portal does not open automatically, visit `http://192.168.4.1/`.
5. Select a nearby network or enter its SSID manually, then enter its password.
6. Choose “Save and reboot.”
7. Reconnect your phone/computer to the normal local network.
8. Open `http://es200battery.local/`, or use the LAN IP printed in Serial Monitor.

The default setup AP is open because `AP_PASSWORD` is `nullptr`. Configure an AP password before deployment if appropriate:

```cpp
const char* AP_PASSWORD = "choose-a-unique-password";
```

Use at least eight characters. This protects joining the setup AP; it does not add authentication to the dashboard.

Credentials are stored through Preferences and reused after restart. Once the board successfully connects as a Wi-Fi station, it does not also run the setup AP. If Wi-Fi drops during normal operation, the current firmware reboots to retry and may fall back to setup mode.

## Normal operation

After startup networking finishes, the board sends:

```text
3A 13 01 16 79
```

It repeats the command approximately every 5000 ms. The interval is managed in the main loop, so it is not a hard real-time guarantee.

The battery's incoming bytes are counted and buffered. A receive gap greater than 100 ms ends the captured frame. The buffer is 128 bytes; buffer overflow also forces processing of the accumulated bytes. Expected status reports are 36 bytes beginning with:

```text
3A 16 20
```

A correctly shaped frame shows that the software received the expected header/length family. It does not establish that every decoded field is correct, and the current decoder does not verify a checksum.

### Dashboard

The page includes:

- Network name, IP, hostname, uptime, and Wi-Fi RSSI.
- Wake command count and battery RX/TX byte counts.
- Instantaneous RX pin level.
- Provisional operating mode, voltage, current, SOC, and temperatures.
- Last capture age, frame length, and decoder notes.
- Candidate raw field values.
- Latest frame submitted to the decoder.
- Separate last-two TX and last-two RX frame histories.

An RX pin level of `1` is only a point-in-time sample. It does not prove that replies are being received. Rising RX counters and repeatable captured frames are more useful communication checks.

### Dashboard actions

| Path | Behavior |
| --- | --- |
| `/` | Dashboard in normal station mode; setup portal in AP mode |
| `/sendWake` | Send the wake command immediately, then return to dashboard |
| `/clear` | Clear TX/RX histories, decoder state, and communication counters |
| `/json` | Return network, frame-history, and decoded fields as JSON |
| `/resetwifi` | Clear saved Wi-Fi credentials and reboot |

Setup mode also provides `/scan`, POST `/save`, and `/clearwifi`. The setup-mode `/clearwifi` handler clears credentials but does not reboot automatically.

Clearing history does not disable periodic wake commands. There is no dashboard stop-wake control in this baseline.

## Telemetry decoding: implemented behavior, not a verified specification

All offsets below are zero-based. `BE16` means a big-endian unsigned 16-bit value: `(data[i] << 8) | data[i + 1]`.

The decoder first requires exactly 36 bytes and the `3A 16 20` header.

| Field | Current implementation | Limitations |
| --- | --- | --- |
| Mode indicator | Byte 3; `0x02` labeled normal/discharge, `0x0F` labeled charging | Observed interpretation; not an authoritative protocol definition |
| Pack voltage | `BE16[32..33] / 100.0` volts | Accepted between 20 and 60 V; verify against a meter |
| State of charge | Byte 34 interpreted directly as percent | Accepted if 0–100; verify across charge/discharge states |
| Temperatures | Bytes 7–10 interpreted directly as °C | Minimum/maximum computed; main temperature uses maximum; sensor meaning and encoding remain unverified |
| Current | `(60415 - BE16[25..26]) / 13446.0` amperes | Empirical provisional calibration, not a confirmed protocol conversion |
| Cycle-like value | `BE16[5..6]` | Placeholder meaning; do not treat as a confirmed cycle count |
| MOSFET/mode-like value | Byte 11 | Placeholder meaning |
| Trailing value | Byte 35 | Unknown purpose; not checked as a checksum |

Current magnitudes below 0.12 A are forced to zero. The code accepts the resulting current if it is between -10 and 20 A, but this range check does not establish measurement accuracy. With the existing unsigned raw field and constants, the formula cannot span that entire acceptance range.

Mode selection prioritizes byte 3 equal to `0x0F` as “Charging.” Otherwise it uses the provisional current: approximately zero becomes “Idle,” positive becomes “Discharging,” and negative also becomes “Idle.” Consequently, a mode label is not an independently verified indication of power flow.

The `valid` status is true if any decoded field passes its internal check. It does not mean all telemetry is valid. Temperature validity is set after the header/length check without a separate plausibility filter.

The status structure contains low/high cell-voltage fields, but this baseline does not populate them. It does not provide verified individual-cell monitoring.

## JSON endpoint and integration caveats

From a machine on the same network:

```sh
curl http://es200battery.local/json
```

Use the board's actual LAN IP if mDNS is unavailable.

The response contains:

- Network fields: `ip`, `mdns`, `ssid`, `rssi`, `uptime_sec`.
- Communication fields: `rx_count`, `tx_count`, `wake_count`, `rx_pin_level`.
- `tx_frames` and `rx_frames`, each containing two entries with `hex`, `len`, and a human-readable `age`.
- A `decoded` object containing `valid`, `mode`, `charging_detected`, `voltage`, `current`, `soc`, `temp_c`, `last_frame_len`, `note`, and `last_frame_hex`.

Important limitations for consumers:

- Unknown numeric telemetry is serialized as `0.0`, not JSON `null`. A zero may mean unavailable data rather than a real zero reading.
- Individual field-validity flags are not included in the JSON response.
- `decoded.valid` only means at least one field passed the current checks.
- Raw frame ages help assess freshness; decoded values are not automatically expired after a timeout.
- There is no persistent history, streaming API, or measurement accuracy guarantee.

Use the endpoint for diagnostics and experimental display integrations, not automatic safety decisions.

## Configuration

| Setting | Default | Purpose |
| --- | --- | --- |
| `BAT_RX_PIN` | `6` | Battery reply input |
| `BAT_TX_PIN` | `7` | Battery command output |
| `wakeIntervalMs` | `5000` | Keep-alive interval |
| `frameGapMs` | `100` | Idle gap used to end a received frame |
| `DEVICE_NAME` | `es200battery` | Setup AP name |
| `MDNS_NAME` | `es200battery` | Local hostname prefix |
| `AP_PASSWORD` | `nullptr` | Open setup AP unless configured |
| Startup Wi-Fi timeout | `20000` ms | Time spent trying saved credentials |

Changes to these constants require recompiling and uploading. For multiple monitors on one network, choose distinct device and mDNS names.

## Troubleshooting

### Compilation fails or ESP32-C6 is missing

- Install/update the official Espressif ESP32 board package.
- Confirm that the selected target is the ESP32-C6, not a classic ESP32, C3, or S3.
- Confirm that the source is a complete plain-text `.ino` file.
- Check that the sketch folder and main filename match.

### Upload fails or no USB port appears

- Use a known USB data cable and another USB port.
- Check the board's connector documentation; some boards have separate native USB and UART connectors.
- Install the board's USB-to-UART bridge driver if applicable.
- Try the BOOT/RESET download procedure described by the board vendor.
- Re-select the port if it changes after reset or entering download mode.
- Reduce upload speed if the connection is unreliable.

### Serial Monitor is blank

- Select the current port and use 115200 baud.
- Reset the board after opening the monitor.
- For native USB, check USB CDC settings and the board's USB configuration.
- Debug Serial and the battery UART are different interfaces.

### Setup portal does not appear

- Join `es200battery` directly and open `http://192.168.4.1/` using HTTP.
- A phone may disconnect from a network without Internet access; keep it connected for setup.
- If the board already joined its saved Wi-Fi, use its LAN IP or hostname instead: the setup AP is not active then.

### Hostname does not resolve

Use the LAN IP from Serial Monitor or your router's client list. mDNS can be blocked across VLANs, guest networks, or client-isolated Wi-Fi.

### TX increases but RX remains zero

- Recheck the latest mapping: RX GPIO 6, TX GPIO 7.
- Verify the battery's actual command/reply wires, not just their colors.
- Verify a common signal ground and a compatible logic-level interface.
- Check that the reply-line pull-up is connected to 3.3 V.
- Confirm 9600 baud and 8N1.
- Measure the reply signal or inspect it with a suitably rated logic analyzer.

Do not change the decoder to solve a zero-byte receive problem: first establish that data is arriving.

### RX increases but frame length/header is wrong

- Check signal quality, levels, ground, baud rate, and wiring.
- Check whether the pack uses a different protocol revision.
- Remember that the current receiver splits frames by an idle gap, not by protocol length or checksum. Partial packets or back-to-back responses can be grouped incorrectly.
- Capture raw traffic before changing offsets or timing.

### Voltage shows `--` or current is implausible

`--` means the dashboard value is unavailable. It can result from a rejected frame or a failed range check. Repeated clean raw frames with implausible decoded values point toward a decoder issue rather than necessarily a UART problem.

Verify voltage against a meter and current against an appropriate independent instrument. Do not fit arbitrary offsets just to produce a plausible display. Keep current labeled provisional until validated across multiple operating conditions.

### Reboots after Wi-Fi disappears

This is the current firmware behavior. A reboot interrupts normal processing and restarts networking before the first startup wake command is sent. Do not assume continuous keep-alive operation during network faults.

## Security and deployment limitations

The baseline uses HTTP without authentication or TLS. Any client that can reach it may view telemetry, send wake requests, clear counters, or reset Wi-Fi. State-changing actions use GET routes.

- Keep it on a trusted, restricted local network.
- Do not expose it with Internet port forwarding.
- Set an AP password if the setup network should not be open.
- Treat saved Wi-Fi credentials and the board itself as sensitive; this sketch does not implement application-level credential encryption.
- Recognize that Wi-Fi failure can expose the setup AP again.

The firmware has no hard real-time wake guarantee, checksum validation, watchdog-based battery control policy, or independent protection logic. It is a development monitor and communication tool.

## Contributing and reporting issues

Useful reports include:

1. ESP32-C6 board manufacturer/model and selected Arduino board profile.
2. Arduino IDE version, Espressif core version, and relevant USB settings.
3. Battery model, connector identification, and hardware/firmware revision if known.
4. Actual GPIO wiring and pull-up arrangement.
5. TX/RX counters, frame lengths, and complete raw hexadecimal frames.
6. Independent voltage/current measurements and whether the pack was idle, charging, or under a known load.
7. Reproduction steps and whether the failure follows a network interruption.

Remove Wi-Fi passwords, private network details, and unnecessary identifiers before posting. Protocol changes should include evidence from multiple captures and operating states, not only a single plausible-looking value.

## Background and further reading

This project was developed while following Jehu Garcia / JAG35 battery reuse guidance. It is an independent project and does not imply affiliation, endorsement, or compatibility certification.

- JAG35: https://jag35.com/
- Arduino IDE: https://www.arduino.cc/en/software
- Espressif Arduino installation: https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
- Espressif board/tool settings: https://docs.espressif.com/projects/arduino-esp32/en/latest/guides/tools_menu.html
- Espressif USB CDC flashing: https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html
- ESP32-C6 datasheet: https://documentation.espressif.com/esp32-c6_datasheet_en.html

## License

See the repository's `LICENSE` file for the applicable terms, if one has been added. This README does not assign a license or grant rights to third-party material. Before distributing the project as open source, the repository owner should choose and include an appropriate license.

