# Device Companion consumer app

This standalone React/Vite application gives a home user a clear Start flow, controller connection, account access, and a chronological session history. It is deliberately separate from the existing developer frontend.

## Run independently

Requirements: Node.js 20+ and npm 10+.

```powershell
cd frontend-consumer
npm install
Copy-Item .env.example .env.local
# Set VITE_API_BASE_URL to the existing FastAPI backend in .env.local
npm run dev
```

Open the URL printed by Vite. For a production bundle run `npm run build`; `npm run preview` serves the resulting bundle. Bluetooth pairing uses the browser’s Web Bluetooth permission picker and requires a supported browser on HTTPS (localhost is allowed for development).

## Backend integration

The app calls the existing service directly with `VITE_API_BASE_URL`:

- `POST /api/v1/auth/register`, `POST /api/v1/auth/login`, `POST /api/v1/auth/logout`
- `GET /api/v1/profile`, `PUT /api/v1/profile`
- `GET /api/v1/devices`
- `GET /api/v1/sessions?limit=50&offset=0`, `GET /api/v1/sessions/{id}`
- `POST /api/v1/sessions` and `POST /api/v1/sessions/{id}/complete`

The existing device-owned session lifecycle remains responsible for remote command dispatch and acknowledgement. Bluetooth uses the current `PhoneBleClient` and firmware GATT contract; no device commands run until the user presses Start or Stop. Authentication tokens live in tab-scoped `sessionStorage`. No backend URL or credential is bundled into source code.

## Known product limits

The backend has no validated systolic/diastolic estimator. This app therefore shows session dates and duration, never blood pressure values, diagnostic classifications, or inferred trends. BLE session history is saved only in this browser and is not uploaded to the account. Cloud pairing/provisioning, device registration, and recovery workflows are not consumer capabilities in this version; a cloud device must already belong to the account and be online and idle. Bluetooth sessions are not mirrored into backend history. The system remains an experimental prototype and must not be used for diagnosis, treatment, or medical decisions.

See [SURFACE-BRIEF.md](./SURFACE-BRIEF.md) for information architecture and design rationale.
