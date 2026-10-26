#include <Arduino.h>
#include <math.h>
#include "korotkoff_detector.h"

// Existing microphone connection
static constexpr int MIC_PIN = 4;
static constexpr uint32_t FS = 2000;
static constexpr uint32_t SAMPLE_US = 1000000UL / FS;

KorotkoffDetector detector;

// Lightweight 20 Hz high-pass + 300 Hz low-pass filters
float previousInput = 0.0f;
float highPassOutput = 0.0f;
float lowPassOutput = 0.0f;

static constexpr float HP_ALPHA = 0.9409f;
static constexpr float LP_ALPHA = 0.4852f;

uint32_t nextSampleUs = 0;
uint32_t sampleCount = 0;
uint8_t lastReportedEventCount = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    analogReadResolution(12);
    analogSetPinAttenuation(MIC_PIN, ADC_11db);

    detector.reset();

    // No pressure sensor connected in this test.
    detector.setPressure(NAN, false);

    // Enable event collection for acoustic testing.
    // This does not operate any pump or valve.
    detector.startDeflation();

    nextSampleUs = micros();

    Serial.println();
    Serial.println("Korotkoff acoustic detector test");
    Serial.println("Sampling: 2000 Hz");
    Serial.println("Pressure input: unavailable");
    Serial.println("Place microphone steadily and observe events.");
}

void loop() {
    uint32_t nowUs = micros();

    if ((int32_t)(nowUs - nextSampleUs) < 0) {
        return;
    }

    nextSampleUs += SAMPLE_US;

    // If timing falls behind, resynchronize instead of
    // processing a rapid burst of late samples.
    if ((int32_t)(nowUs - nextSampleUs) > (int32_t)(5 * SAMPLE_US)) {
        nextSampleUs = nowUs + SAMPLE_US;
    }

    uint16_t raw = analogRead(MIC_PIN);

    // Remove DC offset using the high-pass filter.
    float input = (float)raw;
    highPassOutput = HP_ALPHA *
        (highPassOutput + input - previousInput);
    previousInput = input;

    // Low-pass filter to reduce higher-frequency noise.
    lowPassOutput += LP_ALPHA *
        (highPassOutput - lowPassOutput);

    uint32_t nowMs = millis();

    detector.setPressure(NAN, false);
    detector.process(lowPassOutput, nowMs);

    sampleCount++;

    if (detector.eventCount != lastReportedEventCount) {
        lastReportedEventCount = detector.eventCount;

        const KorotkoffEvent &event =
            detector.events[detector.eventCount - 1];

        Serial.print("Acoustic event | t=");
        Serial.print(event.timeMs);
        Serial.print(" ms | strength=");
        Serial.print(event.strength, 1);
        Serial.println(" | pressure unavailable");
    }

    // Periodic heartbeat so you can tell the firmware is running.
    static uint32_t lastStatusMs = 0;

    if (nowMs - lastStatusMs >= 2000) {
        lastStatusMs = nowMs;

        Serial.print("Running | samples=");
        Serial.print(sampleCount);
        Serial.print(" | envelope=");
        Serial.print(detector.envelope, 1);
        Serial.print(" | noise=");
        Serial.println(detector.noiseFloor, 1);
    }
}
