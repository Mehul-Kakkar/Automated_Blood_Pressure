#pragma once
#include <Arduino.h>
#include <math.h>

// GPIO4 is pump IN2; microphone input uses GPIO10.
constexpr uint8_t MIC_PIN=10;
constexpr uint8_t PRESSURE_DATA_PIN=8, PRESSURE_CLOCK_PIN=9;
constexpr uint8_t PUMP_IN1_PIN=5, PUMP_IN2_PIN=4, PUMP_ENABLE_PIN=15;
constexpr uint8_t VALVE_IN1_PIN=6, VALVE_IN2_PIN=7, VALVE_ENABLE_PIN=16;
constexpr uint32_t SENSOR_SAMPLE_RATE_HZ=2000;
constexpr uint32_t SENSOR_SAMPLE_INTERVAL_US=1000000UL/SENSOR_SAMPLE_RATE_HZ;
// Experimental transducer calibration. Keep the coefficients centralized.
// P_psi = 5e-7 * (signed raw counts - 170000); 1 psi = 51.7149 mmHg.
constexpr float PRESSURE_PSI_PER_COUNT=5.0e-7f;
constexpr int32_t PRESSURE_ZERO_OFFSET_COUNTS=170000;
constexpr float PSI_TO_MMHG=51.7149f;
constexpr int32_t HX710B_SIGNED_POSITIVE_SATURATION=8388607;
constexpr int32_t HX710B_SIGNED_NEGATIVE_SATURATION=-8388608;
constexpr int32_t MIN_PLAUSIBLE_PRESSURE_RAW=-8000000;
constexpr bool PRESSURE_CALIBRATION_CONFIGURED=true;
constexpr float MAX_INFLATION_PRESSURE_MMHG=130.0f;
constexpr float FAULT_RESET_SAFE_PRESSURE_MMHG=5.0f;
constexpr uint32_t PUMP_MAX_RUNTIME_MS=120000; // bench sketch limit, not a clinical pressure limit
// Legacy timed-pump test constants remain for source compatibility only.
// The compile-time assertion below prohibits enabling this unsafe bypass.
constexpr bool TIMED_PUMP_TEST_MODE=false;
constexpr uint32_t TIMED_PUMP_TEST_DURATION_MS=1000;
constexpr uint32_t TIMED_PUMP_TEST_MAX_DURATION_MS=5000;
constexpr uint32_t TIMED_PUMP_TEST_CAPTURE_MS=15000;
static_assert(TIMED_PUMP_TEST_DURATION_MS>0 && TIMED_PUMP_TEST_DURATION_MS<=TIMED_PUMP_TEST_MAX_DURATION_MS,
              "Timed pump duration must be between 1 ms and the hard maximum.");
static_assert(!TIMED_PUMP_TEST_MODE,
              "Timed pump test mode bypasses pressure feedback and is prohibited in this controller firmware.");
constexpr uint32_t MAX_TELEMETRY_BATCH_SAMPLES=12;
constexpr uint32_t TELEMETRY_QUEUE_CAPACITY=64;
constexpr uint32_t COMMAND_QUEUE_CAPACITY=8;
constexpr uint32_t STATUS_INTERVAL_MS=3000;
constexpr uint8_t ESP_NOW_CHANNEL=1; // must match display and Wi-Fi AP channel
constexpr uint32_t ESP_NOW_MAGIC=0x42505031;
constexpr uint16_t ESP_NOW_PROTOCOL_VERSION=1; // legacy test packet layout
constexpr char FIRMWARE_VERSION[]="1.0.0-safe";
constexpr char PROCESSING_ALGORITHM_VERSION[]="korotkoff-existing-1";

// Confirmed pump and normally-open valve behavior; energizing closes the valve.
constexpr bool PRESSURE_CALIBRATION_CONFIRMED=true;
constexpr bool ACTUATOR_PINMAP_CONFIRMED=true;
constexpr bool VALVE_RELEASE_BEHAVIOR_CONFIRMED=true;
constexpr bool AUTOMATIC_INFLATION_ENABLED=true;
