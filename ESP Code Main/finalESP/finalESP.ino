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
#include "wifi_manager.h"

namespace { uint32_t lastDisplayStatus=0,plateauStartedAtMs=0,lastPressureUpdateMs=0;float plateauReferenceIntegerMmhg=0.0f;uint8_t outsideBandCount=0;bool plateauTimerActive=false; }
static const char* faultName(FaultCode f){switch(f){case FaultCode::NONE:return "none";case FaultCode::PRESSURE_UNCALIBRATED:return "pressure_uncalibrated";case FaultCode::PINMAP_UNVERIFIED:return "pinmap_unverified";case FaultCode::VALVE_UNVERIFIED:return "valve_unverified";case FaultCode::SENSOR_DISCONNECTED:return "sensor_disconnected";case FaultCode::INFLATION_TIMEOUT:return "inflation_timeout";case FaultCode::PRESSURE_LIMIT:return "pressure_limit";case FaultCode::AUTO_INFLATION_DISABLED:return "automatic_inflation_disabled";case FaultCode::STOP_RELEASE_UNVERIFIED:return "stop_release_unverified";case FaultCode::ADC_SATURATION:return "adc_saturation";case FaultCode::IMPLAUSIBLE_PRESSURE:return "implausible_pressure";default:return "controller_fault";}}
static const char* displayStateName(MeasurementState s){switch(s){case MeasurementState::BOOT:return "boot";case MeasurementState::IDLE:return "idle";case MeasurementState::PRECHECK:return "initializing";case MeasurementState::INFLATING:return "inflating";case MeasurementState::MEASURING:return "measuring";case MeasurementState::DEFLATING:return "deflating";case MeasurementState::COMPLETED:return "completed";case MeasurementState::FAULT:return "error";case MeasurementState::EMERGENCY_STOP:return "emergency_stop";}return "unknown";}
static void publishLocalStatus(){static uint8_t demoSequence=0;DeviceState s=deviceStateSnapshot();StaticJsonDocument<220>d;d["state"]=displayStateName(s.state);d["uptime_ms"]=s.uptimeMs;const bool rawAvailable=pressureSensorConnected(millis())&&s.pressureAvailable;if(rawAvailable)d["pressure_raw"]=latestPressureRaw();else d["pressure_raw"]=nullptr;if(rawAvailable&&s.pressureMmhgAvailable&&isfinite(s.pressureMmhg))d["pressure_mmhg"]=s.pressureMmhg;else d["pressure_mmhg"]=nullptr;float systolic=0,diastolic=0;const bool bpValid=PRESSURE_CALIBRATION_CONFIRMED&&sensorManagerBloodPressureEstimate(systolic,diastolic);d["bp_valid"]=bpValid;d["bp_demo"]=!bpValid;if(bpValid){d["sys"]=systolic;d["dia"]=diastolic;}else{demoSequence++;d["sys"]=85+(demoSequence%11);d["dia"]=55+((demoSequence*7)%11);}d["fault"]=faultName(s.fault);char body[220];serializeJson(d,body,sizeof(body));displayPublishStatus(body);}

void setup(){
  Serial.begin(115200);
  // Latch actuator outputs low before configuring their GPIO directions.
  digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);digitalWrite(PUMP_IN1_PIN,LOW);digitalWrite(VALVE_IN1_PIN,LOW);digitalWrite(VALVE_IN2_PIN,LOW);
  deviceStateMutex=xSemaphoreCreateMutex();if(!deviceStateMutex){while(true){digitalWrite(PUMP_ENABLE_PIN,LOW);digitalWrite(VALVE_ENABLE_PIN,LOW);delay(100);}}
  pumpControllerBegin();pressureManagerBegin();sessionManagerBegin();sensorManagerBegin();
  xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::IDLE;deviceState.uptimeMs=millis();xSemaphoreGive(deviceStateMutex);
  displayEspNowBegin();
  wifiManagerBegin();
  bleManagerBegin();
  if(!sessionManagerReady()||!sensorManagerReady()){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=FaultCode::COMMAND_QUEUE_FULL;xSemaphoreGive(deviceStateMutex);Serial.println("Queue allocation failed; remote measurement commands remain disabled.");return;}
  mqttManagerBegin();
  Serial.println("IntegratedController: inflate to target, pump OFF with valve closed for passive backflow; open valve fully at 60 mmHg.");
}

void loop(){
  // Sensor acquisition and control are local; MQTT runs in its own core/task.
  sensorManagerPoll();sessionManagerPoll();bleManagerPoll();const uint32_t now=millis();
  bool pressureConnected=pressureSensorConnected(now);xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.uptimeMs=now;deviceState.pressureAvailable=pressureConnected;if(!pressureConnected)deviceState.pressureMmhgAvailable=false;xSemaphoreGive(deviceStateMutex);
  DeviceState s=deviceStateSnapshot();
  if(s.state!=MeasurementState::INFLATING){plateauTimerActive=false;outsideBandCount=0;}
  if(s.pressureUpdatedAtMs!=lastPressureUpdateMs){
    lastPressureUpdateMs=s.pressureUpdatedAtMs;
    float currentPressureMmhg=NAN;
    if(s.pressureMmhgAvailable&&isfinite(s.pressureMmhg))currentPressureMmhg=s.pressureMmhg;
    else if(s.pressureSaturated)currentPressureMmhg=PRESSURE_CALIBRATION_INTERCEPT_MMHG+PRESSURE_CALIBRATION_MMHG_PER_COUNT*(float)s.pressureRaw;
    if(s.state==MeasurementState::INFLATING&&isfinite(currentPressureMmhg)){
      const float currentIntegerMmhg=(float)(int32_t)currentPressureMmhg;
      if(!plateauTimerActive){plateauTimerActive=true;plateauStartedAtMs=now;plateauReferenceIntegerMmhg=currentIntegerMmhg;outsideBandCount=0;}
      else if(fabsf(currentIntegerMmhg-plateauReferenceIntegerMmhg)>PRESSURE_PLATEAU_TOLERANCE_MMHG){
        if(outsideBandCount<3)outsideBandCount++;
        if(outsideBandCount>=3){plateauStartedAtMs=now;plateauReferenceIntegerMmhg=currentIntegerMmhg;outsideBandCount=0;}
      } else outsideBandCount=0;
    } else if(!s.pressureSaturated){plateauTimerActive=false;outsideBandCount=0;}
  }
  // A sensor disconnect is recoverable once fresh HX710B conversions resume.
  // All actuator paths already stop the pump and release the normally-open valve.
  if(s.state==MeasurementState::FAULT&&s.fault==FaultCode::SENSOR_DISCONNECTED&&pressureConnected){
    xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::IDLE;deviceState.fault=FaultCode::NONE;xSemaphoreGive(deviceStateMutex);s=deviceStateSnapshot();
  }
  if(measurementActive(s.state)&&!pressureSensorConnected(now)&&!s.timedPumpTest){
    FaultCode stopFault=safetyEmergencyStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=stopFault==FaultCode::NONE?FaultCode::SENSOR_DISCONNECTED:stopFault;xSemaphoreGive(deviceStateMutex);
  } else if(s.state==MeasurementState::INFLATING&&s.fault==FaultCode::IMPLAUSIBLE_PRESSURE){
    FaultCode cause=s.fault;FaultCode releaseFault=safetyEmergencyStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=releaseFault==FaultCode::NONE?cause:releaseFault;xSemaphoreGive(deviceStateMutex);
  } else if(s.state==MeasurementState::INFLATING&&!s.timedPumpTest&&s.pressureMmhgAvailable&&s.pressureMmhg>=MAX_INFLATION_PRESSURE_MMHG){
    // Hard pressure limit: shut down and fail open immediately.
    const FaultCode releaseFault=safetyEmergencyStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::FAULT;deviceState.fault=releaseFault==FaultCode::NONE?FaultCode::PRESSURE_LIMIT:releaseFault;xSemaphoreGive(deviceStateMutex);
  } else if(s.state==MeasurementState::INFLATING&&!s.timedPumpTest&&plateauTimerActive&&(!s.pressureMmhgAvailable||s.pressureMmhg<MEASUREMENT_INFLATION_TARGET_MMHG)&&(uint32_t)(now-plateauStartedAtMs)>=PRESSURE_SATURATION_GRACE_MS){
    // A pressure that stays within the integer-mmHg tolerance band has
    // plateaued, even if small sensor oscillations avoid the ADC rail.
      pumpStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::MEASURING;deviceState.deflationStartedAtMs=now;xSemaphoreGive(deviceStateMutex);
      plateauTimerActive=false;
      Serial.println("Pressure stayed within the integer-mmHg plateau band for 3.5 s; pump OFF, continuing measurement.");
  } else if(s.state==MeasurementState::INFLATING&&!s.timedPumpTest&&s.pressureMmhgAvailable&&s.pressureMmhg>=MEASUREMENT_INFLATION_TARGET_MMHG){
    // The calibrated target ends inflation. Passive backflow is intentional:
    // pump OFF, normally-open exhaust held CLOSED.
    pumpStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::MEASURING;deviceState.deflationStartedAtMs=now;xSemaphoreGive(deviceStateMutex);
    Serial.printf("Inflation target reached at %.2f mmHg; pump OFF, exhaust valve CLOSED for passive backflow.\n",s.pressureMmhg);
  } else if(s.state==MeasurementState::MEASURING&&!s.timedPumpTest&&s.deflationStartedAtMs&&(uint32_t)(now-s.deflationStartedAtMs)>=MEASUREMENT_PASSIVE_BACKFLOW_HOLD_MS&&s.pressureMmhgAvailable&&s.pressureMmhg<=PASSIVE_DEFLATION_VALVE_OPEN_MMHG){
    // At/below 60 mmHg the exhaust valve is opened completely (de-energized).
    pumpStop();const bool released=valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=released?MeasurementState::DEFLATING:MeasurementState::FAULT;deviceState.fault=released?FaultCode::NONE:FaultCode::STOP_RELEASE_UNVERIFIED;xSemaphoreGive(deviceStateMutex);
    Serial.printf("Pressure reached %.2f mmHg; normally-open exhaust valve de-energized for full release.\n",s.pressureMmhg);
  } else if(s.state==MeasurementState::INFLATING&&pumpTimedOut(now)){
    const FaultCode releaseFault=valveRequestVerifiedRelease()?FaultCode::NONE:FaultCode::STOP_RELEASE_UNVERIFIED;xSemaphoreTake(deviceStateMutex,portMAX_DELAY);if(s.timedPumpTest&&releaseFault==FaultCode::NONE){deviceState.state=MeasurementState::DEFLATING;deviceState.fault=FaultCode::NONE;deviceState.timedTestReleaseStartedAtMs=now;}else{deviceState.state=MeasurementState::FAULT;deviceState.fault=releaseFault==FaultCode::NONE?FaultCode::INFLATION_TIMEOUT:releaseFault;deviceState.timedPumpTest=false;}xSemaphoreGive(deviceStateMutex);
  } else if(s.state==MeasurementState::DEFLATING&&s.timedPumpTest&&s.timedTestReleaseStartedAtMs&&(uint32_t)(now-s.timedTestReleaseStartedAtMs)>=TIMED_PUMP_TEST_CAPTURE_MS){
    xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::COMPLETED;deviceState.fault=FaultCode::NONE;deviceState.timedPumpTest=false;deviceState.timedTestReleaseStartedAtMs=0;xSemaphoreGive(deviceStateMutex);
  } else if(s.state==MeasurementState::DEFLATING&&!s.timedPumpTest&&s.pressureMmhgAvailable&&s.pressureMmhg<=MEASUREMENT_RELEASE_COMPLETE_MMHG){
    xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::COMPLETED;deviceState.fault=FaultCode::NONE;xSemaphoreGive(deviceStateMutex);
  }
  if((uint32_t)(now-lastDisplayStatus)>=1000){lastDisplayStatus=now;publishLocalStatus();}
  delayMicroseconds(50);
}
