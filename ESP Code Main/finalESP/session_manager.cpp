#include "session_manager.h"
#include "device_state.h"
#include "safety_manager.h"
#include "pump_controller.h"
#include "display_espnow.h"
#include "config.h"
#include "ble_manager.h"
#include <freertos/queue.h>
#include <cstring>
namespace {QueueHandle_t commands=nullptr,results=nullptr;}
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
    if(cmd.kind==CommandKind::STATUS_REQUEST){strlcpy(result.status,"completed",sizeof(result.status));strlcpy(result.reasonCode,"status_reported",sizeof(result.reasonCode));}
    else if(cmd.kind==CommandKind::START){
      if(current.state==MeasurementState::FAULT||current.state==MeasurementState::EMERGENCY_STOP){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"device_faulted",sizeof(result.reasonCode));}
      else if(measurementActive(current.state)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"measurement_active",sizeof(result.reasonCode));}
      else if(cmd.origin==CommandOrigin::ESP_NOW_DISPLAY&&!validUuid(cmd.sessionId)){
        if(!validUuid(cmd.requestId)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"invalid_start_request_id",sizeof(result.reasonCode));}
        else {DeviceState snapshot=deviceStateSnapshot();if(validUuid(snapshot.startRequestId)&&strcmp(snapshot.startRequestId,cmd.requestId)!=0){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"start_request_pending",sizeof(result.reasonCode));}
          else {xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.startRequestId,cmd.requestId,sizeof(deviceState.startRequestId));xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"accepted",sizeof(result.status));strlcpy(result.reasonCode,"backend_session_requested",sizeof(result.reasonCode));strlcpy(result.deviceState,"idle",sizeof(result.deviceState));if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);else (void)xQueueSend(results,&result,0);return;}}
      }
      else {
        xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.state=MeasurementState::PRECHECK;xSemaphoreGive(deviceStateMutex);
        FaultCode fault=safetyPrecheck(millis());
        if(fault!=FaultCode::NONE){pumpStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=fault;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"safety_interlock_unverified",sizeof(result.reasonCode));}
        else if(current.pressureMmhg>=MAX_INFLATION_PRESSURE_MMHG){
          pumpStop();valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::PRESSURE_LIMIT;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"pressure_already_at_or_above_limit",sizeof(result.reasonCode));
        }
        else {
          // Keep the normally-open release valve energized closed during fill.
          valveEnergizeForInflation();
          if(pumpStart(millis())){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::NONE;deviceState.state=MeasurementState::INFLATING;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"accepted",sizeof(result.status));strlcpy(result.reasonCode,"inflation_started",sizeof(result.reasonCode));}
          else {pumpStop();valveRequestVerifiedRelease();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::INFLATION_TIMEOUT;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"pump_start_failed",sizeof(result.reasonCode));}
        }
      }
      if(validUuid(cmd.sessionId)){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.sessionId,cmd.sessionId,sizeof(deviceState.sessionId));deviceState.startRequestId[0]='\0';xSemaphoreGive(deviceStateMutex);}
    } else {
      if(!measurementActive(current.state)){strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"no_active_measurement",sizeof(result.reasonCode));DeviceState afterStop=deviceStateSnapshot();strlcpy(result.deviceState,backendState(afterStop.state),sizeof(result.deviceState));if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);else if(result.origin==CommandOrigin::BLE_PHONE)bleManagerPublishCommandResult(result);else (void)xQueueSend(results,&result,0);return;}
      FaultCode fault=safetyEmergencyStop();xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=fault;deviceState.state=MeasurementState::EMERGENCY_STOP;xSemaphoreGive(deviceStateMutex);
      strlcpy(result.status,fault==FaultCode::NONE?"completed":"rejected",sizeof(result.status));strlcpy(result.reasonCode,fault==FaultCode::NONE?"pump_stopped_valve_released":"release_failed",sizeof(result.reasonCode));
      if(validUuid(cmd.sessionId)){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);strlcpy(deviceState.sessionId,cmd.sessionId,sizeof(deviceState.sessionId));xSemaphoreGive(deviceStateMutex);}
    }
    DeviceState after=deviceStateSnapshot();strlcpy(result.deviceState,backendState(after.state),sizeof(result.deviceState));
    if(result.origin==CommandOrigin::ESP_NOW_DISPLAY)displayPublishResult(result);
    else if(result.origin==CommandOrigin::BLE_PHONE)bleManagerPublishCommandResult(result);
    else if(xQueueSend(results,&result,0)!=pdTRUE){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::COMMAND_QUEUE_FULL;deviceState.state=MeasurementState::FAULT;xSemaphoreGive(deviceStateMutex);}
}
