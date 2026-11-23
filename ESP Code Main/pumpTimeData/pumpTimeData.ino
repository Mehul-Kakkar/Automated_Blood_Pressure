
#include <Arduino.h>
#include "HX710B.h"

// =====================================================
// HX710B PRESSURE SENSOR
// =====================================================

#define PRESSURE_DATA_PIN  8
#define PRESSURE_CLOCK_PIN 9

// HX710B_DIFF2 = 40 Hz
HX710B pressureSensor(
    PRESSURE_CLOCK_PIN,
    PRESSURE_DATA_PIN,
    HX710B_DIFF2
);

// =====================================================
// L298N PUMP PINS
// =====================================================

#define PUMP_IN1 5
#define PUMP_IN2 4
#define PUMP_EN  15

// =====================================================
// L298N VALVE PINS
// =====================================================

#define VALVE_IN1 6
#define VALVE_IN2 7
#define VALVE_EN  16

// =====================================================
// TEST SETTINGS
// =====================================================

const unsigned long INFLATION_TIME_MS = 15000UL;
const unsigned long ACTUATOR_TIMEOUT = 120000UL;

unsigned long pumpStartTime = 0;
unsigned long valveStartTime = 0;
unsigned long testStartTime = 0;

bool pumpRunning = false;
bool valveEnergized = false;
bool testRunning = false;
bool sensorReady = false;

// =====================================================
// PRESSURE CALIBRATION
// =====================================================

float calculatePressure(int32_t centeredCount) {
    return -1.622066f +
           0.00001132803915f * centeredCount;
}

// =====================================================
// PUMP CONTROL
// =====================================================

void pumpON() {
    digitalWrite(PUMP_IN1, HIGH);
    digitalWrite(PUMP_IN2, LOW);
    digitalWrite(PUMP_EN, HIGH);

    pumpRunning = true;
    pumpStartTime = millis();

    Serial.println("Pump: ON");
}

void pumpOFF() {
    digitalWrite(PUMP_EN, LOW);
    digitalWrite(PUMP_IN1, LOW);
    digitalWrite(PUMP_IN2, LOW);

    pumpRunning = false;

    Serial.println("Pump: OFF");
}

// =====================================================
// VALVE CONTROL
// =====================================================

void energizeValve() {
    digitalWrite(VALVE_IN1, HIGH);
    digitalWrite(VALVE_IN2, LOW);
    digitalWrite(VALVE_EN, HIGH);

    valveEnergized = true;
    valveStartTime = millis();

    Serial.println("Valve: ENERGIZED");
}

void deenergizeValve() {
    digitalWrite(VALVE_EN, LOW);
    digitalWrite(VALVE_IN1, LOW);
    digitalWrite(VALVE_IN2, LOW);

    valveEnergized = false;

    Serial.println("Valve: DE-ENERGIZED");
}

// =====================================================
// PRESSURE SENSOR READING
// Uses the exact read pattern from your working code.
// =====================================================

void readPressure() {

    uint32_t sensorValue = 0;

    uint8_t status = pressureSensor.read(&sensorValue, 25UL);

    if (status == HX710B_OK) {

        // The library has already XORed the MSB.
        // Center the unsigned 24-bit representation.

        int32_t centeredValue =
            (int32_t)sensorValue - 0x800000L;

        float pressure_mmHg =
            calculatePressure(centeredValue);

        unsigned long elapsed;

        if (testRunning) {
            elapsed = millis() - testStartTime;
        } else {
            elapsed = millis();
        }

        // CSV output

        Serial.print(elapsed);
        Serial.print(",");

        Serial.print(sensorValue);
        Serial.print(",");

        Serial.print(centeredValue);
        Serial.print(",");

        Serial.println(pressure_mmHg, 2);

    } else {

        // Report failed reads rather than silently hiding them.
        static unsigned long lastErrorTime = 0;

        if (millis() - lastErrorTime >= 1000) {
            Serial.print("HX710B read error. Status: ");
            Serial.println(status);
            lastErrorTime = millis();
        }
    }
}

// =====================================================
// TEST CONTROL
// =====================================================

void startTest() {

    if (!sensorReady) {
        Serial.println("Cannot start: HX710B not initialized.");
        return;
    }

    if (testRunning) {
        Serial.println("Test already running.");
        return;
    }

    Serial.println("Preparing inflation test...");

    // Energize valve before starting pump.
    energizeValve();

    delay(300);

    testStartTime = millis();
    testRunning = true;

    pumpON();

    Serial.println("TEST_STARTED");
}

void stopTest() {

    pumpOFF();
    deenergizeValve();

    testRunning = false;

    Serial.println("TEST_STOPPED");
}

// =====================================================
// SERIAL COMMANDS
// =====================================================

void printHelp() {

    Serial.println("\n========== BP PUMP TEST ==========");
    Serial.println("P - Start 15-second inflation test");
    Serial.println("X - Pump OFF");
    Serial.println("E - Energize valve");
    Serial.println("D - De-energize valve");
    Serial.println("S - Stop pump and valve");
    Serial.println("H - Help");
    Serial.println("==================================\n");
}

void handleSerial() {

    if (!Serial.available()) return;

    char command = Serial.read();

    switch (command) {

        case 'P':
        case 'p':
            startTest();
            break;

        case 'X':
        case 'x':
            pumpOFF();
            testRunning = false;
            break;

        case 'E':
        case 'e':
            energizeValve();
            break;

        case 'D':
        case 'd':
            deenergizeValve();
            break;

        case 'S':
        case 's':
            stopTest();
            break;

        case 'H':
        case 'h':
            printHelp();
            break;
    }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

    Serial.begin(115200);
    delay(1000);

    pinMode(PUMP_IN1, OUTPUT);
    pinMode(PUMP_IN2, OUTPUT);
    pinMode(PUMP_EN, OUTPUT);

    pinMode(VALVE_IN1, OUTPUT);
    pinMode(VALVE_IN2, OUTPUT);
    pinMode(VALVE_EN, OUTPUT);

    pumpOFF();
    deenergizeValve();

    Serial.println("\nInitializing HX710B...");

    // CRITICAL: Initialize the sensor before reading it.
    uint8_t status = pressureSensor.init();

    if (status != HX710B_OK) {

        Serial.print("HX710B initialization failed. Status: ");
        Serial.println(status);

        pumpOFF();
        deenergizeValve();

        while (true) {
            delay(1000);
        }
    }

    sensorReady = true;

    Serial.println("HX710B initialized successfully.");

    Serial.println("\nTime_ms,Raw_Count,Centered_Count,Pressure_mmHg");

    printHelp();

    Serial.println("Continuous pressure readings started.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

    handleSerial();

    // Pump safety timeout
    if (pumpRunning &&
        millis() - pumpStartTime >= ACTUATOR_TIMEOUT) {

        pumpOFF();
        testRunning = false;

        Serial.println("Pump safety timeout!");
    }

    // Valve safety timeout
    if (valveEnergized &&
        millis() - valveStartTime >= ACTUATOR_TIMEOUT) {

        deenergizeValve();

        Serial.println("Valve safety timeout!");
    }

    // Automatic end of 15-second test
    if (testRunning &&
        millis() - testStartTime >= INFLATION_TIME_MS) {

        Serial.println("15-second test duration reached.");

        stopTest();
    }

    // Continuously read pressure, including when pump is OFF.
    if (sensorReady) {
        readPressure();
    }
}
