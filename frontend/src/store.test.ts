import { afterEach, describe, expect, it } from 'vitest'
import { useAppStore } from './store'
import { phoneBle, type PhoneBleEvent } from './ble'
import { vi } from 'vitest'

afterEach(() => {
  vi.restoreAllMocks()
  useAppStore.getState().setDemo(false)
  useAppStore.setState({ state: 'disconnected', connected: false, busy: false, sessions: [], samples: [], error: undefined })
  localStorage.removeItem('pulsefield.ble.sessions.v1')
})

describe('session commands', () => {
  it('starts the BLE elapsed timer and exposes only valid calibrated pressure', async () => {
    let notify: ((event: PhoneBleEvent) => void) | undefined
    vi.spyOn(phoneBle, 'connect').mockImplementation(async listener => { notify = listener; return 'BP Controller' })
    await useAppStore.getState().connectPhone()
    notify?.({ kind: 'state', value: { state: 'idle', fault: 'none' } })
    notify?.({ kind: 'state', value: { state: 'inflating', fault: 'none' } })
    expect(useAppStore.getState().startedAt).toBeTypeOf('number')
    notify?.({ kind: 'pressure', value: { raw_counts: 1000000, mmhg: 21.48, pressure_calibrated: true, valid: true } })
    expect(useAppStore.getState().blePressureMmHg).toBe(21.48)
    expect(useAppStore.getState().blePressureSamples.at(-1)?.cuffPressureMmHg).toBe(21.48)
    notify?.({ kind: 'pressure', value: { raw_counts: 1000000, mmhg: 21.48, pressure_calibrated: true, valid: false } })
    expect(useAppStore.getState().blePressureMmHg).toBeUndefined()
  })

  it('streams BLE microphone samples and saves a completed BLE session locally', async () => {
    let notify: ((event: PhoneBleEvent) => void) | undefined
    vi.spyOn(phoneBle, 'connect').mockImplementation(async listener => { notify = listener; return 'BP Controller' })
    vi.spyOn(phoneBle, 'send').mockResolvedValue(undefined)
    useAppStore.setState({ sessions: [], state: 'disconnected', apiToken: undefined, backendAvailable: false, deviceOnline: false })
    await useAppStore.getState().connectPhone()
    notify?.({ kind: 'state', value: { state: 'idle', fault: 'none' } })
    expect(await useAppStore.getState().startFromHome()).toBe(true)
    notify?.({ kind: 'korotkoff', value: { device_timestamp_ms: 1000, raw_adc: 2030, filtered: -7.3, envelope: 4.2, event_detected: false, pressure_valid: true, mmhg: 31.5 } })
    expect(useAppStore.getState().bleMicSamples.at(-1)?.acousticEnvelope).toBe(4.2)
    expect(useAppStore.getState().bleMicSamples.at(-1)?.microphoneFiltered).toBe(-7.3)
    notify?.({ kind: 'state', value: { state: 'completed', fault: 'none' } })
    expect(useAppStore.getState().sessions[0]).toMatchObject({ source: 'ble', state: 'completed' })
    expect(JSON.parse(localStorage.getItem('pulsefield.ble.sessions.v1') ?? '[]')).toHaveLength(1)
  })

  it('offers a reset command for a BLE fault and waits for the controller idle report', async () => {
    let notify: ((event: PhoneBleEvent) => void) | undefined
    const send = vi.spyOn(phoneBle, 'send').mockResolvedValue(undefined)
    vi.spyOn(phoneBle, 'connect').mockImplementation(async listener => { notify = listener; return 'BP Controller' })
    await useAppStore.getState().connectPhone()
    notify?.({ kind: 'state', value: { state: 'idle', fault: 'none' } })
    notify?.({ kind: 'state', value: { state: 'fault', fault: 'adc_saturation' } })
    expect(useAppStore.getState().state).toBe('device_fault')
    await useAppStore.getState().resetDeviceFault()
    expect(send).toHaveBeenCalledWith('RESET_FAULT')
    notify?.({ kind: 'state', value: { state: 'idle', fault: 'none' } })
    expect(useAppStore.getState().state).toBe('connected_idle')
  })

  it('fails clearly when the real backend is not configured and does not fall back to demo', async () => {
    useAppStore.getState().setDemo(false)
    useAppStore.getState().setApiToken('fixture-owner-token')
    expect(await useAppStore.getState().startFromHome()).toBe(false)
    expect(useAppStore.getState().state).toBe('error')
    expect(useAppStore.getState().error).toContain('Backend is unavailable')
  })

  it('uses Start to connect the demo device and start only after acknowledgement', async () => {
    useAppStore.getState().setDemo(true)
    expect(await useAppStore.getState().startFromHome()).toBe(true)
    expect(useAppStore.getState().state).toBe('measuring')
    await useAppStore.getState().stop()
  })

  it('requires an explicit start and prevents duplicate start and stop commands', async () => {
    useAppStore.getState().setDemo(true)
    await useAppStore.getState().connect()
    expect(useAppStore.getState().state).toBe('connected_idle')
    const firstStart = useAppStore.getState().start()
    await useAppStore.getState().start()
    expect(useAppStore.getState().state).toBe('starting')
    await firstStart
    expect(useAppStore.getState().state).toBe('measuring')
    const firstStop = useAppStore.getState().stop()
    await useAppStore.getState().stop()
    expect(useAppStore.getState().state).toBe('stopping')
    await firstStop
    expect(useAppStore.getState().state).toBe('completed')
    expect(useAppStore.getState().sessions).toHaveLength(1)
    expect(useAppStore.getState().sessions[0].source).toBe('demo')
  })

  it('will not disable demo mode while a session is running', async () => {
    useAppStore.getState().setDemo(true)
    await useAppStore.getState().connect()
    await useAppStore.getState().start()
    useAppStore.getState().setDemo(false)
    expect(useAppStore.getState().demo).toBe(true)
    expect(useAppStore.getState().state).toBe('measuring')
    await useAppStore.getState().stop()
  })

  it('reconciles a connection loss to the device-reported measuring state', async () => {
    useAppStore.getState().setDemo(true)
    await useAppStore.getState().connect()
    await useAppStore.getState().start()
    useAppStore.getState().simulateDisconnect()
    expect(useAppStore.getState().state).toBe('uncertain')
    await useAppStore.getState().connect()
    expect(useAppStore.getState().state).toBe('measuring')
    await useAppStore.getState().stop()
    expect(useAppStore.getState().state).toBe('completed')
  })

  it('keeps Start unavailable after a device-reported fault until status is checked', async () => {
    useAppStore.getState().setDemo(true)
    await useAppStore.getState().connect()
    useAppStore.getState().simulateFault()
    expect(useAppStore.getState().state).toBe('device_fault')
    await useAppStore.getState().connect()
    expect(useAppStore.getState().state).toBe('connected_idle')
  })
})
