import { describe, expect, it } from 'vitest'
import { decodeSerialCsv, TelemetrySchema } from './domain'

describe('device data validation', () => {
  it('accepts the firmware acoustic debug CSV without inventing pressure or heart rate', () => {
    const sample = decodeSerialCsv('1000,2048,2.4,1.7,3.0,12.0,0')
    expect(sample?.acousticEnvelope).toBe(1.7)
    expect(sample?.cuffPressureMmHg).toBeUndefined()
    expect(sample?.heartRateBpm).toBeUndefined()
  })
  it('rejects malformed or invalid telemetry', () => {
    expect(decodeSerialCsv('1000,2048,2.4')).toBeNull()
    expect(decodeSerialCsv('x,2048,2.4,1.7,3.0,12.0,0')).toBeNull()
    expect(() => TelemetrySchema.parse({ receivedAt: 1, heartRateBpm: 0, deviceState: 'measuring' })).toThrow()
  })
})
