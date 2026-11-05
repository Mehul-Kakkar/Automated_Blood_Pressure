#include "pump_controller.h"
#include "config.h"
namespace {bool running=false;uint32_t startedAt=0;}
void pumpControllerBegin(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(PUMP_IN2_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);pinMode(PUMP_ENABLE_PIN,OUTPUT);pinMode(PUMP_IN1_PIN,OUTPUT);pinMode(PUMP_IN2_PIN,OUTPUT);pinMode(VALVE_ENABLE_PIN,OUTPUT);pinMode(VALVE_IN1_PIN,OUTPUT);pinMode(VALVE_IN2_PIN,OUTPUT);pumpStop();valveDeenergize();}
bool pumpStart(uint32_t now){
  if(running)return true;
  digitalWrite(PUMP_IN1_PIN,HIGH);digitalWrite(PUMP_IN2_PIN,LOW);digitalWrite(PUMP_ENABLE_PIN,HIGH);
  startedAt=now;running=true;return true;
}
void pumpStop(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(PUMP_IN2_PIN,LOW);running=false;}
bool pumpTimedOut(uint32_t now){if(running&&(uint32_t)(now-startedAt)>=PUMP_MAX_RUNTIME_MS){pumpStop();return true;}return false;}
void valveDeenergize(){digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);}
void valveEnergizeForInflation(){digitalWrite(VALVE_IN1_PIN,HIGH);digitalWrite(VALVE_IN2_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,HIGH);}
bool valveRequestVerifiedRelease(){
  // The normally-open valve releases when its coil is deenergized.
  valveDeenergize();return true;
}
