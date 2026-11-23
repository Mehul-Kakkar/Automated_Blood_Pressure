#include "ble_manager.h"
#include "ble_protocol.h"
#include "config.h"
#include "device_state.h"
#include "session_manager.h"
#include "sensor_manager.h"
#include "wifi_manager.h"
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <WiFi.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstring>

namespace {
constexpr uint32_t BLE_LIVE_SAMPLE_INTERVAL_MS=250;
constexpr uint32_t BLE_WAVEFORM_BATCH_INTERVAL_MS=20;
constexpr size_t BLE_WAVEFORM_BATCH_SAMPLES=42; // 12-byte header + 4-byte samples fits the negotiated MTU 185.
constexpr uint16_t BLE_GATT_SERVICE_HANDLES=40; // 1 service + 10 characteristics + 8 notification descriptors, with room to grow.
struct Request { char commandId[37]; char command[32]; };
BLEServer* server=nullptr;
BLECharacteristic *infoChar=nullptr,*stateChar=nullptr,*pressureChar=nullptr,*heartChar=nullptr;
BLECharacteristic *eventChar=nullptr,*progressChar=nullptr,*commandChar=nullptr,*responseChar=nullptr,*faultChar=nullptr;
QueueHandle_t requests=nullptr;
struct WifiRequest { char commandId[37]; char command[16]; char ssid[33]; char password[64]; };
QueueHandle_t wifiRequests=nullptr;
BLECharacteristic* wifiChar=nullptr;
bool wifiScanPending=false;
bool wifiScanStartPending=false;
char wifiScanCommandId[37]{};
uint8_t wifiScanAttempts=0;
uint32_t wifiScanStartAt=0;
uint32_t wifiGenerationReported=0;
wl_status_t lastWifiStatus=WL_IDLE_STATUS;
volatile bool connected=false;
volatile bool advertisingRestartPending=false;
volatile uint32_t advertisingRestartAt=0;
uint32_t lastUpdate=0;
uint32_t lastLiveSampleUpdate=0;
uint32_t lastWaveformBatchUpdate=0;
char recentIds[8][37]{};
uint8_t recentAt=0;

bool uuidText(const char* s){return s&&validUuid(s);}
bool seen(const char* id){for(const auto& item:recentIds)if(item[0]&&strcmp(item,id)==0)return true;strlcpy(recentIds[recentAt],id,sizeof(recentIds[recentAt]));recentAt=(recentAt+1)%8;return false;}
void notifyJson(BLECharacteristic* ch,JsonDocument& doc){if(!ch)return;char bytes[240];size_t n=serializeJson(doc,bytes,sizeof(bytes));if(n&&n<sizeof(bytes)){ch->setValue((uint8_t*)bytes,n);if(connected)ch->notify();}}
void response(const char* id,bool ok,const char* code){if(!responseChar)return;StaticJsonDocument<160>d;d["v"]=PhoneBle::VERSION;d["command_id"]=id;d["ok"]=ok;d["error_code"]=ok?nullptr:code;d["state"]=backendState(deviceStateSnapshot().state);notifyJson(responseChar,d);}
const char* stateName(MeasurementState s){switch(s){case MeasurementState::BOOT:return "boot";case MeasurementState::IDLE:return "idle";case MeasurementState::PRECHECK:return "precheck";case MeasurementState::INFLATING:return "inflating";case MeasurementState::MEASURING:return "measuring";case MeasurementState::DEFLATING:return "deflating";case MeasurementState::COMPLETED:return "completed";case MeasurementState::FAULT:return "fault";case MeasurementState::EMERGENCY_STOP:return "emergency_stop";}return "unknown";}
const char* faultName(FaultCode f){switch(f){case FaultCode::NONE:return "none";case FaultCode::PRESSURE_UNCALIBRATED:return "pressure_uncalibrated";case FaultCode::PINMAP_UNVERIFIED:return "pinmap_unverified";case FaultCode::VALVE_UNVERIFIED:return "valve_unverified";case FaultCode::SENSOR_DISCONNECTED:return "sensor_disconnected";case FaultCode::INFLATION_TIMEOUT:return "inflation_timeout";case FaultCode::PRESSURE_LIMIT:return "pressure_limit";case FaultCode::AUTO_INFLATION_DISABLED:return "automatic_inflation_disabled";case FaultCode::STOP_RELEASE_UNVERIFIED:return "stop_release_unverified";case FaultCode::COMMAND_QUEUE_FULL:return "command_queue_full";case FaultCode::ADC_SATURATION:return "adc_saturation";case FaultCode::IMPLAUSIBLE_PRESSURE:return "implausible_pressure";}return "unknown";}
class ServerCallbacks final:public BLEServerCallbacks{void onConnect(BLEServer*)override{connected=true;advertisingRestartPending=false;Serial.println("Phone BLE client connected.");}void onDisconnect(BLEServer*)override{connected=false;advertisingRestartAt=millis()+500;advertisingRestartPending=true;Serial.println("Phone BLE client disconnected; advertising restart scheduled.");}};
class CommandCallbacks final:public BLECharacteristicCallbacks{void onWrite(BLECharacteristic* ch)override{
  String value=ch->getValue();if(!value.length()||value.length()>128)return;
  StaticJsonDocument<192> doc;if(deserializeJson(doc,value.c_str(),value.length()))return;
  const char* id=doc["command_id"]|"";const char* name=doc["command"]|"";
  if(!uuidText(id)||!name)return;
  Request r{};if(strlen(name)>=sizeof(r.command))return;strlcpy(r.commandId,id,sizeof(r.commandId));
  if(doc["v"]!=PhoneBle::VERSION)strlcpy(r.command,"INVALID_PROTOCOL",sizeof(r.command));else strlcpy(r.command,name,sizeof(r.command));
  if(requests)xQueueSend(requests,&r,0);
}};
class WifiCallbacks final:public BLECharacteristicCallbacks{void onWrite(BLECharacteristic* ch)override{
  String value=ch->getValue();if(!value.length()||value.length()>192)return;
  StaticJsonDocument<256> doc;if(deserializeJson(doc,value.c_str(),value.length()))return;
  const char* id=doc["command_id"]|"";const char* name=doc["command"]|"";if(!uuidText(id)||!name||doc["v"]!=PhoneBle::VERSION)return;
  WifiRequest r{};if(strlen(name)>=sizeof(r.command))return;strlcpy(r.commandId,id,sizeof(r.commandId));strlcpy(r.command,name,sizeof(r.command));
  if(!strcmp(name,"SET_WIFI")){const char* ssid=doc["ssid"]|"";const char* password=doc["password"]|"";if(strlen(ssid)>32||strlen(password)>63)return;strlcpy(r.ssid,ssid,sizeof(r.ssid));strlcpy(r.password,password,sizeof(r.password));}
  if(wifiRequests)xQueueSend(wifiRequests,&r,0);
}};
BLECharacteristic* makeCharacteristic(BLEService* service,const char* uuid,uint32_t properties,bool notify=false){auto* ch=service->createCharacteristic(uuid,properties);if(notify)ch->addDescriptor(new BLE2902());return ch;}
void publishSnapshot(){static bool demoValuesReady=false,wasMeasurementActive=false;static float demoSystolic=90.0f,demoDiastolic=60.0f;DeviceState s=deviceStateSnapshot();const bool measurementRunning=measurementActive(s.state);if(!demoValuesReady||(measurementRunning&&!wasMeasurementActive)){demoSystolic=(float)random(85,96);demoDiastolic=(float)random(55,66);demoValuesReady=true;}wasMeasurementActive=measurementRunning;StaticJsonDocument<320> state;state["v"]=PhoneBle::VERSION;state["state"]=stateName(s.state);state["uptime_ms"]=s.uptimeMs;state["session_id"]=validUuid(s.sessionId)?s.sessionId:nullptr;state["fault"]=faultName(s.fault);state["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIRMED;float systolic=0,diastolic=0;const bool bpValid=PRESSURE_CALIBRATION_CONFIRMED&&sensorManagerBloodPressureEstimate(systolic,diastolic);state["bp_valid"]=bpValid;state["bp_demo"]=!bpValid;if(bpValid){state["sys_mmhg"]=systolic;state["dia_mmhg"]=diastolic;}else{state["sys_mmhg"]=demoSystolic;state["dia_mmhg"]=demoDiastolic;}notifyJson(stateChar,state);
  StaticJsonDocument<192> pressure;pressure["v"]=PhoneBle::VERSION;pressure["uptime_ms"]=s.pressureUpdatedAtMs;if(s.pressureAvailable)pressure["raw_counts"]=s.pressureRaw;else pressure["raw_counts"]=nullptr;if(s.pressureAvailable&&s.pressureMmhgAvailable&&isfinite(s.pressureMmhg)&&s.pressureMmhg>=0.0f&&s.pressureMmhg<=400.0f)pressure["mmhg"]=s.pressureMmhg;else pressure["mmhg"]=nullptr;pressure["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIRMED;pressure["valid"]=s.pressureAvailable&&s.pressureMmhgAvailable&&isfinite(s.pressureMmhg)&&s.pressureMmhg>=0.0f&&s.pressureMmhg<=400.0f;pressure["estimate_only"]=true;notifyJson(pressureChar,pressure);
  StaticJsonDocument<96> heart;heart["v"]=PhoneBle::VERSION;heart["bpm"]=nullptr;heart["valid"]=false;notifyJson(heartChar,heart);
  StaticJsonDocument<96> progress;progress["v"]=PhoneBle::VERSION;progress["percent"]=nullptr;progress["available"]=false;notifyJson(progressChar,progress);
  StaticJsonDocument<96> fault;fault["v"]=PhoneBle::VERSION;fault["code"]=faultName(s.fault);fault["state"]=stateName(s.state);notifyJson(faultChar,fault);
}
void publishLiveSample(){
  if(!connected||!eventChar)return;
  const DeviceState state=deviceStateSnapshot();
  if(state.state!=MeasurementState::INFLATING&&state.state!=MeasurementState::MEASURING&&state.state!=MeasurementState::DEFLATING)return;
  SensorSample sample{};if(!sensorManagerLatestSample(sample))return;
  StaticJsonDocument<384> live;live["v"]=PhoneBle::VERSION;live["device_timestamp_ms"]=sample.timestampMs;live["raw_adc"]=sample.microphoneRaw;live["filtered"]=sample.filtered;live["envelope"]=sample.envelope;live["noise_floor"]=sample.noiseFloor;live["threshold"]=sample.threshold;live["adc_baseline"]=sample.adcBaseline;live["sample_rate_hz"]=SENSOR_SAMPLE_RATE_HZ;const char* modes[]={"unknown","unpressurized_pump_stopped","pressurized_pump_stopped","active_deflation","pump_inflating"};live["calibration_mode"]=sample.calibrationMode<5?modes[sample.calibrationMode]:"unknown";live["calibrated"]=sample.calibrated;live["event_detected"]=sample.eventDetected;live["event_duration_ms"]=sample.eventDurationMs;live["event_strength"]=sample.eventStrength;live["adc_clipped"]=sample.adcClipped;
  const float samplePressure=sample.eventDetected?sample.eventPressureMmhg:sample.pressureMmhg;
  const bool pressureValid=sample.pressureReady&&isfinite(samplePressure)&&samplePressure>=0.0f&&samplePressure<=400.0f;
  live["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIRMED;live["pressure_valid"]=pressureValid;if(pressureValid)live["mmhg"]=samplePressure;
  notifyJson(eventChar,live);
}
void putU32le(uint8_t* output,uint32_t value){output[0]=(uint8_t)value;output[1]=(uint8_t)(value>>8);output[2]=(uint8_t)(value>>16);output[3]=(uint8_t)(value>>24);}
void putU16le(uint8_t* output,uint16_t value){output[0]=(uint8_t)value;output[1]=(uint8_t)(value>>8);}
void publishWaveformBatch(){
  if(!connected||!eventChar||!measurementActive(deviceStateSnapshot().state))return;
  WaveformSample samples[BLE_WAVEFORM_BATCH_SAMPLES]{};uint32_t firstSequence=0,firstTimestampUs=0;
  const size_t count=sensorManagerReadWaveformBatch(samples,BLE_WAVEFORM_BATCH_SAMPLES,firstSequence,firstTimestampUs);if(!count)return;
  uint8_t packet[12+BLE_WAVEFORM_BATCH_SAMPLES*4];packet[0]=2;packet[1]=(uint8_t)count;putU32le(packet+2,firstSequence);putU32le(packet+6,firstTimestampUs);putU16le(packet+10,samples[0].thresholdQ2);
  for(size_t i=0;i<count;i++){putU16le(packet+12+i*4,(uint16_t)samples[i].filteredQ4);putU16le(packet+14+i*4,samples[i].envelopeQ2);}
  // Reuse the existing microphone characteristic so already-paired browsers
  // keep the same GATT database and don't need a new service/characteristic grant.
  eventChar->setValue(packet,12+count*4);eventChar->notify();
}
}

void bleInitTask(void*){Serial.printf("BLE init: starting GATT server as '%s'\n",PhoneBle::DEVICE_NAME);BLEDevice::init(PhoneBle::DEVICE_NAME);BLEDevice::setMTU(185);server=BLEDevice::createServer();if(!server){Serial.println("BLE init failed: createServer returned null");vTaskDelete(nullptr);return;}server->setCallbacks(new ServerCallbacks());auto* service=server->createService(BLEUUID(String(PhoneBle::SERVICE_UUID)),BLE_GATT_SERVICE_HANDLES);if(!service){Serial.println("BLE init failed: createService returned null");vTaskDelete(nullptr);return;}
  auto* security=new BLESecurity();security->setAuthenticationMode(ESP_LE_AUTH_BOND);security->setCapability(ESP_IO_CAP_NONE);
  infoChar=makeCharacteristic(service,PhoneBle::DEVICE_INFO_UUID,BLECharacteristic::PROPERTY_READ);stateChar=makeCharacteristic(service,PhoneBle::STATE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);pressureChar=makeCharacteristic(service,PhoneBle::PRESSURE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);heartChar=makeCharacteristic(service,PhoneBle::HEART_RATE_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);eventChar=makeCharacteristic(service,PhoneBle::KOROTKOFF_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);progressChar=makeCharacteristic(service,PhoneBle::PROGRESS_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);commandChar=makeCharacteristic(service,PhoneBle::COMMAND_UUID,BLECharacteristic::PROPERTY_WRITE);responseChar=makeCharacteristic(service,PhoneBle::RESPONSE_UUID,BLECharacteristic::PROPERTY_NOTIFY,true);faultChar=makeCharacteristic(service,PhoneBle::FAULT_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY,true);wifiChar=makeCharacteristic(service,PhoneBle::WIFI_UUID,BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_NOTIFY,true);wifiChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);commandChar->setCallbacks(new CommandCallbacks());wifiChar->setCallbacks(new WifiCallbacks());
  StaticJsonDocument<128> info;info["v"]=PhoneBle::VERSION;info["name"]=PhoneBle::DEVICE_NAME;info["firmware"]=FIRMWARE_VERSION;info["role"]="measurement_controller";char b[160];size_t n=serializeJson(info,b,sizeof(b));infoChar->setValue((uint8_t*)b,n);if(!service->start()){Serial.printf("BLE init failed: GATT service start failed (UUID=%s, handles=%u)\n",PhoneBle::SERVICE_UUID,BLE_GATT_SERVICE_HANDLES);vTaskDelete(nullptr);return;}auto* advertising=server->getAdvertising();advertising->addServiceUUID(PhoneBle::SERVICE_UUID);advertising->setScanResponse(true);BLEAdvertisementData scanResponse;scanResponse.setName(PhoneBle::DEVICE_NAME);bool scanResponseReady=advertising->setScanResponseData(scanResponse);bool advertisingStarted=advertising->start();if(scanResponseReady&&advertisingStarted)Serial.printf("Phone BLE advertising started: name='%s', service=%s, handles=%u\n",PhoneBle::DEVICE_NAME,PhoneBle::SERVICE_UUID,BLE_GATT_SERVICE_HANDLES);else Serial.printf("Phone BLE advertising failed: scan_response=%s start=%s\n",scanResponseReady?"ok":"failed",advertisingStarted?"ok":"failed");vTaskDelete(nullptr);
}
void bleManagerBegin(){requests=xQueueCreate(COMMAND_QUEUE_CAPACITY,sizeof(Request));wifiRequests=xQueueCreate(4,sizeof(WifiRequest));if(!requests||!wifiRequests)return;if(xTaskCreatePinnedToCore(bleInitTask,"phone-ble-init",8192,nullptr,1,nullptr,0)!=pdPASS)Serial.println("Phone BLE initialization task could not start; measurement remains available.");}
void bleManagerPoll(){WifiRequest wr{};while(wifiRequests&&xQueueReceive(wifiRequests,&wr,0)==pdTRUE){if(seen(wr.commandId)){response(wr.commandId,false,"duplicate_command");continue;}if(measurementActive(deviceStateSnapshot().state)){response(wr.commandId,false,"measurement_active");continue;}
    if(!strcmp(wr.command,"SCAN_WIFI")){if(wifiScanPending||wifiScanStartPending){response(wr.commandId,false,"scan_in_progress");continue;}StaticJsonDocument<96>d;d["type"]="scan_started";notifyJson(wifiChar,d);strlcpy(wifiScanCommandId,wr.commandId,sizeof(wifiScanCommandId));wifiScanAttempts=0;wifiScanStartPending=true;wifiScanStartAt=millis()+150;if(WiFi.status()!=WL_CONNECTED)WiFi.disconnect(false,false);continue;}
    if(!strcmp(wr.command,"SET_WIFI")){const bool saved=wifiManagerSetCredentials(wr.ssid,wr.password);response(wr.commandId,saved,saved?"ok":"invalid_or_storage_error");StaticJsonDocument<128>d;d["type"]="status";d["state"]=saved?"saved":"failed";d["message"]=saved?"Wi-Fi settings saved. Connecting…":"Wi-Fi settings could not be saved; check password length and storage.";d["ssid"]=saved?wr.ssid:"";notifyJson(wifiChar,d);continue;}
    response(wr.commandId,false,"unsupported_wifi_command");
  }
  if(wifiScanStartPending&&(int32_t)(millis()-wifiScanStartAt)>=0){WiFi.scanDelete();const int result=WiFi.scanNetworks(true,false);if(result==WIFI_SCAN_FAILED){if(wifiScanAttempts++==0){if(WiFi.status()!=WL_CONNECTED)WiFi.disconnect(false,false);wifiScanStartAt=millis()+300;}else{response(wifiScanCommandId,false,"scan_failed");wifiScanCommandId[0]=0;wifiScanStartPending=false;}}else{wifiScanStartPending=false;wifiScanPending=true;}}
  if(wifiScanPending){const int result=WiFi.scanComplete();if(result>=0){int sent=0;for(int i=0;i<result;i++){const String ssid=WiFi.SSID(i);if(!ssid.length())continue;StaticJsonDocument<128>d;d["type"]="network";d["ssid"]=ssid;d["rssi"]=WiFi.RSSI(i);d["secure"]=WiFi.encryptionType(i)!=WIFI_AUTH_OPEN;notifyJson(wifiChar,d);sent++;}StaticJsonDocument<96>d;d["type"]="scan_complete";d["count"]=sent;notifyJson(wifiChar,d);response(wifiScanCommandId,true,"ok");wifiScanCommandId[0]=0;wifiScanPending=false;WiFi.scanDelete();}
    else if(result==WIFI_SCAN_FAILED){wifiScanPending=false;WiFi.scanDelete();if(wifiScanAttempts++==0){if(WiFi.status()!=WL_CONNECTED)WiFi.disconnect(false,false);wifiScanStartPending=true;wifiScanStartAt=millis()+300;}else{response(wifiScanCommandId,false,"scan_failed");wifiScanCommandId[0]=0;}}}
  const wl_status_t currentWifiStatus=WiFi.status();if(currentWifiStatus!=lastWifiStatus){lastWifiStatus=currentWifiStatus;StaticJsonDocument<112>d;d["type"]="status";d["state"]=currentWifiStatus==WL_CONNECTED?"connected":"disconnected";d["message"]=currentWifiStatus==WL_CONNECTED?"Controller connected to Wi-Fi.":"Controller is not connected to Wi-Fi.";notifyJson(wifiChar,d);}
  Request r{};while(requests&&xQueueReceive(requests,&r,0)==pdTRUE){if(seen(r.commandId)){response(r.commandId,false,"duplicate_command");continue;}if(!strcmp(r.command,"INVALID_PROTOCOL")){response(r.commandId,false,"unsupported_protocol_version");continue;}if(!strcmp(r.command,"GET_DEVICE_INFO")){response(r.commandId,true,"ok");continue;}if(!strcmp(r.command,"GET_STATUS")||!strcmp(r.command,"REQUEST_LIVE_DATA")){publishSnapshot();response(r.commandId,true,"ok");continue;}
    ControllerCommand cmd{};cmd.origin=CommandOrigin::BLE_PHONE;strlcpy(cmd.commandId,r.commandId,sizeof(cmd.commandId));if(!strcmp(r.command,"START_MEASUREMENT"))cmd.kind=CommandKind::START;else if(!strcmp(r.command,"STOP_MEASUREMENT"))cmd.kind=CommandKind::STOP;else if(!strcmp(r.command,"RESET_FAULT"))cmd.kind=CommandKind::RESET_FAULT;else{response(r.commandId,false,"unsupported_command");continue;}
    if(!sessionManagerEnqueue(cmd))response(r.commandId,false,"command_queue_full");
  }
  if(advertisingRestartPending&&server&&(int32_t)(millis()-advertisingRestartAt)>=0){advertisingRestartPending=false;server->startAdvertising();Serial.println("Phone BLE advertising restarted.");}
  uint32_t now=millis();if(connected&&(uint32_t)(now-lastUpdate)>=500){lastUpdate=now;publishSnapshot();}if(connected&&(uint32_t)(now-lastLiveSampleUpdate)>=BLE_LIVE_SAMPLE_INTERVAL_MS){lastLiveSampleUpdate=now;publishLiveSample();}if(connected&&(uint32_t)(now-lastWaveformBatchUpdate)>=BLE_WAVEFORM_BATCH_INTERVAL_MS){lastWaveformBatchUpdate=now;publishWaveformBatch();}
}
void bleManagerPublishCommandResult(const CommandResult& result){if(result.origin!=CommandOrigin::BLE_PHONE)return;const bool accepted=!strcmp(result.status,"accepted")||!strcmp(result.status,"completed");response(result.commandId,accepted,result.reasonCode[0]?result.reasonCode:"command_rejected");publishSnapshot();}
