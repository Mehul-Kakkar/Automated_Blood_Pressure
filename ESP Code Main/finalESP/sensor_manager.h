#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <freertos/queue.h>

struct SensorSample {
  uint32_t timestampMs;
  uint16_t microphoneRaw;
  uint16_t eventDurationMs;
  float filtered,envelope,noiseFloor,threshold,adcBaseline,pressureMmhg,eventPressureMmhg;
  float eventStrength;
  int32_t pressureRaw;
  uint8_t calibrationMode;
  bool eventDetected,pressureReady,adcClipped,calibrated;
};
struct WaveformSample { int16_t filteredQ4; uint16_t envelopeQ2,thresholdQ2; };
void sensorManagerBegin();
void sensorManagerPoll();
bool sensorManagerReady();
QueueHandle_t sensorSampleQueue();
uint32_t sensorDroppedCount();
bool sensorManagerLatestSample(SensorSample& sample);
bool sensorManagerBloodPressureEstimate(float& systolicMmHg,float& diastolicMmHg);
size_t sensorManagerReadWaveformBatch(WaveformSample* output, size_t capacity, uint32_t& firstSequence, uint32_t& firstTimestampUs);
