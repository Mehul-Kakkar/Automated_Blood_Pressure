import { afterEach, describe, expect, it, vi } from 'vitest'
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { UserProfile } from './App'
import { ApiError, backendApi } from './api'
import { useAppStore } from './store'

const auth = { access_token: 'profile-test-token', token_type: 'bearer' as const, expires_at: '2027-01-02T00:00:00Z', username: 'mehul', display_name: 'Mehul' }
const profile = { username: 'mehul', display_name: 'Mehul', created_at: '2026-01-01T00:00:00Z', avatar_available: false, password_enabled: true }

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
  backendApi.setToken(undefined)
  useAppStore.setState({ apiToken: undefined, busy: false })
  sessionStorage.removeItem('pulsefield.auth.session.v1')
})

describe('account sign-in UI', () => {
  it('leaves the login view only after the returned token loads the account profile', async () => {
    useAppStore.setState({ apiToken: undefined, busy: false })
    vi.spyOn(backendApi, 'login').mockResolvedValue(auth)
    vi.spyOn(backendApi, 'getProfile').mockResolvedValue(profile)
    render(<UserProfile />)

    fireEvent.change(screen.getByLabelText('Username'), { target: { value: 'mehul' } })
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'correct horse battery staple' } })
    fireEvent.click(screen.getAllByRole('button', { name: 'Sign in' }).at(-1)!)

    expect(await screen.findByText('@mehul · account profile')).toBeTruthy()
    expect(screen.queryByText('Sign in to save your history')).toBeNull()
    expect(screen.queryByRole('button', { name: 'Please wait…' })).toBeNull()
    await waitFor(() => expect(useAppStore.getState().apiToken).toBe(auth.access_token))
    expect(backendApi.getProfile).toHaveBeenCalledTimes(1)
  })

  it('clears loading and keeps sign-in available when profile verification fails', async () => {
    useAppStore.setState({ apiToken: undefined, busy: false })
    vi.spyOn(backendApi, 'login').mockResolvedValue(auth)
    vi.spyOn(backendApi, 'getProfile').mockRejectedValue(new Error('Profile service unavailable'))
    render(<UserProfile />)

    fireEvent.change(screen.getByLabelText('Username'), { target: { value: 'mehul' } })
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'correct horse battery staple' } })
    fireEvent.click(screen.getAllByRole('button', { name: 'Sign in' }).at(-1)!)

    expect((await screen.findByRole('alert')).textContent).toContain('Profile service unavailable')
    expect(screen.getAllByRole('button', { name: 'Sign in' }).at(-1)).toBeTruthy()
    expect(useAppStore.getState().apiToken).toBeUndefined()
    expect(useAppStore.getState().busy).toBe(false)
  })

  it('reloads the account profile once when the active token changes', async () => {
    const savedProfile = { ...profile, avatar_available: true }
    vi.spyOn(backendApi, 'getProfile').mockResolvedValue(savedProfile)
    vi.spyOn(backendApi, 'downloadAvatar').mockRejectedValue(new ApiError('Avatar unavailable', 'response', 404))
    useAppStore.getState().setApiToken(auth.access_token, auth.expires_at)
    render(<UserProfile />)

    const removeButton = await screen.findByRole('button', { name: 'Remove profile image' })
    expect((removeButton as HTMLButtonElement).disabled).toBe(false)

    act(() => useAppStore.getState().setApiToken('updated-profile-test-token', '2027-02-02T00:00:00Z'))
    await waitFor(() => expect(backendApi.getProfile).toHaveBeenCalledTimes(2))
    expect((screen.getByRole('button', { name: 'Remove profile image' }) as HTMLButtonElement).disabled).toBe(false)
  })
})
