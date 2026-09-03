# 2026 FRC REBUILT Hub node

This firmware is a wired W5500 Hub node for the UDP protocol used by FRC 4201's
[`showdown-arena`](https://github.com/4201VitruvianBots/showdown-arena) upper
computer. It has no Cheesy Arena Lite REST or WebSocket compatibility layer.

## 4201 UDP protocol

The Hub listens for `node_command` JSON on UDP port `5300` and sends a
`node_status` JSON packet to the arena on UDP port `5301` every 100 ms.

```json
{
  "type": "node_command",
  "hubState": "SCORING_ACTIVE",
  "motorDuty": 1.0,
  "ledPattern": "red_alliance",
  "matchState": 3
}
```

```json
{
  "type": "node_status",
  "role": "RED_HUB",
  "fieldEStop": false,
  "stations": [
    {"eStop": false, "aStop": false},
    {"eStop": false, "aStop": false},
    {"eStop": false, "aStop": false}
  ],
  "score": 42
}
```

`score` is a monotonically increasing local counter. Showdown Arena records its
baseline at match start and calculates match deltas itself; the Hub must not send
HTTP score increments or reset its counter at a match transition.

Blue builds advertise `BLUE_HUB`; red builds advertise `RED_HUB`. The arena
must use the corresponding exact role and configure each Hub's IP address.

## Network

The existing static network assignment is retained:

| Device | Address |
| --- | --- |
| Arena / upper computer | `67.67.67.1` |
| Blue Hub | `67.67.67.2` |
| Red Hub | `67.67.67.3` |

In Showdown Arena, enable Alternate IO and set `Blue Hub Address` to
`67.67.67.2` and `Red Hub Address` to `67.67.67.3`. Before the first valid
command, status packets target `67.67.67.1`; afterward the Hub replies to the
source IP of the most recent valid `node_command`. This permits a 4201-format
upper computer at another address on the same field subnet. Commands are not
authenticated, so operate the system on an isolated field network: any host
that can send a valid command can update the Hub state and status target.

## Hardware

The W5500 uses CS `14`, MISO `12`, MOSI `11`, and SCK `13`. The four
photoelectric inputs are GPIO `33`, `34`, `35`, and `36`; they are low-active
and configured with internal pull-ups. Every debounced low transition adds one
to the cumulative score. As in the 4201 reference Hub, a fresh
`DEBUG_MOTOR_SPINUP` command clears that raw counter for manual testing.

ESP32 GPIO `15` connects to Arduino Uno `D2` with a shared ground. It is a
one-way `9600` baud, 8N1 UART link from ESP32 TX to Uno RX, not a binary
active/inactive signal. The ESP32 sends one command every 40 ms:

| Byte | Value |
| --- | --- |
| 0 | `0xA5` |
| 1 | `0x5A` |
| 2 | `0xA0` protocol version, plus bit 0 for motor enabled |
| 3 | LED pattern enum emitted by the ESP32: `0` off, `1` red, `2` blue |
| 4 | Motor duty from `0` to `255` |
| 5 | CRC-8/ATM of bytes 0 through 4 |

The upstream `ledPattern` field accepts `off`, `red_alliance`, and
`blue_alliance`; any other value is treated as `off` by the ESP32. The Uno
ignores incomplete, malformed, or CRC-invalid frames. Its `D6` drives
the WS2812B LED data signal and its `D9` provides the motor PWM signal. The
Uno controls all 4201 LED patterns, including the flash and chase animations.
It sends a neutral `1500 us` motor pulse and turns the strip off if no valid
frame arrives for 300 ms. The ESP32 independently sends an explicit safe frame
after 300 ms without a valid 4201 command. A Hub command must include valid
`hubState` and `motorDuty`; an omitted `ledPattern` is treated as `off`.

For motor state gating, the ESP32 follows the 4201 reference Hub:
`SCORING_ACTIVE`, `SCORING_INACTIVE`, and `DEBUG_MOTOR_SPINUP` may apply
`motorDuty`; `DISABLED`, `DEBUG_SCORING_TEST`, unknown states, stale commands,
and negative duty all command motor stop. This Hub's existing motor path has
only one direction: duty `0` maps to `1500 us` (stop) and duty `1` maps to
`1350 us` (full forward) on Uno `D9`.

`matchState` remains parsed for protocol compatibility and logging, but neither
the 4201 reference Hub nor this ESP32-to-Uno output link needs it to decide LED
or motor behavior. `hubState`, `motorDuty`, and `ledPattern` are sufficient.

The Uno disables D2's internal pull-up so it cannot feed 5 V into ESP32 GPIO
15. A shared ground is mandatory. For a long or electrically noisy GPIO15/D2
cable, add a 3.3 V-to-5 V HCT-level buffer; no additional signal wire is
required. GPIO15 is no longer compatible with the earlier one-bit Uno sketch,
so upload the ESP32 and Uno firmware in this repository together.

## Deployment

### 1. Install the host tools

Install Python 3 with `pip`, then install the only required Python package:

```powershell
py -m pip install --upgrade pip
py -m pip install --upgrade platformio
```

PlatformIO automatically downloads the ESP32 and Uno toolchains plus the
firmware libraries (`Ethernet`, `ArduinoJson`, and `FastLED`) on the first
build. Do not install those Arduino libraries with `pip`.

### 2. Build before connecting hardware

From the repository root, build all three firmware targets:

```powershell
py -m platformio run -e blue-hub -e red-hub
py -m platformio run -d uno
```

### 3. Upload each physical Hub

Every physical Hub contains one ESP32 and one Uno. The Uno firmware is shared;
the ESP32 must use the target that matches its alliance. Stop the motor path
before flashing. Use the actual USB serial port in place of `COMx`, or omit
`--upload-port COMx` only when one compatible board is connected.

For the blue Hub:

```powershell
py -m platformio run -d uno -t upload --upload-port COMx
py -m platformio run -e blue-hub -t upload --upload-port COMy
```

For the red Hub:

```powershell
py -m platformio run -d uno -t upload --upload-port COMx
py -m platformio run -e red-hub -t upload --upload-port COMy
```

The Uno and ESP32 firmwares must be updated as a pair: the current GPIO15/D2
UART protocol is not compatible with the earlier one-bit Uno sketch. Do not
flash `blue-hub` onto the red ESP32 or `red-hub` onto the blue ESP32; the build
selects the node role and static IP address.

### 4. Verify on the field network

Connect the W5500 to the isolated field network, enable Alternate IO in
Showdown Arena, and configure the addresses listed in the Network section.
The ESP32 serial log is available at 115200 baud:

```powershell
py -m platformio device monitor --port COMy --baud 115200
```

With no valid `node_command`, the deployed outputs remain safe: the Uno turns
the LED strip off and sends the D9 motor neutral pulse within 300 ms.
