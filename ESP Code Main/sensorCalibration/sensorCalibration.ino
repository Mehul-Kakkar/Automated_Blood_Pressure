
#include <Arduino.h>
#include "HX710B.h"

// =====================================================
// HX710B PRESSURE SENSOR
// =====================================================
#define PRESSURE_DATA_PIN  8
#define PRESSURE_CLOCK_PIN 9

// 27 = HX710B_DIFF2 (40 Hz)
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
// SAFETY
// =====================================================
const unsigned long ACTUATOR_TIMEOUT = 120000UL;

unsigned long pumpStartTime = 0;
unsigned long valveStartTime = 0;

bool pumpRunning = false;
bool valveEnergized = false;

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
// =====================================================
void readPressure() {
    uint32_t sensorValue = 0;

    uint8_t status = pressureSensor.read(&sensorValue, 25UL);

    if (status == HX710B_OK) {

        // The library has already XORed the MSB.
        // Convert its unsigned, offset representation
        // into a signed count centred around zero.
        int32_t centeredValue =
            (int32_t)sensorValue - 0x800000L;

        Serial.print("Raw_Counts:");
        Serial.print(sensorValue);

        Serial.print("\tCentered_Counts:");
        Serial.println(centeredValue);
    }
}

// =====================================================
// SERIAL COMMANDS
// =====================================================
void printHelp() {
    Serial.println("\n========== PRESSURE CALIBRATION ==========");
    Serial.println("P - Pump ON");
    Serial.println("X - Pump OFF");
    Serial.println("E - Energize valve");
    Serial.println("D - De-energize valve");
    Serial.println("S - Stop pump and de-energize valve");
    Serial.println("R - Read pressure once");
    Serial.println("H - Help");
    Serial.println("==========================================\n");
}

void handleSerial() {
    if (!Serial.available()) return;

    char command = Serial.read();

    switch (command) {

        case 'P':
        case 'p':
            pumpON();
            break;

        case 'X':
        case 'x':
            pumpOFF();
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
            pumpOFF();
            deenergizeValve();
            break;

        case 'R':
        case 'r':
            readPressure();
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

    uint8_t status = pressureSensor.init();

    if (status != HX710B_OK) {
        Serial.print("HX710B initialization failed. Status: ");
        Serial.println(status);

        while (true) {
            pumpOFF();
            deenergizeValve();
            delay(1000);
        }
    }

    Serial.println("HX710B initialized successfully.");

    printHelp();

    Serial.println("Continuous pressure readings started.");
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {

    handleSerial();

    // Pump timeout
    if (pumpRunning &&
        millis() - pumpStartTime >= ACTUATOR_TIMEOUT) {

        pumpOFF();
        Serial.println("Pump safety timeout!");
    }

    // Valve timeout
    if (valveEnergized &&
        millis() - valveStartTime >= ACTUATOR_TIMEOUT) {

        deenergizeValve();
        Serial.println("Valve safety timeout!");
    }

    // Continuously read pressure
    readPressure();
}
