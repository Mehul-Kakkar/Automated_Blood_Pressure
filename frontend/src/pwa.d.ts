declare module 'virtual:pwa-register/react' {
  import type { Dispatch, SetStateAction } from 'react'
  export function useRegisterSW(options?: {
    onRegisteredSW?: (url: string, registration: ServiceWorkerRegistration | undefined) => void
    onRegisterError?: (error: unknown) => void
  }): {
    needRefresh: [boolean, Dispatch<SetStateAction<boolean>>]
    offlineReady: [boolean, Dispatch<SetStateAction<boolean>>]
    updateServiceWorker: (reloadPage?: boolean) => Promise<void>
  }
}
