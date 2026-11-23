import { z } from 'zod'

const DeviceSchema = z.object({ id: z.string().uuid(), online: z.boolean(), reported_state: z.string().nullable(), mqtt_enrolled: z.boolean() })
const Timestamp = z.string().refine(value => Number.isFinite(Date.parse(value)))
const SessionSchema = z.object({ id: z.string().uuid(), device_id: z.string().uuid(), lifecycle: z.string(), created_at: Timestamp, started_at: Timestamp.nullable(), ended_at: Timestamp.nullable(), sample_count: z.number(), acoustic_event_count: z.number() })
const AuthSchema = z.object({ access_token: z.string(), token_type: z.literal('bearer'), expires_at: z.string(), username: z.string(), display_name: z.string() })
const ProfileSchema = z.object({ username: z.string(), display_name: z.string(), created_at: z.string(), password_enabled: z.boolean() })
export type Device = z.infer<typeof DeviceSchema>
export type Session = z.infer<typeof SessionSchema>
export type Profile = z.infer<typeof ProfileSchema>
export type Auth = z.infer<typeof AuthSchema>

export class ConsumerApiError extends Error {
  constructor(readonly kind: 'configuration' | 'network' | 'timeout' | 'authorization' | 'validation' | 'unavailable' | 'response', message: string) { super(message) }
}

export class ConsumerApi {
  private token?: string
  private readonly base: string
  constructor(baseUrl = (import.meta.env.VITE_API_BASE_URL as string | undefined) ?? '') { this.base = baseUrl.replace(/\/$/, '') }
  setToken(token?: string) { this.token = token }

  private async request<T>(path: string, schema: z.ZodType<T>, init: RequestInit = {}): Promise<T> {
    if (!this.base) throw new ConsumerApiError('configuration', 'The service address is not set up yet. Ask your app administrator for help.')
    const controller = new AbortController()
    const timer = window.setTimeout(() => controller.abort(), 12000)
    const headers = new Headers(init.headers)
    headers.set('Accept', 'application/json')
    if (init.body) headers.set('Content-Type', 'application/json')
    if (this.token) headers.set('Authorization', `Bearer ${this.token}`)
    try {
      const response = await fetch(`${this.base}${path}`, { ...init, headers, signal: controller.signal })
      if (!response.ok) {
        if (response.status === 401 || response.status === 403) throw new ConsumerApiError('authorization', 'Your sign-in has expired. Please sign in again.')
        if (response.status === 422) throw new ConsumerApiError('validation', 'Some details need another look. Please check the form and try again.')
        if (response.status >= 500) throw new ConsumerApiError('unavailable', 'The service is having trouble right now. Please try again shortly.')
        const message = response.status === 409 && path.startsWith('/api/v1/sessions') ? 'This device is busy or not ready yet. Check its status before starting.' : response.status === 409 && path.endsWith('/auth/register') ? 'That username is already in use. Try another username or sign in.' : 'That request could not be completed. Please try again.'
        throw new ConsumerApiError('response', message)
      }
      if (response.status === 204) return undefined as T
      const data: unknown = await response.json()
      const parsed = schema.safeParse(data)
      if (!parsed.success) throw new ConsumerApiError('response', 'The service sent an unexpected response. Please try again later.')
      return parsed.data
    } catch (error) {
      if (error instanceof ConsumerApiError) throw error
      if (error instanceof DOMException && error.name === 'AbortError') throw new ConsumerApiError('timeout', 'The service took too long to respond. Check your connection and try again.')
      throw new ConsumerApiError('network', 'We could not reach the service. Check your connection and try again.')
    } finally { window.clearTimeout(timer) }
  }

  register(username: string, displayName: string, password: string) { return this.request('/api/v1/auth/register', AuthSchema, { method: 'POST', body: JSON.stringify({ username, display_name: displayName, password }) }) }
  login(username: string, password: string) { return this.request('/api/v1/auth/login', AuthSchema, { method: 'POST', body: JSON.stringify({ username, password }) }) }
  logout() { return this.request('/api/v1/auth/logout', z.undefined(), { method: 'POST' }) }
  profile() { return this.request('/api/v1/profile', ProfileSchema) }
  updateProfile(displayName: string) { return this.request('/api/v1/profile', ProfileSchema, { method: 'PUT', body: JSON.stringify({ display_name: displayName }) }) }
  devices() { return this.request('/api/v1/devices', z.object({ items: z.array(DeviceSchema) })) }
  sessions() { return this.request('/api/v1/sessions?limit=50&offset=0', z.object({ items: z.array(SessionSchema), total: z.number() })) }
  session(id: string) { return this.request(`/api/v1/sessions/${encodeURIComponent(id)}`, SessionSchema) }
  start(deviceId: string, idempotencyKey: string) { return this.request('/api/v1/sessions', SessionSchema, { method: 'POST', body: JSON.stringify({ device_id: deviceId, idempotency_key: idempotencyKey }) }) }
  stop(id: string) { return this.request(`/api/v1/sessions/${encodeURIComponent(id)}/complete`, SessionSchema, { method: 'POST' }) }
}

export const consumerApi = new ConsumerApi()
