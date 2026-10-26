# Device companion frontend

A mobile-first companion UI for an experimental ESP32 signal-monitoring prototype. The opening screen has one primary Start action: it checks for the device, starts only after the device is available, and reports a connection error when no supported device can be found. In demo mode the same action connects to the simulator and starts a labeled demo session. The live session replaces the start surface in place, keeping the flow on Home without a separate Measure tab. This application is not a clinically validated blood-pressure monitor.

## Requirements and setup

- Node.js 20 or newer
- npm 10 or newer

```sh
npm install
npm run dev
```

The app is a React + TypeScript single-page application built with Vite. React Router handles navigation; Zustand owns the in-memory session state; Zod validates telemetry; Lucide supplies interface icons. Tailwind CSS is configured, with the interface's design tokens and responsive styles currently authored in `src/styles.css`.

## Environment

Copy `.env.example` to `.env.local` if configuration is needed.

| Variable | Purpose |
| --- | --- |
| `VITE_API_BASE_URL` | Reserved backend API base URL. No backend API contract was present in the repository. |
| `VITE_DEMO_MODE` | Set to `true` in development to enable the demo adapter on launch. It defaults off. Demo mode can also be explicitly enabled in Settings. |

All `VITE_` values are public. Do not put credentials or secrets in them. No API calls are currently made and demo data is never uploaded.

## Demo mode

Enable **Development demo mode** in Settings or set `VITE_DEMO_MODE=true` while running `npm run dev`. A persistent DEMO MODE banner indicates simulated data. Press Start to connect the demo adapter and begin; Stop remains explicit on the in-place session view. Settings also includes demo-only fault and connection-loss scenarios for recovery-state review. The demo adapter generates only low-rate acoustic-envelope samples; it never invents heart rate, cuff pressure, or systolic/diastolic values. Session history currently lives in memory and clears on page reload. Demo content cannot be sent to a backend.

## Real device integration

Real device communication is intentionally unavailable. In production mode, Start shows a connection-check loading state and then reports that no compatible device can be found because the inspected firmware does not advertise a supported BLE service. The firmware in `../ESP Code Main/korotkoffDebug/korotkoffDebug.ino` streams a seven-column diagnostic CSV over USB Serial. It samples a microphone, applies digital filters, and reports an acoustic detector envelope and event flag. It does not expose BLE, a pressure sensor, heart-rate telemetry, a session state API, or Start/Stop commands. Reference UUIDs and binary packet layouts from the original brief were not present in firmware and have not been implemented.

`src/domain.ts` defines the adapter interface and a bounds-checked parser for the observed CSV fields. The application does not connect that parser to a serial port and does not transmit commands. Before enabling a real adapter, establish and verify a versioned protocol with acknowledgement semantics, authoritative running/stopped state, telemetry units/validity, timeout/reconnect behavior, and device identity. Keep all pump and valve GPIO control out of the frontend. Firmware owns hardware safety, shutdown, pressure release, timeouts, and watchdog behavior.

Browser Web Bluetooth support varies by browser and platform; iOS Safari does not provide the same Web Bluetooth API as Chromium desktop/Android. A future Capacitor build will require a native BLE plugin/adapter and platform permission handling. Keep device services behind `DeviceAdapter` so transport changes do not alter session UI logic.

## Backend integration

No backend API files or schemas were present. `VITE_API_BASE_URL` is reserved for a future typed client. Define endpoints and idempotency semantics before implementing Start/Stop or session synchronization. A backend outage must not be used as proof that a device has stopped. Do not cache sensitive API responses or upload demo sessions.

## PWA and deployment

The Vite PWA plugin emits an installable manifest and a service worker that precaches static app assets only. It does not cache API responses or health data. Offline app shell availability does not imply device or backend connectivity. Updates use the plugin's prompt strategy; the service worker update UX should be connected before production rollout.

For Vercel, set the project root directory to `frontend/`, use `npm install` and `npm run build`, and publish `dist`. `vercel.json` rewrites client routes to the app entry point. Set `VITE_API_BASE_URL` only when a real API is implemented; never put secrets in Vite variables.

## Checks

```sh
npm run typecheck
npm test
npm run build
```

## Known limitations

- Firmware currently supports USB Serial acoustic debug output only. BLE and safe session-control protocol are missing.
- Pressure, heart rate, live waveform, clinical summaries, and validated blood-pressure results are not available.
- Backend integration, account handling, durable session storage/sync, and authentication are not implemented.
- Demo history is memory-only and disappears on reload.
- No native Capacitor build is included. A native BLE adapter and permissions will be needed later.
- This experimental prototype is not for diagnosis or treatment decisions.
