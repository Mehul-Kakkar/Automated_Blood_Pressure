import { create } from 'zustand'
import { DemoDevice, type Session, type SessionState, type Telemetry } from './domain'

type Store = { demo: boolean; connected: boolean; state: SessionState; samples: Telemetry[]; sessions: Session[]; startedAt?: number; busy: boolean; error?: string; connect: () => Promise<void>; startFromHome: () => Promise<boolean>; start: () => Promise<void>; stop: () => Promise<void>; resetSession: () => void; addSample: (sample: Telemetry) => void; clearError: () => void; setDemo: (value: boolean) => void; simulateDisconnect: () => void; simulateFault: () => void }
const demoDevice = new DemoDevice()
const initialDemo = import.meta.env.DEV && import.meta.env.VITE_DEMO_MODE === 'true'
let unsubscribeDemo: (() => void) | undefined

export const useAppStore = create<Store>((set, get) => ({
  demo: initialDemo, connected: false, state: 'disconnected', samples: [], sessions: [], busy: false,
  setDemo: value => { const state=get().state; if (!get().busy && state!=='measuring' && state!=='stopping' && state!=='starting') { set({ demo: value, connected: false, state: 'disconnected', error: undefined, samples: [] }); void demoDevice.disconnect() } },
  simulateDisconnect: () => { if (!get().demo || !get().connected) return; const wasMeasuring=get().state==='measuring'; demoDevice.simulateLinkLoss(); set({ connected: false, state: wasMeasuring?'uncertain':'disconnected', error: wasMeasuring?'Connection lost. The device stop state is not confirmed.':undefined }) },
  simulateFault: () => { if (!get().demo || !get().connected || get().busy) return; demoDevice.simulateFault(); set({ state:'device_fault', error:'The demo device reported a simulated fault.' }) },
  clearError: () => set({ error: undefined }),
  addSample: sample => set(s => ({ samples: [...s.samples.slice(-119), sample] })),
  connect: async () => {
    if (!get().demo) { set({ error: 'Real device connection is not available until firmware provides a verified session protocol.' }); return }
    set({ busy: true, error: undefined, state: 'disconnected' })
    try { await demoDevice.connect(); const status = await demoDevice.reconcile(); set({ busy: false, connected: true, state: status === 'measuring' ? 'measuring' : status==='fault'?'device_fault':'connected_idle' }) }
    catch (e) { set({ busy: false, state: 'error', error: e instanceof Error ? e.message : 'Could not connect to the demo device.' }) }
  },
  startFromHome: async () => {
    const current=get(); if(current.busy || current.state==='measuring' || current.state==='stopping') return false
    set({ busy:true, error:undefined, state:'connecting' })
    if (!current.demo) {
      await new Promise(resolve=>setTimeout(resolve,800))
      set({ busy:false, connected:false, state:'disconnected', error:'No compatible device found. The current ESP32 firmware does not advertise a supported BLE service.' })
      return false
    }
    try {
      if (!current.connected) await demoDevice.connect()
      set({ busy:false, connected:true, state:'connected_idle' })
      await get().start()
      return get().state==='measuring'
    } catch (e) {
      set({ busy:false, connected:false, state:'error', error:e instanceof Error?e.message:'Could not connect to the demo device.' })
      return false
    }
  },
  start: async () => {
    const current = get(); if (current.busy || current.state !== 'connected_idle') return
    set({ state: 'starting', busy: true, error: undefined, samples: [], startedAt: Date.now() })
    try { unsubscribeDemo?.(); unsubscribeDemo = demoDevice.subscribe(sample => get().addSample(sample)); await demoDevice.start(); set({ state: 'measuring', busy: false }) }
    catch (e) { set({ state: 'error', busy: false, error: e instanceof Error ? e.message : 'The device did not confirm the start.' }) }
  },
  stop: async () => {
    const current = get(); if (current.busy || current.state !== 'measuring') return
    set({ state: 'stopping', busy: true, error: undefined })
    try {
      await demoDevice.stop(); unsubscribeDemo?.(); unsubscribeDemo = undefined
      const now = Date.now(); const session: Session = { id: crypto.randomUUID(), startedAt: current.startedAt ?? now, endedAt: now, state: 'completed', source: 'demo', samples: get().samples }
      set(s => ({ state: 'completed', busy: false, sessions: [session, ...s.sessions] }))
    } catch (e) { set({ state: 'uncertain', busy: false, error: e instanceof Error ? e.message : 'Stop could not be confirmed. Check the device.' }) }
  },
  resetSession: () => { const s=get(); if(s.busy || (s.state!=='completed' && s.state!=='cancelled')) return; set({ state:s.connected?'connected_idle':'disconnected', samples:[], startedAt:undefined, error:undefined }) },
}))
