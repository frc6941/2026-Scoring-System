/*
 * 2026 FRC REBUILT Hub node for the 4201 Showdown Arena UDP protocol.
 *
 * The arena sends node_command packets to UDP 5300. This node returns its
 * cumulative score in node_status packets to UDP 5301 at 10 Hz.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Ethernet.h>
#include <SPI.h>
#include <math.h>

#ifndef HUB_IS_RED
#define HUB_IS_RED 0
#endif

constexpr const char *HUB_ROLE = HUB_IS_RED ? "RED_HUB" : "BLUE_HUB";

// Preserve the existing field network assignment. Configure these same node
// addresses in Showdown Arena's Red Hub Address and Blue Hub Address settings.
byte MAC_ADDRESS[] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, HUB_IS_RED ? 0xED : 0xBE};
IPAddress DEVICE_IP(67, 67, 67, HUB_IS_RED ? 3 : 2);
IPAddress DEFAULT_ARENA_IP(67, 67, 67, 1);
IPAddress SUBNET_MASK(255, 255, 255, 0);

constexpr int W5500_CS = 14;
constexpr int W5500_MISO = 12;
constexpr int W5500_MOSI = 11;
constexpr int W5500_SCK = 13;

// ESP32 GPIO 15 is a one-way UART TX to the Arduino Uno's D2 RX. The Uno
// owns the LED strip and motor output, while this node supplies 4201 commands.
constexpr int UNO_COMMAND_TX_PIN = 15;
constexpr uint32_t UNO_COMMAND_BAUD = 9600;
constexpr unsigned long UNO_COMMAND_INTERVAL_MS = 40;
constexpr uint8_t UNO_FRAME_SYNC_0 = 0xA5;
constexpr uint8_t UNO_FRAME_SYNC_1 = 0x5A;
constexpr uint8_t UNO_FRAME_CONTROL_VERSION = 0xA0;

constexpr int IR_SENSOR_PINS[] = {33, 34, 35, 36};
constexpr size_t SENSOR_COUNT = sizeof(IR_SENSOR_PINS) / sizeof(IR_SENSOR_PINS[0]);
constexpr unsigned long DEBOUNCE_MS = 50;

// 4201 Showdown Arena protocol constants.
constexpr uint16_t UDP_COMMAND_PORT = 5300;
constexpr uint16_t UDP_STATUS_PORT = 5301;
constexpr unsigned long STATUS_INTERVAL_MS = 100;
// Showdown Arena sends commands every 100 ms. Stop actuators after three
// missing command intervals instead of retaining a motor command for seconds.
constexpr unsigned long COMMAND_TIMEOUT_MS = 300;
constexpr size_t MAX_COMMAND_PACKET_BYTES = 1024;

enum class UnoLedPattern : uint8_t {
  Off = 0,
  Red = 1,
  Blue = 2,
  RedFlash = 3,
  BlueFlash = 4,
  RedChase = 5,
  BlueChase = 6,
  Green = 7,
  Purple = 8,
  White = 9,
};

struct NodeCommand {
  String hubState = "DISABLED";
  float motorDuty = 0.0f;
  String ledPattern = "off";
  int matchState = 0;
};

EthernetUDP udp;
HardwareSerial unoLink(1);
NodeCommand command;
IPAddress arenaStatusTarget = DEFAULT_ARENA_IP;
unsigned long lastCommandReceivedMs = 0;
bool hasReceivedCommand = false;
unsigned long lastStatusSentMs = 0;
unsigned long lastUnoCommandSentMs = 0;
unsigned long lastSensorTrigger[SENSOR_COUNT] = {};
int lastSensorValue[SENSOR_COUNT] = {};
uint32_t cumulativeScore = 0;

bool commandIsFresh() {
  return hasReceivedCommand &&
         static_cast<unsigned long>(millis() - lastCommandReceivedMs) < COMMAND_TIMEOUT_MS;
}

bool hubStateAllowsMotor(const String &hubState) {
  return hubState == "SCORING_ACTIVE" || hubState == "SCORING_INACTIVE" ||
         hubState == "DEBUG_MOTOR_SPINUP";
}

UnoLedPattern toUnoLedPattern(const String &pattern) {
  if (pattern == "red") return UnoLedPattern::Red;
  if (pattern == "blue") return UnoLedPattern::Blue;
  if (pattern == "red_flash") return UnoLedPattern::RedFlash;
  if (pattern == "blue_flash") return UnoLedPattern::BlueFlash;
  if (pattern == "red_chase") return UnoLedPattern::RedChase;
  if (pattern == "blue_chase") return UnoLedPattern::BlueChase;
  if (pattern == "green") return UnoLedPattern::Green;
  if (pattern == "purple") return UnoLedPattern::Purple;
  if (pattern == "white") return UnoLedPattern::White;
  return UnoLedPattern::Off;
}

uint8_t crc8Atm(const uint8_t *data, size_t length) {
  uint8_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                           : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

void sendUnoCommand() {
  const bool fresh = commandIsFresh();
  const bool motorEnabled = fresh && hubStateAllowsMotor(command.hubState);
  const float effectiveDuty = motorEnabled ? constrain(command.motorDuty, 0.0f, 1.0f) : 0.0f;
  const uint8_t motorDuty = static_cast<uint8_t>(effectiveDuty * 255.0f + 0.5f);
  const UnoLedPattern pattern = fresh ? toUnoLedPattern(command.ledPattern) : UnoLedPattern::Off;

  uint8_t frame[] = {
      UNO_FRAME_SYNC_0,
      UNO_FRAME_SYNC_1,
      static_cast<uint8_t>(UNO_FRAME_CONTROL_VERSION | (motorEnabled ? 0x01 : 0x00)),
      static_cast<uint8_t>(pattern),
      motorDuty,
      0,
  };
  frame[sizeof(frame) - 1] = crc8Atm(frame, sizeof(frame) - 1);
  unoLink.write(frame, sizeof(frame));
}

void serviceUnoCommand() {
  const unsigned long now = millis();
  if (now - lastUnoCommandSentMs < UNO_COMMAND_INTERVAL_MS) return;

  // Send exactly one frame per interval. Catch-up bursts would overlap the
  // Uno's WS2812 update window and make SoftwareSerial reception unreliable.
  lastUnoCommandSentMs = now;
  sendUnoCommand();
}

void connectEthernet() {
  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);
  Ethernet.begin(MAC_ADDRESS, DEVICE_IP, DEFAULT_ARENA_IP, DEFAULT_ARENA_IP, SUBNET_MASK);

  if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) {
    Serial.println("[ETH] Failed to configure static Ethernet address");
    while (true) delay(1000);
  }

  if (udp.begin(UDP_COMMAND_PORT) == 0) {
    Serial.println("[UDP] Failed to bind command port");
    while (true) delay(1000);
  }

  Serial.printf("[ETH] IP: %s\n", Ethernet.localIP().toString().c_str());
  Serial.printf("[UDP] Listening on %u; arena status target %s:%u\n", UDP_COMMAND_PORT,
                arenaStatusTarget.toString().c_str(), UDP_STATUS_PORT);
}

void drainIncomingPacket() {
  while (udp.available()) udp.read();
}

bool decodeNodeCommand(const char *payload, size_t length) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  if (error) {
    Serial.printf("[UDP] Ignored malformed JSON: %s\n", error.c_str());
    return false;
  }

  const char *type = document["type"] | "";
  if (strcmp(type, "node_command") != 0) {
    Serial.printf("[UDP] Ignored message type: %s\n", type);
    return false;
  }

  const JsonVariantConst hubState = document["hubState"];
  const JsonVariantConst motorDuty = document["motorDuty"];
  if (!hubState.is<const char *>() || !(motorDuty.is<int>() || motorDuty.is<float>())) {
    Serial.println("[UDP] Ignored incomplete Hub command");
    return false;
  }

  const float receivedDuty = motorDuty.as<float>();
  if (!isfinite(receivedDuty)) {
    Serial.println("[UDP] Ignored Hub command with invalid motorDuty");
    return false;
  }

  bool commandChanged = false;
  const char *receivedHubState = hubState.as<const char *>();
  if (command.hubState != receivedHubState) {
    command.hubState = receivedHubState;
    commandChanged = true;
  }

  // The arena may omit an empty ledPattern. Treat that as off rather than
  // retaining a previous active pattern.
  const char *receivedLedPattern = document["ledPattern"].is<const char *>()
                                       ? document["ledPattern"].as<const char *>()
                                       : "off";
  if (command.ledPattern != receivedLedPattern) {
    command.ledPattern = receivedLedPattern;
    commandChanged = true;
  }
  if (document["matchState"].is<int>()) {
    const int receivedMatchState = document["matchState"].as<int>();
    if (command.matchState != receivedMatchState) {
      command.matchState = receivedMatchState;
      commandChanged = true;
    }
  }
  const float constrainedDuty = constrain(receivedDuty, -1.0f, 1.0f);
  if (command.motorDuty != constrainedDuty) {
    command.motorDuty = constrainedDuty;
    commandChanged = true;
  }

  lastCommandReceivedMs = millis();
  hasReceivedCommand = true;
  if (commandChanged) {
    Serial.printf("[UDP] hubState=%s motorDuty=%.2f ledPattern=%s matchState=%d\n",
                  command.hubState.c_str(), command.motorDuty, command.ledPattern.c_str(),
                  command.matchState);
  }
  return true;
}

void serviceCommands() {
  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    const IPAddress sender = udp.remoteIP();
    if (packetSize >= static_cast<int>(MAX_COMMAND_PACKET_BYTES)) {
      Serial.printf("[UDP] Ignored oversized command (%d bytes)\n", packetSize);
      drainIncomingPacket();
    } else {
      char packet[MAX_COMMAND_PACKET_BYTES];
      const int bytesRead = udp.read(reinterpret_cast<uint8_t *>(packet), packetSize);
      if (bytesRead > 0 && decodeNodeCommand(packet, static_cast<size_t>(bytesRead))) {
        if (arenaStatusTarget != sender) {
          arenaStatusTarget = sender;
          Serial.printf("[UDP] Status target updated to %s:%u\n",
                        arenaStatusTarget.toString().c_str(), UDP_STATUS_PORT);
        }
      }
      drainIncomingPacket();
    }
    packetSize = udp.parsePacket();
  }
}

void processSensors() {
  // Match the 4201 Hub's manual motor-spinup mode, which clears the raw
  // counter while the command is fresh so scoring tests can start cleanly.
  // Keep sampling in this mode so a held-low sensor cannot become a false
  // rising-edge score when the mode ends.
  const bool resetScore = commandIsFresh() && command.hubState == "DEBUG_MOTOR_SPINUP";
  if (resetScore) {
    cumulativeScore = 0;
  }

  const unsigned long now = millis();
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    const int value = digitalRead(IR_SENSOR_PINS[i]);
    if (value == LOW && lastSensorValue[i] == HIGH && now - lastSensorTrigger[i] > DEBOUNCE_MS) {
      lastSensorTrigger[i] = now;
      if (!resetScore) {
        ++cumulativeScore;
        Serial.printf("[SENSOR] #%u -> cumulative score %lu\n", static_cast<unsigned>(i + 1),
                      static_cast<unsigned long>(cumulativeScore));
      }
    }
    lastSensorValue[i] = value;
  }
}

void sendNodeStatus() {
  JsonDocument document;
  document["type"] = "node_status";
  document["role"] = HUB_ROLE;
  document["fieldEStop"] = false;
  JsonArray stations = document["stations"].to<JsonArray>();
  for (int i = 0; i < 3; ++i) {
    JsonObject station = stations.add<JsonObject>();
    station["eStop"] = false;
    station["aStop"] = false;
  }
  document["score"] = cumulativeScore;

  char payload[384];
  const size_t length = serializeJson(document, payload, sizeof(payload));
  if (length == 0 || udp.beginPacket(arenaStatusTarget, UDP_STATUS_PORT) == 0) {
    Serial.println("[UDP] Failed to start node_status packet");
    return;
  }
  udp.write(reinterpret_cast<const uint8_t *>(payload), length);
  if (udp.endPacket() == 0) Serial.println("[UDP] Failed to send node_status packet");
}

void serviceStatus() {
  const unsigned long now = millis();
  if (now - lastStatusSentMs < STATUS_INTERVAL_MS) return;

  lastStatusSentMs = now;
  sendNodeStatus();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== 2026 FRC REBUILT 4201 UDP Hub [%s] ===\n", HUB_ROLE);

  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    pinMode(IR_SENSOR_PINS[i], INPUT_PULLUP);
    lastSensorValue[i] = digitalRead(IR_SENSOR_PINS[i]);
  }
  unoLink.begin(UNO_COMMAND_BAUD, SERIAL_8N1, -1, UNO_COMMAND_TX_PIN);
  sendUnoCommand();  // Explicitly initialize the Uno with safe outputs.
  lastUnoCommandSentMs = millis();

  connectEthernet();
}

void loop() {
  serviceCommands();
  processSensors();
  serviceUnoCommand();
  serviceStatus();
  delay(1);
}
