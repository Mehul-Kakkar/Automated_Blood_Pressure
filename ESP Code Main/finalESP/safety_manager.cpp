#include "safety_manager.h"
#include "config.h"
#include "pressure_manager.h"
#include "pump_controller.h"
FaultCode safetyPrecheck(uint32_t now){if(!ACTUATOR_PINMAP_CONFIRMED)return FaultCode::PINMAP_UNVERIFIED;if(!VALVE_RELEASE_BEHAVIOR_CONFIRMED)return FaultCode::VALVE_UNVERIFIED;if(!PRESSURE_CALIBRATION_CONFIRMED)return FaultCode::PRESSURE_UNCALIBRATED;if(!pressureSensorConnected(now))return FaultCode::SENSOR_DISCONNECTED;if(!AUTOMATIC_INFLATION_ENABLED)return FaultCode::AUTO_INFLATION_DISABLED;return FaultCode::NONE;}
FaultCode safetyEmergencyStop(){
  // This runs locally, independent of all radios/backend. It always cuts pump
  // power. Releasing cuff pressure is withheld until the verified valve action
  // and termination criteria are known; caller remains fault/uncertain.
  pumpStop();if(!valveRequestVerifiedRelease())return FaultCode::STOP_RELEASE_UNVERIFIED;return FaultCode::NONE;
}
