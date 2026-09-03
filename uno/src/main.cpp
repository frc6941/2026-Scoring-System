/*
 * Arduino Uno output controller for the 2026 FRC REBUILT Hub.
 *
 * ESP32 GPIO 15 transmits a framed 9600 baud command to D2. This controller
 * renders the requested 4201 LED pattern on D6 and controls the motor signal
 * on D9. No valid ESP32 frame for 300 ms returns both outputs to a safe state.
 */

#include <Arduino.h>

// Keep WS2812 output timing exact. The ESP32 deliberately leaves a quiet
// interval after each serial frame so this does not conflict with reception.
#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>
#include <SoftwareSerial.h>

constexpr uint8_t LED_PIN = 6;
constexpr uint16_t LED_COUNT = 300;
constexpr uint8_t MOTOR_PIN = 9;
constexpr uint8_t COMMAND_RX_PIN = 2;
constexpr uint8_t COMMAND_UNUSED_TX_PIN = 4;
constexpr uint32_t COMMAND_BAUD = 9600;
constexpr unsigned long COMMAND_TIMEOUT_MS = 300;

constexpr uint8_t FRAME_SYNC_0 = 0xA5;
constexpr uint8_t FRAME_SYNC_1 = 0x5A;
constexpr uint8_t FRAME_CONTROL_VERSION = 0xA0;
constexpr uint8_t FRAME_MOTOR_ENABLE = 0x01;
constexpr size_t FRAME_SIZE = 6;

constexpr uint16_t MOTOR_NEUTRAL_US = 1500;
constexpr uint16_t MOTOR_FULL_FORWARD_US = 1350;
constexpr unsigned long ALLIANCE_FLASH_PERIOD_MS = 1000;
constexpr unsigned long ALLIANCE_FLASH_OFF_MS = 500;

enum class LedPattern : uint8_t {
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

CRGB leds[LED_COUNT];
SoftwareSerial commandLink(COMMAND_RX_PIN, COMMAND_UNUSED_TX_PIN);

uint8_t receivedFrame[FRAME_SIZE] = {};
size_t receivedFrameBytes = 0;
LedPattern commandedPattern = LedPattern::Off;
unsigned long lastValidFrameMs = 0;
bool linkHealthy = false;

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

void setupTimer1Pwm() {
  // Fast PWM mode 14, 200 Hz. OC1A is Uno D9.
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS11);
  ICR1 = 9999;
  pinMode(MOTOR_PIN, OUTPUT);
}

void setMotorPulse(uint16_t microseconds) {
  const uint16_t constrained = constrain(microseconds, 1000, 2000);
  OCR1A = constrained * 2;
}

void applyMotorCommand(bool enabled, uint8_t dutyByte) {
  const uint16_t duty = enabled ? dutyByte : 0;
  const uint16_t range = MOTOR_NEUTRAL_US - MOTOR_FULL_FORWARD_US;
  const uint16_t pulseWidth = MOTOR_NEUTRAL_US - (range * duty + 127) / 255;
  setMotorPulse(pulseWidth);
}

bool isAnimated(LedPattern pattern) {
  return pattern == LedPattern::RedFlash || pattern == LedPattern::BlueFlash ||
         pattern == LedPattern::RedChase || pattern == LedPattern::BlueChase;
}

void setAll(const CRGB &color) {
  fill_solid(leds, LED_COUNT, color);
}

void renderAllianceFlash(const CRGB &color, unsigned long now) {
  const bool off = (now % ALLIANCE_FLASH_PERIOD_MS) < ALLIANCE_FLASH_OFF_MS;
  setAll(off ? CRGB::Black : color);
}

void renderChase(const CRGB &baseColor, unsigned long now) {
  setAll(baseColor);

  constexpr int chasePeriodMs = 500;
  constexpr int chaseSpanMs = 250;
  const int chaseIndex =
      static_cast<int>(((now % chasePeriodMs) * LED_COUNT) / chaseSpanMs) % LED_COUNT;
  constexpr int width = (LED_COUNT * 44) / 100;
  constexpr int halfWidth = width / 2;

  for (int offset = -halfWidth; offset <= halfWidth; ++offset) {
    int target = (chaseIndex + offset) % LED_COUNT;
    if (target < 0) target += LED_COUNT;
    const int distance = abs(offset);
    const uint8_t whiteMix =
        static_cast<uint8_t>((static_cast<uint16_t>(halfWidth - distance) * 255) / halfWidth);
    leds[target] = blend(baseColor, CRGB::White, whiteMix);
  }
}

void renderLeds(LedPattern pattern) {
  const unsigned long now = millis();
  switch (pattern) {
    case LedPattern::Red:
      setAll(CRGB::Red);
      break;
    case LedPattern::Blue:
      setAll(CRGB::Blue);
      break;
    case LedPattern::Green:
      setAll(CRGB(0, 255, 0));
      break;
    case LedPattern::Purple:
      setAll(CRGB(128, 0, 128));
      break;
    case LedPattern::White:
      setAll(CRGB::White);
      break;
    case LedPattern::RedFlash:
      renderAllianceFlash(CRGB::Red, now);
      break;
    case LedPattern::BlueFlash:
      renderAllianceFlash(CRGB::Blue, now);
      break;
    case LedPattern::RedChase:
      renderChase(CRGB::Red, now);
      break;
    case LedPattern::BlueChase:
      renderChase(CRGB::Blue, now);
      break;
    case LedPattern::Off:
    default:
      setAll(CRGB::Black);
      break;
  }
  FastLED.show();
}

void applyValidFrame(const uint8_t *frame) {
  const uint8_t control = frame[2];
  const uint8_t patternValue = frame[3];
  if ((control & 0xF0) != FRAME_CONTROL_VERSION || (control & 0x0E) != 0 ||
      patternValue > static_cast<uint8_t>(LedPattern::White)) {
    return;
  }

  const LedPattern newPattern = static_cast<LedPattern>(patternValue);
  const bool patternChanged = newPattern != commandedPattern;
  commandedPattern = newPattern;
  applyMotorCommand((control & FRAME_MOTOR_ENABLE) != 0, frame[4]);
  lastValidFrameMs = millis();
  linkHealthy = true;

  // Render only after a complete command. The ESP32 sends the next frame no
  // sooner than 40 ms later, leaving the WS2812 timing block out of the RX window.
  if (patternChanged || isAnimated(commandedPattern)) renderLeds(commandedPattern);
}

void consumeCommandByte(uint8_t value) {
  if (receivedFrameBytes == 0) {
    if (value == FRAME_SYNC_0) {
      receivedFrame[receivedFrameBytes++] = value;
    }
    return;
  }

  if (receivedFrameBytes == 1) {
    if (value == FRAME_SYNC_1) {
      receivedFrame[receivedFrameBytes++] = value;
    } else if (value == FRAME_SYNC_0) {
      receivedFrame[0] = value;
    } else {
      receivedFrameBytes = 0;
    }
    return;
  }

  receivedFrame[receivedFrameBytes++] = value;
  if (receivedFrameBytes != FRAME_SIZE) return;

  receivedFrameBytes = 0;
  if (crc8Atm(receivedFrame, FRAME_SIZE - 1) == receivedFrame[FRAME_SIZE - 1]) {
    applyValidFrame(receivedFrame);
  }
}

void serviceCommandLink() {
  while (commandLink.available()) {
    consumeCommandByte(static_cast<uint8_t>(commandLink.read()));
  }
}

void enforceCommandTimeout() {
  if (!linkHealthy || millis() - lastValidFrameMs < COMMAND_TIMEOUT_MS) return;

  linkHealthy = false;
  commandedPattern = LedPattern::Off;
  applyMotorCommand(false, 0);
  renderLeds(commandedPattern);
}

void setup() {
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(255);
  renderLeds(LedPattern::Off);

  setupTimer1Pwm();
  applyMotorCommand(false, 0);

  commandLink.begin(COMMAND_BAUD);
  // SoftwareSerial enables its RX pull-up. Keep D2 high impedance so it never
  // drives 5 V back into the ESP32's 3.3 V TX pin.
  digitalWrite(COMMAND_RX_PIN, LOW);
}

void loop() {
  serviceCommandLink();
  enforceCommandTimeout();
}
