#include "session_manager.h"
#include "pressure_manager.h"
#include "device_state.h"
#include "safety_manager.h"
#include "pump_controller.h"
#include "display_espnow.h"
#include "config.h"
#include "ble_manager.h"
#include <freertos/queue.h>
#include <cstring>
namespace {QueueHandle_t commands=nullptr,results=nullptr;}
static const char* faultReason(FaultCode f){switch(f){case FaultCode::PRESSURE_UNCALIBRATED:return "pressure_uncalibrated";case FaultCode::PINMAP_UNVERIFIED:return "pinmap_unverified";case FaultCode::VALVE_UNVERIFIED:return "valve_unverified";case FaultCode::SENSOR_DISCONNECTED:return "sensor_disconnected";case FaultCode::INFLATION_TIMEOUT:return "inflation_timeout";case FaultCode::PRESSURE_LIMIT:return "pressure_limit";case FaultCode::AUTO_INFLATION_DISABLED:return "automatic_inflation_disabled";case FaultCode::STOP_RELEASE_UNVERIFIED:return "stop_release_unverified";case FaultCode::COMMAND_QUEUE_FULL:return "command_queue_full";case FaultCode::ADC_SATURATION:return "adc_saturation";case FaultCode::IMPLAUSIBLE_PRESSURE:return "implausible_pressure";default:return "none";}}
QueueHandle_t sessionCommandQueue(){return commands;}
QueueHandle_t sessionResultQueue(){return results;}
void sessionManagerBegin(){commands=xQueueCreate(COMMAND_QUEUE_CAPACITY,sizeof(ControllerCommand));results=xQueueCreate(COMMAND_QUEUE_CAPACITY,sizeof(CommandResult));}
bool sessionManagerReady(){return commands!=nullptr&&results!=nullptr;}
bool sessionManagerEnqueue(const ControllerCommand& command){return commands&&xQueueSend(commands,&command,0)==pdTRUE;}
static void resultBase(const ControllerCommand& c,CommandResult& r){r={};r.origin=c.origin;r.displaySequence=c.displaySequence;strlcpy(r.commandId,c.commandId,sizeof(r.commandId));}
void sessionManagerPoll(){
    ControllerCommand cmd;
    if(xQueueReceive(commands,&cmd,0)!=pdTRUE)return;
    CommandResult result{};resultBase(cmd,result);
    DeviceState current=deviceStateSnapshot();
    if(cmd.kind==CommandKind::START)Serial.printf("START command received (origin=%u, state=%u)\n",(unsigned)cmd.origin,(unsigned)current.state);
    else if(cmd.kind==CommandKind::STOP)Serial.printf("STOP command received (origin=%u, state=%u)\n",(unsigned)cmd.origin,(unsigned)current.state);
    if(cmd.kind==CommandKind::STATUS_REQUEST){strlcpy(result.status,"completed",sizeof(result.status));strlcpy(result.reasonCode,"status_reported",sizeof(result.reasonCode));}
    else if(cmd.kind==CommandKind::RESET_FAULT){
      if(current.state!=MeasurementState::FAULT&&current.state!=MeasurementState::EMERGENCY_STOP){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"device_not_in_fault",sizeof(result.reasonCode));}
      else {
        pumpStop();const bool released=valveRequestVerifiedRelease();
        DeviceState safe=deviceStateSnapshot();
        const bool pressureSafe=pressureSensorConnected(millis())&&safe.pressureMmhgAvailable&&isfinite(safe.pressureMmhg)&&safe.pressureMmhg<=FAULT_RESET_SAFE_PRESSURE_MMHG;
        if(!released){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"release_failed",sizeof(result.reasonCode));}
        else if(!pressureSafe){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"pressure_not_safe_or_sensor_unavailable",sizeof(result.reasonCode));}
        else {xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::IDLE;deviceState.fault=FaultCode::NONE;deviceState.sessionId[0]='\0';deviceState.startRequestId[0]='\0';deviceState.timedPumpTest=false;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"accepted",sizeof(result.status));strlcpy(result.reasonCode,"fault_reset",sizeof(result.reasonCode));}
      }
    }
    else if(cmd.kind==CommandKind::START){
      if(current.state==MeasurementState::FAULT||current.state==MeasurementState::EMERGENCY_STOP){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,faultReason(current.fault),sizeof(result.reasonCode));}
      else if(measurementActive(current.state)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"measurement_active",sizeof(result.reasonCode));}
      else if(cmd.origin==CommandOrigin::ESP_NOW_DISPLAY&&!validUuid(cmd.sessionId)){
        if(!validUuid(cmd.requestId)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"invalid_start_request_id",sizeof(result.reasonCode));}
        else {DeviceState snapshot=deviceStateSnapshot();if(validUuid(snapshot.startRequestId)&&strcmp(snapshot.startRequestId,cmd.requestId)!=0){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"start_request_pending",sizeof(result.reasonCode));}
          else {xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.startRequestId,cmd.requestId,sizeof(deviceState.startRequestId));xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"accepted",sizeof(result.status));strlcpy(result.reasonCode,"backend_session_requested",sizeof(result.reasonCode));strlcpy(result.deviceState,"idle",sizeof(result.deviceState));if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);else (void)xQueueSend(results,&result,0);return;}}
      }
      else {
        xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::PRECHECK;xSemaphoreGive(deviceStateMutex);
        FaultCode fault=safetyPrecheck(millis(),TIMED_PUMP_TEST_MODE);
        if(fault!=FaultCode::NONE){pumpStop();valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=fault;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,faultReason(fault),sizeof(result.reasonCode));}
        else if(!TIMED_PUMP_TEST_MODE&&pressureSensorConnected(millis())&&current.pressureMmhg>=MAX_INFLATION_PRESSURE_MMHG){
          pumpStop();valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::PRESSURE_LIMIT;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"pressure_already_at_or_above_limit",sizeof(result.reasonCode));
        }
        else {
          // Keep the normally-open release valve energized closed during fill.
          valveEnergizeForInflation();
          const uint32_t startMs=millis();
          const uint32_t pumpLimitMs=TIMED_PUMP_TEST_MODE?TIMED_PUMP_TEST_DURATION_MS:PUMP_MAX_RUNTIME_MS;
          if(pumpStart(startMs,pumpLimitMs)){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::NONE;deviceState.state=MeasurementState::INFLATING;deviceState.timedPumpTest=TIMED_PUMP_TEST_MODE;deviceState.timedTestReleaseStartedAtMs=0;xSemaphoreGive(deviceStateMutex);if(TIMED_PUMP_TEST_MODE)Serial.printf("Timed acoustic test started: pump=%lu ms, capture-after-release=%lu ms; pressure feedback bypassed\n",(unsigned long)TIMED_PUMP_TEST_DURATION_MS,(unsigned long)TIMED_PUMP_TEST_CAPTURE_MS);strlcpy(result.status,"accepted",sizeof(result.status));strlcpy(result.reasonCode,TIMED_PUMP_TEST_MODE?"timed_acoustic_test_started":"inflation_started",sizeof(result.reasonCode));}
          else {pumpStop();valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::INFLATION_TIMEOUT;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"pump_start_failed",sizeof(result.reasonCode));}
        }
      }
      if(validUuid(cmd.sessionId)){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.sessionId,cmd.sessionId,sizeof(deviceState.sessionId));deviceState.startRequestId[0]='\0';xSemaphoreGive(deviceStateMutex);}
    } else {
      if(!measurementActive(current.state)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"no_active_measurement",sizeof(result.reasonCode));DeviceState afterStop=deviceStateSnapshot();strlcpy(result.deviceState,backendState(afterStop.state),sizeof(result.deviceState));if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);else if(result.origin==CommandOrigin::BLE_PHONE)bleManagerPublishCommandResult(result);else (void)xQueueSend(results,&result,0);return;}
      // A requested Stop is a normal release cycle, not an emergency stop.
      pumpStop();const FaultCode fault=valveRequestVerifiedRelease()?FaultCode::NONE:FaultCode::STOP_RELEASE_UNVERIFIED;
      xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=fault;deviceState.state=fault==FaultCode::NONE?MeasurementState::DEFLATING:MeasurementState::EMERGENCY_STOP;if(fault==FaultCode::NONE&&deviceState.timedPumpTest)deviceState.timedTestReleaseStartedAtMs=millis();xSemaphoreGive(deviceStateMutex);
      strlcpy(result.status,fault==FaultCode::NONE?"accepted":"rejected",sizeof(result.status));strlcpy(result.reasonCode,fault==FaultCode::NONE?"release_started":"release_failed",sizeof(result.reasonCode));
      if(validUuid(cmd.sessionId)){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.sessionId,cmd.sessionId,sizeof(deviceState.sessionId));xSemaphoreGive(deviceStateMutex);}
    }
    DeviceState after=deviceStateSnapshot();strlcpy(result.deviceState,backendState(after.state),sizeof(result.deviceState));
    if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);
    else if(result.origin==CommandOrigin::BLE_PHONE)bleManagerPublishCommandResult(result);
    else if(xQueueSend(results,&result,0)!=pdTRUE){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::COMMAND_QUEUE_FULL;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);}
}
