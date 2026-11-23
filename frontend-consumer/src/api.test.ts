import { afterEach, describe, expect, it, vi } from 'vitest'
import { ConsumerApi } from './api'

const originalFetch = globalThis.fetch
const response = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })
const auth = { access_token: 'opaque-token', token_type: 'bearer', expires_at: '2099-01-01T00:00:00Z', username: 'home_user', display_name: 'Home User' }

afterEach(() => { globalThis.fetch = originalFetch; vi.restoreAllMocks() })

describe('consumer API contract', () => {
  it('sends account credentials to the existing auth route and validates the response', async () => {
    const api = new ConsumerApi('https://backend.example.test')
    globalThis.fetch = vi.fn(async () => response(auth))
    const result = await api.login('home_user', 'a sufficiently long password')
    expect(result.username).toBe('home_user')
    expect(globalThis.fetch).toHaveBeenCalledWith(expect.stringMatching(/\/api\/v1\/auth\/login$/), expect.objectContaining({ method: 'POST' }))
  })

  it('attaches the session bearer token and uses the current sessions endpoint', async () => {
    const api = new ConsumerApi('https://backend.example.test')
    api.setToken('opaque-token')
    globalThis.fetch = vi.fn(async () => response({ items: [], total: 0 }))
    expect((await api.sessions()).items).toEqual([])
    expect(globalThis.fetch).toHaveBeenCalledWith(expect.stringMatching(/\/api\/v1\/sessions\?limit=50&offset=0$/), expect.objectContaining({ headers: expect.any(Headers) }))
    const init = vi.mocked(globalThis.fetch).mock.calls[0][1]
    expect(new Headers(init?.headers).get('Authorization')).toBe('Bearer opaque-token')
  })

  it('translates auth and unavailable errors without exposing server response text', async () => {
    const api = new ConsumerApi('https://backend.example.test')
    globalThis.fetch = vi.fn(async () => new Response('private stack trace', { status: 503 }))
    await expect(api.devices()).rejects.toMatchObject({ kind: 'unavailable' })
    globalThis.fetch = vi.fn(async () => new Response('private stack trace', { status: 401 }))
    await expect(api.devices()).rejects.toMatchObject({ kind: 'authorization' })
  })

  it('rejects invalid session shapes rather than showing incomplete data', async () => {
    const api = new ConsumerApi('https://backend.example.test')
    globalThis.fetch = vi.fn(async () => response({ items: [{ internal: 'not a session' }], total: 1 }))
    await expect(api.sessions()).rejects.toMatchObject({ kind: 'response' })
  })
})
