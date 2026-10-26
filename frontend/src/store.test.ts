import { afterEach, describe, expect, it } from 'vitest'
import { useAppStore } from './store'

afterEach(() => {
  useAppStore.getState().setDemo(false)
  useAppStore.setState({ state: 'disconnected', connected: false, busy: false, sessions: [], samples: [], error: undefined })
})

describe('session commands', () => {
  it('uses GO to check availability and reports the unsupported real device', async () => {
    useAppStore.getState().setDemo(false)
    expect(await useAppStore.getState().go()).toBe(false)
    expect(useAppStore.getState().state).toBe('disconnected')
    expect(useAppStore.getState().error).toContain('No compatible device found')
  })

  it('uses GO to connect the demo device and start only after acknowledgement', async () => {
    useAppStore.getState().setDemo(true)
    expect(await useAppStore.getState().go()).toBe(true)
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
