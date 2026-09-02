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

// ESP32 GPIO 15 is wired to the Arduino Uno's EDGE_PIN. The Uno owns the
// strip animation and receives only the Hub active/inactive level.
constexpr int HUB_STATUS_OUTPUT_PIN = 15;

constexpr int IR_SENSOR_PINS[] = {33, 34, 35, 36};
constexpr size_t SENSOR_COUNT = sizeof(IR_SENSOR_PINS) / sizeof(IR_SENSOR_PINS[0]);
constexpr unsigned long DEBOUNCE_MS = 50;

// 4201 Showdown Arena protocol constants.
constexpr uint16_t UDP_COMMAND_PORT = 5300;
constexpr uint16_t UDP_STATUS_PORT = 5301;
constexpr unsigned long STATUS_INTERVAL_MS = 100;
constexpr unsigned long COMMAND_TIMEOUT_MS = 2000;
constexpr size_t MAX_COMMAND_PACKET_BYTES = 1024;

struct NodeCommand {
  String hubState = "DISABLED";
  float motorDuty = 0.0f;
  String ledPattern = "off";
  int matchState = 0;
};

EthernetUDP udp;
NodeCommand command;
IPAddress arenaStatusTarget = DEFAULT_ARENA_IP;
unsigned long lastCommandReceivedMs = 0;
bool hasReceivedCommand = false;
unsigned long lastStatusSentMs = 0;
unsigned long lastSensorTrigger[SENSOR_COUNT] = {};
int lastSensorValue[SENSOR_COUNT] = {};
uint32_t cumulativeScore = 0;
bool lastUnoStatusOutput = false;

bool commandIsFresh() {
  return hasReceivedCommand &&
         static_cast<unsigned long>(millis() - lastCommandReceivedMs) < COMMAND_TIMEOUT_MS;
}

bool isActiveAlliancePattern(const String &pattern) {
  // hubState controls the reference Hub motors, which run for both alliances
  // during much of a match. ledPattern identifies the Hub that is active.
#if HUB_IS_RED
  return pattern == "red" || pattern == "red_flash" || pattern == "red_chase";
#else
  return pattern == "blue" || pattern == "blue_flash" || pattern == "blue_chase";
#endif
}

bool commandRequestsHubActive() {
  return commandIsFresh() && isActiveAlliancePattern(command.ledPattern);
}

void updateUnoStatusOutput() {
  const bool active = commandRequestsHubActive();
  if (active == lastUnoStatusOutput) return;

  digitalWrite(HUB_STATUS_OUTPUT_PIN, active ? HIGH : LOW);
  lastUnoStatusOutput = active;
  Serial.printf("[UNO] Hub status: %s\n", active ? "ACTIVE" : "INACTIVE");
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

  bool commandChanged = false;

  // These fields are optional in the 4201 schema, so retain the current value
  // if the arena deliberately omits one from a command packet.
  if (document["hubState"].is<const char *>()) {
    const char *receivedHubState = document["hubState"].as<const char *>();
    if (command.hubState != receivedHubState) {
      command.hubState = receivedHubState;
      commandChanged = true;
    }
  }
  if (document["ledPattern"].is<const char *>()) {
    const char *receivedLedPattern = document["ledPattern"].as<const char *>();
    if (command.ledPattern != receivedLedPattern) {
      command.ledPattern = receivedLedPattern;
      commandChanged = true;
    }
  }
  if (document["matchState"].is<int>()) {
    const int receivedMatchState = document["matchState"].as<int>();
    if (command.matchState != receivedMatchState) {
      command.matchState = receivedMatchState;
      commandChanged = true;
    }
  }
  const JsonVariantConst motorDuty = document["motorDuty"];
  // encoding/json emits an integral-valued float64 such as 1.0 as `1`.
  if (motorDuty.is<int>() || motorDuty.is<float>()) {
    const float receivedDuty = motorDuty.as<float>();
    if (isfinite(receivedDuty)) {
      const float constrainedDuty = constrain(receivedDuty, -1.0f, 1.0f);
      if (command.motorDuty != constrainedDuty) {
        command.motorDuty = constrainedDuty;
        commandChanged = true;
      }
    }
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
  pinMode(HUB_STATUS_OUTPUT_PIN, OUTPUT);
  digitalWrite(HUB_STATUS_OUTPUT_PIN, LOW);

  connectEthernet();
}

void loop() {
  serviceCommands();
  processSensors();
  updateUnoStatusOutput();
  serviceStatus();
  delay(1);
}
