import { afterEach, describe, expect, it, vi } from 'vitest'
import { ApiError, backendApi } from './api'
import { isTelemetryStale, useAppStore } from './store'

const deviceId='22222222-2222-4222-8222-222222222222'
const session={id:'11111111-1111-4111-8111-111111111111',device_id:deviceId,lifecycle:'starting',created_at:'2026-01-01T00:00:00Z',updated_at:'2026-01-01T00:00:00Z',started_at:null,ended_at:null,termination_reason:null,reported_device_state:'idle',device_last_seen_at:null,device_state_fresh:true,latest_command:null,sample_count:0,acoustic_event_count:0,data_provenance:'device_live'}
const device={id:deviceId,device_identifier:'fixture-device',hardware_revision:null,firmware_version:null,processing_algorithm_version:null,mqtt_enrolled:true,online:true,reported_state:'idle',state_reported_at:null,last_seen_at:null,pressure_calibration_confirmed:false,pressure_calibration_version:null}

afterEach(()=>{
 vi.restoreAllMocks(); backendApi.setToken(undefined)
 useAppStore.setState({demo:false,state:'disconnected',connected:false,deviceOnline:false,backendAvailable:false,busy:false,devices:[],selectedDeviceId:undefined,remoteSession:undefined,samples:[],acousticEvents:[],sessionSync:'uncertain',startRequestId:undefined,error:undefined})
})

describe('remote session workflow and freshness',()=>{
 it('waits for device-confirmed Start and Stop before changing lifecycle state',async()=>{
  useAppStore.setState({demo:false,apiToken:'test-owner',selectedDeviceId:deviceId})
  vi.spyOn(backendApi,'health').mockResolvedValue({status:'ok',service:'test'})
  vi.spyOn(backendApi,'readiness').mockResolvedValue({status:'ready',database:{status:'available'}})
  vi.spyOn(backendApi,'getDevice').mockResolvedValue(device as never)
  vi.spyOn(backendApi,'createSession').mockResolvedValue(session as never)
  vi.spyOn(backendApi,'getSession').mockResolvedValueOnce({...session,lifecycle:'measuring',started_at:'2026-01-01T00:00:01Z'} as never).mockResolvedValueOnce({...session,lifecycle:'completed',ended_at:'2026-01-01T00:00:05Z'} as never)
  vi.spyOn(backendApi,'streamSessionEvents').mockImplementation((_id,_listener,signal)=>new Promise(resolve=>signal.addEventListener('abort',()=>resolve(),{once:true})))
  vi.spyOn(backendApi,'completeSession').mockResolvedValue({...session,lifecycle:'stopping'} as never)
  const startAttempt = useAppStore.getState().startFromHome()
  expect(await useAppStore.getState().startFromHome()).toBe(false)
  expect(await startAttempt).toBe(true)
  expect(useAppStore.getState().state).toBe('measuring')
  await useAppStore.getState().stop()
  expect(useAppStore.getState().state).toBe('completed')
  expect(backendApi.createSession).toHaveBeenCalledTimes(1)
  expect(backendApi.completeSession).toHaveBeenCalledTimes(1)
 })
 it('does not start from offline device status and never falls back to demo',async()=>{
  useAppStore.setState({demo:false,apiToken:'test-owner',selectedDeviceId:deviceId})
  vi.spyOn(backendApi,'health').mockResolvedValue({status:'ok',service:'test'})
  vi.spyOn(backendApi,'readiness').mockResolvedValue({status:'ready'})
  vi.spyOn(backendApi,'getDevice').mockResolvedValue({...device,online:false} as never)
  const create=vi.spyOn(backendApi,'createSession')
  expect(await useAppStore.getState().startFromHome()).toBe(false)
  expect(useAppStore.getState().demo).toBe(false)
  expect(create).not.toHaveBeenCalled()
 })
 it('marks server-received telemetry stale without substituting a value',()=>{
  expect(isTelemetryStale(undefined,50_000)).toBe(true)
  expect(isTelemetryStale(30_000,50_000)).toBe(true)
  expect(isTelemetryStale(40_000,50_000)).toBe(false)
 })
 it('keeps Stop uncertain when the backend cannot confirm the request',async()=>{
  useAppStore.setState({demo:false,state:'measuring',apiToken:'test-owner',remoteSession:{...session,lifecycle:'measuring'} as never,startedAt:Date.now()})
  vi.spyOn(backendApi,'completeSession').mockRejectedValue(new ApiError('Backend unavailable','network'))
  await useAppStore.getState().stop()
  expect(useAppStore.getState().state).toBe('uncertain')
  expect(useAppStore.getState().error).toContain('not confirmed')
 })
})
