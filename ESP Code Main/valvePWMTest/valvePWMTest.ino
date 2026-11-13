#include "HX710B.h"
#include <math.h>
#include <ctype.h>
#include <string.h>

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
// CONFIGURATION
// =====================================================

const uint32_t SENSOR_TIMEOUT = 300UL;

const float CAL_SLOPE = 5.0e-7f;
const float RAW_OFFSET = 170000.0f;
const float PSI_TO_MMHG = 51.7149f;

const float INFLATION_TARGET = 80.0f;
const float DEFLATION_TARGET = 60.0f;
const float RELEASE_TARGET = 5.0f;

const float MAX_PRESSURE = 95.0f;

// Pump continues for this long after reaching 80 mmHg.
const uint32_t POST_TARGET_PUMP_MS = 1000UL;

const uint32_t MAX_INFLATION_MS = 30000UL;
const uint32_t MAX_DEFLATION_MS = 30000UL;
const uint32_t MAX_RELEASE_MS = 30000UL;

const int32_t ADC_MAX = 8388607L;
const int32_t ADC_MIN = -8388608L;

const int MAX_SAMPLES = 3000;

// =====================================================
// SENSOR
// =====================================================

HX710B air_press(SCK_PIN, SDI_PIN);

int32_t rawReading = 0;
float pressureMMHg = 0.0f;
float pressurePSI = 0.0f;

// =====================================================
// SYSTEM STATE
// =====================================================

bool systemFault = false;
bool testRunning = false;
bool runRequested = false;
bool stopRequested = false;

char commandBuffer[20];
int commandLength = 0;

// =====================================================
// CAPTURE DATA
// =====================================================

struct PressureSample {
  uint32_t timeUs;
  float pressure;
  int32_t raw;
};

PressureSample samples[MAX_SAMPLES];

int sampleCount = 0;
uint32_t pumpOffTimeUs = 0;

// =====================================================
// REGRESSION OUTPUT VARIABLES
// =====================================================

uint32_t windowCount = 0;

float windowRegressionRate = NAN;
float windowAverageRate = NAN;
float windowRSquared = NAN;
float windowMaxResidual = NAN;

float windowFirstPressure = NAN;
float windowLastPressure = NAN;
float windowDuration = NAN;

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
  // Normally-open valve: de-energized = open.
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

void pollSerial() {

  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {

      commandBuffer[commandLength] = '\0';

      for (int i = 0; i < commandLength; i++) {
        commandBuffer[i] =
            toupper((unsigned char)commandBuffer[i]);
      }

      if (strcmp(commandBuffer, "STOP") == 0) {

        stopRequested = true;
        safeState();

        Serial.println("STOP REQUESTED.");
        Serial.println("Pump OFF. Valve OPEN.");
      }

      else if (strcmp(commandBuffer, "RUN") == 0) {

        if (systemFault) {
          Serial.println("System fault. Reset required.");
        }

        else if (testRunning || runRequested) {
          Serial.println("Test already running or queued.");
        }

        else {
          stopRequested = false;
          runRequested = true;
        }
      }

      else if (strcmp(commandBuffer, "HELP") == 0) {

        Serial.println();
        Serial.println("COMMANDS:");
        Serial.println("RUN  - Start passive deflation test");
        Serial.println("STOP - Stop pump and open valve");
        Serial.println("HELP - Show commands");
      }

      else if (commandLength > 0) {
        Serial.println("Unknown command.");
      }

      commandLength = 0;
    }

    else if (commandLength < (int)sizeof(commandBuffer) - 1) {
      commandBuffer[commandLength++] = c;
    }

    else {
      commandLength = 0;
    }
  }
}

// =====================================================
// WAIT WHILE CHECKING STOP COMMAND
// =====================================================

bool waitWithStop(uint32_t durationMs) {

  uint32_t startTime = millis();

  while (millis() - startTime < durationMs) {

    pollSerial();

    if (stopRequested) {
      safeState();
      return false;
    }

    delay(1);
  }

  return true;
}

// =====================================================
// SAFETY FAULT
// =====================================================

void triggerFault(const char* reason) {

  safeState();

  systemFault = true;

  Serial.println();
  Serial.println("!!!!!!!! SYSTEM FAULT !!!!!!!!");
  Serial.println(reason);
  Serial.println("Pump OFF. Valve OPEN.");
}

// =====================================================
// STORE SAMPLE
// =====================================================

bool storeSample(
  uint32_t timeUs,
  float pressure,
  int32_t raw
) {

  if (sampleCount >= MAX_SAMPLES) {
    return false;
  }

  samples[sampleCount].timeUs = timeUs;
  samples[sampleCount].pressure = pressure;
  samples[sampleCount].raw = raw;

  sampleCount++;

  return true;
}

// =====================================================
// VENT BEFORE STARTING
// =====================================================

bool ventBeforeRun() {

  Serial.println();
  Serial.println("Checking initial pressure...");

  safeState();

  uint32_t startTime = millis();

  while (millis() - startTime < MAX_RELEASE_MS) {

    pollSerial();

    if (stopRequested) {
      return false;
    }

    if (!readPressure()) {
      triggerFault("Sensor failure before test.");
      return false;
    }

    if (pressureMMHg >= MAX_PRESSURE) {
      triggerFault("Pressure exceeds safety limit.");
      return false;
    }

    if (pressureMMHg <= RELEASE_TARGET) {

      Serial.printf(
        "Initial pressure: %.3f mmHg\n",
        pressureMMHg
      );

      return true;
    }

    delay(5);
  }

  triggerFault("Could not vent initial pressure.");

  return false;
}

// =====================================================
// INFLATE AND BEGIN CAPTURE
// =====================================================

bool inflateAndStartCapture() {

  Serial.println();
  Serial.println("Inflating to 80 mmHg...");

  pumpOff();
  valveClose();

  if (!waitWithStop(150)) {
    return false;
  }

  uint32_t startTime = millis();
  uint32_t lastPrint = 0;

  uint32_t targetReachedTime = 0;

  bool targetReached = false;

  pumpOn();

  while (millis() - startTime < MAX_INFLATION_MS) {

    pollSerial();

    if (stopRequested) {
      safeState();
      return false;
    }

    if (!readPressure()) {
      triggerFault("Sensor failure during inflation.");
      return false;
    }

    if (pressureMMHg >= MAX_PRESSURE) {
      triggerFault("Maximum pressure reached.");
      return false;
    }

    // Detect the first reading at or above 80 mmHg.
    if (!targetReached &&
        pressureMMHg >= INFLATION_TARGET) {

      targetReached = true;
      targetReachedTime = millis();

      Serial.println();
      Serial.println("80 mmHg REACHED.");

      Serial.printf(
        "Pressure at threshold: %.3f mmHg\n",
        pressureMMHg
      );

      Serial.printf(
        "Keeping pump ON for another %lu ms.\n",
        (unsigned long)POST_TARGET_PUMP_MS
      );
    }

    // Continue pumping for one second after the threshold.
    if (targetReached) {

      if (millis() - targetReachedTime >=
          POST_TARGET_PUMP_MS) {

        pumpOff();

        pumpOffTimeUs = micros();

        sampleCount = 0;

        if (!storeSample(0, pressureMMHg, rawReading)) {
          triggerFault("Sample buffer error.");
          return false;
        }

        Serial.println();
        Serial.println("POST-TARGET INFLATION COMPLETE.");

        Serial.printf(
          "Final pump-off pressure: %.3f mmHg\n",
          pressureMMHg
        );

        Serial.println("Pump OFF.");
        Serial.println("Valve CLOSED.");
        Serial.println("Beginning passive pressure recording.");

        Serial.println();
        Serial.println("ELAPSED_MS,PRESSURE_MMHG,RAW");

        Serial.printf(
          "0,%.3f,%ld\n",
          pressureMMHg,
          (long)rawReading
        );

        return true;
      }

      // Report pressure while the pump is still running.
      if (millis() - lastPrint >= 250UL) {

        Serial.printf(
          "Post-target pumping: %.3f mmHg | %lu ms remaining\n",
          pressureMMHg,
          (unsigned long)(
            POST_TARGET_PUMP_MS -
            (millis() - targetReachedTime)
          )
        );

        lastPrint = millis();
      }
    }

    else if (millis() - lastPrint >= 1000UL) {

      Serial.printf(
        "Inflation: %.3f mmHg\n",
        pressureMMHg
      );

      lastPrint = millis();
    }

    delay(1);
  }

  triggerFault("Inflation timeout.");

  return false;
}

// =====================================================
// PASSIVE DEFLATION RECORDING
// =====================================================

bool recordPassiveDeflation() {

  Serial.println();
  Serial.println("PASSIVE DEFLATION STARTED");
  Serial.println("Pump remains OFF.");
  Serial.println("Valve remains CLOSED.");
  Serial.println("No pressure corrections will be made.");

  bool reachedTarget = false;

  uint32_t elapsedUs = 0;

  while (elapsedUs < MAX_DEFLATION_MS * 1000UL) {

    pollSerial();

    if (stopRequested) {
      Serial.println("Recording interrupted by STOP.");
      break;
    }

    if (!readPressure()) {
      triggerFault("Sensor failure during passive deflation.");
      break;
    }

    elapsedUs = micros() - pumpOffTimeUs;

    if (pressureMMHg >= MAX_PRESSURE) {
      triggerFault("Pressure safety limit exceeded.");
      break;
    }

    if (!storeSample(elapsedUs, pressureMMHg, rawReading)) {
      triggerFault("Sample buffer full.");
      break;
    }

    Serial.printf(
      "%lu,%.3f,%ld\n",
      (unsigned long)(elapsedUs / 1000UL),
      pressureMMHg,
      (long)rawReading
    );

    if (pressureMMHg <= DEFLATION_TARGET) {

      reachedTarget = true;

      Serial.println();
      Serial.println("60 mmHg REACHED.");

      break;
    }
  }

  if (!reachedTarget &&
      !stopRequested &&
      !systemFault) {

    Serial.println();
    Serial.println("DEFLATION TIMEOUT.");
    Serial.println("60 mmHg was not reached within 30 seconds.");
  }

  safeState();

  return reachedTarget;
}

// =====================================================
// PRESSURE RELEASE
// =====================================================

bool releasePressure() {

  Serial.println();
  Serial.println("Opening valve and releasing pressure...");

  pumpOff();
  valveOpen();

  uint32_t startTime = millis();

  while (millis() - startTime < MAX_RELEASE_MS) {

    pollSerial();

    if (stopRequested) {
      safeState();
      return false;
    }

    if (!readPressure()) {
      triggerFault("Sensor failure during pressure release.");
      return false;
    }

    if (pressureMMHg <= RELEASE_TARGET) {

      Serial.printf(
        "Pressure released: %.3f mmHg\n",
        pressureMMHg
      );

      return true;
    }

    delay(2);
  }

  triggerFault("Pressure release timeout.");

  return false;
}

// =====================================================
// CALCULATE REGRESSION FOR A TIME WINDOW
// =====================================================

void calculateWindow(
  float startSeconds,
  float endSeconds
) {

  windowCount = 0;

  windowRegressionRate = NAN;
  windowAverageRate = NAN;
  windowRSquared = NAN;
  windowMaxResidual = NAN;

  windowFirstPressure = NAN;
  windowLastPressure = NAN;
  windowDuration = NAN;

  double sumT = 0;
  double sumP = 0;
  double sumTT = 0;
  double sumTP = 0;

  double firstT = 0;
  double lastT = 0;

  for (int i = 0; i < sampleCount; i++) {

    double t =
        samples[i].timeUs / 1000000.0;

    double p = samples[i].pressure;

    if (t < startSeconds || t > endSeconds) {
      continue;
    }

    if (windowCount == 0) {

      windowFirstPressure = p;
      firstT = t;
    }

    windowLastPressure = p;
    lastT = t;

    sumT += t;
    sumP += p;
    sumTT += t * t;
    sumTP += t * p;

    windowCount++;
  }

  if (windowCount < 3) {
    return;
  }

  double n = windowCount;

  double denominator =
      n * sumTT - sumT * sumT;

  if (fabs(denominator) < 1e-12) {
    return;
  }

  double slope =
      (n * sumTP - sumT * sumP) / denominator;

  double intercept =
      (sumP - slope * sumT) / n;

  windowRegressionRate = (float)(-slope);

  windowDuration = (float)(lastT - firstT);

  if (windowDuration > 0) {

    windowAverageRate =
        (windowFirstPressure - windowLastPressure) /
        windowDuration;
  }

  double meanP = sumP / n;

  double totalVariation = 0;
  double residualVariation = 0;
  double maxResidual = 0;

  for (int i = 0; i < sampleCount; i++) {

    double t =
        samples[i].timeUs / 1000000.0;

    double p = samples[i].pressure;

    if (t < startSeconds || t > endSeconds) {
      continue;
    }

    double predicted = intercept + slope * t;

    double residual = p - predicted;

    totalVariation +=
        (p - meanP) * (p - meanP);

    residualVariation +=
        residual * residual;

    if (fabs(residual) > maxResidual) {
      maxResidual = fabs(residual);
    }
  }

  if (totalVariation > 1e-12) {

    windowRSquared =
        (float)(1.0 - residualVariation / totalVariation);
  }

  windowMaxResidual = (float)maxResidual;
}

// =====================================================
// PRINT WINDOW STATISTICS
// =====================================================

void printWindow(
  const char* name,
  float startSeconds,
  float endSeconds
) {

  calculateWindow(startSeconds, endSeconds);

  Serial.println();
  Serial.println(name);

  Serial.printf(
    "  Samples          : %lu\n",
    (unsigned long)windowCount
  );

  if (windowCount < 3 ||
      !isfinite(windowRegressionRate)) {

    Serial.println("  Insufficient data for regression.");
    return;
  }

  Serial.printf(
    "  Regression rate  : %.3f mmHg/s\n",
    windowRegressionRate
  );

  Serial.printf(
    "  Average rate     : %.3f mmHg/s\n",
    windowAverageRate
  );

  Serial.printf(
    "  R-squared        : %.5f\n",
    windowRSquared
  );

  Serial.printf(
    "  Max fit residual : %.3f mmHg\n",
    windowMaxResidual
  );

  Serial.printf(
    "  Pressure change  : %.3f mmHg\n",
    windowFirstPressure - windowLastPressure
  );

  Serial.printf(
    "  Duration         : %.3f s\n",
    windowDuration
  );
}

// =====================================================
// FIND TIME AT WHICH PRESSURE CROSSES A THRESHOLD
// =====================================================

bool findCrossingTime(
  float threshold,
  float &crossingSeconds
) {

  if (sampleCount < 2) {
    return false;
  }

  if (samples[0].pressure <= threshold) {

    crossingSeconds = 0.0f;
    return true;
  }

  for (int i = 1; i < sampleCount; i++) {

    float p1 = samples[i - 1].pressure;
    float p2 = samples[i].pressure;

    if (p1 > threshold && p2 <= threshold) {

      float t1 =
          samples[i - 1].timeUs / 1000000.0f;

      float t2 =
          samples[i].timeUs / 1000000.0f;

      if (fabs(p1 - p2) < 1e-6f) {

        crossingSeconds = t2;
      }

      else {

        float fraction =
            (p1 - threshold) / (p1 - p2);

        crossingSeconds =
            t1 + fraction * (t2 - t1);
      }

      return true;
    }
  }

  return false;
}

// =====================================================
// PRESSURE BAND RATES
// =====================================================

void printPressureBandRates() {

  const float levels[] = {
    80.0f, 75.0f, 70.0f, 65.0f, 60.0f
  };

  float crossingTimes[5];
  bool crossed[5];

  for (int i = 0; i < 5; i++) {

    crossed[i] =
        findCrossingTime(levels[i], crossingTimes[i]);
  }

  Serial.println();
  Serial.println("========== PRESSURE BAND RATES ==========");
  Serial.println("BAND       | DURATION | RATE");

  for (int i = 0; i < 4; i++) {

    Serial.printf(
      "%.0f-%.0f    | ",
      levels[i],
      levels[i + 1]
    );

    if (!crossed[i] || !crossed[i + 1]) {

      Serial.println("Not completed");
      continue;
    }

    float duration =
        crossingTimes[i + 1] - crossingTimes[i];

    if (duration <= 0) {

      Serial.println("Invalid crossing times");
      continue;
    }

    float rate = 5.0f / duration;

    Serial.printf(
      "%.3f s  | %.3f mmHg/s\n",
      duration,
      rate
    );
  }

  Serial.println("=========================================");
}

// =====================================================
// COMPLETE ANALYSIS
// =====================================================

void printAnalysis() {

  if (sampleCount < 3) {

    Serial.println("Insufficient data for analysis.");
    return;
  }

  float endSeconds =
      samples[sampleCount - 1].timeUs / 1000000.0f;

  Serial.println();
  Serial.println();
  Serial.println("============================================");
  Serial.println("         PASSIVE DEFLATION ANALYSIS");
  Serial.println("============================================");

  Serial.printf(
    "Pump-off pressure : %.3f mmHg\n",
    samples[0].pressure
  );

  Serial.printf(
    "Final pressure    : %.3f mmHg\n",
    samples[sampleCount - 1].pressure
  );

  Serial.printf(
    "Total pressure drop: %.3f mmHg\n",
    samples[0].pressure -
    samples[sampleCount - 1].pressure
  );

  Serial.printf(
    "Recording time    : %.3f s\n",
    endSeconds
  );

  Serial.printf(
    "Total samples     : %d\n",
    sampleCount
  );

  Serial.println();
  Serial.println("INITIAL POST-PUMP-OFF CHANGE");

  if (sampleCount >= 2) {

    Serial.printf(
      "First sample time : %.3f ms\n",
      samples[1].timeUs / 1000.0f
    );

    Serial.printf(
      "First sample      : %.3f mmHg\n",
      samples[1].pressure
    );

    Serial.printf(
      "Initial pressure change: %.3f mmHg\n",
      samples[0].pressure -
      samples[1].pressure
    );
  }

  Serial.println();
  Serial.println("TIME-BASED REGRESSION");

  printWindow(
    "INITIAL: 0 - 0.5 seconds",
    0.0f,
    0.5f
  );

  printWindow(
    "SETTLING: 0.5 - 2 seconds",
    0.5f,
    2.0f
  );

  printWindow(
    "SUBSEQUENT: 2 seconds onward",
    2.0f,
    endSeconds + 0.01f
  );

  Serial.println();
  Serial.println("OVERALL REGRESSION");

  printWindow(
    "COMPLETE RECORDING",
    0.0f,
    endSeconds + 0.01f
  );

  printPressureBandRates();

  Serial.println();
  Serial.println("============================================");
  Serial.println("END OF ANALYSIS");
  Serial.println("============================================");
}

// =====================================================
// COMPLETE TEST
// =====================================================

bool runPassiveTest() {

  sampleCount = 0;

  if (!ventBeforeRun()) {
    return false;
  }

  if (!inflateAndStartCapture()) {
    return false;
  }

  bool reachedTarget =
      recordPassiveDeflation();

  if (!stopRequested && !systemFault) {
    releasePressure();
  }

  printAnalysis();

  Serial.println();

  if (reachedTarget && !systemFault && !stopRequested) {
    Serial.println("PASSIVE DEFLATION TEST COMPLETED.");
  }

  else if (stopRequested) {
    Serial.println("TEST STOPPED BY USER.");
  }

  else if (!systemFault) {
    Serial.println("TEST ENDED WITHOUT REACHING 60 mmHg.");
  }

  safeState();

  return reachedTarget && !systemFault && !stopRequested;
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

    Serial.println("HX710B initialization failed.");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("HX710B initialized.");

  Serial.println();
  Serial.println("============================================");
  Serial.println("     PASSIVE DEFLATION CHARACTERIZATION");
  Serial.println("============================================");

  Serial.println("Initial target    : 80 mmHg");
  Serial.println("Extra pump time   : 1000 ms");
  Serial.println("Deflation target  : 60 mmHg");
  Serial.println("Maximum pressure  : 95 mmHg");
  Serial.println("Pump during test  : OFF");
  Serial.println("Valve during test : CLOSED");

  Serial.println();
  Serial.println("COMMANDS:");
  Serial.println("RUN  - Start test");
  Serial.println("STOP - Stop pump and open valve");
  Serial.println("HELP - Show commands");

  Serial.println();
  Serial.println("BENCH TEST ONLY.");
  Serial.println("Pump OFF. Valve OPEN.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  pollSerial();

  if (systemFault) {

    safeState();

    delay(100);
    return;
  }

  if (runRequested && !testRunning) {

    runRequested = false;
    stopRequested = false;

    testRunning = true;

    Serial.println();
    Serial.println("STARTING PASSIVE DEFLATION TEST.");

    runPassiveTest();

    testRunning = false;

    safeState();

    Serial.println();
    Serial.println("SYSTEM IDLE.");
    Serial.println("Pump OFF. Valve OPEN.");
  }

  delay(2);
}