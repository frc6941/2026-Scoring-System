# 2026 FRC REBUILT Ethernet scoring client

This firmware is the wired W5500 equivalent of the project `esp32_scoring.ino`
example. It receives `matchTime` frames from
`ws://<arena-ip>:8080/api/arena/websocket`, sends score increments with
`PATCH /api/scores`, and reads `GET /api/scores` during the transition shift.

## Network

Connect the computer and both ESP32 W5500 modules to the same Ethernet switch.
Configure the computer interface as `67.67.67.1/24`; Cheesy Arena Lite must
listen on port `8080`. The blue Hub is `67.67.67.2` with MAC suffix `BE`; the
red Hub is `67.67.67.3` with MAC suffix `ED`. No Internet gateway is required.

## Hardware

The defaults match the supplied W5500 wiring: CS 14, MISO 12, MOSI 11, SCK 13.
IR inputs are 33, 34, 35, and 36 and trigger on LOW with internal pull-ups.

GPIO 15 is a logic-level Hub status output for the Arduino Uno LED controller:
it is `HIGH` while the Hub is active and `LOW` while inactive. The ESP32 does
not drive the LED strip. Connect GPIO 15 to the Uno `EDGE_PIN` (with a shared
ground); the Uno code owns the solid, breathing, and off LED states. The output
goes LOW immediately on deactivation, including during the three-second scoring
grace period, so the Uno can start its breathing sequence on the falling edge.

## Build and upload

Install PlatformIO, connect the ESP32 over USB, and run:

```powershell
# Flash one board at a time using its alliance environment.
py -m platformio run -e blue-hub -t upload
py -m platformio run -e red-hub -t upload
py -m platformio device monitor
```

Install the `Ethernet` Arduino library appropriate for the W5500 board if it
is not already supplied by the selected ESP32 framework. `ArduinoJson` is
declared in `platformio.ini`.
