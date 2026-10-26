
#include <Arduino.h>
#include <math.h>
#include "korotkoff_detector.h"

static constexpr int MIC_PIN = 4;
static constexpr uint32_t FS = 2000;
static constexpr uint32_t SAMPLE_US = 1000000UL / FS;
static constexpr uint8_t LOG_EVERY_N_SAMPLES = 8;

KorotkoffDetector detector;

float previousInput = 0.0f;
float highPassOutput = 0.0f;
float lowPassOutput = 0.0f;

static constexpr float HP_ALPHA = 0.9409f;
static constexpr float LP_ALPHA = 0.4852f;

uint32_t nextSampleUs = 0;
uint32_t sampleCount = 0;
uint32_t previousEventMs = 0;
bool eventPending = false;

char commandBuffer[40];
uint8_t commandLength = 0;

void handleCommand(const char* command) {
    // Examples: M=3.0, F=12, D=280
    char key = 0;
    float value = 0;

    if (sscanf(command, "%c=%f", &key, &value) != 2 ||
        !isfinite(value)) {
        return;
    }

    switch (key) {
        case 'M':
            if (value >= 1.0f && value <= 10.0f)
                detector.thresholdMultiplier = value;
            break;

        case 'F':
            if (value >= 1.0f && value <= 500.0f)
                detector.thresholdFloor = value;
            break;

        case 'D':
            if (value >= 200.0f && value <= 1000.0f)
                detector.minEventIntervalMs = (uint32_t)value;
            break;

        default:
            break;
    }
}

void readCommands() {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (commandLength > 0) {
                commandBuffer[commandLength] = '\0';
                handleCommand(commandBuffer);
                commandLength = 0;
            }
        } else if (commandLength < sizeof(commandBuffer) - 1) {
            commandBuffer[commandLength++] = c;
        } else {
            commandLength = 0;
        }
    }
}

void setup() {
    Serial.begin(460800);

    analogReadResolution(12);
    analogSetPinAttenuation(MIC_PIN, ADC_11db);

    detector.reset();
    detector.setPressure(NAN, false);
    detector.startDeflation();

    nextSampleUs = micros();

    // CSV columns:
    // ms,raw,filtered,envelope,noise,threshold,event
    Serial.println("ms,raw,filtered,envelope,noise,threshold,event");
}

void loop() {
    readCommands();

    uint32_t nowUs = micros();

    if ((int32_t)(nowUs - nextSampleUs) < 0) {
        return;
    }

    nextSampleUs += SAMPLE_US;

    // Recover gracefully if processing falls behind.
    if ((int32_t)(nowUs - nextSampleUs) >
        (int32_t)(5 * SAMPLE_US)) {
        nextSampleUs = nowUs + SAMPLE_US;
    }

    uint16_t raw = analogRead(MIC_PIN);

    float input = (float)raw;

    highPassOutput = HP_ALPHA *
        (highPassOutput + input - previousInput);
    previousInput = input;

    lowPassOutput += LP_ALPHA *
        (highPassOutput - lowPassOutput);

    uint32_t nowMs = millis();

    detector.setPressure(NAN, false);
    detector.process(lowPassOutput, nowMs);

    if (detector.lastEventMs != previousEventMs &&
        detector.lastEventMs != 0) {
        eventPending = true;
        previousEventMs = detector.lastEventMs;
    }

    sampleCount++;

    if (sampleCount % LOG_EVERY_N_SAMPLES == 0) {
        float threshold = fmaxf(
            detector.thresholdFloor,
            detector.noiseFloor * detector.thresholdMultiplier
        );

        Serial.print(nowMs);
        Serial.print(',');
        Serial.print(raw);
        Serial.print(',');
        Serial.print(lowPassOutput, 2);
        Serial.print(',');
        Serial.print(detector.envelope, 2);
        Serial.print(',');
        Serial.print(detector.noiseFloor, 2);
        Serial.print(',');
        Serial.print(threshold, 2);
        Serial.print(',');
        Serial.println(eventPending ? 1 : 0);

        eventPending = false;
    }
}
