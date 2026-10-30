/*
 BP prototype connection test. No pump/valve GPIOs, no sensors, no Korotkoff detection.
 Libraries: NimBLE-Arduino by h2zero; ArduinoJson by Benoit Blanchon.
 Configure BACKEND_URL before flashing. ESP-NOW/display code intentionally removed for this test.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "BP-Controller";
static const char* FW_VERSION = "connection-test-0.1.0";
// Use laptop LAN IPv4, not localhost. Example: http://192.168.1.50:8000
static const char* BACKEND_URL = "http://10.231.140.174:8000";
static const char* SERVICE_UUID = "7b7e1000-8f4a-4b2d-9c61-6d52e4000000";
static const char* TELEMETRY_UUID = "7b7e1001-8f4a-4b2d-9c61-6d52e4000000";
static const char* COMMAND_UUID = "7b7e1002-8f4a-4b2d-9c61-6d52e4000000";
static const char* RESPONSE_UUID = "7b7e1003-8f4a-4b2d-9c61-6d52e4000000";
static const char* INFO_UUID = "7b7e1004-8f4a-4b2d-9c61-6d52e4000000";

Preferences prefs;
NimBLECharacteristic *telemetryChar, *responseChar;
bool bleConnected = false, wifiAttempting = false;
String savedSsid, savedPassword, mode = "setup", wifiState = "no_credentials";
String backendState = "not_checked", backendHealth = "unknown", backendReady = "unknown";
String lastMessage = "Booting", lastBackendError = "";
int lastHttpCode = 0;
uint32_t wifiStart = 0, lastWifiRetry = 0, lastBackendCheck = 0;
uint32_t lastStatusPush = 0, lastSerialPrint = 0;

String statusJson() {
  JsonDocument d;
  d["device_id"] = WiFi.macAddress(); d["device_name"] = DEVICE_NAME;
  d["firmware"] = FW_VERSION; d["mode"] = mode;
  d["wifi_state"] = wifiState; d["wifi_ssid"] = WiFi.isConnected()?WiFi.SSID():savedSsid;
  d["wifi_ip"] = WiFi.isConnected()?WiFi.localIP().toString():"";
  d["wifi_rssi"] = WiFi.isConnected()?WiFi.RSSI():0;
  d["backend_state"] = backendState; d["backend_health"] = backendHealth;
  d["backend_ready"] = backendReady; d["last_http_code"] = lastHttpCode;
  d["last_backend_error"] = lastBackendError; d["last_message"] = lastMessage;
  d["pump_control"] = "disabled"; d["valve_control"] = "disabled";
  d["korotkoff_detection"] = "disabled";
  String out; serializeJson(d,out); return out;
}
void bleReply(const String& s) {
  responseChar->setValue(s.c_str()); if (bleConnected) responseChar->notify();
  Serial.println("[BLE response] " + s);
}
void publishStatus() {
  String s=statusJson(); telemetryChar->setValue(s.c_str());
  if(bleConnected) telemetryChar->notify();
}
void enterSetup(const String& why) {
  mode="setup"; lastMessage="Setup mode: "+why; Serial.println(lastMessage); publishStatus();
}
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[Wi-Fi] Associated with access point; waiting for IP...");
      wifiState = "associated";
      lastMessage = "Associated; waiting for IP";
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiAttempting = false;
      wifiState = "connected";
      mode = "normal";
      lastMessage = "Wi-Fi connected; IP=" + WiFi.localIP().toString();
      Serial.printf("[Wi-Fi] Got IP: %s, RSSI=%d dBm\\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[Wi-Fi] Disconnected, reason=%d\\n",
                    info.wifi_sta_disconnected.reason);
      if (wifiAttempting) {
        wifiState = "connecting";
        lastMessage = "Wi-Fi disconnected during connection; reason=" +
                      String(info.wifi_sta_disconnected.reason);
      } else if (!savedSsid.isEmpty()) {
        wifiState = "disconnected";
        lastMessage = "Wi-Fi disconnected; reason=" +
                      String(info.wifi_sta_disconnected.reason);
      }
      break;
    default:
      break;
  }
}

void beginWiFi() {
  if(savedSsid.isEmpty()){wifiState="no_credentials";mode="setup";return;}
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  wifiAttempting=true; wifiStart=lastWifiRetry=millis();
  wifiState="connecting"; lastMessage="Connecting to Wi-Fi";
}
void clearWiFi() {
  prefs.begin("bp-device",false); prefs.remove("ssid"); prefs.remove("password"); prefs.end();
  savedSsid=""; savedPassword=""; WiFi.disconnect(true,true);
  wifiAttempting=false; wifiState="no_credentials"; mode="setup";
  backendState="not_checked"; lastMessage="Wi-Fi credentials cleared";
  bleReply("{\"ok\":true,\"event\":\"credentials_cleared\"}"); publishStatus();
}
void setWiFi(const String& ssid,const String& pass) {
  if(ssid.isEmpty()||ssid.length()>32||pass.length()>63){
    bleReply("{\"ok\":false,\"error\":\"invalid_ssid_or_password_length\"}");return;
  }
  prefs.begin("bp-device",false); prefs.putString("ssid",ssid); prefs.putString("password",pass); prefs.end();
  savedSsid=ssid; savedPassword=pass; mode="normal";
  WiFi.disconnect(false,false); wifiAttempting=false;
  backendState="not_checked"; backendHealth="unknown"; backendReady="unknown";
  lastBackendError=""; beginWiFi();
  bleReply("{\"ok\":true,\"event\":\"credentials_saved\",\"message\":\"Connecting to Wi-Fi\"}");
}
void commandJson(const String& raw) {
  JsonDocument d; if(deserializeJson(d,raw)){bleReply("{\"ok\":false,\"error\":\"invalid_json\"}");return;}
  String cmd=d["cmd"]|""; Serial.println("[CMD] "+cmd);
  if(cmd=="STATUS"){bleReply(statusJson());publishStatus();}
  else if(cmd=="PING") bleReply("{\"ok\":true,\"event\":\"pong\"}");
  else if(cmd=="ENTER_SETUP"){enterSetup("requested");bleReply("{\"ok\":true,\"event\":\"setup_mode\"}");}
  else if(cmd=="CANCEL_SETUP"){
    if(savedSsid.isEmpty()) bleReply("{\"ok\":false,\"error\":\"no_saved_wifi\"}");
    else {mode="normal";bleReply("{\"ok\":true,\"event\":\"normal_mode\"}");if(!WiFi.isConnected())beginWiFi();}
    publishStatus();
  } else if(cmd=="CLEAR_WIFI") clearWiFi();
  else if(cmd=="SET_WIFI") setWiFi(String((const char*)(d["ssid"]|"")),String((const char*)(d["password"]|"")));
  else if(cmd=="SCAN_WIFI"){
    bool wasConnected=WiFi.isConnected(); WiFi.mode(WIFI_STA);
    int n=WiFi.scanNetworks(false,true); JsonDocument r;
    r["ok"]=true;r["event"]="wifi_scan";JsonArray a=r["networks"].to<JsonArray>();
    for(int i=0;i<n&&i<20;i++){JsonObject o=a.add<JsonObject>();o["ssid"]=WiFi.SSID(i);o["rssi"]=WiFi.RSSI(i);o["secure"]=WiFi.encryptionType(i)!=WIFI_AUTH_OPEN;o["channel"]=WiFi.channel(i);}
    String out;serializeJson(r,out);bleReply(out);WiFi.scanDelete();
    if(!wasConnected&&!savedSsid.isEmpty())beginWiFi();
  } else bleReply("{\"ok\":false,\"error\":\"unknown_command\"}");
}
class CmdCallbacks: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string v=c->getValue(); if(v.size()>500){bleReply("{\"ok\":false,\"error\":\"too_large\"}");return;}
    if(!v.empty())commandJson(String(v.c_str()));
  }
};
class ServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*,NimBLEConnInfo&) override {bleConnected=true;publishStatus();bleReply(statusJson());}
  void onDisconnect(NimBLEServer*,NimBLEConnInfo&,int) override {bleConnected=false;NimBLEDevice::startAdvertising();}
};
void setupBLE() {
  NimBLEDevice::init(DEVICE_NAME); NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  auto* server=NimBLEDevice::createServer();server->setCallbacks(new ServerCallbacks());
  auto* service=server->createService(SERVICE_UUID);
  telemetryChar=service->createCharacteristic(TELEMETRY_UUID,NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
  responseChar=service->createCharacteristic(RESPONSE_UUID,NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
  auto* info=service->createCharacteristic(INFO_UUID,NIMBLE_PROPERTY::READ);
  auto* cmd=service->createCharacteristic(COMMAND_UUID,NIMBLE_PROPERTY::WRITE|NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new CmdCallbacks());
  JsonDocument d;d["name"]=DEVICE_NAME;d["firmware"]=FW_VERSION;d["device_mac"]=WiFi.macAddress();d["security"]="development_only";
  String s;serializeJson(d,s);info->setValue(s.c_str());
  telemetryChar->setValue(statusJson().c_str());responseChar->setValue("{\"ok\":true}");
  service->start();auto* adv=NimBLEDevice::getAdvertising();adv->addServiceUUID(SERVICE_UUID);adv->setName(DEVICE_NAME);adv->start();
  Serial.println("[BLE] Advertising as BP-Controller");
}
int getUrl(const String& url) {
  HTTPClient h;h.setConnectTimeout(2500);h.setTimeout(3000);
  if(!h.begin(url))return -1;h.addHeader("Accept","application/json");int code=h.GET();
  String body=code>0?h.getString():"";Serial.printf("[HTTP] GET %s -> %d\n",url.c_str(),code);
  if(body.length())Serial.println(body);h.end();return code;
}
void checkBackend() {
  if(!WiFi.isConnected()){backendState="waiting_for_wifi";return;}
  String base=BACKEND_URL;while(base.endsWith("/"))base.remove(base.length()-1);
  int h=getUrl(base+"/health"),r=getUrl(base+"/ready");lastHttpCode=r;
  backendHealth=(h>=200&&h<300)?"ok":"failed";backendReady=(r>=200&&r<300)?"ok":"failed";
  if(backendHealth=="ok"&&backendReady=="ok"){backendState="reachable";lastBackendError="";lastMessage="Backend endpoints reachable";}
  else {backendState="unreachable_or_error";lastBackendError="health="+String(h)+",ready="+String(r);lastMessage="Backend check failed";}
}
void setup() {
  Serial.begin(115200);delay(300);
  Serial.println("\nBP connection test. Actuators/sensors intentionally unused.");
  WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);
  prefs.begin("bp-device",true);savedSsid=prefs.getString("ssid","");savedPassword=prefs.getString("password","");prefs.end();
  if(savedSsid.isEmpty()){mode="setup";wifiState="no_credentials";}else{mode="normal";wifiState="connecting";}
  setupBLE();
  Serial.printf("[Wi-Fi] STA MAC %s\n",WiFi.macAddress().c_str());
  if(!savedSsid.isEmpty())beginWiFi();
  publishStatus();
  Serial.println("Serial commands: STATUS, SETUP, CLEAR_WIFI, PING");
}
void loop() {
  uint32_t now=millis();
  if(WiFi.isConnected()){wifiAttempting=false;wifiState="connected";}
  else if(wifiAttempting&&now-wifiStart>15000){wifiAttempting=false;wifiState="failed";mode="setup";lastMessage="Wi-Fi timeout; BLE setup remains available";}
  else if(!savedSsid.isEmpty()&&!wifiAttempting&&mode=="normal"&&now-lastWifiRetry>30000)beginWiFi();
  if(WiFi.isConnected()&&now-lastBackendCheck>15000){lastBackendCheck=now;checkBackend();}
  if(now-lastStatusPush>2000){lastStatusPush=now;publishStatus();}
  if(now-lastSerialPrint>5000){lastSerialPrint=now;Serial.println(statusJson());}
  if(Serial.available()){
    String s=Serial.readStringUntil('\n');s.trim();
    if(s.equalsIgnoreCase("STATUS"))Serial.println(statusJson());
    else if(s.equalsIgnoreCase("SETUP"))enterSetup("USB serial");
    else if(s.equalsIgnoreCase("CLEAR_WIFI"))clearWiFi();
    else if(s.equalsIgnoreCase("PING")){
      Serial.println("PONG");
      }
  }
  delay(10);
}
