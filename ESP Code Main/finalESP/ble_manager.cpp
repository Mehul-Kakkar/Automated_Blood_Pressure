#include "ble_manager.h"
#include "ble_protocol.h"
#include "config.h"
#include "device_state.h"
#include "session_manager.h"
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstring>

namespace {
struct Request { char commandId[37]; char command[32]; };
BLEServer* server=nullptr;
BLECharacteristic *infoChar=nullptr,*stateChar=nullptr,*pressureChar=nullptr,*heartChar=nullptr;
BLECharacteristic *eventChar=nullptr,*progressChar=nullptr,*commandChar=nullptr,*responseChar=nullptr,*faultChar=nullptr;
QueueHandle_t requests=nullptr;
volatile bool connected=false;
volatile bool advertisingRestartPending=false;
uint32_t lastUpdate=0;
char recentIds[8][37]{};
uint8_t recentAt=0;

bool uuidText(const char* s){return s&&validUuid(s);}
bool seen(const char* id){for(const auto& item:recentIds)if(item[0]&&strcmp(item,id)==0)return true;strlcpy(recentIds[recentAt],id,sizeof(recentIds[recentAt]));recentAt=(recentAt+1)%8;return false;}
void notifyJson(BLECharacteristic* ch,JsonDocument& doc){if(!ch)return;char bytes[240];size_t n=serializeJson(doc,bytes,sizeof(bytes));if(n&&n<sizeof(bytes)){ch->setValue((uint8_t*)bytes,n);if(connected)ch->notify();}}
void response(const char* id,bool ok,const char* code){if(!responseChar)return;StaticJsonDocument<160>d;d["v"]=PhoneBle::VERSION;d["command_id"]=id;d["ok"]=ok;d["error_code"]=ok?nullptr:code;d["state"]=backendState(deviceStateSnapshot().state);notifyJson(responseChar,d);}
const char* stateName(MeasurementState s){switch(s){case MeasurementState::BOOT:return "boot";case MeasurementState::IDLE:return "idle";case MeasurementState::PRECHECK:return "precheck";case MeasurementState::INFLATING:return "inflating";case MeasurementState::MEASURING:return "measuring";case MeasurementState::DEFLATING:return "deflating";case MeasurementState::COMPLETED:return "completed";case MeasurementState::FAULT:return "fault";case MeasurementState::EMERGENCY_STOP:return "emergency_stop";}return "unknown";}
const char* faultName(FaultCode f){switch(f){case FaultCode::NONE:return "none";case FaultCode::PRESSURE_UNCALIBRATED:return "pressure_uncalibrated";case FaultCode::PINMAP_UNVERIFIED:return "pinmap_unverified";case FaultCode::VALVE_UNVERIFIED:return "valve_unverified";case FaultCode::SENSOR_DISCONNECTED:return "sensor_disconnected";case FaultCode::INFLATION_TIMEOUT:return "inflation_timeout";case FaultCode::PRESSURE_LIMIT:return "pressure_limit";case FaultCode::AUTO_INFLATION_DISABLED:return "automatic_inflation_disabled";case FaultCode::STOP_RELEASE_UNVERIFIED:return "stop_release_unverified";case FaultCode::COMMAND_QUEUE_FULL:return "command_queue_full";}return "unknown";}
class ServerCallbacks final:public BLEServerCallbacks{void onConnect(BLEServer*)override{connected=true;}void onDisconnect(BLEServer*)override{connected=false;advertisingRestartPending=true;}};
class CommandCallbacks final:public BLECharacteristicCallbacks{void onWrite(BLECharacteristic* ch)override{
  String value=ch->getValue();if(!value.length()||value.length()>128)return;
  StaticJsonDocument<192> doc;if(deserializeJson(doc,value.c_str(),value.length()))return;
  const char* id=doc["command_id"]|"";const char* name=doc["command"]|"";
  if(!uuidText(id)||!name)return;
  Request r{};if(strlen(name)>=sizeof(r.command))return;strlcpy(r.commandId,id,sizeof(r.commandId));
  if(doc["v"]!=PhoneBle::VERSION)strlcpy(r.command,"INVALID_PROTOCOL",sizeof(r.command));else strlcpy(r.command,name,sizeof(r.command));
  if(requests)xQueueSend(requests,&r,0);
}};
BLECharacteristic* makeCharacteristic(BLEService* service,const char* uuid,uint32_t properties,bool notify=false){auto* ch=service->createCharacteristic(uuid,properties);if(notify)ch->addDescriptor(new BLE2902());return ch;}
void publishSnapshot(){DeviceState s=deviceStateSnapshot();StaticJsonDocument<224> state;state["v"]=PhoneBle::VERSION;state["state"]=stateName(s.state);state["uptime_ms"]=s.uptimeMs;state["session_id"]=validUuid(s.sessionId)?s.sessionId:nullptr;state["fault"]=faultName(s.fault);state["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIRMED;notifyJson(stateChar,state);
  StaticJsonDocument<160> pressure;pressure["v"]=PhoneBle::VERSION;if(s.pressureAvailable)pressure["raw_counts"]=s.pressureRaw;else pressure["raw_counts"]=nullptr;if(s.pressureMmhgAvailable)pressure["mmhg"]=s.pressureMmhg;else pressure["mmhg"]=nullptr;pressure["valid"]=false;pressure["estimate_only"]=true;notifyJson(pressureChar,pressure);
  StaticJsonDocument<96> heart;heart["v"]=PhoneBle::VERSION;heart["bpm"]=nullptr;heart["valid"]=false;notifyJson(heartChar,heart);
  StaticJsonDocument<112> events;events["v"]=PhoneBle::VERSION;events["event_count"]=nullptr;events["available"]=false;notifyJson(eventChar,events);
  StaticJsonDocument<96> progress;progress["v"]=PhoneBle::VERSION;progress["percent"]=nullptr;progress["available"]=false;notifyJson(progressChar,progress);
  StaticJsonDocument<96> fault;fault["v"]=PhoneBle::VERSION;fault["code"]=faultName(s.fault);fault["state"]=stateName(s.state);notifyJson(faultChar,fault);
}
}

void bleInitTask(void*){Serial.printf("BLE init: starting GATT server as '%s'\n",PhoneBle::DEVICE_NAME);BLEDevice::init(PhoneBle::DEVICE_NAME);server=BLEDevice::createServer();if(!server){Serial.println("BLE init failed: createServer returned null");vTaskDelete(nullptr);return;}server->setCallbacks(new ServerCallbacks());auto* service=server->createService(PhoneBle::SERVICE_UUID);if(!service){Serial.println("BLE init failed: createService returned null");vTaskDelete(nullptr);return;}
  infoChar=makeCharacteristic(service,PhoneBle::DEVICE_INFO_UUID,BLECharacteristic::PROPERTY_READ);stateChar=makeCharacteristic(service,PhoneBle::STATE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);pressureChar=makeCharacteristic(service,PhoneBle::PRESSURE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);heartChar=makeCharacteristic(service,PhoneBle::HEART_RATE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);eventChar=makeCharacteristic(service,PhoneBle::KOROTKOFF_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);progressChar=makeCharacteristic(service,PhoneBle::PROGRESS_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);commandChar=makeCharacteristic(service,PhoneBle::COMMAND_UUID,BLECharacteristic::PROPERTY_WRITE);responseChar=makeCharacteristic(service,PhoneBle::RESPONSE_UUID,BLECharacteristic::PROPERTY_NOTIFY,true);faultChar=makeCharacteristic(service,PhoneBle::FAULT_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);commandChar->setCallbacks(new CommandCallbacks());
  StaticJsonDocument<128> info;info["v"]=PhoneBle::VERSION;info["name"]=PhoneBle::DEVICE_NAME;info["firmware"]=FIRMWARE_VERSION;info["role"]="measurement_controller";char b[160];size_t n=serializeJson(info,b,sizeof(b));infoChar->setValue((uint8_t*)b,n);service->start();auto* advertising=server->getAdvertising();advertising->addServiceUUID(PhoneBle::SERVICE_UUID);advertising->setScanResponse(true);BLEAdvertisementData scanResponse;scanResponse.setName(PhoneBle::DEVICE_NAME);bool scanResponseReady=advertising->setScanResponseData(scanResponse);bool advertisingStarted=advertising->start();if(scanResponseReady&&advertisingStarted)Serial.printf("Phone BLE advertising started: name='%s', service=%s\n",PhoneBle::DEVICE_NAME,PhoneBle::SERVICE_UUID);else Serial.printf("Phone BLE advertising failed: scan_response=%s start=%s\n",scanResponseReady?"ok":"failed",advertisingStarted?"ok":"failed");vTaskDelete(nullptr);
}
void bleManagerBegin(){requests=xQueueCreate(COMMAND_QUEUE_CAPACITY,sizeof(Request));if(!requests)return;if(xTaskCreatePinnedToCore(bleInitTask,"phone-ble-init",8192,nullptr,1,nullptr,0)!=pdPASS)Serial.println("Phone BLE initialization task could not start; measurement remains available.");}
void bleManagerPoll(){Request r{};while(requests&&xQueueReceive(requests,&r,0)==pdTRUE){if(seen(r.commandId)){response(r.commandId,false,"duplicate_command");continue;}if(!strcmp(r.command,"INVALID_PROTOCOL")){response(r.commandId,false,"unsupported_protocol_version");continue;}if(!strcmp(r.command,"GET_DEVICE_INFO")){response(r.commandId,true,"ok");continue;}if(!strcmp(r.command,"GET_STATUS")||!strcmp(r.command,"REQUEST_LIVE_DATA")){publishSnapshot();response(r.commandId,true,"ok");continue;}
    ControllerCommand cmd{};cmd.origin=CommandOrigin::BLE_PHONE;strlcpy(cmd.commandId,r.commandId,sizeof(cmd.commandId));if(!strcmp(r.command,"START_MEASUREMENT"))cmd.kind=CommandKind::START;else if(!strcmp(r.command,"STOP_MEASUREMENT"))cmd.kind=CommandKind::STOP;else{response(r.commandId,false,"unsupported_command");continue;}
    if(!sessionManagerEnqueue(cmd))response(r.commandId,false,"command_queue_full");
  }
  if(advertisingRestartPending&&server){advertisingRestartPending=false;server->startAdvertising();}
  uint32_t now=millis();if(connected&&(uint32_t)(now-lastUpdate)>=500){lastUpdate=now;publishSnapshot();}
}
void bleManagerPublishCommandResult(const CommandResult& result){if(result.origin!=CommandOrigin::BLE_PHONE)return;const bool accepted=!strcmp(result.status,"accepted")||!strcmp(result.status,"completed");response(result.commandId,accepted,result.reasonCode[0]?result.reasonCode:"command_rejected");publishSnapshot();}
