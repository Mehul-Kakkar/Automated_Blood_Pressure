#pragma once
#include <Arduino.h>
size_t telemetryBuildNextBatch(const char* deviceId,const char* sessionId,char* output,size_t capacity);
void telemetryRecordPublishFailure(size_t sampleCount);
uint32_t telemetryLostSamples();
