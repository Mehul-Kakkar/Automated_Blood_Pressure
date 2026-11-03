#pragma once
#include <Arduino.h>
void pressureManagerBegin();
void pressureManagerRecord(bool ready,int32_t raw,uint32_t nowMs);
bool pressureSensorConnected(uint32_t nowMs);
bool pressureMmHgAvailable();
int32_t latestPressureRaw();
