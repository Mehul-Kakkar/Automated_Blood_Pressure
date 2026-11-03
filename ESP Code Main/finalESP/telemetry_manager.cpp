#include "telemetry_manager.h"
#include "config.h"
#include "sensor_manager.h"
#include "device_state.h"
#include "uuid_util.h"
#include <ArduinoJson.h>
#include <freertos/queue.h>
namespace {uint32_t lost=0;StaticJsonDocument<8192> doc;}
size_t telemetryBuildNextBatch(const char* deviceId,const char* sessionId,char* output,size_t capacity){
  if(!validUuid(deviceId)||!validUuid(sessionId)||!output||capacity<1024)return 0;
  SensorSample samples[MAX_TELEMETRY_BATCH_SAMPLES];size_t count=0;QueueHandle_t q=sensorSampleQueue();while(q&&count<MAX_TELEMETRY_BATCH_SAMPLES&&xQueueReceive(q,&samples[count],0)==pdTRUE)count++;if(!count)return 0;
  doc.clear();doc["schema_version"]="1";JsonObject payload=doc.createNestedObject("payload");char batchId[37];makeUuidV4(batchId);payload["batch_id"]=batchId;payload["device_id"]=deviceId;payload["session_id"]=sessionId;payload["schema_version"]="1";payload["firmware_version"]=FIRMWARE_VERSION;payload["processing_algorithm_version"]=PROCESSING_ALGORITHM_VERSION;payload["data_provenance"]="device_live";
  JsonArray records=payload.createNestedArray("samples"),events=payload.createNestedArray("acoustic_events");
  for(size_t i=0;i<count;i++){char sampleId[37];makeUuidV4(sampleId);uint32_t sequence; if(deviceStateMutex){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);sequence=++deviceState.sampleSequence;xSemaphoreGive(deviceStateMutex);}else sequence=(uint32_t)i+1;JsonObject s=records.createNestedObject();s["sample_id"]=sampleId;s["sample_sequence"]=sequence;s["device_timestamp_ms"]=samples[i].timestampMs;s["raw_adc"]=samples[i].microphoneRaw;s["filtered"]=samples[i].filtered;s["acoustic_envelope"]=samples[i].envelope;s["noise_floor"]=samples[i].noiseFloor;s["threshold"]=samples[i].threshold;s["event_detected"]=samples[i].eventDetected;s["is_valid"]=true;JsonArray flags=s.createNestedArray("quality_flags");flags.add("pressure_uncalibrated");if(!samples[i].pressureReady)flags.add("pressure_sample_unavailable");
    if(samples[i].eventDetected){char eventId[37];makeUuidV4(eventId);JsonObject e=events.createNestedObject();e["event_id"]=eventId;e["event_sequence"]=deviceState.sampleSequence;e["event_type"]="korotkoff_candidate";e["device_timestamp_ms"]=samples[i].timestampMs;e["strength"]=samples[i].envelope;e["is_valid"]=true;e.createNestedArray("quality_flags");e.createNestedObject("metadata")["pressure_calibrated"]=false;}
  }
  size_t bytes=serializeJson(doc,output,capacity);if(!bytes||bytes>=capacity){lost+=count;return 0;}return count;
}
void telemetryRecordPublishFailure(size_t n){lost+=(uint32_t)n;}
uint32_t telemetryLostSamples(){return lost+sensorDroppedCount();}
