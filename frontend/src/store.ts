import { create } from 'zustand'
import { ApiError, backendApi, type Device, type RemoteAcousticEvent, type RemoteSample, type RemoteSession } from './api'
import { DemoDevice, TelemetrySchema, type Session, type SessionState, type Telemetry } from './domain'
import { phoneBle, type PhoneBleEvent } from './ble'

type SyncState = 'not_applicable' | 'pending' | 'synced' | 'uncertain'
type Store = {
  demo: boolean; connected: boolean; bleConnected: boolean; deviceOnline: boolean; backendAvailable: boolean; databaseStatus: string
  state: SessionState; samples: Telemetry[]; sessions: Session[]; remoteSessions: RemoteSession[]
  acousticEvents: RemoteAcousticEvent[]; remoteSession?: RemoteSession; devices: Device[]; selectedDeviceId?: string
  startedAt?: number; endedAt?: number; busy: boolean; error?: string; apiToken?: string; sessionSync: SyncState
  startRequestId?: string
  bleState?: string; blePressureRaw?: number; blePressureMmHg?: number; blePressureSamples: Telemetry[]; bleMicSamples: Telemetry[]; activeBleSessionId?: string; bleFault?: string; bleError?: string; bleConnectionState: 'disconnected' | 'connecting' | 'connected' | 'reconnecting'
  setApiToken: (token?: string) => void; refreshBackend: () => Promise<void>; refreshDevices: () => Promise<void>
  selectDevice: (id: string) => void; loadHistory: (offset?: number) => Promise<void>; refreshRemoteSession: () => Promise<RemoteSession | undefined>
  connect: () => Promise<void>; startFromHome: () => Promise<boolean>; start: () => Promise<void>; stop: () => Promise<void>
  connectPhone: () => Promise<void>; disconnectPhone: () => void; sendPhoneCommand: (command: 'GET_STATUS' | 'START_MEASUREMENT' | 'STOP_MEASUREMENT' | 'RESET_FAULT' | 'REQUEST_LIVE_DATA' | 'GET_DEVICE_INFO') => Promise<void>; resetDeviceFault: () => Promise<void>
  resetSession: () => void; addSample: (sample: Telemetry) => void; clearError: () => void; setDemo: (value: boolean) => void
  simulateDisconnect: () => void; simulateFault: () => void; appendRemoteSamples: (samples: RemoteSample[], events: RemoteAcousticEvent[]) => void
}
const demoDevice = new DemoDevice()
const initialDemo = import.meta.env.DEV && import.meta.env.VITE_DEMO_MODE === 'true'
let unsubscribeDemo: (() => void) | undefined
let remoteAbort: AbortController | undefined
let liveSessionId: string | undefined
let lastSampleOffset = 0
const CONFIRM_TIMEOUT_MS = 20000
const POLL_INTERVAL_MS = 800
const MAX_BUFFERED_SAMPLES = 120
const TELEMETRY_STALE_MS = 15000
const BLE_SESSION_STORAGE_KEY = 'pulsefield.ble.sessions.v1'

function loadBleSessions(): Session[] {
  try {
    const value: unknown = JSON.parse(localStorage.getItem(BLE_SESSION_STORAGE_KEY) ?? '[]')
    if (!Array.isArray(value)) return []
    return value.filter((item): item is Session => item && item.source === 'ble' && typeof item.id === 'string' &&
      typeof item.startedAt === 'number' && typeof item.endedAt === 'number' &&
      ['completed', 'incomplete'].includes(item.state) && Array.isArray(item.samples) &&
      item.samples.every((sample: unknown) => TelemetrySchema.safeParse(sample).success)).slice(0, 8)
  } catch { return [] }
}

function persistBleSession(session: Session, existing: Session[]): Session[] {
  const sessions = [session, ...existing.filter(item => item.id !== session.id)].slice(0, 8)
  try { localStorage.setItem(BLE_SESSION_STORAGE_KEY, JSON.stringify(sessions)) } catch { /* keep this session available in memory */ }
  return sessions
}

function errorMessage(error: unknown, fallback: string): string {
  if (error instanceof ApiError) return error.message
  return error instanceof Error ? error.message : fallback
}
function sleep(ms: number) { return new Promise(resolve => setTimeout(resolve, ms)) }
function toTelemetry(row: RemoteSample): Telemetry {
  return {
    receivedAt: Date.parse(row.server_received_at),
    ...(row.device_timestamp_ms != null ? { deviceTimestampMs: row.device_timestamp_ms } : {}),
    ...(row.sequence != null ? { sequence: row.sequence } : {}),
    valid: row.is_valid,
    qualityFlags: row.quality_flags,
    ...(row.event_detected != null ? { eventDetected: row.event_detected } : {}),
    ...(row.is_valid && row.heart_rate_bpm != null ? { heartRateBpm: row.heart_rate_bpm } : {}),
    ...(row.is_valid && row.cuff_pressure_mmhg != null && row.pressure_calibration_version ? { cuffPressureMmHg: row.cuff_pressure_mmhg } : {}),
    ...(row.processed_signal.filtered != null ? { microphoneFiltered: row.processed_signal.filtered } : {}),
    ...(row.processed_signal.envelope != null ? { acousticEnvelope: row.processed_signal.envelope } : {}),
    deviceState: 'measuring',
  }
}

export const useAppStore = create<Store>((set, get) => ({
  demo: initialDemo, connected: false, bleConnected: false, deviceOnline: false, backendAvailable: false, databaseStatus: 'unknown',
  state: 'disconnected', samples: [], sessions: loadBleSessions(), remoteSessions: [], acousticEvents: [], devices: [], busy: false,
  sessionSync: initialDemo ? 'not_applicable' : 'uncertain',
  bleState: undefined, blePressureRaw: undefined, blePressureMmHg: undefined, blePressureSamples: [], bleMicSamples: [], bleFault: undefined, bleError: undefined, bleConnectionState: 'disconnected',
  setApiToken: token => { const trimmed = token?.trim(); backendApi.setToken(trimmed); set({ apiToken: trimmed || undefined, error: undefined }) },
  refreshBackend: async () => {
    try {
      await backendApi.health()
      set({ backendAvailable: true })
      try { const readiness = await backendApi.readiness(); set({ databaseStatus: readiness.database?.status ?? 'unknown' }) }
      catch (error) { set({ databaseStatus: error instanceof ApiError && error.status === 503 ? 'unavailable' : 'unknown' }) }
    } catch { set({ backendAvailable: false, databaseStatus: 'unknown', deviceOnline: false, connected: false }) }
  },
  refreshDevices: async () => {
    if (!get().apiToken) throw new ApiError('Add your owner API token in Settings to access registered devices.', 'authorization')
    const result = await backendApi.listDevices()
    const selected = result.items.find(d => d.id === get().selectedDeviceId) ?? result.items[0]
    const online = selected?.online === true
    set({ devices: result.items, selectedDeviceId: selected?.id, deviceOnline: online, connected: online,
      state: online && selected?.reported_state === 'idle' && get().state === 'disconnected' ? 'connected_idle' : get().state })
    if (selected) {
      const page = await backendApi.listSessions(50, 0)
      const active = page.items.find(item => item.device_id === selected.id && ['created', 'starting', 'measuring', 'stopping', 'interrupted'].includes(item.lifecycle))
      const known = get().remoteSession
      if (active) adoptRemoteSession(active, set, get)
      else if (known?.device_id === selected.id && ['completed', 'error', 'cancelled'].includes(known.lifecycle)) {
        set({ remoteSession: undefined, state: selected.reported_state === 'idle' ? 'connected_idle' : get().state })
      }
    }
  },
  connectPhone: async () => {
    set({ bleError: undefined, busy: true, bleConnectionState: 'connecting' })
    try {
      await phoneBle.connect((event: PhoneBleEvent) => {
        if (event.kind === 'connected') set({ bleConnected: true, bleConnectionState: 'connected', bleError: undefined })
        else if (event.kind === 'reconnecting') set({ bleConnected: false, bleConnectionState: 'reconnecting' })
        else if (event.kind === 'disconnected') set({ bleConnected: false, bleConnectionState: 'disconnected' })
        else if (event.kind === 'state') {
          const state = String(event.value.state ?? 'unknown')
          const fault = String(event.value.fault ?? 'none')
          const active = ['measuring', 'inflating', 'deflating'].includes(state)
          const wasActive = ['starting', 'measuring', 'stopping'].includes(get().state)
          set({ bleState: state, bleFault: fault,
            ...(state === 'measuring' || state === 'inflating' ? { state: 'measuring' as SessionState } : {}),
            ...(state === 'deflating' ? { state: (get().state === 'stopping' ? 'stopping' : 'measuring') as SessionState } : {}),
            ...(state === 'fault' || state === 'emergency_stop' ? { state: 'device_fault' as SessionState } : {}),
            ...(state === 'completed' && wasActive ? { state: 'completed' as SessionState, endedAt: Date.now() } : {}),
            ...(active && !get().startedAt ? { startedAt: Date.now() } : {}),
            ...(state === 'idle' && (get().state === 'disconnected' || (get().state === 'device_fault' && fault === 'none')) ? { state: 'connected_idle' as SessionState, startedAt: undefined, endedAt: undefined, activeBleSessionId: undefined } : {}) })
          if (state === 'completed' && wasActive && get().activeBleSessionId && get().startedAt) {
            const endedAt = Date.now(); const current = get()
            const session: Session = { id: current.activeBleSessionId!, startedAt: current.startedAt!, endedAt, state: 'completed', source: 'ble', samples: current.bleMicSamples.slice(-1200) }
            set({ endedAt, sessions: persistBleSession(session, current.sessions), activeBleSessionId: undefined })
          }
          if (state === 'fault' && wasActive && get().activeBleSessionId && get().startedAt) {
            const endedAt = Date.now(); const current = get()
            const session: Session = { id: current.activeBleSessionId!, startedAt: current.startedAt!, endedAt, state: 'incomplete', source: 'ble', samples: current.bleMicSamples.slice(-1200) }
            set({ endedAt, sessions: persistBleSession(session, current.sessions), activeBleSessionId: undefined })
          }
        } else if (event.kind === 'pressure') {
          const raw = typeof event.value.raw_counts === 'number' ? event.value.raw_counts : undefined
          const mmhg = typeof event.value.mmhg === 'number' ? event.value.mmhg : undefined
          const valid = event.value.valid === true && event.value.pressure_calibrated === true && mmhg !== undefined && Number.isFinite(mmhg) && mmhg >= 0 && mmhg <= 400
          set(s => ({ blePressureRaw: raw, blePressureMmHg: valid ? mmhg : undefined,
            blePressureSamples: valid && ['measuring', 'stopping'].includes(s.state)
              ? [...s.blePressureSamples.slice(-599), { receivedAt: Date.now(), cuffPressureMmHg: mmhg!, deviceState: 'measuring' }]
              : s.blePressureSamples }))
        } else if (event.kind === 'korotkoff') {
          const timestamp = event.value.device_timestamp_ms
          const envelope = event.value.envelope
          const raw = event.value.raw_adc
          const mmhg = event.value.mmhg
          const filtered = event.value.filtered
          if (typeof timestamp === 'number' && typeof envelope === 'number' && Number.isFinite(envelope) && typeof filtered === 'number' && Number.isFinite(filtered) && typeof raw === 'number') {
            const pressureValid = event.value.pressure_valid === true && typeof mmhg === 'number' && Number.isFinite(mmhg) && mmhg >= 0 && mmhg <= 400
            const sample: Telemetry = { receivedAt: Date.now(), deviceTimestampMs: timestamp, microphoneFiltered: filtered, acousticEnvelope: envelope, eventDetected: event.value.event_detected === true,
              ...(pressureValid ? { cuffPressureMmHg: mmhg as number } : {}), deviceState: 'measuring' }
            set(s => ({ bleMicSamples: [...s.bleMicSamples.slice(-1199), sample] }))
          }
        }
        else if (event.kind === 'fault') set({ bleFault: String(event.value.code ?? 'unknown') })
        else if (event.kind === 'error') set({ bleError: String(event.value.message ?? 'Device connection failed.') })
      })
      set({ bleConnected: true, bleConnectionState: 'connected' })
    } catch (error) {
      phoneBle.disconnect()
      set({ bleConnected: false, bleConnectionState: 'disconnected', bleError: errorMessage(error, 'Device connection failed.') })
      throw error
    } finally { set({ busy: false }) }
  },
  disconnectPhone: () => { phoneBle.disconnect(); set({ bleConnected: false, bleConnectionState: 'disconnected' }) },
  sendPhoneCommand: async command => {
    try { await phoneBle.send(command); set({ bleError: undefined }) }
    catch (error) { set({ bleError: errorMessage(error, 'Device command failed.') }); throw error }
  },
  resetDeviceFault: async () => {
    if (!get().bleConnected) throw new Error('Connect to the controller over BLE before resetting its fault.')
    set({ busy: true, error: undefined, bleError: undefined })
    try { await get().sendPhoneCommand('RESET_FAULT'); await get().sendPhoneCommand('GET_STATUS') }
    catch (error) { const message = errorMessage(error, 'The controller did not accept fault reset.'); set({ error: message, bleError: message }); throw error }
    finally { set({ busy: false }) }
  },
  selectDevice: id => { const device = get().devices.find(item => item.id === id); set({ selectedDeviceId: id, deviceOnline: device?.online === true, connected: device?.online === true, state: device?.online && device.reported_state === 'idle' ? 'connected_idle' : 'disconnected' }) },
  loadHistory: async (offset = 0) => {
    if (get().demo || !get().apiToken) return
    const page = await backendApi.listSessions(25, offset)
    set({ remoteSessions: offset ? [...get().remoteSessions, ...page.items] : page.items })
  },
  refreshRemoteSession: async () => {
    const id = get().remoteSession?.id
    if (!id) return undefined
    const session = await backendApi.getSession(id)
    set({ remoteSession: session })
    if (session.lifecycle === 'measuring') set({ state: 'measuring', sessionSync: 'synced', startedAt: session.started_at ? Date.parse(session.started_at) : get().startedAt })
    else if (session.lifecycle === 'completed') set({ state: 'completed', sessionSync: 'synced' })
    else if (session.lifecycle === 'error' || session.lifecycle === 'cancelled') set({ state: session.lifecycle === 'cancelled' ? 'cancelled' : 'error', sessionSync: 'synced' })
    else if (session.lifecycle === 'interrupted') set({ state: 'uncertain', sessionSync: 'uncertain' })
    return session
  },
  appendRemoteSamples: (rows, events) => {
    if (get().demo) return
    const mapped = rows.filter(row => row.data_provenance !== 'synthetic_test' && row.data_provenance !== 'demo').map(toTelemetry)
    set(s => ({ samples: [...s.samples, ...mapped].slice(-MAX_BUFFERED_SAMPLES), acousticEvents: [...s.acousticEvents, ...events].slice(-MAX_BUFFERED_SAMPLES) }))
  },
  connect: async () => {
    if (get().demo) {
      set({ busy: true, error: undefined, state: 'disconnected' })
      try { await demoDevice.connect(); const status = await demoDevice.reconcile(); set({ busy: false, connected: true, bleConnected: true, deviceOnline: true, state: status === 'measuring' ? 'measuring' : status==='fault'?'device_fault':'connected_idle' }) }
      catch (e) { set({ busy: false, state: 'error', error: errorMessage(e, 'Could not connect to the demo device.') }) }
      return
    }
    set({ busy: true, error: undefined })
    try {
      await get().refreshBackend()
      if (!get().backendAvailable) throw new ApiError('Backend is unavailable.', 'network')
      await get().refreshDevices()
      const selected = get().devices.find(d => d.id === get().selectedDeviceId)
      if (!selected?.online) throw new ApiError('The selected device is offline or has not been enrolled for MQTT.', 'response')
      const knownSession = get().remoteSession
      if (selected.reported_state === 'measuring' && knownSession?.device_id !== selected.id) {
        set({ connected: true, deviceOnline: true, state: 'uncertain', error: 'The device reports an active measurement. Select its existing session in History to reconcile; no session was started.' })
      } else set({ connected: true, deviceOnline: true,
        state: knownSession?.device_id === selected.id && ['created', 'starting', 'measuring', 'stopping', 'interrupted'].includes(knownSession.lifecycle)
          ? get().state : selected.reported_state === 'idle' ? 'connected_idle' : 'disconnected' })
    } catch (e) { set({ connected: false, deviceOnline: false, state: 'disconnected', error: errorMessage(e, 'Could not check backend/device status.') }) }
    finally { set({ busy: false }) }
  },
  startFromHome: async () => {
    const current = get()
    if (current.busy || current.state === 'measuring' || current.state === 'stopping' || current.state === 'starting') return false
    if (current.demo) {
      set({ busy: true, error: undefined, state: 'connecting' })
      try { if (!current.connected) await demoDevice.connect(); set({ busy: false, connected: true, bleConnected: true, state: 'connected_idle' }); await get().start(); return get().state === 'measuring' }
      catch (e) { set({ busy: false, connected: false, state: 'error', error: errorMessage(e, 'Could not connect to the demo device.') }); return false }
    }
    if (current.bleConnected && (!current.apiToken || !current.backendAvailable || !current.deviceOnline)) {
      set({ busy: true, error: undefined, startedAt: undefined, endedAt: undefined, blePressureSamples: [], bleMicSamples: [], activeBleSessionId: crypto.randomUUID() })
      try { await get().sendPhoneCommand('START_MEASUREMENT'); if (get().bleState !== 'fault' && get().bleState !== 'emergency_stop') set({ state: 'measuring', startedAt: get().startedAt ?? Date.now(), endedAt: undefined }); return get().bleState === 'measuring' || get().state === 'measuring' }
      catch (e) { set({ state: get().bleState === 'fault' ? 'device_fault' : 'error', activeBleSessionId: undefined, error: errorMessage(e, 'The device rejected Start over BLE.') }); return false }
      finally { set({ busy: false }) }
    }
    set({ busy: true, error: undefined, state: 'connecting', sessionSync: 'pending', samples: [], acousticEvents: [], remoteSession: undefined })
    let commandAttempted = false
    try {
      if (!get().apiToken) throw new ApiError('Add your owner API token in Settings before starting a remote session.', 'authorization')
      await get().refreshBackend()
      if (!get().backendAvailable) throw new ApiError('Backend is unavailable. No session was started.', 'network')
      let deviceId = get().selectedDeviceId
      if (!deviceId) {
        const registered = await backendApi.listDevices()
        deviceId = registered.items[0]?.id
        if (deviceId) set({ devices: registered.items, selectedDeviceId: deviceId })
      }
      if (!deviceId) throw new ApiError('Register or select an enrolled device in the Device page before starting.', 'response')
      const device = await backendApi.getDevice(deviceId)
      if (!device.online || device.reported_state !== 'idle') throw new ApiError('Device must be online and report idle before Start. No session was started.', 'response')
        const requestId = get().startRequestId ?? crypto.randomUUID()
        set({ startRequestId: requestId })
        commandAttempted = true
        const session = await backendApi.createSession(device.id, requestId)
      set({ remoteSession: session, state: 'starting', connected: true, deviceOnline: true, startedAt: Date.now(), busy: false })
      const deadline = Date.now() + CONFIRM_TIMEOUT_MS
      let firstPoll = true
      while (Date.now() < deadline) {
        if (!firstPoll) await sleep(POLL_INTERVAL_MS)
        firstPoll = false
        const fresh = await backendApi.getSession(session.id)
        set({ remoteSession: fresh })
        if (fresh.lifecycle === 'measuring') {
          set({ state: 'measuring', busy: false, sessionSync: 'synced', startRequestId: undefined, startedAt: fresh.started_at ? Date.parse(fresh.started_at) : Date.now() })
          beginLiveStream(session.id, set, get)
          return true
        }
        if (fresh.lifecycle === 'error' || fresh.lifecycle === 'interrupted' || fresh.lifecycle === 'cancelled') {
          set({ state: fresh.lifecycle === 'interrupted' ? 'uncertain' : 'error', busy: false, sessionSync: 'synced', error: 'The device did not confirm Start. Check device status before trying again.' })
          return false
        }
      }
      set({ state: 'uncertain', busy: false, sessionSync: 'uncertain', error: 'Start was sent but device confirmation timed out. The device may still be active; check status before retrying.' })
      return false
    } catch (e) {
      const uncertain = commandAttempted && e instanceof ApiError && ['network', 'timeout', 'server'].includes(e.kind)
      set({ state: uncertain ? 'uncertain' : 'error', busy: false, sessionSync: uncertain ? 'uncertain' : 'pending',
        error: uncertain ? `${errorMessage(e, 'Start could not be confirmed.')} Start may have reached the backend; retry uses the same idempotency key, and device state must be checked.` : errorMessage(e, 'Start could not be confirmed. No demo fallback was used.') })
      return false
    }
  },
  start: async () => {
    if (get().demo) {
      const current = get(); if (current.busy || current.state !== 'connected_idle') return
      set({ state: 'starting', busy: true, error: undefined, samples: [], startedAt: Date.now() })
      try { unsubscribeDemo?.(); unsubscribeDemo = demoDevice.subscribe(sample => get().addSample(sample)); await demoDevice.start(); set({ state: 'measuring', busy: false }) }
      catch (e) { set({ state: 'error', busy: false, error: errorMessage(e, 'The demo device did not confirm the start.') }) }
      return
    }
    if (get().state !== 'connected_idle') return
    await get().startFromHome()
  },
  stop: async () => {
    const current = get(); if (current.busy || current.state !== 'measuring') return
    if (current.demo) {
      set({ state: 'stopping', busy: true, error: undefined })
      try { await demoDevice.stop(); unsubscribeDemo?.(); unsubscribeDemo = undefined
        const now = Date.now(); const session: Session = { id: crypto.randomUUID(), startedAt: current.startedAt ?? now, endedAt: now, state: 'completed', source: 'demo', samples: get().samples }
        set(s => ({ state: 'completed', busy: false, sessions: [session, ...s.sessions] }))
      } catch (e) { set({ state: 'uncertain', busy: false, error: errorMessage(e, 'Stop could not be confirmed. Check the device.') }) }
      return
    }
    if (current.bleConnected && (!current.remoteSession || !current.apiToken || !current.backendAvailable)) {
      set({ state: 'stopping', busy: true, error: undefined })
      try { await get().sendPhoneCommand('STOP_MEASUREMENT'); set({ busy: false, state: 'stopping', error: undefined }) }
      catch (e) { set({ state: 'uncertain', busy: false, error: errorMessage(e, 'Stop was not confirmed over BLE.') }) }
      return
    }
    const sessionId = current.remoteSession?.id
    if (!sessionId) { set({ state: 'uncertain', error: 'No active server session is associated with the device; stopped state is unknown.' }); return }
    set({ state: 'stopping', busy: true, error: undefined })
    try {
      await backendApi.completeSession(sessionId)
      set({ busy: false })
      const deadline = Date.now() + CONFIRM_TIMEOUT_MS
      let firstPoll = true
      while (Date.now() < deadline) {
        if (!firstPoll) await sleep(POLL_INTERVAL_MS)
        firstPoll = false
        const fresh = await backendApi.getSession(sessionId)
        set({ remoteSession: fresh })
        if (fresh.lifecycle === 'completed') { remoteAbort?.abort(); set({ state: 'completed', busy: false, sessionSync: 'synced' }); void get().loadHistory().catch(() => undefined); return }
        if (fresh.lifecycle === 'measuring' && fresh.latest_command?.status === 'rejected') { set({ state: 'measuring', busy: false, error: 'The device rejected Stop and still reports measuring.' }); return }
      }
      set({ state: 'uncertain', busy: false, sessionSync: 'uncertain', error: 'Stop was requested but completion was not confirmed. The device may still be running.' })
    } catch (e) { set({ state: 'uncertain', busy: false, sessionSync: 'uncertain', error: `${errorMessage(e, 'Stop request failed.')} Device stop is not confirmed.` }) }
  },
  resetSession: () => { const s = get(); if (s.busy || (s.state !== 'completed' && s.state !== 'cancelled')) return; remoteAbort?.abort(); liveSessionId = undefined; set({ state: s.connected || s.bleConnected ? 'connected_idle' : 'disconnected', samples: [], acousticEvents: [], bleMicSamples: [], blePressureSamples: [], blePressureMmHg: undefined, startedAt: undefined, endedAt: undefined, activeBleSessionId: undefined, remoteSession: undefined, error: undefined }) },
  addSample: sample => set(s => ({ samples: [...s.samples.slice(-(MAX_BUFFERED_SAMPLES - 1)), sample] })),
  clearError: () => set({ error: undefined }),
  setDemo: value => { const state = get().state; if (!get().busy && state !== 'measuring' && state !== 'stopping' && state !== 'starting') { remoteAbort?.abort(); set({ demo: value, connected: false, bleConnected: false, deviceOnline: false, state: 'disconnected', error: undefined, samples: [], acousticEvents: [], blePressureMmHg: undefined, blePressureSamples: [], startedAt: undefined, endedAt: undefined, remoteSession: undefined, sessionSync: value ? 'not_applicable' : 'uncertain' }); void demoDevice.disconnect() } },
  simulateDisconnect: () => { if (!get().demo || !get().connected) return; const wasMeasuring = get().state === 'measuring'; demoDevice.simulateLinkLoss(); set({ connected: false, bleConnected: false, state: wasMeasuring ? 'uncertain' : 'disconnected', error: wasMeasuring ? 'Connection lost. The device stop state is not confirmed.' : undefined }) },
  simulateFault: () => { if (!get().demo || !get().connected || get().busy) return; demoDevice.simulateFault(); set({ state: 'device_fault', error: 'The demo device reported a simulated fault.' }) },
}))

function beginLiveStream(sessionId: string, set: (partial: Partial<Store>) => void, get: () => Store) {
  if (liveSessionId === sessionId && remoteAbort && !remoteAbort.signal.aborted) return
  remoteAbort?.abort()
  liveSessionId = sessionId
  const controller = new AbortController()
  remoteAbort = controller
  lastSampleOffset = 0
  let eventCursor = 0
  void (async () => {
    while (!controller.signal.aborted) {
      try {
        await backendApi.streamSessionEvents(sessionId, event => {
          if (typeof event.id === 'number') eventCursor = Math.max(eventCursor, event.id)
          if (event.event_type === 'telemetry_batch') {
            void (async () => {
              try {
                let page
                do {
                  page = await backendApi.getSamples(sessionId, 200, lastSampleOffset)
                  get().appendRemoteSamples(page.samples, page.acoustic_events)
                  lastSampleOffset += page.samples.length
                } while (lastSampleOffset < page.total_samples && page.samples.length > 0)
              } catch { set({ sessionSync: 'uncertain', error: 'Live telemetry sync was interrupted; stored session data may be incomplete.' }) }
            })()
          } else if (event.event_type === 'command_response') void get().refreshRemoteSession()
        }, controller.signal, eventCursor)
      } catch (e) {
        if (!controller.signal.aborted) set({ sessionSync: 'uncertain' })
      }
      if (!controller.signal.aborted) await sleep(1500)
    }
  })()
}

function adoptRemoteSession(session: RemoteSession, set: (partial: Partial<Store>) => void, get: () => Store) {
  set({ remoteSession: session, connected: true, deviceOnline: true, startedAt: session.started_at ? Date.parse(session.started_at) : get().startedAt })
  if (session.lifecycle === 'measuring') {
    set({ state: 'measuring', sessionSync: 'synced' })
    beginLiveStream(session.id, set, get)
  } else if (session.lifecycle === 'starting' || session.lifecycle === 'created') set({ state: 'starting', sessionSync: 'pending' })
  else if (session.lifecycle === 'stopping') set({ state: 'stopping', sessionSync: 'pending' })
  else if (session.lifecycle === 'interrupted') set({ state: 'uncertain', sessionSync: 'uncertain' })
}

export const telemetryStaleAfterMs = TELEMETRY_STALE_MS
export function isTelemetryStale(receivedAt: number | undefined, now = Date.now()): boolean {
  return receivedAt === undefined || now - receivedAt > TELEMETRY_STALE_MS
}
