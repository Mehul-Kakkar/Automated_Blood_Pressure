import { z } from 'zod'

export const TelemetrySchema = z.object({ receivedAt: z.number().finite(), heartRateBpm: z.number().finite().min(25).max(240).optional(), cuffPressureMmHg: z.number().finite().min(0).max(400).optional(), acousticEnvelope: z.number().finite().min(0).optional(), deviceState: z.enum(['idle', 'measuring', 'stopped', 'fault']) })
export type Telemetry = z.infer<typeof TelemetrySchema>
export type SessionState = 'disconnected'|'connecting'|'connected_idle'|'starting'|'measuring'|'stopping'|'completed'|'cancelled'|'error'|'device_fault'|'uncertain'
export type Session = { id: string; startedAt: number; endedAt?: number; state: 'completed'|'cancelled'; source: 'demo'; samples: Telemetry[] }

export interface DeviceAdapter {
  connect(): Promise<void>
  start(): Promise<'acknowledged'>
  stop(): Promise<'acknowledged'>
  reconcile(): Promise<'idle'|'measuring'|'fault'>
  disconnect(): Promise<void>
  subscribe(listener: (sample: Telemetry) => void): () => void
}

// Firmware currently exposes USB Serial acoustic debug CSV only. It has no
// Start/Stop command or BLE protocol. Never send commands to unverified hardware.
export const realDevice = { available: false, reason: 'The connected firmware has no verified remote session protocol yet.' }

export class DemoDevice implements DeviceAdapter {
  private timer?: ReturnType<typeof setInterval>
  private listeners = new Set<(sample: Telemetry) => void>()
  private state: 'idle'|'measuring'|'stopped'|'fault' = 'idle'
  async connect() { await new Promise(r => setTimeout(r, 450)); if (this.state === 'fault' || this.state === 'stopped') this.state = 'idle' }
  async start() { if (this.state !== 'idle' && this.state !== 'stopped') throw new Error('Device is not ready to start.'); await new Promise(r => setTimeout(r, 550)); this.state = 'measuring'; this.timer = setInterval(() => { const sample = TelemetrySchema.parse({ receivedAt: Date.now(), acousticEnvelope: Number((4 + Math.random() * 3).toFixed(1)), deviceState: 'measuring' }); this.listeners.forEach(fn => fn(sample)) }, 1400); return 'acknowledged' as const }
  async stop() { if (this.state !== 'measuring') throw new Error('No demo session is running.'); clearInterval(this.timer); await new Promise(r => setTimeout(r, 500)); this.state = 'stopped'; return 'acknowledged' as const }
  async reconcile() { return this.state === 'measuring' ? 'measuring' as const : this.state === 'fault' ? 'fault' as const : 'idle' as const }
  async disconnect() { clearInterval(this.timer); this.state = 'idle' }
  simulateLinkLoss() { clearInterval(this.timer) }
  simulateFault() { clearInterval(this.timer); this.state = 'fault' }
  subscribe(listener: (sample: Telemetry) => void) { this.listeners.add(listener); return () => this.listeners.delete(listener) }
}

export function decodeSerialCsv(line: string): Telemetry | null {
  const parts = line.trim().split(',')
  if (parts.length !== 7) return null
  const [ms, raw, filtered, envelope, noise, threshold, event] = parts.map(Number)
  if (![ms, raw, filtered, envelope, noise, threshold, event].every(Number.isFinite) || ms < 0 || envelope < 0 || event < 0 || event > 1) return null
  // CSV is diagnostic acoustic data. Pressure and heart rate are not provided.
  return TelemetrySchema.parse({ receivedAt: ms, acousticEnvelope: envelope, deviceState: 'idle' })
}
