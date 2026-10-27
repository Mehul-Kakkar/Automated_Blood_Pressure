import { z } from 'zod'

const HealthSchema = z.object({ status: z.string(), service: z.string().optional() })
const DeviceSchema = z.object({
  id: z.string().uuid(), device_identifier: z.string(), hardware_revision: z.string().nullable().optional(),
  firmware_version: z.string().nullable().optional(), processing_algorithm_version: z.string().nullable().optional(),
  mqtt_enrolled: z.boolean(), online: z.boolean(), reported_state: z.string().nullable().optional(),
  state_reported_at: z.string().datetime().nullable().optional(), last_seen_at: z.string().datetime().nullable().optional(),
  pressure_calibration_confirmed: z.boolean(), pressure_calibration_version: z.string().nullable().optional(),
})
const SessionSchema = z.object({
  id: z.string().uuid(), device_id: z.string().uuid(), lifecycle: z.string(), created_at: z.string(), updated_at: z.string(),
  started_at: z.string().nullable().optional(), ended_at: z.string().nullable().optional(), termination_reason: z.string().nullable().optional(),
  reported_device_state: z.string().nullable().optional(), device_last_seen_at: z.string().nullable().optional(),
  device_state_fresh: z.boolean(), sample_count: z.number(), acoustic_event_count: z.number(), data_provenance: z.string(),
  latest_command: z.object({ command_id: z.string(), command_type: z.string(), status: z.string(), issued_at: z.string(), expires_at: z.string(), response_code: z.string().nullable().optional() }).nullable().optional(),
})
const SessionPageSchema = z.object({ items: z.array(SessionSchema), limit: z.number(), offset: z.number(), total: z.number() })
const SamplesSchema = z.object({
  samples: z.array(z.object({ sample_id: z.string(), sequence: z.number().nullable(), device_timestamp_ms: z.number().nullable(),
    device_recorded_at: z.string().nullable(), server_received_at: z.string(), raw_acoustic_sample: z.object({ adc_counts: z.number().nullable() }),
    processed_signal: z.object({ filtered: z.number().nullable(), envelope: z.number().nullable(), noise_floor: z.number().nullable(), threshold: z.number().nullable() }),
    event_detected: z.boolean().nullable(), cuff_pressure_mmhg: z.number().nullable(), pressure_calibration_version: z.string().nullable(),
    heart_rate_bpm: z.number().nullable(), is_valid: z.boolean(), quality_flags: z.array(z.string()), firmware_version: z.string().nullable(),
    processing_algorithm_version: z.string().nullable(), data_provenance: z.string() })),
  acoustic_events: z.array(z.object({ event_id: z.string(), event_sequence: z.number().nullable(), event_type: z.string(),
    device_timestamp_ms: z.number().nullable(), device_recorded_at: z.string().nullable(), server_received_at: z.string(),
    strength: z.number().nullable(), is_valid: z.boolean(), quality_flags: z.array(z.string()), algorithm_version: z.string().nullable(),
    metadata: z.record(z.string(), z.unknown()), associated_pressure_sample_id: z.string().nullable().optional() })),
  limit: z.number(), offset: z.number(), total_samples: z.number(), total_acoustic_events: z.number(), next_offset: z.number().nullable(),
})
const ReportSchema = z.object({ id: z.string().uuid(), session_id: z.string().uuid(), report_version: z.string(), generator_version: z.string(),
  status: z.string(), validation_status: z.string().nullable(), sample_count: z.number().nullable(), content_sha256: z.string().nullable(),
  generated_at: z.string().nullable(), report: z.record(z.string(), z.unknown()).nullable().optional() })
const ProfileSchema = z.object({ username: z.string(), display_name: z.string(), created_at: z.string(), avatar_available: z.boolean() })
const ApiErrorSchema = z.object({ error: z.object({ code: z.string(), message: z.string(), details: z.array(z.unknown()).optional() }) })

export type Device = z.infer<typeof DeviceSchema>
export type RemoteSession = z.infer<typeof SessionSchema>
export type RemoteSample = z.infer<typeof SamplesSchema>['samples'][number]
export type RemoteAcousticEvent = z.infer<typeof SamplesSchema>['acoustic_events'][number]
export type SamplesPage = z.infer<typeof SamplesSchema>
export type SessionPage = z.infer<typeof SessionPageSchema>
export type Report = z.infer<typeof ReportSchema>
export type Profile = z.infer<typeof ProfileSchema>
export type SessionEvent = { id?: number; event_id?: string; session_id?: string | null; event_type: string; data?: Record<string, unknown> }
export type ApiFailureKind = 'configuration' | 'timeout' | 'authorization' | 'validation' | 'server' | 'network' | 'response'
export class ApiError extends Error { constructor(message: string, readonly kind: ApiFailureKind, readonly status?: number) { super(message); this.name = 'ApiError' } }

export class BackendApi {
  private token?: string
  constructor(private readonly baseUrl = (import.meta.env.VITE_API_BASE_URL as string | undefined)?.replace(/\/$/, '') ?? '',
              private readonly fetcher: typeof fetch = fetch) {}
  setToken(token?: string) { this.token = token?.trim() || undefined }
  get configured() { return this.baseUrl.length > 0 }

  private async request(path: string, init: RequestInit = {}, timeoutMs: number | null = 10000): Promise<Response> {
    if (!this.configured) throw new ApiError('Backend URL is not configured.', 'configuration')
    const controller = new AbortController()
    const timer = timeoutMs === null ? undefined : setTimeout(() => controller.abort(), timeoutMs)
    try {
      const headers = new Headers(init.headers)
      if (!headers.has('Accept')) headers.set('Accept', 'application/json')
      if (init.body) headers.set('Content-Type', 'application/json')
      if (this.token) headers.set('Authorization', `Bearer ${this.token}`)
      const requestSignal = init.signal ? AbortSignal.any([init.signal, controller.signal]) : controller.signal
      const response = await this.fetcher(`${this.baseUrl}${path}`, { ...init, headers, signal: requestSignal })
      if (!response.ok) {
        let message = response.statusText || 'The backend request failed.'
        try { const parsed = ApiErrorSchema.parse(await response.clone().json()); message = parsed.error.message } catch { /* use safe status text */ }
        const kind: ApiFailureKind = response.status === 401 || response.status === 403 ? 'authorization' : response.status === 422 ? 'validation' : response.status >= 500 ? 'server' : 'response'
        throw new ApiError(message, kind, response.status)
      }
      return response
    } catch (error) {
      if (error instanceof ApiError) throw error
      if (error instanceof DOMException && error.name === 'AbortError') throw new ApiError('The backend request timed out.', 'timeout')
      throw new ApiError('The backend could not be reached.', 'network')
    } finally { if (timer !== undefined) clearTimeout(timer) }
  }
  private async json<T>(path: string, schema: z.ZodType<T>, init?: RequestInit): Promise<T> {
    const response = await this.request(path, init)
    try { return schema.parse(await response.json()) } catch { throw new ApiError('The backend returned an invalid response.', 'response', response.status) }
  }
  async health() { return this.json('/health', HealthSchema) }
  async readiness() { return this.json('/ready', z.object({ status: z.string(), database: z.object({ status: z.string() }).optional() })) }
  async listDevices() { return this.json('/api/v1/devices', z.object({ items: z.array(DeviceSchema) })) }
  async getDevice(id: string) { return this.json(`/api/v1/devices/${encodeURIComponent(id)}`, DeviceSchema) }
  async registerDevice(deviceIdentifier: string, hardwareRevision?: string) {
    return this.json('/api/v1/devices', DeviceSchema, { method: 'POST', body: JSON.stringify({ device_identifier: deviceIdentifier, hardware_revision: hardwareRevision || null }) })
  }
  async createSession(deviceId: string, idempotencyKey: string) {
    return this.json('/api/v1/sessions', SessionSchema, { method: 'POST', body: JSON.stringify({ device_id: deviceId, idempotency_key: idempotencyKey }) })
  }
  async getSession(id: string) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}`, SessionSchema) }
  async listSessions(limit = 25, offset = 0) { return this.json(`/api/v1/sessions?limit=${limit}&offset=${offset}`, SessionPageSchema) }
  async completeSession(id: string) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/complete`, SessionSchema, { method: 'POST' }) }
  async cancelSession(id: string) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/cancel`, SessionSchema, { method: 'POST' }) }
  async getSamples(id: string, limit = 200, offset = 0) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/samples?limit=${limit}&offset=${offset}`, SamplesSchema) }
  async getEvents(id: string, limit = 100, afterId = 0) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/events?limit=${limit}&after_id=${afterId}`, z.object({ items: z.array(z.object({ id: z.number(), event_id: z.string(), session_id: z.string().nullable(), event_type: z.string(), device_sequence: z.number().nullable(), device_recorded_at: z.string().nullable(), server_received_at: z.string(), data: z.record(z.string(), z.unknown()) })), after_id: z.number(), limit: z.number(), next_after_id: z.number().nullable() })) }
  async generateReport(id: string) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/report`, ReportSchema, { method: 'POST' }) }
  async getReport(id: string) { return this.json(`/api/v1/sessions/${encodeURIComponent(id)}/report`, ReportSchema) }
  async downloadReport(id: string): Promise<Blob> { return (await this.request(`/api/v1/sessions/${encodeURIComponent(id)}/report.pdf`, { headers: { Accept: 'application/pdf' } })).blob() }
  async getProfile() { return this.json('/api/v1/profile', ProfileSchema) }
  async updateProfile(displayName: string) { return this.json('/api/v1/profile', ProfileSchema, { method: 'PUT', body: JSON.stringify({ display_name: displayName }) }) }
  async uploadAvatar(file: File) {
    if (file.size > 1_048_576) throw new ApiError('Profile image must be no larger than 1 MiB.', 'validation')
    if (!['image/png', 'image/jpeg', 'image/webp', 'image/gif'].includes(file.type)) throw new ApiError('Choose a PNG, JPEG, WebP or GIF image.', 'validation')
    const bytes = new Uint8Array(await file.arrayBuffer())
    let binary = ''
    for (let offset = 0; offset < bytes.length; offset += 0x8000) binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000))
    return this.json('/api/v1/profile/avatar', ProfileSchema, { method: 'PUT', body: JSON.stringify({ content_type: file.type, data_base64: btoa(binary) }) })
  }
  async downloadAvatar(): Promise<Blob> { return (await this.request('/api/v1/profile/avatar', { headers: { Accept: 'image/*' } })).blob() }
  async deleteAvatar() { return this.json('/api/v1/profile/avatar', ProfileSchema, { method: 'DELETE' }) }

  async streamSessionEvents(id: string, onEvent: (event: SessionEvent) => void, signal: AbortSignal, afterId = 0): Promise<void> {
    const response = await this.request(`/api/v1/sessions/${encodeURIComponent(id)}/events/stream?after_id=${afterId}`, { signal, headers: { Accept: 'text/event-stream' } }, null)
    if (!response.body) throw new ApiError('The backend event stream is unavailable.', 'response')
    const reader = response.body.getReader()
    const decoder = new TextDecoder()
    let buffer = ''
    try {
      while (!signal.aborted) {
        const { value, done } = await reader.read()
        if (done) break
        buffer += decoder.decode(value, { stream: true })
        let boundary: number
        while ((boundary = buffer.indexOf('\n\n')) >= 0) {
          const frame = buffer.slice(0, boundary).replace(/\r/g, '')
          buffer = buffer.slice(boundary + 2)
          const data = frame.split('\n').filter(line => line.startsWith('data:')).map(line => line.slice(5).trim()).join('\n')
          if (!data) continue
          try {
            const event = JSON.parse(data) as SessionEvent
            if (typeof event.event_type === 'string') onEvent(event)
          } catch { /* ignore malformed event frame; next event may still be valid */ }
        }
      }
    } finally { await reader.cancel().catch(() => undefined) }
  }
}

export const backendApi = new BackendApi()
