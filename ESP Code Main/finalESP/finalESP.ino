#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "device_state.h"
#include "pump_controller.h"
#include "pressure_manager.h"
#include "sensor_manager.h"
#include "session_manager.h"
#include "display_espnow.h"
#include "mqtt_manager.h"
#include "safety_manager.h"
#include "ble_manager.h"

namespace { uint32_t lastDisplayStatus=0; }
static const char* faultName(FaultCode f){switch(f){case FaultCode::NONE:return "none";case FaultCode::PRESSURE_UNCALIBRATED:return "pressure_uncalibrated";case FaultCode::PINMAP_UNVERIFIED:return "pinmap_unverified";case FaultCode::VALVE_UNVERIFIED:return "valve_unverified";case FaultCode::SENSOR_DISCONNECTED:return "sensor_disconnected";case FaultCode::INFLATION_TIMEOUT:return "inflation_timeout";case FaultCode::PRESSURE_LIMIT:return "pressure_limit";case FaultCode::AUTO_INFLATION_DISABLED:return "automatic_inflation_disabled";case FaultCode::STOP_RELEASE_UNVERIFIED:return "stop_release_unverified";default:return "controller_fault";}}
static void publishLocalStatus(){DeviceState s=deviceStateSnapshot();StaticJsonDocument<220>d;d["state"]=backendState(s.state);d["uptime_ms"]=s.uptimeMs;if(pressureSensorConnected(millis())){d["pressure_raw"]=latestPressureRaw();d["pressure_mmhg"]=s.pressureMmhg;}else{d["pressure_raw"]=nullptr;d["pressure_mmhg"]=nullptr;}d["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIRMED;d["pressure_estimate_only"]=true;d["heart_rate_bpm"]=nullptr;d["fault"]=faultName(s.fault);char body[220];serializeJson(d,body,sizeof(body));displayPublishStatus(body);}

void setup(){
  Serial.begin(115200);
  // Latch actuator outputs low before configuring their GPIO directions.
  digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);
  deviceStateMutex=xSemaphoreCreateMutex();if(!deviceStateMutex){while(true){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);delay(100);}}
  pumpControllerBegin();pressureManagerBegin();sessionManagerBegin();sensorManagerBegin();
  xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::IDLE;deviceState.uptimeMs=millis();xSemaphoreGive(deviceStateMutex);
  displayEspNowBegin();
  bleManagerBegin();
  if(!sessionManagerReady()||!sensorManagerReady()){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=FaultCode::COMMAND_QUEUE_FULL;xSemaphoreGive(deviceStateMutex);Serial.println("Queue allocation failed; remote measurement commands remain disabled.");return;}
  mqttManagerBegin();
  Serial.println("IntegratedController safe mode: automatic inflation disabled; pressure is raw counts only.");
}

void loop(){
  // Sensor acquisition and control are local; MQTT runs in its own core/task.
  sensorManagerPoll();sessionManagerPoll();bleManagerPoll();const uint32_t now=millis();
  bool pressureConnected=pressureSensorConnected(now);xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.uptimeMs=now;deviceState.pressureAvailable=pressureConnected;deviceState.pressureMmhgAvailable=pressureConnected&&pressureMmHgAvailable();xSemaphoreGive(deviceStateMutex);
  DeviceState s=deviceStateSnapshot();
  if(measurementActive(s.state)&&!pressureSensorConnected(now)){
    FaultCode stopFault=safetyEmergencyStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=stopFault==FaultCode::NONE?FaultCode::SENSOR_DISCONNECTED:stopFault;xSemaphoreGive(deviceStateMutex);
  }
  if((uint32_t)(now-lastDisplayStatus)>=1000){lastDisplayStatus=now;publishLocalStatus();}
  delayMicroseconds(50);
}
