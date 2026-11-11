#include "pump_controller.h"
#include "config.h"
namespace {bool running=false;uint32_t startedAt=0,runLimitMs=PUMP_MAX_RUNTIME_MS;}
void pumpControllerBegin(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(PUMP_IN2_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);pinMode(PUMP_ENABLE_PIN,OUTPUT);pinMode(PUMP_IN1_PIN,OUTPUT);pinMode(PUMP_IN2_PIN,OUTPUT);pinMode(VALVE_ENABLE_PIN,OUTPUT);pinMode(VALVE_IN1_PIN,OUTPUT);pinMode(VALVE_IN2_PIN,OUTPUT);pumpStop();valveDeenergize();}
bool pumpStart(uint32_t now,uint32_t maxRunMs){
  if(running)return true;
  digitalWrite(PUMP_IN1_PIN,HIGH);digitalWrite(PUMP_IN2_PIN,LOW);digitalWrite(PUMP_ENABLE_PIN,HIGH);
  startedAt=now;runLimitMs=maxRunMs?(maxRunMs<PUMP_MAX_RUNTIME_MS?maxRunMs:PUMP_MAX_RUNTIME_MS):PUMP_MAX_RUNTIME_MS;running=true;Serial.printf("Pump start commanded (limit=%lu ms): IN1/GPIO%u=%d IN2/GPIO%u=%d EN/GPIO%u=%d\n",(unsigned long)runLimitMs,PUMP_IN1_PIN,digitalRead(PUMP_IN1_PIN),PUMP_IN2_PIN,digitalRead(PUMP_IN2_PIN),PUMP_ENABLE_PIN,digitalRead(PUMP_ENABLE_PIN));return true;
}
void pumpStop(){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(PUMP_IN2_PIN,LOW);if(running)Serial.println("Pump stopped");running=false;}
bool pumpTimedOut(uint32_t now){if(running&&(uint32_t)(now-startedAt)>=runLimitMs){pumpStop();return true;}return false;}
void valveDeenergize(){digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);}
void valveEnergizeForInflation(){digitalWrite(VALVE_IN1_PIN,HIGH);digitalWrite(VALVE_IN2_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,HIGH);}
bool valveRequestVerifiedRelease(){
  // The normally-open valve releases when its coil is deenergized.
  valveDeenergize();return true;
}
