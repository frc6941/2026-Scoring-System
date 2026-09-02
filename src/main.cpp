/* 2026 FRC REBUILT - W5500 Ethernet scoring client.
 * The REST and WebSocket messages match examples/esp32_scoring exactly.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Ethernet.h>
#include <SPI.h>

#ifndef HUB_IS_RED
#define HUB_IS_RED 0
#endif

constexpr const char *ALLIANCE_NAME = HUB_IS_RED ? "red" : "blue";
byte MAC_ADDRESS[] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, HUB_IS_RED ? 0xED : 0xBE};
IPAddress DEVICE_IP(67, 67, 67, HUB_IS_RED ? 3 : 2);
IPAddress ARENA_IP(67, 67, 67, 1);
constexpr uint16_t ARENA_PORT = 8080;
constexpr bool DEFAULT_RED_INACTIVE_ON_TIE = true;

constexpr int W5500_CS = 14;
constexpr int W5500_MISO = 12;
constexpr int W5500_MOSI = 11;
constexpr int W5500_SCK = 13;

// Logic-level output to the Arduino Uno LED controller (Uno EDGE_PIN 2).
// HIGH means the Hub is active; LOW means inactive. The Uno owns all LED effects.
constexpr int HUB_STATUS_OUTPUT_PIN = 15;

constexpr int IR_SENSOR_PINS[] = {33, 34, 35, 36};
constexpr size_t SENSOR_COUNT = sizeof(IR_SENSOR_PINS) / sizeof(IR_SENSOR_PINS[0]);
constexpr unsigned long DEBOUNCE_MS = 50;
constexpr unsigned long GRACE_PERIOD_MS = 3000;
constexpr unsigned long WS_RECONNECT_MS = 3000;
constexpr unsigned long NETWORK_TIMEOUT_MS = 2000;

enum MatchState : uint8_t {
  STATE_PRE_MATCH, STATE_START_MATCH, STATE_AUTO_PERIOD, STATE_PAUSE_PERIOD,
  STATE_TELEOP_PERIOD, STATE_POST_MATCH, STATE_TIMEOUT_ACTIVE, STATE_POST_TIMEOUT,
};

EthernetClient wsClient;
MatchState currentMatchState = STATE_PRE_MATCH;
int currentMatchTimeSec = 0;
bool hubIsActive = false;
bool hubActiveInShift1 = true;
bool shiftOrderDetermined = false;
bool webSocketConnected = false;
unsigned long gracePeriodEnd = 0;
unsigned long nextWebSocketAttempt = 0;
unsigned long lastSensorTrigger[SENSOR_COUNT] = {};
int lastSensorValue[SENSOR_COUNT] = {};
int localAutoScore = 0;
int localTeleopScore = 0;

struct FrameReader {
  uint8_t header[2] = {};
  uint8_t headerBytes = 0, opcode = 0, extNeeded = 0, extRead = 0, maskBytes = 0;
  uint8_t mask[4] = {};
  bool finalFrame = false, masked = false;
  uint64_t length = 0, read = 0;
  String message;
  String controlPayload;
  uint8_t messageOpcode = 0;

  void resetFrame() {
    headerBytes = opcode = extNeeded = extRead = maskBytes = 0;
    finalFrame = masked = false;
    length = read = 0;
  }
} frame;

void connectEthernet() {
  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);
  Ethernet.begin(MAC_ADDRESS, DEVICE_IP, ARENA_IP, ARENA_IP, IPAddress(255, 255, 255, 0));
  if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) {
    Serial.println("[ETH] Failed to configure static Ethernet address");
    while (true) delay(1000);
  }
  Serial.printf("[ETH] Connected. IP: %s\n", Ethernet.localIP().toString().c_str());
}

String readHttpResponse(EthernetClient &client) {
  String response;
  const unsigned long deadline = millis() + NETWORK_TIMEOUT_MS;
  while (millis() < deadline) {
    while (client.available()) {
      response += static_cast<char>(client.read());
      if (response.length() > 4096) return response;
    }
    if (!client.connected()) break;
    delay(1);
  }
  return response;
}

bool httpOk(const String &response) {
  return response.startsWith("HTTP/1.1 200") || response.startsWith("HTTP/1.0 200");
}

String httpBody(const String &response) {
  const int start = response.indexOf("\r\n\r\n");
  return start < 0 ? String() : response.substring(start + 4);
}

bool sendHttpRequest(const char *method, const char *path, const String &body, String *response) {
  EthernetClient client;
  if (!client.connect(ARENA_IP, ARENA_PORT)) {
    Serial.println("[HTTP] Connection to Cheesy Arena Lite failed");
    return false;
  }
  client.printf("%s %s HTTP/1.1\r\n", method, path);
  client.printf("Host: %s:%u\r\n", ARENA_IP.toString().c_str(), ARENA_PORT);
  client.print("Connection: close\r\n");
  if (body.length()) {
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %u\r\n", static_cast<unsigned>(body.length()));
  }
  client.print("\r\n");
  if (body.length()) client.print(body);
  const String received = readHttpResponse(client);
  client.stop();
  if (response) *response = received;
  return httpOk(received);
}

void patchScore(const char *category, int points) {
  JsonDocument document;
  JsonObject alliance = document[ALLIANCE_NAME].to<JsonObject>();
  alliance[category] = points;
  String body;
  serializeJson(document, body);
  String response;
  if (sendHttpRequest("PATCH", "/api/scores", body, &response)) {
    Serial.printf("[HTTP] PATCH Success (%s +%d): %s\n", category, points, body.c_str());
  } else {
    Serial.printf("[HTTP] PATCH Failed: %s\n", response.c_str());
  }
}

void determineShiftOrderFromApi() {
  String response;
  if (!sendHttpRequest("GET", "/api/scores", String(), &response)) {
    Serial.printf("[HTTP] GET /api/scores failed: %s\n", response.c_str());
    return;
  }
  JsonDocument document;
  if (deserializeJson(document, httpBody(response))) {
    Serial.println("[HTTP] GET /api/scores returned invalid JSON");
    return;
  }
  const int redAuto = document["red"]["auto"] | 0;
  const int blueAuto = document["blue"]["auto"] | 0;
  const int myAuto = strcmp(ALLIANCE_NAME, "red") == 0 ? redAuto : blueAuto;
  const int opponentAuto = strcmp(ALLIANCE_NAME, "red") == 0 ? blueAuto : redAuto;
  Serial.printf("[GAME LOGIC] Auto Scores -> Red: %d, Blue: %d\n", redAuto, blueAuto);
  if (myAuto > opponentAuto) hubActiveInShift1 = false;
  else if (myAuto < opponentAuto) hubActiveInShift1 = true;
  else hubActiveInShift1 = strcmp(ALLIANCE_NAME, "red") == 0 ? !DEFAULT_RED_INACTIVE_ON_TIE
                                                          : DEFAULT_RED_INACTIVE_ON_TIE;
  shiftOrderDetermined = true;
  Serial.printf("[GAME LOGIC] Shift 1 Hub Status for %s: %s\n", ALLIANCE_NAME,
                hubActiveInShift1 ? "ACTIVE" : "INACTIVE");
}

void updateHubStatus() {
  bool newHubStatus = false;
  if (currentMatchState == STATE_AUTO_PERIOD) {
    newHubStatus = true;
  } else if (currentMatchState == STATE_TELEOP_PERIOD) {
    if (currentMatchTimeSec < 33) {
      newHubStatus = true;
      if (!shiftOrderDetermined) determineShiftOrderFromApi();
    } else if (currentMatchTimeSec < 58) newHubStatus = hubActiveInShift1;
    else if (currentMatchTimeSec < 83) newHubStatus = !hubActiveInShift1;
    else if (currentMatchTimeSec < 108) newHubStatus = hubActiveInShift1;
    else if (currentMatchTimeSec < 133) newHubStatus = !hubActiveInShift1;
    else newHubStatus = true;
  }
  if (hubIsActive && !newHubStatus) {
    gracePeriodEnd = millis() + GRACE_PERIOD_MS;
    Serial.println("[GAME LOGIC] Hub Deactivated -> 3s Grace Period Started");
  }
  hubIsActive = newHubStatus;
  // The Uno detects this falling/rising edge and drives the physical strip.
  digitalWrite(HUB_STATUS_OUTPUT_PIN, hubIsActive ? HIGH : LOW);
}

bool canScoreNow() { return hubIsActive || static_cast<long>(gracePeriodEnd - millis()) > 0; }

void sendWebSocketFrame(uint8_t opcode, const uint8_t *payload, size_t length) {
  if (!webSocketConnected || !wsClient.connected() || length > 125) return;
  const uint8_t mask[] = {static_cast<uint8_t>(random(256)), static_cast<uint8_t>(random(256)),
                          static_cast<uint8_t>(random(256)), static_cast<uint8_t>(random(256))};
  wsClient.write(static_cast<uint8_t>(0x80 | opcode));
  wsClient.write(static_cast<uint8_t>(0x80 | length));
  wsClient.write(mask, sizeof(mask));
  for (size_t i = 0; i < length; ++i) wsClient.write(payload[i] ^ mask[i % 4]);
}

void handleWebSocketMessage(const String &message) {
  JsonDocument document;
  if (deserializeJson(document, message)) return;
  if (strcmp(document["type"] | "", "matchTime") != 0) return;
  const int newState = document["data"]["MatchState"] | 0;
  const int newTime = document["data"]["MatchTimeSec"] | 0;
  if (newState == STATE_PRE_MATCH && currentMatchState != STATE_PRE_MATCH) {
    Serial.println("[GAME LOGIC] New Match Loaded -> Resetting Local Counters");
    localAutoScore = localTeleopScore = 0;
    shiftOrderDetermined = false;
    gracePeriodEnd = 0;
  }
  currentMatchState = static_cast<MatchState>(newState);
  currentMatchTimeSec = newTime;
  updateHubStatus();
}

void disconnectWebSocket() {
  if (wsClient.connected()) wsClient.stop();
  webSocketConnected = false;
  frame.resetFrame();
  frame.message = "";
  frame.controlPayload = "";
  frame.messageOpcode = 0;
  nextWebSocketAttempt = millis() + WS_RECONNECT_MS;
}

bool connectWebSocket() {
  if (!wsClient.connect(ARENA_IP, ARENA_PORT)) return false;
  wsClient.printf("GET /api/arena/websocket HTTP/1.1\r\nHost: %s:%u\r\n", ARENA_IP.toString().c_str(), ARENA_PORT);
  wsClient.print("Upgrade: websocket\r\nConnection: Upgrade\r\n");
  wsClient.print("Sec-WebSocket-Key: MjAyNkZSQy1FdGhlcm5ldA==\r\nSec-WebSocket-Version: 13\r\n\r\n");
  String response;
  const unsigned long deadline = millis() + NETWORK_TIMEOUT_MS;
  while (millis() < deadline && response.indexOf("\r\n\r\n") < 0) {
    while (wsClient.available()) {
      response += static_cast<char>(wsClient.read());
      if (response.endsWith("\r\n\r\n")) break;
    }
    if (!wsClient.connected()) break;
    delay(1);
  }
  if (!response.startsWith("HTTP/1.1 101")) {
    wsClient.stop();
    return false;
  }
  webSocketConnected = true;
  Serial.println("[WS] Connected to Cheesy Arena Lite");
  return true;
}

void completeFrame() {
  if (frame.opcode == 0x8) {
    disconnectWebSocket();
    return;
  }
  if (frame.opcode == 0x9) {
    sendWebSocketFrame(0xA, reinterpret_cast<const uint8_t *>(frame.controlPayload.c_str()),
                       frame.controlPayload.length());
  }
  frame.controlPayload = "";
  if (frame.finalFrame && frame.messageOpcode == 0x1) {
    handleWebSocketMessage(frame.message);
    frame.message = "";
    frame.messageOpcode = 0;
  }
  frame.resetFrame();
}

void consumeWebSocketByte(uint8_t value) {
  if (frame.headerBytes < 2) {
    frame.header[frame.headerBytes++] = value;
    if (frame.headerBytes != 2) return;
    frame.finalFrame = (frame.header[0] & 0x80) != 0;
    frame.opcode = frame.header[0] & 0x0F;
    frame.masked = (frame.header[1] & 0x80) != 0;
    frame.length = frame.header[1] & 0x7F;
    frame.extNeeded = frame.length == 126 ? 2 : (frame.length == 127 ? 8 : 0);
    if (frame.extNeeded) frame.length = 0;
    if (frame.extNeeded == 0 && !frame.masked && frame.length == 0) completeFrame();
    return;
  }
  if (frame.extRead < frame.extNeeded) {
    frame.length = (frame.length << 8) | value;
    ++frame.extRead;
    if (frame.extRead == frame.extNeeded && !frame.masked && frame.length == 0) completeFrame();
    return;
  }
  if (frame.masked && frame.maskBytes < 4) {
    frame.mask[frame.maskBytes++] = value;
    return;
  }
  if (frame.length > 2048) { disconnectWebSocket(); return; }
  if (frame.masked) value ^= frame.mask[frame.read % 4];
  if (frame.opcode == 0x1) frame.messageOpcode = 0x1;
  if (frame.opcode == 0x1 || frame.opcode == 0x0) frame.message += static_cast<char>(value);
  else if (frame.opcode == 0x8 || frame.opcode == 0x9 || frame.opcode == 0xA)
    frame.controlPayload += static_cast<char>(value);
  ++frame.read;
  if (frame.read == frame.length) completeFrame();
}

void serviceWebSocket() {
  if (!webSocketConnected) {
    if (millis() >= nextWebSocketAttempt && !connectWebSocket()) {
      nextWebSocketAttempt = millis() + WS_RECONNECT_MS;
      Serial.println("[WS] Connection failed; retrying");
    }
    return;
  }
  if (!wsClient.connected()) {
    Serial.println("[WS] Disconnected from Cheesy Arena Lite");
    disconnectWebSocket();
    return;
  }
  while (wsClient.available()) consumeWebSocketByte(wsClient.read());
}

void processSensors() {
  if (currentMatchState != STATE_AUTO_PERIOD && currentMatchState != STATE_TELEOP_PERIOD) return;
  const unsigned long now = millis();
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    const int value = digitalRead(IR_SENSOR_PINS[i]);
    if (value == LOW && lastSensorValue[i] == HIGH && now - lastSensorTrigger[i] > DEBOUNCE_MS) {
      lastSensorTrigger[i] = now;
      if (canScoreNow()) {
        Serial.printf("[SENSOR] IR Sensor #%u triggered!\n", static_cast<unsigned>(i + 1));
        if (currentMatchState == STATE_AUTO_PERIOD) { ++localAutoScore; patchScore("auto", 1); }
        else { ++localTeleopScore; patchScore("teleop", 1); }
      } else {
        Serial.printf("[SENSOR] IR Sensor #%u triggered but Hub is INACTIVE (ignored)\n", static_cast<unsigned>(i + 1));
      }
    }
    lastSensorValue[i] = value;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== 2026 FRC REBUILT Ethernet Scoring Client [%s Alliance] ===\n", ALLIANCE_NAME);
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    pinMode(IR_SENSOR_PINS[i], INPUT_PULLUP);
    lastSensorValue[i] = digitalRead(IR_SENSOR_PINS[i]);
  }
  pinMode(HUB_STATUS_OUTPUT_PIN, OUTPUT);
  digitalWrite(HUB_STATUS_OUTPUT_PIN, LOW);
  randomSeed(micros());
  connectEthernet();
}

void loop() {
  serviceWebSocket();
  processSensors();
  delay(1);
}
