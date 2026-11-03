#pragma once
#include <Arduino.h>
#include <freertos/queue.h>

struct SensorSample {
  uint32_t timestampMs;
  uint16_t microphoneRaw;
  float filtered,envelope,noiseFloor,threshold;
  int32_t pressureRaw;
  bool eventDetected,pressureReady;
};
void sensorManagerBegin();
void sensorManagerPoll();
bool sensorManagerReady();
QueueHandle_t sensorSampleQueue();
uint32_t sensorDroppedCount();
