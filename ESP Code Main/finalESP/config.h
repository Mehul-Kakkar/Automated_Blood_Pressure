#pragma once
#include <Arduino.h>

// Values come from the existing test sketches. Those sketches assign GPIO4
// to both MIC_PIN and PUMP_IN2_PIN. Do not enable actuators until the physical
// harness is checked and the pin conflict is resolved.
constexpr uint8_t MIC_PIN=10;
constexpr uint8_t PRESSURE_DATA_PIN=8, PRESSURE_CLOCK_PIN=9;
constexpr uint8_t PUMP_IN1_PIN=5, PUMP_IN2_PIN=4, PUMP_ENABLE_PIN=15;
constexpr uint8_t VALVE_IN1_PIN=6, VALVE_IN2_PIN=7, VALVE_ENABLE_PIN=16;
constexpr uint32_t SENSOR_SAMPLE_RATE_HZ=2000;
constexpr uint32_t SENSOR_SAMPLE_INTERVAL_US=1000000UL/SENSOR_SAMPLE_RATE_HZ;
constexpr uint32_t PRESSURE_POLL_INTERVAL_MS=100;
constexpr uint32_t HX710B_READY_TIMEOUT_US=50000;
constexpr uint32_t PUMP_MAX_RUNTIME_MS=120000; // bench sketch limit, not a clinical pressure limit
constexpr uint32_t MAX_TELEMETRY_BATCH_SAMPLES=12;
constexpr uint32_t TELEMETRY_QUEUE_CAPACITY=64;
constexpr uint32_t COMMAND_QUEUE_CAPACITY=8;
constexpr uint32_t STATUS_INTERVAL_MS=3000;
constexpr uint8_t ESP_NOW_CHANNEL=1; // must match display and Wi-Fi AP channel
constexpr uint32_t ESP_NOW_MAGIC=0x42505031;
constexpr uint16_t ESP_NOW_PROTOCOL_VERSION=1; // legacy test packet layout
constexpr char FIRMWARE_VERSION[]="1.0.0-safe";
constexpr char PROCESSING_ALGORITHM_VERSION[]="korotkoff-existing-1";

// Do not change these based on raw-count observations. These require reviewed
// calibration/wiring records and verified valve behavior.
constexpr bool PRESSURE_CALIBRATION_CONFIRMED=false;
constexpr bool ACTUATOR_PINMAP_CONFIRMED=false;
constexpr bool VALVE_RELEASE_BEHAVIOR_CONFIRMED=false;
constexpr bool AUTOMATIC_INFLATION_ENABLED=false;
