#pragma once
#include <Arduino.h>
void pumpControllerBegin();
bool pumpStart(uint32_t nowMs,uint32_t maxRunMs);
void pumpStop();
bool pumpTimedOut(uint32_t nowMs);
void valveDeenergize();
void valveEnergizeForInflation();
bool valveRequestVerifiedRelease();
