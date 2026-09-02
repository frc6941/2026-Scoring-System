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
  "ledPattern": "red",
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

GPIO `15` is the documented one-bit interface to the Arduino Uno LED
controller's `EDGE_PIN`, with a shared ground. It is high only for this Hub's
active alliance patterns: `red`, `red_flash`, and `red_chase` on the red build;
`blue`, `blue_flash`, and `blue_chase` on the blue build. It is low for `off`,
`green`, `purple`, `white`, an unknown pattern, or a stale command. The Uno
remains responsible for the physical strip's alliance colour, breathing, and
off behavior; its one-bit input cannot represent the 4201 flash/chase animation
itself.

`hubState` is still parsed from every command, but Showdown Arena uses it for
the Hub motors and keeps both Hub motors active during much of a match. The
per-Hub active indication comes from `ledPattern`.

The current Hub hardware contract has no ESP32-to-Uno PWM, serial, I2C, or
other motor-control connection. `motorDuty` is parsed and retained for protocol
diagnostics, but the existing Uno firmware owns the fixed motor output. Do not
assign 4201's direct motor GPIOs to this hardware without an explicit wiring
and Arduino protocol change.

## Build and upload

Install PlatformIO, connect one ESP32 over USB, and run:

```powershell
py -m platformio run -e blue-hub -t upload
py -m platformio run -e red-hub -t upload
py -m platformio device monitor
```

The project uses the ESP32 Arduino framework with `Ethernet` and `ArduinoJson`.
