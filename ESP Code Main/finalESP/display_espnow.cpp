#include "display_espnow.h"
#include "config.h"
#include "device_state.h"
#include "session_manager.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <cstring>
namespace {
constexpr uint8_t BROADCAST_MAC[6]={255,255,255,255,255,255};
struct __attribute__((packed)) LegacyPacket{uint32_t magic;uint16_t version,seq;char type[16];char payload[180];};
uint16_t txSequence=0,lastRxSequence=0;uint32_t rxWindow=0;bool haveRx=false;
uint8_t displayPeerMac[6]={0};bool displayPeerLearned=false;
bool acceptSequence(uint16_t seq){if(!haveRx){haveRx=true;lastRxSequence=seq;rxWindow=1;return true;}uint16_t forward=(uint16_t)(seq-lastRxSequence);if(!forward)return false;if(forward<0x8000){rxWindow=forward>=32?1U:(rxWindow<<forward)|1U;lastRxSequence=seq;return true;}uint16_t behind=(uint16_t)(lastRxSequence-seq);if(behind>=32)return false;uint32_t bit=1UL<<behind;if(rxWindow&bit)return false;rxWindow|=bit;return true;}
void send(const char* type,const char* payload){LegacyPacket p{};p.magic=ESP_NOW_MAGIC;p.version=ESP_NOW_PROTOCOL_VERSION;p.seq=++txSequence;strlcpy(p.type,type,sizeof(p.type));strlcpy(p.payload,payload,sizeof(p.payload));esp_now_send(BROADCAST_MAC,(const uint8_t*)&p,sizeof(p));}
void receive(const esp_now_recv_info_t* info,const uint8_t* data,int length){if(length!=(int)sizeof(LegacyPacket)||!info||!data)return;LegacyPacket packet{};memcpy(&packet,data,sizeof(packet));if(packet.magic!=ESP_NOW_MAGIC||packet.version!=ESP_NOW_PROTOCOL_VERSION)return;if(!memchr(packet.type,'\0',sizeof(packet.type))||!memchr(packet.payload,'\0',sizeof(packet.payload)))return;if(!packet.type[0])return;if(!displayPeerLearned){if(strcmp(packet.type,"PING")!=0)return;memcpy(displayPeerMac,info->src_addr,6);displayPeerLearned=true;}else if(memcmp(displayPeerMac,info->src_addr,6)!=0)return;if(!acceptSequence(packet.seq))return;
  if(strcmp(packet.type,"PING")==0){send("PONG","{\"protocol_version\":1}");return;}
  if(strcmp(packet.type,"COMMAND")!=0)return;
  StaticJsonDocument<192> json;if(deserializeJson(json,packet.payload))return;const char* name=json["cmd"]|"";ControllerCommand request{};request.origin=CommandOrigin::ESP_NOW_DISPLAY;request.displaySequence=packet.seq;
  if(!strcmp(name,"START"))request.kind=CommandKind::START;else if(!strcmp(name,"STOP"))request.kind=CommandKind::STOP;else if(!strcmp(name,"STATUS")||!strcmp(name,"STATUS_REQUEST"))request.kind=CommandKind::STATUS_REQUEST;else return;
  const char* requestId=json["request_id"]|"";if(validUuid(requestId))strlcpy(request.requestId,requestId,sizeof(request.requestId));
  const char* sid=json["session_id"]|"";if(validUuid(sid))strlcpy(request.sessionId,sid,sizeof(request.sessionId));
  if(xQueueSend(sessionCommandQueue(),&request,0)!=pdTRUE){CommandResult result{};result.origin=CommandOrigin::ESP_NOW_DISPLAY;result.displaySequence=packet.seq;strlcpy(result.status,"rejected",sizeof(result.status));strlcpy(result.reasonCode,"command_queue_full",sizeof(result.reasonCode));strlcpy(result.deviceState,"fault",sizeof(result.deviceState));char body[180];snprintf(body,sizeof(body),"{\"ack_sequence\":%u,\"result\":\"%s\",\"reason\":\"%s\",\"state\":\"%s\"}",result.displaySequence,result.status,result.reasonCode,result.deviceState);send("ACK",body);}
}
}
void displayEspNowBegin(){WiFi.mode(WIFI_STA);WiFi.setSleep(false);esp_wifi_set_channel(ESP_NOW_CHANNEL,WIFI_SECOND_CHAN_NONE);if(esp_now_init()!=ESP_OK)return;esp_now_register_recv_cb(receive);esp_now_peer_info_t peer{};memcpy(peer.peer_addr,BROADCAST_MAC,6);peer.channel=0;peer.ifidx=WIFI_IF_STA;peer.encrypt=false;esp_now_add_peer(&peer);}
void displayPublishStatus(const char* statusJson){send("MAIN_STATUS",statusJson);}
void displayPublishResult(const CommandResult& r){char body[180];snprintf(body,sizeof(body),"{\"ack_sequence\":%u,\"result\":\"%s\",\"reason\":\"%s\",\"state\":\"%s\"}",r.displaySequence,r.status,r.reasonCode,backendState(deviceStateSnapshot().state));send("ACK",body);}
