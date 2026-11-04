#pragma once
#include <Arduino.h>
void pressureManagerBegin();
void pressureManagerRecord(bool ready,int32_t raw,uint32_t nowMs);
bool pressureSensorConnected(uint32_t nowMs);
bool pressureMmHgAvailable();
float pressureMmHgEstimate(int32_t rawCounts);
int32_t latestPressureRaw();
