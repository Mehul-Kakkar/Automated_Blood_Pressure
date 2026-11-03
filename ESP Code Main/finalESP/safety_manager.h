#pragma once
#include "device_state.h"
FaultCode safetyPrecheck(uint32_t nowMs);
FaultCode safetyEmergencyStop();
