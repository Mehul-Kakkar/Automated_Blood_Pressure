#pragma once
#include "device_state.h"
FaultCode safetyPrecheck(uint32_t nowMs,bool allowMissingPressure=false);
FaultCode safetyEmergencyStop();
