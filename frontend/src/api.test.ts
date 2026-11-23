import { describe, expect, it } from 'vitest'
import { ApiError, BackendApi } from './api'

const response = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })
const validSession = { id:'11111111-1111-4111-8111-111111111111',device_id:'22222222-2222-4222-8222-222222222222',lifecycle:'starting',created_at:'2026-01-01T00:00:00Z',updated_at:'2026-01-01T00:00:00Z',started_at:null,ended_at:null,termination_reason:null,reported_device_state:'idle',device_last_seen_at:null,device_state_fresh:true,latest_command:null,sample_count:0,acoustic_event_count:0,data_provenance:'device_live' }

describe('typed backend API client', () => {
 it('sends owner authorization only in request headers and validates session responses', async () => {
  let seenInit:RequestInit|undefined
  const fetcher:typeof fetch=async (_url,init)=>{seenInit=init;return response(validSession,202)}
  const api=new BackendApi('https://backend.example.test',fetcher);api.setToken('owner-secret-test')
  const result=await api.createSession(validSession.device_id,'33333333-3333-4333-8333-333333333333')
  expect(result.lifecycle).toBe('starting')
  expect(new Headers(seenInit?.headers).get('Authorization')).toBe('Bearer owner-secret-test')
  expect(String(seenInit?.body)).not.toContain('owner-secret-test')
 })
 it('maps authorization and validation failures consistently', async () => {
  const unauthorized=new BackendApi('http://api.test',async()=>response({error:{code:'unauthorized',message:'Sign-in required',details:[]}},401))
  await expect(unauthorized.listSessions()).rejects.toMatchObject({kind:'authorization',status:401,message:'Sign-in required'})
  const invalid=new BackendApi('http://api.test',async()=>response({error:{code:'request_validation_error',message:'Invalid request',details:[]}},422))
  await expect(invalid.createSession('bad','bad')).rejects.toMatchObject({kind:'validation',status:422})
 })
 it('rejects malformed success payloads and reports backend configuration errors', async () => {
  const invalidResponse=new BackendApi('http://api.test',async()=>response({unexpected:'ok'}))
  await expect(invalidResponse.health()).rejects.toBeInstanceOf(ApiError)
  await expect(new BackendApi('',async()=>response({})).health()).rejects.toMatchObject({kind:'configuration'})
 })
 it('downloads PDF bytes using the owner token', async () => {
  let seenInit:RequestInit|undefined
  const fetcher:typeof fetch=async (_url,init)=>{seenInit=init;return new Response(new Uint8Array([37,80,68,70,45]),{status:200,headers:{'Content-Type':'application/pdf'}})}
  const api=new BackendApi('https://backend.example.test',fetcher);api.setToken('pdf-test-token')
  const blob=await api.downloadReport(validSession.id)
  expect(blob.type).toBe('application/pdf')
  expect(new Headers(seenInit?.headers).get('Authorization')).toBe('Bearer pdf-test-token')
 })
 it('encodes paginated session-history requests', async () => {
  let seenUrl='';const fetcher:typeof fetch=async(url)=>{seenUrl=String(url);return response({items:[],limit:10,offset:20,total:45})}
  const api=new BackendApi('http://api.test',fetcher)
  const page=await api.listSessions(10,20)
  expect(page).toMatchObject({limit:10,offset:20,total:45})
  expect(seenUrl).toContain('/api/v1/sessions?limit=10&offset=20')
 })
 it('reads and updates username and display name as separate profile fields', async () => {
  const profile={username:'device_owner',display_name:'Local Owner',created_at:'2026-01-01T00:00:00Z',avatar_available:false,password_enabled:true}
  const seen:RequestInit[]=[]
  const api=new BackendApi('http://api.test',async(_url,init)=>{seen.push(init??{});return response({...profile,...(init?.method==='PUT'?{display_name:'Updated Name'}:{})})})
  api.setToken('profile-owner-token')
  expect(await api.getProfile()).toMatchObject({username:'device_owner',display_name:'Local Owner'})
  expect(await api.updateProfile('Updated Name')).toMatchObject({username:'device_owner',display_name:'Updated Name'})
  expect(new Headers(seen[1].headers).get('Authorization')).toBe('Bearer profile-owner-token')
  expect(JSON.parse(String(seen[1].body))).toEqual({display_name:'Updated Name'})
 })
 it('registers and signs in through password endpoints without putting credentials in the token response body', async () => {
  const requests:Array<{url:string;body:unknown}> = []
  const fetcher:typeof fetch=async(url,init)=>{requests.push({url:String(url),body:init?.body?JSON.parse(String(init.body)):undefined});return response({access_token:'one-time-bearer',token_type:'bearer',expires_at:'2026-01-02T00:00:00Z',username:'device_owner',display_name:'Local Owner'})}
  const api=new BackendApi('https://backend.example.test',fetcher)
  expect((await api.register('Device_Owner','Local Owner','correct horse battery staple')).access_token).toBe('one-time-bearer')
  expect((await api.login('device_owner','correct horse battery staple')).username).toBe('device_owner')
  expect(requests.map(x=>x.url)).toEqual(['https://backend.example.test/api/v1/auth/register','https://backend.example.test/api/v1/auth/login'])
  expect(requests[0].body).toMatchObject({username:'Device_Owner',display_name:'Local Owner'})
 })
 it('sends logout once with the active bearer token', async () => {
  const requests:Array<{url:string;authorization:string|null}> = []
  const api=new BackendApi('https://backend.example.test',async(url,init)=>{requests.push({url:String(url),authorization:new Headers(init?.headers).get('Authorization')});return new Response(null,{status:204})})
  api.setToken('current-token')
  await api.logout()
  expect(requests).toEqual([
   {url:'https://backend.example.test/api/v1/auth/logout',authorization:'Bearer current-token'},
  ])
 })
 it('uses the issued login token to verify and load the account profile', async () => {
  const requests:Array<{path:string;authorization:string|null}> = []
  const auth={access_token:'session-token',token_type:'bearer',expires_at:'2027-01-02T00:00:00Z',username:'device_owner',display_name:'Local Owner'}
  const profile={username:'device_owner',display_name:'Local Owner',created_at:'2026-01-01T00:00:00Z',avatar_available:false,password_enabled:true}
  const api=new BackendApi('https://backend.example.test',async(url,init)=>{
   requests.push({path:new URL(String(url)).pathname,authorization:new Headers(init?.headers).get('Authorization')})
   return String(url).endsWith('/login')?response(auth):response(profile)
  })
  const result=await api.login('device_owner','correct horse battery staple')
  api.setToken(result.access_token)
  expect(await api.getProfile()).toMatchObject({username:'device_owner',display_name:'Local Owner'})
  expect(requests).toEqual([
   {path:'/api/v1/auth/login',authorization:null},
   {path:'/api/v1/profile',authorization:'Bearer session-token'},
  ])
 })
 it('enrolls an owner device and tracks the non-actuating status command acknowledgement', async () => {
  const deviceId='22222222-2222-4222-8222-222222222222'
  const commandId='44444444-4444-4444-8444-444444444444'
  const device={id:deviceId,device_identifier:'bp_device_001',hardware_revision:null,firmware_version:null,
   processing_algorithm_version:null,mqtt_enrolled:true,online:false,reported_state:null,state_reported_at:null,last_seen_at:null,
   pressure_calibration_confirmed:false,pressure_calibration_version:null}
  const command={command_id:commandId,command_type:'STATUS_REQUEST',status:'sent',issued_at:'2026-01-01T00:00:00Z',
   expires_at:'2026-01-01T00:00:30Z',response_code:null,response_state:null}
  const requests:Array<{path:string;method:string|undefined;body:unknown}> = []
  const api=new BackendApi('https://backend.example.test',async(url,init)=>{
   const parsedUrl=new URL(String(url));requests.push({path:parsedUrl.pathname,method:init?.method,body:init?.body?JSON.parse(String(init.body)):undefined})
   return parsedUrl.pathname.endsWith('/commands/'+commandId)?response({...command,status:'completed',response_state:'idle'}):
    parsedUrl.pathname.endsWith('/status-request')?response(command,202):response(device)
  })
  api.setToken('owner-token')
  expect((await api.enrollDeviceMqtt(deviceId,'bp_device_001')).mqtt_enrolled).toBe(true)
  expect((await api.requestDeviceStatus(deviceId)).status).toBe('sent')
  expect((await api.getDeviceCommand(deviceId,commandId)).status).toBe('completed')
  expect(requests).toEqual([
   {path:`/api/v1/devices/${deviceId}/mqtt-enrollment`,method:'POST',body:{mqtt_principal:'bp_device_001'}},
   {path:`/api/v1/devices/${deviceId}/status-request`,method:'POST',body:undefined},
   {path:`/api/v1/devices/${deviceId}/commands/${commandId}`,method:undefined,body:undefined},
  ])
 })
})
