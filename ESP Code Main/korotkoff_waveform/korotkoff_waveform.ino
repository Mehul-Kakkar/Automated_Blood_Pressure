#include <Arduino.h>

// =====================================================
// HARDWARE
// =====================================================

static constexpr int MIC_PIN = 10;

static constexpr uint32_t FS = 2000;

static constexpr uint32_t SAMPLE_INTERVAL_US =
    1000000UL / FS;

static constexpr uint32_t BAUD = 460800;

static constexpr size_t BLOCK_SIZE = 64;

// =====================================================
// SERIAL BUFFER
// =====================================================

uint16_t sampleBuffer[BLOCK_SIZE];

size_t bufferIndex = 0;

uint32_t nextSampleUs = 0;

// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(BAUD);

    analogReadResolution(12);

    analogSetPinAttenuation(
        MIC_PIN,
        ADC_11db
    );

    // Allow analogue input to settle.
    for (int i = 0; i < 32; i++)
    {
        analogRead(MIC_PIN);
        delay(2);
    }

    nextSampleUs = micros();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
    // Wait for next sampling instant.
    while (
        (int32_t)(micros() - nextSampleUs) < 0
    )
    {
        // Sampling wait
    }

    uint32_t sampleTimeUs = micros();

    nextSampleUs += SAMPLE_INTERVAL_US;

    // ADC sampling
    uint16_t sample = analogRead(MIC_PIN);

    sampleBuffer[bufferIndex++] = sample;

    // If timing falls significantly behind, resynchronise.
    if (
        (int32_t)(micros() - nextSampleUs)
        > (int32_t)SAMPLE_INTERVAL_US
    )
    {
        nextSampleUs = micros() + SAMPLE_INTERVAL_US;
    }

    // Transmit batches of 64 samples.
    if (bufferIndex >= BLOCK_SIZE)
    {
        Serial.write(
            reinterpret_cast<const uint8_t*>(sampleBuffer),
            sizeof(sampleBuffer)
        );

        bufferIndex = 0;
    }
}
