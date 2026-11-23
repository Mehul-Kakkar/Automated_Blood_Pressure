import { act, type ReactNode } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { BrowserRouter } from 'react-router-dom'
import { afterEach, describe, expect, it, vi } from 'vitest'
import App from './App'

vi.mock('../../frontend/src/ble', () => ({ PhoneBleClient: class { connected = false; connect = vi.fn(); disconnect = vi.fn(); send = vi.fn() } }))

let root: Root | undefined
let mount: HTMLDivElement | undefined
const render = async (element: ReactNode) => {
  mount = document.createElement('div'); document.body.append(mount)
  root = createRoot(mount)
  await act(async () => { root?.render(element) })
}

afterEach(async () => {
  if (root) await act(async () => root?.unmount())
  root = undefined; mount?.remove(); mount = undefined
  sessionStorage.clear(); localStorage.clear(); vi.restoreAllMocks()
})

describe('consumer start flow', () => {
  it('does not start a measurement before a device is connected and the user presses Start', async () => {
    ;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true
    const fetchSpy = vi.spyOn(globalThis, 'fetch')
    await render(<BrowserRouter><App/></BrowserRouter>)
    const start = mount?.querySelector<HTMLButtonElement>('button[aria-label="Start measurement"]')
    expect(start).not.toBeNull()
    expect(start?.disabled).toBe(true)
    expect(fetchSpy).not.toHaveBeenCalled()
    expect(mount?.textContent).toContain('Connect a device first to make Start available.')
  })
})
