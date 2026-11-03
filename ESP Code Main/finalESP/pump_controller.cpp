#include "pump_controller.h"
#include "config.h"
namespace {bool running=false;uint32_t startedAt=0;}
void pumpControllerBegin(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);pinMode(PUMP_ENABLE_PIN,OUTPUT);pinMode(PUMP_IN1_PIN,OUTPUT);pinMode(VALVE_ENABLE_PIN,OUTPUT);pinMode(VALVE_IN1_PIN,OUTPUT);pinMode(VALVE_IN2_PIN,OUTPUT);/* GPIO4 stays input for MIC_PIN; do not configure conflicting pump IN2. */pumpStop();valveDeenergize();}
bool pumpStart(uint32_t){ // Hard interlock: never activate until all safety evidence exists.
  pumpStop();return false;
}
void pumpStop(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);/* PUMP_IN2_PIN aliases MIC_PIN; intentionally untouched. */running=false;}
bool pumpTimedOut(uint32_t now){if(running&&(uint32_t)(now-startedAt)>=PUMP_MAX_RUNTIME_MS){pumpStop();return true;}return false;}
void valveDeenergize(){digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);}
bool valveRequestVerifiedRelease(){
  // Coil direction and safe terminal pressure have not been established.
  // Never energize an unknown valve based on a guessed polarity/limit.
  valveDeenergize();return false;
}
