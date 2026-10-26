#pragma once
#include <Arduino.h>
#include <math.h>

struct KorotkoffEvent {
    uint32_t timeMs;
    float pressureMmHg;
    float strength;
};

class KorotkoffDetector {
public:
    static constexpr uint8_t MAX_EVENTS = 64;

    KorotkoffEvent events[MAX_EVENTS];
    uint8_t eventCount = 0;

    bool pressureValid = false;
    float currentPressure = NAN;

    bool systolicCandidateValid = false;
    float systolicCandidate = NAN;

    bool disappearanceCandidateValid = false;
    float disappearanceCandidate = NAN;

    uint32_t lastEventMs = 0;
    uint32_t burstStartMs = 0;
    uint32_t lastAboveThresholdMs = 0;
    uint32_t silenceStartMs = 0;

    float envelope = 0.0f;
    float noiseFloor = 3.0f;
    float burstPeak = 0.0f;

    bool inBurst = false;
    bool deflationActive = false;

    uint8_t consistentEvents = 0;

    // Initial tuning values; calibrate against recorded signals.
    float envelopeAlpha = 0.08f;
    float noiseAlpha = 0.002f;
    float thresholdFloor = 12.0f;
    float thresholdMultiplier = 3.0f;

    uint32_t minEventIntervalMs = 280;
    uint32_t minBurstMs = 8;
    uint32_t maxBurstMs = 180;
    uint32_t disappearanceDelayMs = 4000;

    void reset() {
        eventCount = 0;
        pressureValid = false;
        currentPressure = NAN;
        systolicCandidateValid = false;
        systolicCandidate = NAN;
        disappearanceCandidateValid = false;
        disappearanceCandidate = NAN;
        lastEventMs = 0;
        burstStartMs = 0;
        lastAboveThresholdMs = 0;
        silenceStartMs = 0;
        envelope = 0.0f;
        noiseFloor = 3.0f;
        burstPeak = 0.0f;
        inBurst = false;
        deflationActive = false;
        consistentEvents = 0;
    }

    void setPressure(float mmHg, bool valid) {
        pressureValid = valid && isfinite(mmHg)
                        && mmHg >= 0.0f && mmHg <= 400.0f;
        currentPressure = pressureValid ? mmHg : NAN;
    }

    void startDeflation() {
        reset();
        deflationActive = true;
    }

    void stopDeflation() {
        deflationActive = false;
        inBurst = false;
    }

    // Call at the actual microphone sample rate.
    // bandpassedSample must be the AC-coupled, filtered microphone signal.
    void process(float bandpassedSample, uint32_t nowMs) {
        float magnitude = fabsf(bandpassedSample);

        envelope += envelopeAlpha * (magnitude - envelope);

        float threshold = fmaxf(
            thresholdFloor,
            noiseFloor * thresholdMultiplier
        );

        if (!inBurst) {
            // Learn background only while no event is active.
            noiseFloor += noiseAlpha * (envelope - noiseFloor);
        }

        if (envelope >= threshold) {
            if (!inBurst) {
                inBurst = true;
                burstStartMs = nowMs;
                burstPeak = envelope;
            }

            lastAboveThresholdMs = nowMs;
            burstPeak = fmaxf(burstPeak, envelope);

        } else if (inBurst &&
                   (uint32_t)(nowMs - lastAboveThresholdMs) >= 20) {
            uint32_t duration = lastAboveThresholdMs - burstStartMs;

            inBurst = false;

            if (duration >= minBurstMs &&
                duration <= maxBurstMs &&
                (lastEventMs == 0 ||
                 (uint32_t)(nowMs - lastEventMs) >= minEventIntervalMs)) {
                recordEvent(nowMs, burstPeak);
            }

            burstPeak = 0.0f;
        }

        if (deflationActive && pressureValid &&
            lastEventMs != 0 &&
            (uint32_t)(nowMs - lastEventMs) >= disappearanceDelayMs) {
            silenceStartMs = lastEventMs + disappearanceDelayMs;
        }
    }

    void recordEvent(uint32_t nowMs, float strength) {
        lastEventMs = nowMs;

        if (eventCount < MAX_EVENTS) {
            events[eventCount++] = {
                nowMs,
                pressureValid ? currentPressure : NAN,
                strength
            };
        }

        if (!pressureValid || !deflationActive) {
            return;
        }

        // Candidate only: require several events before reporting
        // a possible systolic endpoint.
        if (!systolicCandidateValid) {
            systolicCandidate = currentPressure;
            systolicCandidateValid = true;
            consistentEvents = 1;
        } else if (consistentEvents < 255) {
            consistentEvents++;
        }

        // A new event cancels a provisional disappearance marker.
        disappearanceCandidateValid = false;
    }

    bool hasPossibleSystolic() const {
        return systolicCandidateValid && consistentEvents >= 3;
    }

    bool hasPossibleDisappearance(uint32_t nowMs) {
        if (!deflationActive || !pressureValid ||
            lastEventMs == 0 ||
            (uint32_t)(nowMs - lastEventMs) < disappearanceDelayMs) {
            return false;
        }

        // This is only the pressure during sustained silence,
        // not a validated diastolic pressure estimate.
        disappearanceCandidate = currentPressure;
        disappearanceCandidateValid = true;
        return true;
    }
};
