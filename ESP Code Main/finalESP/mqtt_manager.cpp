#include "mqtt_manager.h"
#include "config.h"
#include "device_state.h"
#include "command_protocol.h"
#include "session_manager.h"
#include "telemetry_manager.h"
#include "uuid_util.h"
#include "display_espnow.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ctype.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define BP_MQTT_CONFIGURED 1
#else
#define BP_MQTT_CONFIGURED 0
#endif

#if BP_MQTT_CONFIGURED
namespace {
WiFiClientSecure tls;
PubSubClient mqtt(tls);
char telemetryTopic[256],statusTopic[256],commandsTopic[256],ackTopic[256];
uint32_t statusSequence=0,lastStatus=0,lastWifiAttempt=0,lastMqttAttempt=0,lastSubscribeAttempt=0,seenWifiGeneration=0;
bool commandsSubscribed=false,loggedWaitingForTime=false;wl_status_t lastLoggedWifiStatus=WL_IDLE_STATUS;
char recentCommands[32][37]{};uint8_t recentCursor=0;

bool uuidValid(const char* s){return validUuid(s);}
int64_t daysFromCivil(int y,unsigned m,unsigned d){y-=m<=2;int era=(y>=0?y:y-399)/400;unsigned yoe=(unsigned)(y-era*400);unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;unsigned doe=yoe*365+yoe/4-yoe/100+doy;return (int64_t)era*146097+(int64_t)doe-719468;}
bool parseIsoEpoch(const char* s,int64_t& out){if(!s||strlen(s)<20)return false;int y,mo,d,h,m,sec;if(sscanf(s,"%4d-%2d-%2dT%2d:%2d:%2d",&y,&mo,&d,&h,&m,&sec)!=6||mo<1||mo>12||d<1||d>31||h>23||m>59||sec>60)return false;size_t i=19;while(s[i]&&s[i]!='Z'&&s[i]!='+'&&s[i]!='-')i++;int offset=0;if(s[i]=='+'||s[i]=='-'){int sign=s[i++]=='+'?1:-1;int oh=0,om=0;if(sscanf(s+i,"%2d:%2d",&oh,&om)!=2||oh>23||om>59)return false;offset=sign*(oh*3600+om*60);}else if(s[i]!='Z'&&s[i]!='\0')return false;out=daysFromCivil(y,(unsigned)mo,(unsigned)d)*86400+h*3600+m*60+sec-offset;return true;}
bool seen(const char* id){for(auto& item:recentCommands)if(item[0]&&!strcmp(item,id))return true;return false;}
void remember(const char* id){strlcpy(recentCommands[recentCursor],id,sizeof(recentCommands[recentCursor]));recentCursor=(uint8_t)((recentCursor+1)%32);}
void reportCommand(const char* id,const char* status,const char* reason,const char* reported){StaticJsonDocument<384>d;d["schema_version"]="1";char responseId[37];makeUuidV4(responseId);d["response_id"]=responseId;d["command_id"]=id;d["device_id"]=BP_DEVICE_ID;d["status"]=status;if(reason&&reason[0])d["reason_code"]=reason;d["device_state"]=reported;char body[384];size_t n=serializeJson(d,body,sizeof(body));if(n&&n<sizeof(body))mqtt.publish(ackTopic,body,false);}
void commandCallback(char* topic,uint8_t* bytes,unsigned int length){if(strcmp(topic,commandsTopic)||length>1024)return;StaticJsonDocument<768>d;if(deserializeJson(d,bytes,length))return;const char* schema=d["schema_version"]|"";const char* commandId=d["command_id"]|"";const char* deviceId=d["device_id"]|"";const char* command=d["command_type"]|"";if(strcmp(schema,"1")||!uuidValid(commandId)||strcmp(deviceId,BP_DEVICE_ID)||seen(commandId))return;remember(commandId);
  const char* issuedText=d["issued_at"]|"";const char* expiresText=d["expires_at"]|"";int64_t issued=0,expires=0,now=(int64_t)time(nullptr);if(now<1700000000||!parseIsoEpoch(issuedText,issued)||!parseIsoEpoch(expiresText,expires)){reportCommand(commandId,"rejected","device_time_unavailable",backendState(deviceStateSnapshot().state));return;}if(expires<=now||issued>now+60){reportCommand(commandId,"rejected","command_expired",backendState(deviceStateSnapshot().state));return;}
  ControllerCommand request{};request.origin=CommandOrigin::MQTT;strlcpy(request.commandId,commandId,sizeof(request.commandId));const char* session=d["session_id"]|"";if(!strcmp(command,"START")){if(!uuidValid(session)){reportCommand(commandId,"rejected","invalid_session_id",backendState(deviceStateSnapshot().state));return;}request.kind=CommandKind::START;strlcpy(request.sessionId,session,sizeof(request.sessionId));}else if(!strcmp(command,"STOP")){if(!uuidValid(session)){reportCommand(commandId,"rejected","invalid_session_id",backendState(deviceStateSnapshot().state));return;}request.kind=CommandKind::STOP;strlcpy(request.sessionId,session,sizeof(request.sessionId));}else if(!strcmp(command,"STATUS_REQUEST"))request.kind=CommandKind::STATUS_REQUEST;else{reportCommand(commandId,"rejected","unsupported_command",backendState(deviceStateSnapshot().state));return;}
  if(xQueueSend(sessionCommandQueue(),&request,0)!=pdTRUE)reportCommand(commandId,"rejected","command_queue_full","fault");
}
void publishDeviceStatus(){DeviceState s=deviceStateSnapshot();StaticJsonDocument<512>d;d["schema_version"]="1";char id[37];makeUuidV4(id);d["message_id"]=id;d["device_id"]=BP_DEVICE_ID;d["sequence"]=++statusSequence;d["state"]=backendState(s.state);d["device_uptime_ms"]=millis();d["firmware_version"]=FIRMWARE_VERSION;d["processing_algorithm_version"]=PROCESSING_ALGORITHM_VERSION;if(validUuid(s.sessionId))d["session_id"]=s.sessionId;if(validUuid(s.startRequestId)&&!validUuid(s.sessionId))d["start_request_id"]=s.startRequestId;d["lost_sample_count"]=s.lostSamples+telemetryLostSamples();char body[512];size_t n=serializeJson(d,body,sizeof(body));if(n&&n<sizeof(body))mqtt.publish(statusTopic,body,false);}
void publishAvailableTelemetry(){DeviceState s=deviceStateSnapshot();if(!validUuid(s.sessionId))return;static char body[8192];size_t count=telemetryBuildNextBatch(BP_DEVICE_ID,s.sessionId,body,sizeof(body));if(count&&!mqtt.publish(telemetryTopic,body,false))telemetryRecordPublishFailure(count);}
void mqttTask(void*){snprintf(telemetryTopic,sizeof(telemetryTopic),"bp/devices/%s/telemetry",BP_MQTT_DEVICE_KEY);snprintf(statusTopic,sizeof(statusTopic),"bp/devices/%s/status",BP_MQTT_DEVICE_KEY);snprintf(commandsTopic,sizeof(commandsTopic),"bp/devices/%s/commands",BP_MQTT_DEVICE_KEY);snprintf(ackTopic,sizeof(ackTopic),"bp/devices/%s/ack",BP_MQTT_DEVICE_KEY);WiFi.mode(WIFI_STA);WiFi.setSleep(false);Serial.println("MQTT network task started; Wi-Fi station mode enabled.");tls.setCACert(BP_BROKER_CA_CERT);mqtt.setServer(BP_MQTT_HOST,BP_MQTT_PORT);mqtt.setCallback(commandCallback);mqtt.setBufferSize(9000);mqtt.setKeepAlive(60);mqtt.setSocketTimeout(2);configTime(0,0,"pool.ntp.org","time.google.com");
  for(;;){uint32_t tick=millis();const wl_status_t wifiStatus=WiFi.status();if(wifiStatus!=lastLoggedWifiStatus){lastLoggedWifiStatus=wifiStatus;Serial.printf("ESP32 Wi-Fi link state changed: status=%d connected=%s.\n",(int)wifiStatus,wifiStatus==WL_CONNECTED?"yes":"no");}char wifiSsid[33]{},wifiPassword[64]{};const uint32_t generation=wifiManagerGeneration();const bool haveWifi=wifiManagerGetCredentials(wifiSsid,sizeof(wifiSsid),wifiPassword,sizeof(wifiPassword));if(generation!=seenWifiGeneration){seenWifiGeneration=generation;commandsSubscribed=false;mqtt.disconnect();WiFi.disconnect(false,false);lastWifiAttempt=0;}if(WiFi.status()!=WL_CONNECTED){commandsSubscribed=false;if(haveWifi&&(uint32_t)(tick-lastWifiAttempt)>=10000){lastWifiAttempt=tick;Serial.println("Starting ESP32 Wi-Fi connection using configured credentials.");WiFi.begin(wifiSsid,wifiPassword);}vTaskDelay(pdMS_TO_TICKS(50));continue;}if(time(nullptr)<1700000000){if(!loggedWaitingForTime){Serial.println("MQTT TLS waiting for network time synchronization before certificate validation.");loggedWaitingForTime=true;}vTaskDelay(pdMS_TO_TICKS(250));continue;}if(loggedWaitingForTime){Serial.println("Network time synchronized; MQTT TLS connection may start.");loggedWaitingForTime=false;}if(!mqtt.connected()){commandsSubscribed=false;if((uint32_t)(tick-lastMqttAttempt)>=5000){lastMqttAttempt=tick;char clientId[64];snprintf(clientId,sizeof(clientId),"bp-device-%s",BP_DEVICE_ID);const bool connectedToBroker=mqtt.connect(clientId,BP_MQTT_USERNAME,BP_MQTT_PASSWORD);Serial.printf("ESP32 MQTT broker connect %s; PubSubClient state=%d.\n",connectedToBroker?"succeeded":"failed",mqtt.state());}vTaskDelay(pdMS_TO_TICKS(20));continue;}mqtt.loop();if(!commandsSubscribed&&(uint32_t)(tick-lastSubscribeAttempt)>=5000){lastSubscribeAttempt=tick;commandsSubscribed=mqtt.subscribe(commandsTopic,1)==1;if(!commandsSubscribed)Serial.println("MQTT command subscription failed; retrying with bounded backoff.");}publishAvailableTelemetry();if((uint32_t)(tick-lastStatus)>=STATUS_INTERVAL_MS){lastStatus=tick;publishDeviceStatus();}CommandResult result;while(xQueueReceive(sessionResultQueue(),&result,0)==pdTRUE)reportCommand(result.commandId,result.status,result.reasonCode,result.deviceState);vTaskDelay(pdMS_TO_TICKS(2));}
}
}
#endif
void mqttManagerBegin(){
#if BP_MQTT_CONFIGURED
  if(!validUuid(BP_DEVICE_ID)||!BP_MQTT_DEVICE_KEY[0]||!BP_BROKER_CA_CERT[0]||!BP_MQTT_HOST[0]||!BP_MQTT_USERNAME[0]||!BP_MQTT_PASSWORD[0])return;
  xTaskCreatePinnedToCore(mqttTask,"bp-network",8192,nullptr,1,nullptr,0);
#endif
}
