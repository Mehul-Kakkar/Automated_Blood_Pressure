
#include "HX710B.h"
#include <math.h>

// =====================================================
// PIN ASSIGNMENTS
// =====================================================

#define SCK_PIN  9
#define SDI_PIN  8

#define PUMP_IN1 5
#define PUMP_IN2 4
#define PUMP_EN  15

#define VALVE_IN1 6
#define VALVE_IN2 7
#define VALVE_EN  16

// =====================================================
// TEST MODES
// =====================================================

#define BASELINE   0
#define CONTINUOUS 1
#define PULSED     2

const char* testNames[] = {
  "A_BASELINE",
  "B_CONTINUOUS",
  "C_PULSED"
};

// =====================================================
// CONFIGURATION
// =====================================================

const uint32_t SENSOR_TIMEOUT = 300UL;

const float CAL_SLOPE = 5.0e-7f;
const float RAW_OFFSET = 170000.0f;
const float PSI_TO_MMHG = 51.7149f;

const float INFLATION_TARGET = 80.0f;
const float MAX_PRESSURE = 130.0f;
const float RELEASE_TARGET = 5.0f;

const uint32_t TEST_DURATION_MS = 10000UL;
const uint32_t SAMPLE_INTERVAL_MS = 100UL;
const uint32_t MAX_INFLATION_MS = 30000UL;
const uint32_t MAX_RELEASE_MS = 30000UL;

const int32_t ADC_MAX = 8388607L;
const int32_t ADC_MIN = -8388608L;

HX710B air_press(SCK_PIN, SDI_PIN);

int32_t rawReading = 0;
float pressureMMHg = 0.0f;
float pressurePSI = 0.0f;

bool testRunning = false;
bool systemFault = false;
bool stopRequested = false;

uint32_t cycleNumber = 0;

// =====================================================
// SENSOR CONVERSION
// =====================================================

float convertPSI(int32_t raw) {
  return CAL_SLOPE * ((float)raw - RAW_OFFSET);
}

float convertMMHg(int32_t raw) {
  return convertPSI(raw) * PSI_TO_MMHG;
}

// =====================================================
// SENSOR READING
// =====================================================

bool readPressure() {

  uint32_t dataRaw = 0;

  uint8_t status =
      air_press.read(&dataRaw, SENSOR_TIMEOUT);

  if (status != HX710B_OK) {
    Serial.println("ERROR: HX710B read failed.");
    return false;
  }

  uint32_t raw24 =
      (dataRaw ^ 0x800000UL) & 0xFFFFFFUL;

  int32_t signedRaw;

  if (raw24 & 0x800000UL) {
    signedRaw = (int32_t)(raw24 | 0xFF000000UL);
  } else {
    signedRaw = (int32_t)raw24;
  }

  if (signedRaw >= ADC_MAX || signedRaw <= ADC_MIN) {
    Serial.println("ERROR: ADC saturation.");
    return false;
  }

  rawReading = signedRaw;

  pressurePSI = convertPSI(signedRaw);
  pressureMMHg = convertMMHg(signedRaw);

  if (!isfinite(pressureMMHg) ||
      pressureMMHg < -20.0f ||
      pressureMMHg > MAX_PRESSURE + 10.0f) {

    Serial.println("ERROR: Implausible pressure.");
    return false;
  }

  return true;
}

// =====================================================
// ACTUATOR CONTROL
// =====================================================

void pumpOn() {
  digitalWrite(PUMP_IN1, HIGH);
  digitalWrite(PUMP_IN2, LOW);
  digitalWrite(PUMP_EN, HIGH);
}

void pumpOff() {
  digitalWrite(PUMP_EN, LOW);
}

void valveOpen() {
  digitalWrite(VALVE_EN, LOW);
}

void valveClose() {
  digitalWrite(VALVE_IN1, HIGH);
  digitalWrite(VALVE_IN2, LOW);
  digitalWrite(VALVE_EN, HIGH);
}

void safeState() {
  pumpOff();
  valveOpen();
}

// =====================================================
// SERIAL COMMANDS
// =====================================================

void checkCommand() {

  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "STOP") {
    stopRequested = true;
    safeState();
    Serial.println("STOP REQUESTED.");
  }
}

bool checkSafety() {

  checkCommand();

  if (stopRequested) {
    safeState();
    return false;
  }

  if (!readPressure()) {
    safeState();
    systemFault = true;
    Serial.println("SYSTEM FAULT: Sensor failure.");
    return false;
  }

  if (pressureMMHg >= MAX_PRESSURE) {
    safeState();
    systemFault = true;
    Serial.println("SYSTEM FAULT: Maximum pressure reached.");
    return false;
  }

  return true;
}

// =====================================================
// INFLATION
// =====================================================

bool inflateToTarget() {

  Serial.println();
  Serial.println("INFLATING TO 80 mmHg...");

  pumpOff();
  valveClose();
  delay(150);

  uint32_t startTime = millis();
  uint32_t lastPrint = 0;

  pumpOn();

  while (millis() - startTime < MAX_INFLATION_MS) {

    if (!checkSafety()) return false;

    if (millis() - lastPrint >= 1000UL) {
      Serial.printf("Inflation: %.2f mmHg\n", pressureMMHg);
      lastPrint = millis();
    }

    if (pressureMMHg >= INFLATION_TARGET) {

      pumpOff();
      valveOpen();

      Serial.printf(
        "Inflation complete: %.2f mmHg\n",
        pressureMMHg
      );

      delay(200);
      return true;
    }

    delay(5);
  }

  safeState();
  systemFault = true;
  Serial.println("SYSTEM FAULT: Inflation timeout.");

  return false;
}

// =====================================================
// PRESSURE RELEASE
// =====================================================

bool releasePressure() {

  Serial.println("RELEASING PRESSURE...");

  pumpOff();
  valveOpen();

  uint32_t startTime = millis();

  while (millis() - startTime < MAX_RELEASE_MS) {

    if (stopRequested) {
      safeState();
      return false;
    }

    if (!readPressure()) {
      safeState();
      systemFault = true;
      Serial.println("SYSTEM FAULT: Sensor failure during release.");
      return false;
    }

    if (pressureMMHg <= RELEASE_TARGET) {
      Serial.println("Pressure released.");
      return true;
    }

    delay(10);
  }

  safeState();
  systemFault = true;
  Serial.println("SYSTEM FAULT: Release timeout.");

  return false;
}

// =====================================================
// RUN ONE TEST
// =====================================================

bool runTest(uint8_t mode) {

  if (!inflateToTarget()) return false;

  Serial.println();
  Serial.println("-----------------------------------------------");
  Serial.printf("CYCLE: %lu\n", (unsigned long)cycleNumber);
  Serial.printf("TEST: %s\n", testNames[mode]);
  Serial.println("PUMP AND VALVE TEST STARTED");
  Serial.println("-----------------------------------------------");

  // Valve remains continuously open throughout the test.
  valveOpen();

  uint32_t startTime = millis();
  uint32_t lastSample = 0;

  float startPressure = pressureMMHg;
  float minPressure = pressureMMHg;
  float maxPressure = pressureMMHg;

  uint32_t sampleCount = 0;

  Serial.println();
  Serial.println(
    "CYCLE,TEST,ELAPSED_MS,PRESSURE_MMHG,RAW,PUMP_ON,VALVE_OPEN"
  );

  while (millis() - startTime < TEST_DURATION_MS) {

    if (stopRequested || systemFault) {
      safeState();
      return false;
    }

    uint32_t elapsed = millis() - startTime;

    // Pump operating mode
    if (mode == BASELINE) {
      pumpOff();
    }

    else if (mode == CONTINUOUS) {
      pumpOn();
    }

    else if (mode == PULSED) {
      if ((elapsed % 2000UL) < 1000UL) {
        pumpOn();
      } else {
        pumpOff();
      }
    }

    if (elapsed - lastSample >= SAMPLE_INTERVAL_MS) {

      if (!checkSafety()) return false;

      elapsed = millis() - startTime;

      if (pressureMMHg < minPressure) {
        minPressure = pressureMMHg;
      }

      if (pressureMMHg > maxPressure) {
        maxPressure = pressureMMHg;
      }

      bool pumpState = digitalRead(PUMP_EN) == HIGH;

      Serial.printf(
        "%lu,%s,%lu,%.3f,%ld,%d,1\n",
        (unsigned long)cycleNumber,
        testNames[mode],
        (unsigned long)elapsed,
        pressureMMHg,
        (long)rawReading,
        pumpState ? 1 : 0
      );

      sampleCount++;
      lastSample = elapsed;
    }

    delay(5);
  }

  pumpOff();
  valveOpen();

  float endPressure = pressureMMHg;
  float duration = (millis() - startTime) / 1000.0f;
  float netRate = (startPressure - endPressure) / duration;

  Serial.println();
  Serial.println("---------- TEST SUMMARY ----------");

  Serial.printf("Test            : %s\n", testNames[mode]);
  Serial.printf("Start pressure  : %.3f mmHg\n", startPressure);
  Serial.printf("End pressure    : %.3f mmHg\n", endPressure);
  Serial.printf("Minimum pressure: %.3f mmHg\n", minPressure);
  Serial.printf("Maximum pressure: %.3f mmHg\n", maxPressure);
  Serial.printf("Pressure change : %.3f mmHg\n",
                endPressure - startPressure);
  Serial.printf("Net decay rate  : %.3f mmHg/s\n", netRate);
  Serial.printf("Duration        : %.2f s\n", duration);
  Serial.printf("Samples         : %lu\n",
                (unsigned long)sampleCount);

  Serial.println("----------------------------------");

  return true;
}

// =====================================================
// COMPLETE CONTINUOUS TEST SEQUENCE
// =====================================================

void runAllTests() {

  if (systemFault) {
    Serial.println("System fault. Reset required.");
    return;
  }

  testRunning = true;
  stopRequested = false;

  Serial.println();
  Serial.println("==============================================");
  Serial.println("PUMP + OPEN VALVE CHARACTERIZATION");
  Serial.println("A: Pump OFF");
  Serial.println("B: Pump continuously ON");
  Serial.println("C: Pump pulsed 1s ON / 1s OFF");
  Serial.println("Each test: 80 mmHg start, 10s recording");
  Serial.println("==============================================");

  while (!systemFault && !stopRequested) {

    cycleNumber++;

    for (uint8_t i = 0; i < 3; i++) {

      if (systemFault || stopRequested) break;

      if (!runTest(i)) break;

      if (systemFault || stopRequested) break;

      if (!releasePressure()) break;

      delay(1000);
    }

    if (!systemFault && !stopRequested) {
      Serial.println();
      Serial.printf("CYCLE %lu COMPLETED\n",
                    (unsigned long)cycleNumber);
    }
  }

  safeState();
  testRunning = false;

  Serial.println();
  Serial.println("==============================================");
  Serial.println("TEST SEQUENCE STOPPED");
  Serial.printf("Completed cycles: %lu\n",
                (unsigned long)cycleNumber);
  Serial.println("Pump OFF. Valve OPEN.");
  Serial.println("==============================================");
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(500);

  pinMode(PUMP_EN, OUTPUT);
  pinMode(VALVE_EN, OUTPUT);

  digitalWrite(PUMP_EN, LOW);
  digitalWrite(VALVE_EN, LOW);

  pinMode(PUMP_IN1, OUTPUT);
  pinMode(PUMP_IN2, OUTPUT);

  pinMode(VALVE_IN1, OUTPUT);
  pinMode(VALVE_IN2, OUTPUT);

  digitalWrite(PUMP_IN1, LOW);
  digitalWrite(PUMP_IN2, LOW);

  digitalWrite(VALVE_IN1, HIGH);
  digitalWrite(VALVE_IN2, LOW);

  safeState();

  Serial.println();
  Serial.println("Initializing HX710B...");

  if (air_press.init() != HX710B_OK) {

    safeState();

    Serial.println("HX710B initialization failed!");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("HX710B initialized.");
  Serial.println("Pump OFF. Valve OPEN.");

  Serial.println();
  Serial.println("COMMANDS:");
  Serial.println("RUN  - Start continuous A/B/C tests");
  Serial.println("STOP - Stop tests and open valve");
  Serial.println("HELP - Show commands");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  if (systemFault) {
    safeState();
    delay(100);
    return;
  }

  if (Serial.available()) {

    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toUpperCase();

    if (command == "RUN") {

      if (!testRunning) {
        runAllTests();
      } else {
        Serial.println("Tests already running.");
      }
    }

    else if (command == "STOP") {
      stopRequested = true;
      safeState();
      Serial.println("STOP REQUESTED.");
    }

    else if (command == "HELP") {
      Serial.println("RUN  - Start continuous tests");
      Serial.println("STOP - Stop tests");
      Serial.println("Pump OFF and valve OPEN when idle.");
    }
  }

  delay(5);
}
