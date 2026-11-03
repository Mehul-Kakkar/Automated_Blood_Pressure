#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <cstring>

// Display board pin assignments and panel drivers are intentionally untouched:
// this transport sketch provides hooks for the existing touchscreen UI.
namespace {
constexpr uint32_t MAGIC=0x42505031;
constexpr uint16_t VERSION=1;
constexpr uint8_t CHANNEL=1;
constexpr uint8_t BROADCAST_MAC[6]={255,255,255,255,255,255};
constexpr uint32_t HEARTBEAT_MS=5000;
struct Packet{uint32_t magic;uint16_t version,seq;char type[16];char payload[180];} __attribute__((packed));
uint16_t txSequence=0;
uint16_t lastMainSequence=0;
uint32_t mainSequenceWindow=0;
bool haveMainSequence=false;
uint8_t mainPeerMac[6]={0};
bool mainPeerLearned=false;
uint32_t lastHeartbeat=0;
uint32_t lastMainAt=0;
bool mainSeen=false;
bool staleReported=false;
String mainState="unknown";
}

String uuid4(){uint32_t a=esp_random(),b=esp_random(),c=esp_random(),d=esp_random();char out[37];snprintf(out,sizeof(out),"%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",(unsigned long)a,(unsigned long)(b>>16),(unsigned long)(c&0xfff),(unsigned long)((d>>16&0x3fff)|0x8000),(unsigned long)d,(unsigned long)(c>>16));return String(out);}
void sendPacket(const char* type,const char* payload){Packet p{};p.magic=MAGIC;p.version=VERSION;p.seq=++txSequence;strlcpy(p.type,type,sizeof(p.type));strlcpy(p.payload,payload,sizeof(p.payload));esp_now_send(BROADCAST_MAC,(const uint8_t*)&p,sizeof(p));}
bool acceptMainSequence(uint16_t seq){if(!haveMainSequence){haveMainSequence=true;lastMainSequence=seq;mainSequenceWindow=1;return true;}uint16_t forward=(uint16_t)(seq-lastMainSequence);if(forward==0)return false;if(forward<0x8000){mainSequenceWindow=forward>=32?1U:(mainSequenceWindow<<forward)|1U;lastMainSequence=seq;return true;}uint16_t behind=(uint16_t)(lastMainSequence-seq);if(behind>=32)return false;uint32_t bit=1UL<<behind;if(mainSequenceWindow&bit)return false;mainSequenceWindow|=bit;return true;}

// Connect the actual touch UI's Start button to this function.
void displayRequestStart(){StaticJsonDocument<128>d;d["cmd"]="START";String req=uuid4();d["request_id"]=req;char body[180];serializeJson(d,body,sizeof(body));sendPacket("COMMAND",body);Serial.printf("START request sent: %s\n",req.c_str());}
// Connect the actual touch UI's Stop button to this function.
void displayRequestStop(){sendPacket("COMMAND","{\"cmd\":\"STOP\"}");Serial.println("STOP request sent; waiting for controller response");}
void displayRequestStatus(){sendPacket("COMMAND","{\"cmd\":\"STATUS_REQUEST\"}");}

void onReceive(const esp_now_recv_info_t* info,const uint8_t* data,int len){if(len!=(int)sizeof(Packet)||!info)return;Packet p{};memcpy(&p,data,sizeof(p));if(p.magic!=MAGIC||p.version!=VERSION)return;if(!mainPeerLearned){if(strcmp(p.type,"PONG")&&strcmp(p.type,"MAIN_STATUS"))return;memcpy(mainPeerMac,info->src_addr,6);mainPeerLearned=true;}else if(memcmp(mainPeerMac,info->src_addr,6)!=0)return;if(!acceptMainSequence(p.seq))return;mainSeen=true;lastMainAt=millis();staleReported=false;
  if(strcmp(p.type,"MAIN_STATUS")==0){StaticJsonDocument<192>d;if(!deserializeJson(d,p.payload)){mainState=d["state"]|"unknown";Serial.printf("Controller state=%s uptime_ms=%lu pressure_raw=%ld\n",mainState.c_str(),(unsigned long)(d["uptime_ms"]|0UL),(long)(d["pressure_raw"]|0L));}return;}
  if(strcmp(p.type,"ACK")==0){StaticJsonDocument<192>d;if(!deserializeJson(d,p.payload)){Serial.printf("ACK seq=%u result=%s reason=%s state=%s\n",(unsigned)(d["ack_sequence"]|0),(d["result"]|"unknown"),(d["reason"]|""),(d["state"]|"unknown"));}return;}
  Serial.printf("Controller message type=%s payload=%s\n",p.type,p.payload);
}

void setup(){Serial.begin(115200);WiFi.mode(WIFI_STA);WiFi.setSleep(false);esp_wifi_set_channel(CHANNEL,WIFI_SECOND_CHAN_NONE);Serial.printf("BP display transport, STA MAC=%s\n",WiFi.macAddress().c_str());if(esp_now_init()!=ESP_OK){Serial.println("ESP-NOW init failed");return;}esp_now_register_recv_cb(onReceive);esp_now_peer_info_t peer{};memcpy(peer.peer_addr,BROADCAST_MAC,6);peer.channel=CHANNEL;peer.ifidx=WIFI_IF_STA;peer.encrypt=false;esp_now_add_peer(&peer);Serial.println("Touch UI hooks: displayRequestStart(), displayRequestStop(), displayRequestStatus()");Serial.println("Serial commands: START, STOP, STATUS");sendPacket("PING","{\"source\":\"display\",\"protocol_version\":1}");}
void loop(){if(Serial.available()){String cmd=Serial.readStringUntil('\n');cmd.trim();if(cmd.equalsIgnoreCase("START"))displayRequestStart();else if(cmd.equalsIgnoreCase("STOP"))displayRequestStop();else if(cmd.equalsIgnoreCase("STATUS"))displayRequestStatus();else if(cmd.length())Serial.println("Use START, STOP, or STATUS");}if((uint32_t)(millis()-lastHeartbeat)>=HEARTBEAT_MS){lastHeartbeat=millis();sendPacket("PING","{\"source\":\"display\",\"protocol_version\":1}");}if(mainSeen&&!staleReported&&(uint32_t)(millis()-lastMainAt)>5000){staleReported=true;Serial.println("CONTROLLER STATUS UNCERTAIN: heartbeat/status timeout");}delay(5);}
