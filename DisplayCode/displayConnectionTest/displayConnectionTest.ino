/*
 BP prototype display ESP32 - ESP-NOW transport test only.
 No display UI in this sketch. Serial Monitor: 115200 baud.
 Commands: STATUS, SETUP, CANCEL, CLEAR_WIFI, PING.z
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
static const uint8_t CHANNEL=1;
static const uint32_t MAGIC=0x42505031;
struct __attribute__((packed)) Packet {
  uint32_t magic;uint16_t version;uint16_t seq;char type[16];char payload[180];
};
uint8_t MAIN_MAC[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint16_t seqNo=0;uint32_t lastPing=0;
String macString(const uint8_t*m){char b[18];snprintf(b,sizeof(b),"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);return String(b);}
void sendPacket(const char*t,const String& s){
  Packet p{};p.magic=MAGIC;p.version=1;p.seq=++seqNo;strlcpy(p.type,t,sizeof(p.type));strlcpy(p.payload,s.c_str(),sizeof(p.payload));
  Serial.printf("[TX] %s result=%d\n",t,(int)esp_now_send(MAIN_MAC,(uint8_t*)&p,sizeof(p)));
}
void command(const char*c){sendPacket("COMMAND",String("{\"cmd\":\"")+c+"\"}");}
void onReceive(const esp_now_recv_info_t* info,const uint8_t* data,int len){
  if(len!=(int)sizeof(Packet))return;Packet p{};memcpy(&p,data,sizeof(p));if(p.magic!=MAGIC||p.version!=1)return;
  Serial.printf("[RX] %s type=%s seq=%u\n",macString(info->src_addr).c_str(),p.type,p.seq);Serial.println(p.payload);
  if(MAIN_MAC[0]==0xFF){
    memcpy(MAIN_MAC,info->src_addr,6);Serial.printf("Learned main MAC: %s\n",macString(MAIN_MAC).c_str());
    esp_now_peer_info_t peer{};memcpy(peer.peer_addr,MAIN_MAC,6);peer.channel=CHANNEL;peer.ifidx=WIFI_IF_STA;peer.encrypt=false;
    Serial.printf("Add learned peer: %d\n",(int)esp_now_add_peer(&peer));
  }
}
void onSent(const wifi_tx_info_t*,esp_now_send_status_t s){Serial.printf("[TX status] %s\n",s==ESP_NOW_SEND_SUCCESS?"success":"fail");}
void setup(){
  Serial.begin(115200);delay(300);WiFi.mode(WIFI_STA);WiFi.disconnect();delay(100);
  Serial.printf("\nDisplay ESP-NOW test. STA MAC: %s\n",WiFi.macAddress().c_str());
  Serial.printf("Set channel %u: %d\n",CHANNEL,(int)esp_wifi_set_channel(CHANNEL,WIFI_SECOND_CHAN_NONE));
  if(esp_now_init()!=ESP_OK){Serial.println("ESP-NOW init failed");while(true)delay(1000);}
  esp_now_register_recv_cb(onReceive);esp_now_register_send_cb(onSent);
  esp_now_peer_info_t b{};memset(b.peer_addr,0xFF,6);b.channel=CHANNEL;b.ifidx=WIFI_IF_STA;b.encrypt=false;
  Serial.printf("Add broadcast peer: %d\n",(int)esp_now_add_peer(&b));
  Serial.println("Commands: STATUS, SETUP, CANCEL, CLEAR_WIFI, PING");sendPacket("PING","{\"source\":\"display\"}");
}
void loop(){
  if(Serial.available()){String s=Serial.readStringUntil('\n');s.trim();
    if(s.equalsIgnoreCase("STATUS"))command("STATUS");
    else if(s.equalsIgnoreCase("SETUP"))command("ENTER_SETUP");
    else if(s.equalsIgnoreCase("CANCEL"))command("CANCEL_SETUP");
    else if(s.equalsIgnoreCase("CLEAR_WIFI"))command("CLEAR_WIFI");
    else if(s.equalsIgnoreCase("PING"))sendPacket("PING","{\"source\":\"display\"}");
    else if(s.length())Serial.println("Unknown command");
  }
  if(millis()-lastPing>10000){lastPing=millis();sendPacket("PING","{\"source\":\"display\",\"periodic\":true}");}
  delay(10);
}
