
#include <Arduino.h>

// =====================================================
// ESP32-S3 + L298N + AUTO BOTIX HX710B
// Pump / Valve / Pressure Sensor Bench Test
//
// IMPORTANT:
// - GPIO assignments are provisional.
// - No automatic inflation.
// - Pressure is displayed as raw ADC counts.
// - kPa conversion requires calibration.
// - Actuator activation is time-limited.
// =====================================================

// ---------- HX710B pressure sensor ----------
constexpr uint8_t PRESSURE_DATA_PIN  = 8;  // OUT
constexpr uint8_t PRESSURE_CLOCK_PIN = 9;  // SCK

// ---------- L298N pump channel ----------
constexpr uint8_t PUMP_IN1 = 5;
constexpr uint8_t PUMP_IN2 = 4;
constexpr uint8_t PUMP_EN  = 15;

// ---------- L298N valve channel ----------
constexpr uint8_t VALVE_IN1 = 6;
constexpr uint8_t VALVE_IN2 = 7;
constexpr uint8_t VALVE_EN  = 16;

// ---------- Test limits ----------
constexpr uint32_t PUMP_TIMEOUT_MS  = 120000;
constexpr uint32_t VALVE_TIMEOUT_MS = 120000;
constexpr uint32_t PRESSURE_INTERVAL_MS = 100;

// HX710B conversion-ready timeout
constexpr uint32_t SENSOR_READY_TIMEOUT_US = 50000;

// =====================================================
// STATE
// =====================================================

bool pumpRunning = false;
bool valveRunning = false;

uint32_t pumpStartTime = 0;
uint32_t valveStartTime = 0;
uint32_t lastPressureTime = 0;

bool sensorAvailable = false;
bool zeroCaptured = false;

int32_t zeroRaw = 0;

// =====================================================
// ACTUATOR CONTROL
// =====================================================

void stopPump() {
  digitalWrite(PUMP_EN, LOW);
  digitalWrite(PUMP_IN1, LOW);
  digitalWrite(PUMP_IN2, LOW);

  pumpRunning = false;
}

void stopValve() {
  digitalWrite(VALVE_EN, LOW);
  digitalWrite(VALVE_IN1, LOW);
  digitalWrite(VALVE_IN2, LOW);

  valveRunning = false;
}

void stopAll() {
  stopPump();
  stopValve();
}

void startPump() {
  //stopAll();

  // One direction only.
  digitalWrite(PUMP_IN1, HIGH);
  digitalWrite(PUMP_IN2, LOW);
  digitalWrite(PUMP_EN, HIGH);

  pumpStartTime = millis();
  pumpRunning = true;

  Serial.println("Pump ON: timed bench test");
}

void startValve() {
  stopAll();

  // Energizes the valve coil.
  // This does not establish whether the valve
  // opens or closes when energized.
  digitalWrite(VALVE_IN1, HIGH);
  digitalWrite(VALVE_IN2, LOW);
  digitalWrite(VALVE_EN, HIGH);

  valveStartTime = millis();
  valveRunning = true;

  Serial.println("Valve coil energized: timed test");
}

// =====================================================
// HX710B READING
// =====================================================

// Returns false if a conversion is not ready.
// On success, raw receives the signed 24-bit result.
bool readHX710B(int32_t &raw) {
  uint32_t started = micros();

  // Wait for DOUT/OUT to go LOW.
  while (digitalRead(PRESSURE_DATA_PIN) == HIGH) {
    if ((uint32_t)(micros() - started) >=
        SENSOR_READY_TIMEOUT_US) {
      return false;
    }

    delayMicroseconds(10);
  }

  uint32_t value = 0;

  // Read 24 bits, MSB first.
  for (int i = 0; i < 24; i++) {
    digitalWrite(PRESSURE_CLOCK_PIN, HIGH);
    delayMicroseconds(1);

    value = (value << 1) |
            (digitalRead(PRESSURE_DATA_PIN) ? 1UL : 0UL);

    digitalWrite(PRESSURE_CLOCK_PIN, LOW);
    delayMicroseconds(1);
  }

  // 25th pulse: select Channel A, gain 128
  // for the next conversion on compatible HX710B modules.
  digitalWrite(PRESSURE_CLOCK_PIN, HIGH);
  delayMicroseconds(1);
  digitalWrite(PRESSURE_CLOCK_PIN, LOW);
  delayMicroseconds(1);

  // Sign-extend the 24-bit two's-complement result.
  if (value & 0x800000UL) {
    value |= 0xFF000000UL;
  }

  raw = (int32_t)value;
  return true;
}

// =====================================================
// PRESSURE DIAGNOSTICS
// =====================================================

void printPressure() {
  int32_t raw;

  if (!readHX710B(raw)) {
    Serial.println("PRESSURE ERROR: conversion not ready");
    sensorAvailable = false;
    return;
  }

  sensorAvailable = true;

  Serial.print("Pressure raw: ");
  Serial.print(raw);

  if (zeroCaptured) {
    Serial.print(" | Change from zero: ");
    Serial.print((int64_t)raw - zeroRaw);
    Serial.print(" counts");
  }

  Serial.println();
}

// =====================================================
// SERIAL COMMANDS
// =====================================================

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  p = run pump for up to 2 seconds");
  Serial.println("  v = energize valve for up to 1 second");
  Serial.println("  s = stop all actuators");
  Serial.println("  r = read pressure immediately");
  Serial.println("  z = capture current raw zero baseline");
  Serial.println("  h = show this help");
  Serial.println();
  Serial.println("Automatic inflation: DISABLED");
  Serial.println("Pressure units: raw counts; not calibrated kPa");
}

void handleCommand(char command) {
  switch (command) {
    case 'p':
    case 'P':
      startPump();
      break;

    case 'v':
    case 'V':
      startValve();
      break;

    case 's':
    case 'S':
      stopAll();
      Serial.println("All actuators stopped");
      break;

    case 'r':
    case 'R':
      printPressure();
      break;

    case 'z':
    case 'Z': {
      int32_t raw;

      if (readHX710B(raw)) {
        zeroRaw = raw;
        zeroCaptured = true;

        Serial.print("Raw baseline captured: ");
        Serial.println(zeroRaw);
        Serial.println(
          "This is a reference count, not pressure calibration."
        );
      } else {
        Serial.println("Cannot capture baseline: sensor not ready");
      }
      break;
    }

    case 'h':
    case 'H':
      printHelp();
      break;

    case '\r':
    case '\n':
      break;

    default:
      Serial.println("Unknown command. Type h for help.");
      break;
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  // Set output latches LOW before configuring outputs.
  digitalWrite(PUMP_EN, LOW);
  digitalWrite(VALVE_EN, LOW);

  digitalWrite(PUMP_IN1, LOW);
  digitalWrite(PUMP_IN2, LOW);
  digitalWrite(VALVE_IN1, LOW);
  digitalWrite(VALVE_IN2, LOW);

  pinMode(PUMP_EN, OUTPUT);
  pinMode(VALVE_EN, OUTPUT);

  pinMode(PUMP_IN1, OUTPUT);
  pinMode(PUMP_IN2, OUTPUT);
  pinMode(VALVE_IN1, OUTPUT);
  pinMode(VALVE_IN2, OUTPUT);

  stopAll();

  // HX710B digital interface
  pinMode(PRESSURE_DATA_PIN, INPUT);
  pinMode(PRESSURE_CLOCK_PIN, OUTPUT);
  digitalWrite(PRESSURE_CLOCK_PIN, LOW);

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("BP prototype hardware test");
  Serial.println("ESP32-S3 + L298N + HX710B");
  Serial.println("Pump and valve initially disabled.");

  printHelp();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  uint32_t now = millis();

  // Enforce actuator timeouts.
  if (pumpRunning &&
      (uint32_t)(now - pumpStartTime) >= PUMP_TIMEOUT_MS) {
    stopPump();
    Serial.println("Pump stopped: timeout");
  }

  if (valveRunning &&
      (uint32_t)(now - valveStartTime) >= VALVE_TIMEOUT_MS) {
    stopValve();
    Serial.println("Valve de-energized: timeout");
  }

  // Handle serial commands.
  while (Serial.available() > 0) {
    handleCommand((char)Serial.read());
  }

  // Periodic sensor diagnostics.
  now = millis();

  if ((uint32_t)(now - lastPressureTime) >=
      PRESSURE_INTERVAL_MS) {
    lastPressureTime = now;
    printPressure();
  }
}
