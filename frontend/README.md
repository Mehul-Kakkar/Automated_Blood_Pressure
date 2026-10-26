# Device companion frontend

Mobile-first React + TypeScript application for the experimental ESP32 signal-monitoring project. React Router preserves the existing page structure, Zustand holds in-memory application state, and Zod validates frontend and backend payloads. The UI is experimental and not clinically validated.

## Setup

Requirements: Node.js 20+ and npm 10+.

```powershell
Set-Location .\frontend
npm install
Copy-Item .env.example .env.local
# Set VITE_API_BASE_URL in .env.local to http://127.0.0.1:8000 for a local backend.
npm run dev
```

`VITE_API_BASE_URL` is the base URL for the typed `src/api.ts` client. `VITE_DEMO_MODE=true` starts the explicit development simulator on local development builds. All `VITE_` variables are public; never put owner tokens or broker credentials in them.

## Backend/API integration

The client validates health/readiness, owner-scoped device registration/status, sessions/history, sample/event pages, reports, and PDF downloads. Owner access uses the backend-issued bearer token entered on Settings; it is kept in memory for the current page session only. Refreshing the page clears it. Backend CORS must allow the exact frontend origin, normally `http://localhost:5173`.

- Registering a device creates only an owner-scoped database record. It does not enroll MQTT credentials, provision the hardware, or make the device online.
- The session Start button requires a selected device with fresh `online` and `idle` status. It uses an idempotency key and waits for the backend session to reach device-confirmed `measuring` before showing the active state.
- Stop displays a stopping state and waits for device-confirmed completion. Lost or late confirmation is shown as uncertain; a successful HTTP response alone is never treated as hardware stop.
- Live updates use the authenticated session SSE endpoint. Batch events cause bounded sample-page retrieval; graph buffers are capped at 120 records and rendering follows incoming batches. Missing values stay unavailable. Telemetry is marked stale after 15 seconds without a server-received sample; this is a frontend freshness indicator, not a device safety threshold.
- History is owner-scoped, paginated, and filters records marked demo/synthetic. PDF generation shows the report's status and stored limitations.

## Demo mode

Enable **Development demo mode** in Settings or set `VITE_DEMO_MODE=true` on a local dev build. The banner labels all generated samples. Demo sessions remain in memory, never use the backend API, and never mix with server sessions. Demo signals are low-rate envelope samples only; they do not fabricate pressure, heart rate, event markers, or clinical results. There is no automatic fallback to demo if an API/device call fails.

## Real ESP32 limitations

The inspected `ESP Code Main/korotkoffDebug/korotkoffDebug.ino` produces USB Serial CSV (`ms,raw,filtered,envelope,noise,threshold,event`) at 460800 baud. It has no Wi-Fi/MQTT, BLE, device identity, pressure sensor/conversion, heart-rate source, network session state, or Start/Stop acknowledgement. `korotkoffTest` is a separate serial detector test. The backend's versioned MQTT envelope is a proposed future contract and these sketches do not implement it. Consequently, device registration in the UI cannot make the current firmware online and real remote Start/Stop/live pressure is not yet available.

The UI shows discrete stored envelope/pressure points and exact-timestamp event markers only. It does not reconstruct audio-rate waveforms, infer pressure from detector events, or display systolic/diastolic estimates. Pressure appears only when the backend provides a valid mmHg sample carrying confirmed calibration metadata. No report is clinically validated.

## Checks

```powershell
npm run typecheck
npm test
npm run build
```
