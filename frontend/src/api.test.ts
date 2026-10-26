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
})
