
#include "HX710B.h"
#include <math.h>

// =====================================================
// PIN ASSIGNMENTS
// =====================================================

// HX710B
#define SCK_PIN  9
#define SDI_PIN  8

// Pump - L298N Channel A
#define PUMP_IN1 5
#define PUMP_IN2 4
#define PUMP_EN  15

// Normally-open valve - L298N Channel B
#define VALVE_IN1 6
#define VALVE_IN2 7
#define VALVE_EN  16

// =====================================================
// CONFIGURATION
// =====================================================

#define SENSOR_TIMEOUT 300UL

const float CAL_SLOPE = 5.0e-7f;
const float RAW_OFFSET = 170000.0f;
const float PSI_TO_MMHG = 51.7149f;

const float START_PRESSURE = 80.0f;
const float MAX_PRESSURE = 130.0f;
const float RELEASE_PRESSURE = 5.0f;
const float TARGET_RATE = 2.5f;

const uint32_t TEST_DURATION_MS = 10000UL;
const uint32_t MAX_INFLATION_MS = 30000UL;
const uint32_t MAX_RELEASE_MS = 30000UL;

const int32_t ADC_MAX = 8388607L;
const int32_t ADC_MIN = -8388608L;

// =====================================================
// SENSOR
// =====================================================

HX710B air_press(SCK_PIN, SDI_PIN);

int32_t rawReading = 0;
float pressureMMHg = 0.0f;
float pressurePSI = 0.0f;

// =====================================================
// VALVE PWM
// =====================================================

volatile uint32_t pwmFrequency = 1;
volatile uint8_t valveDuty = 0;

volatile bool pumpRunning = false;
volatile bool valveClosed = false;

volatile uint32_t pwmVersion = 0;

TaskHandle_t valveTaskHandle = nullptr;

// =====================================================
// TEST DEFINITIONS
// =====================================================

struct TestSetting {
  uint32_t frequency;
  uint8_t duty;
};

const TestSetting tests[] = {
  {1, 15},
  {1, 20},
  {1, 25},
  {2, 15},
  {2, 20},
  {2, 25}
};

const int NUM_TESTS = sizeof(tests) / sizeof(tests[0]);
const int REPEATS_PER_SETTING = 3;

struct TestResult {
  uint32_t frequency;
  uint8_t duty;

  float startPressure;
  float endPressure;
  float minimumPressure;
  float maximumPressure;

  float averageRate;
  uint32_t samples;

  bool valid;
};

TestResult results[NUM_TESTS][REPEATS_PER_SETTING];

// =====================================================
// SYSTEM STATE
// =====================================================

bool sensorReady = false;
bool systemFault = false;
bool testRunning = false;
bool testAllRunning = false;

// =====================================================
// PRESSURE CONVERSION
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

  // Undo library bit-23 XOR conversion.
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
    Serial.println("ERROR: Implausible pressure reading.");
    return false;
  }

  return true;
}

// =====================================================
// EMERGENCY OUTPUTS
// =====================================================

void stopPump() {
  digitalWrite(PUMP_EN, LOW);
  pumpRunning = false;
}

void openValve() {
  valveDuty = 0;
  pwmVersion++;
}

void closeValve() {
  valveDuty = 100;
  pwmVersion++;
}

void emergencyStop(const char* reason) {

  Serial.println();
  Serial.println("!!!!!!!! SAFETY FAULT !!!!!!!!");
  Serial.println(reason);

  stopPump();
  openValve();

  systemFault = true;
  testRunning = false;
  testAllRunning = false;
}

// =====================================================
// VALVE PWM TASK
// =====================================================

void valvePWMTask(void *parameter) {

  uint32_t localVersion = 0;
  uint32_t cycleStart = micros();

  bool outputState = false;

  while (true) {

    uint32_t version = pwmVersion;

    if (version != localVersion) {
      localVersion = version;
      cycleStart = micros();
    }

    bool shouldClose;

    if (pumpRunning) {
      shouldClose = true;
    }
    else {

      uint8_t duty = valveDuty;
      uint32_t frequency = pwmFrequency;

      if (duty == 0) {
        shouldClose = false;
      }
      else if (duty >= 100) {
        shouldClose = true;
      }
      else {

        uint32_t period = 1000000UL / frequency;

        uint32_t elapsed = micros() - cycleStart;

        if (elapsed >= period) {
          cycleStart += (elapsed / period) * period;
          elapsed = micros() - cycleStart;
        }

        uint32_t closedTime =
            (period * duty) / 100;

        shouldClose = elapsed < closedTime;
      }
    }

    if (shouldClose != outputState) {

      digitalWrite(
        VALVE_EN,
        shouldClose ? HIGH : LOW
      );

      outputState = shouldClose;
      valveClosed = shouldClose;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// =====================================================
// VALVE CONFIGURATION
// =====================================================

void configureValve(uint32_t frequency, uint8_t duty) {

  if (frequency < 1 || frequency > 10 ||
      duty > 100) {
    return;
  }

  pwmFrequency = frequency;
  valveDuty = duty;
  pwmVersion++;
}

// =====================================================
// PUMP CONTROL
// =====================================================

void startPump() {

  if (systemFault) {
    return;
  }

  closeValve();

  delay(100);

  digitalWrite(PUMP_IN1, HIGH);
  digitalWrite(PUMP_IN2, LOW);

  pumpRunning = true;
  digitalWrite(PUMP_EN, HIGH);
}

void finishPump() {

  digitalWrite(PUMP_EN, LOW);
  pumpRunning = false;
}

// =====================================================
// INFLATE TO START PRESSURE
// =====================================================

bool inflateToTarget() {

  Serial.println();
  Serial.println("Inflating to 80 mmHg...");

  configureValve(1, 100);

  delay(100);

  uint32_t startTime = millis();
  startPump();

  uint32_t lastPrint = 0;

  while (millis() - startTime < MAX_INFLATION_MS) {

    if (!readPressure()) {
      emergencyStop("Sensor failure during inflation.");
      return false;
    }

    if (millis() - lastPrint >= 500) {

      Serial.printf(
        "INFLATING | RAW: %ld | PRESSURE: %.2f mmHg | PUMP: %s | VALVE: %s\n",
        (long)rawReading,
        pressureMMHg,
        pumpRunning ? "ON" : "OFF",
        valveClosed ? "CLOSED" : "OPEN"
      );

      lastPrint = millis();
    }

    if (pressureMMHg >= MAX_PRESSURE) {
      emergencyStop("130 mmHg safety ceiling reached.");
      return false;
    }

    if (pressureMMHg >= START_PRESSURE) {

      finishPump();

      Serial.printf(
        "Inflation complete: %.2f mmHg\n",
        pressureMMHg
      );

      return true;
    }
  }

  emergencyStop("Inflation timeout.");
  return false;
}

// =====================================================
// RELEASE PRESSURE
// =====================================================

bool releasePressure() {

  Serial.println("Releasing pressure...");

  finishPump();
  openValve();

  uint32_t startTime = millis();

  while (millis() - startTime < MAX_RELEASE_MS) {

    if (!readPressure()) {
      emergencyStop("Sensor failure during release.");
      return false;
    }

    if (pressureMMHg <= RELEASE_PRESSURE) {
      Serial.println("Pressure released.");
      return true;
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }

  emergencyStop("Pressure release timeout.");
  return false;
}

// =====================================================
// LINEAR REGRESSION
// =====================================================

struct Regression {

  double sumT = 0;
  double sumP = 0;
  double sumTT = 0;
  double sumTP = 0;

  uint32_t n = 0;

  float minimum = 100000.0f;
  float maximum = -100000.0f;

  float first = 0;
  float last = 0;

  void add(float timeSeconds, float pressure) {

    if (n == 0) {
      first = pressure;
    }

    last = pressure;

    if (pressure < minimum) minimum = pressure;
    if (pressure > maximum) maximum = pressure;

    sumT += timeSeconds;
    sumP += pressure;
    sumTT += timeSeconds * timeSeconds;
    sumTP += timeSeconds * pressure;

    n++;
  }

  float getRate() {

    if (n < 3) return NAN;

    double denominator =
        n * sumTT - sumT * sumT;

    if (fabs(denominator) < 1e-9) {
      return NAN;
    }

    double slope =
        (n * sumTP - sumT * sumP) /
        denominator;

    // Positive means pressure is decreasing.
    return (float)(-slope);
  }
};

// =====================================================
// RUN ONE DEFLATION TEST
// =====================================================

bool runOneTest(
  int settingIndex,
  int repeatNumber,
  TestResult &result
) {

  const TestSetting setting = tests[settingIndex];

  Serial.println();
  Serial.println("--------------------------------");

  Serial.printf(
    "SETTING %d / %d | REPEAT %d / %d\n",
    settingIndex + 1,
    NUM_TESTS,
    repeatNumber + 1,
    REPEATS_PER_SETTING
  );

  Serial.printf(
    "Frequency: %lu Hz | Duty: %u%%\n",
    (unsigned long)setting.frequency,
    setting.duty
  );

  Serial.println("--------------------------------");

  // Establish repeatable starting pressure.
  if (!inflateToTarget()) {
    return false;
  }

  // Configure valve after stopping pump.
  configureValve(
    setting.frequency,
    setting.duty
  );

  delay(200);

  if (!readPressure()) {
    emergencyStop("Initial test pressure read failed.");
    return false;
  }

  Regression regression;

  float startingPressure = pressureMMHg;

  uint32_t testStart = millis();
  uint32_t lastProgress = testStart;

  Serial.printf(
    "Starting pressure: %.2f mmHg\n",
    startingPressure
  );

  testRunning = true;

  while (millis() - testStart < TEST_DURATION_MS) {

    if (!readPressure()) {
      emergencyStop("Sensor failure during test.");
      return false;
    }

    if (pressureMMHg >= MAX_PRESSURE) {
      emergencyStop("Pressure ceiling reached during test.");
      return false;
    }

    float elapsed =
        (millis() - testStart) / 1000.0f;

    regression.add(elapsed, pressureMMHg);

    if (millis() - lastProgress >= 2000) {

      Serial.printf(
        "Progress: %.1f / 10 sec | Pressure: %.2f mmHg\n",
        elapsed,
        pressureMMHg
      );

      lastProgress = millis();
    }
  }

  testRunning = false;

  float rate = regression.getRate();

  result.frequency = setting.frequency;
  result.duty = setting.duty;

  result.startPressure = startingPressure;
  result.endPressure = regression.last;

  result.minimumPressure = regression.minimum;
  result.maximumPressure = regression.maximum;

  result.averageRate = rate;
  result.samples = regression.n;

  result.valid = regression.n >= 3 && isfinite(rate);

  // Release pressure after every test.
  if (!releasePressure()) {
    return false;
  }

  Serial.println();
  Serial.println("========== TEST RESULT ==========");

  Serial.printf(
    "Frequency:       %lu Hz\n",
    (unsigned long)result.frequency
  );

  Serial.printf(
    "Duty:            %u%%\n",
    result.duty
  );

  Serial.printf(
    "Start pressure:  %.2f mmHg\n",
    result.startPressure
  );

  Serial.printf(
    "End pressure:    %.2f mmHg\n",
    result.endPressure
  );

  Serial.printf(
    "Min pressure:    %.2f mmHg\n",
    result.minimumPressure
  );

  Serial.printf(
    "Max pressure:    %.2f mmHg\n",
    result.maximumPressure
  );

  Serial.printf(
    "Average rate:    %.3f mmHg/s\n",
    result.averageRate
  );

  Serial.printf(
    "Samples:         %lu\n",
    (unsigned long)result.samples
  );

  Serial.println("=================================");

  return true;
}

// =====================================================
// FINAL RESULTS
// =====================================================

void printFinalResults() {

  Serial.println();
  Serial.println();
  Serial.println("===============================================================");
  Serial.println("          VALVE CHARACTERIZATION - INDIVIDUAL RUNS");
  Serial.println("===============================================================");

  Serial.println(
    "SET | RUN | FREQ | DUTY | RATE(mmHg/s) | SAMPLES | STATUS"
  );

  Serial.println("---------------------------------------------------------------");

  for (int i = 0; i < NUM_TESTS; i++) {

    for (int r = 0; r < REPEATS_PER_SETTING; r++) {

      const TestResult &result = results[i][r];

      if (!result.valid) {

        Serial.printf(
          "%3d | %3d | %4lu | %3u%% |       --     | %7lu | FAILED\n",
          i + 1,
          r + 1,
          (unsigned long)tests[i].frequency,
          tests[i].duty,
          (unsigned long)result.samples
        );

      } else {

        Serial.printf(
          "%3d | %3d | %4lu | %3u%% | %12.3f | %7lu | OK\n",
          i + 1,
          r + 1,
          (unsigned long)result.frequency,
          result.duty,
          result.averageRate,
          (unsigned long)result.samples
        );
      }
    }
  }

  Serial.println("===============================================================");
  Serial.println("          SUMMARY: MEAN AND SAMPLE STANDARD DEVIATION");
  Serial.println("===============================================================");

  Serial.println(
    "FREQ | DUTY | VALID RUNS | MEAN RATE | STD DEV | TARGET ERROR"
  );

  Serial.println("---------------------------------------------------------------");

  for (int i = 0; i < NUM_TESTS; i++) {

    double sum = 0.0;
    double sumSquares = 0.0;

    int validCount = 0;

    for (int r = 0; r < REPEATS_PER_SETTING; r++) {

      if (results[i][r].valid &&
          isfinite(results[i][r].averageRate)) {

        const double rate = results[i][r].averageRate;

        sum += rate;
        sumSquares += rate * rate;

        validCount++;
      }
    }

    if (validCount == 0) {

      Serial.printf(
        "%4lu | %3u%% | %10d |      --   |    --   |      --\n",
        (unsigned long)tests[i].frequency,
        tests[i].duty,
        validCount
      );

      continue;
    }

    const double mean = sum / validCount;

    double standardDeviation = NAN;

    if (validCount >= 2) {

      double variance =
          (sumSquares - (sum * sum) / validCount) /
          (validCount - 1);

      if (variance < 0.0 && variance > -1e-9) {
        variance = 0.0;
      }

      if (variance >= 0.0) {
        standardDeviation = sqrt(variance);
      }
    }

    const double targetError = mean - TARGET_RATE;

    Serial.printf(
      "%4lu | %3u%% | %10d | %9.3f | %7.3f | %+8.3f\n",
      (unsigned long)tests[i].frequency,
      tests[i].duty,
      validCount,
      mean,
      standardDeviation,
      targetError
    );
  }

  Serial.println("---------------------------------------------------------------");
  Serial.println("Target rate: approximately 2.5 mmHg/s");
  Serial.println("Std dev is sample standard deviation across valid repeats.");
}

// =====================================================
// AUTOMATIC TEST SEQUENCE
// =====================================================

void runAllTests() {

  if (systemFault) {
    Serial.println("System fault. Cannot start tests.");
    return;
  }

  testAllRunning = true;

  Serial.println();
  Serial.println("STARTING REPEATABILITY CHARACTERIZATION");

  Serial.println(
    "Six settings, three repeats per setting (18 runs total)."
  );

  Serial.println(
    "Each run starts by inflating to 80 mmHg."
  );

  Serial.println(
    "Pump and valve will operate automatically."
  );

  Serial.println(
    "BENCH TEST ONLY - DO NOT CONNECT TO A PERSON."
  );

  for (int i = 0; i < NUM_TESTS; i++) {

    if (systemFault) break;

    for (int r = 0; r < REPEATS_PER_SETTING; r++) {

      if (systemFault) break;

      results[i][r].valid = false;
      results[i][r].samples = 0;

      Serial.println();

      Serial.printf(
        "SETTING %d/%d | REPEAT %d/%d\n",
        i + 1,
        NUM_TESTS,
        r + 1,
        REPEATS_PER_SETTING
      );

      if (!runOneTest(i, r, results[i][r])) {
        break;
      }

      // Pressure has already been released.
      delay(1000);
    }

    if (systemFault) break;
  }

  testAllRunning = false;

  if (systemFault) {
    Serial.println("Sequence aborted due to safety fault.");
    return;
  }

  printFinalResults();

  Serial.println();
  Serial.println("ALL TESTS COMPLETED.");
  Serial.println("Valve is OPEN. Pump is OFF.");
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(500);

  // Initialize actuator outputs.
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

  pumpRunning = false;
  valveDuty = 0;

  Serial.println();
  Serial.println("Initializing HX710B...");

  if (air_press.init() != HX710B_OK) {

    Serial.println("HX710B initialization failed!");

    digitalWrite(PUMP_EN, LOW);
    digitalWrite(VALVE_EN, LOW);

    while (true) {
      delay(1000);
    }
  }

  sensorReady = true;

  Serial.println("HX710B initialized.");

  // Create valve PWM task.
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    valvePWMTask,
    "ValvePWM",
    3072,
    nullptr,
    2,
    &valveTaskHandle,
    1
  );

  if (taskCreated != pdPASS) {

    Serial.println("Valve task creation failed.");

    digitalWrite(PUMP_EN, LOW);
    digitalWrite(VALVE_EN, LOW);

    while (true) {
      delay(1000);
    }
  }

  Serial.println("System ready.");
  Serial.println("Pump OFF. Valve OPEN.");

  Serial.println();
  Serial.println("Enter RUN to start 18 characterization runs.");
  Serial.println("Enter HELP for commands.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  if (systemFault) {

    stopPump();
    openValve();

    delay(100);
    return;
  }

  if (Serial.available()) {

    String command = Serial.readStringUntil('\n');

    command.trim();
    command.toUpperCase();

    if (command == "RUN") {

      if (testAllRunning) {
        Serial.println("Tests already running.");
      } else {
        runAllTests();
      }
    }

    else if (command == "HELP") {

      Serial.println();
      Serial.println("COMMANDS:");

      Serial.println(
        "RUN  - Run six settings, three repeats each"
      );

      Serial.println(
        "HELP - Show commands"
      );

      Serial.println(
        "No manual pump control in this sketch."
      );
    }

    else if (command.length() > 0) {
      Serial.println("Unknown command.");
    }
  }

  delay(5);
}
