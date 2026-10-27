# Backend/frontend integration (Phase 7)

This is a local integration guide for the experimental prototype. It does not establish clinical validity. Frontend demo data is synthetic and stays in the browser; it is never sent to the backend.

## Start PostgreSQL and backend

Install PostgreSQL 14+ locally (Docker is optional). Create a database and role, then from PowerShell:

```powershell
Set-Location .\backend
py -3.10 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
Copy-Item .env.example .env
```

Set `DATABASE_URL` in `backend/.env` to a local PostgreSQL SQLAlchemy URL and set:

```dotenv
CORS_ORIGINS=http://localhost:5173,http://127.0.0.1:5173
```

No broker or cloud service is currently configured in this repository. Choose a managed or self-hosted MQTT broker that supports TLS and per-device ACLs; provision the backend service identity and device principals there. Keep `MQTT_ENABLED=false` until a TLS broker, service credential, device ACLs, and device firmware protocol are ready. When configured, backend-only messaging variables include `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `MQTT_CA_FILE`; mTLS additionally needs both `MQTT_CLIENT_CERT_FILE` and `MQTT_CLIENT_KEY_FILE`. Keep credentials and key files outside source control.

Apply migrations and start the API:

```powershell
alembic upgrade head
python -m uvicorn app.main:app --reload --host 127.0.0.1 --port 8000
```

In another backend terminal, create an owner credential. The token is printed once; do not commit it or put it in a `VITE_` variable:

```powershell
Set-Location .\backend
.\.venv\Scripts\Activate.ps1
python -m app.cli create-owner device_owner "Local Device Owner"
```

The owner token is required for `/api/v1/profile`, `/api/v1/devices`, sessions, history, reports, and PDF downloads. Paste it into Settings in the frontend; it is held in page memory only and cleared on reload. `owners.username` is the unique database primary key, separate from the editable `display_name`; profile name and image are persisted. Device enrollment is still a trusted provisioning task. UI registration creates an offline, MQTT-disabled database record only.

## Start the frontend

```powershell
Set-Location .\frontend
npm install
Copy-Item .env.example .env.local
# Edit .env.local: VITE_API_BASE_URL=http://127.0.0.1:8000
npm run dev
```

The Vite development origin `http://localhost:5173` must appear exactly in backend `CORS_ORIGINS`. In deployment, serve the frontend and backend over HTTPS and configure only trusted HTTPS origins. Never expose MQTT credentials, client certificates, database credentials, or owner tokens through Vite environment variables.

## Test with simulated data

Run `npm test` in `frontend/` for explicit demo adapter and API-client tests. To view the simulator, enable **Development demo mode** in Settings or set `VITE_DEMO_MODE=true` for a local development build. The banner labels synthetic telemetry. It stays local, is not uploaded, and is excluded from backend history. A backend/device failure never activates demo mode automatically.

Backend automated tests use synthetic fixtures marked as test data:

```powershell
Set-Location .\backend
.\.venv\Scripts\Activate.ps1
python -m pytest -q
```

The suite skips PostgreSQL-specific checks unless `TEST_POSTGRES_URL` is set to a dedicated disposable test database. It does not test a live TLS broker or physical device.

## Verify remote API/report behavior

Check `GET /health` and `GET /ready`. In Settings, enter the owner token. The Device page can create a device record and list/report its status; newly registered records show `MQTT enrolled: Not provisioned` and `Offline` until an operator completes trusted backend/broker enrollment and a compatible device status envelope is received.

A report can be requested from a stored session in History. It is generated from persisted validated rows, and the UI shows its validation status and limitations before downloading the PDF. Unit tests exercise synthetic report inputs and PDF bytes. To verify live pressure, acoustic events, report alignment, and command acknowledgement end-to-end, the backend needs PostgreSQL, a provisioned TLS MQTT broker, a protocol-compatible test publisher/device, an enrolled owner/device relationship, and device-generated stored samples. Use only verified pressure calibration and units.

## Real ESP32 status and remaining work

The current `ESP Code Main/korotkoffDebug/korotkoffDebug.ino` sends USB Serial CSV only: `ms,raw,filtered,envelope,noise,threshold,event`. It has no Wi-Fi/MQTT/TLS, BLE service, identity/provisioning, sequence/session envelope, pressure or heart rate source, remote commands, or command acknowledgements. The backend's v1 MQTT envelope is proposed and is not emitted by the firmware. As a result:

- Owner/device REST, backend health, sessions/history/report APIs and UI wiring are implemented, but real session creation requires an online MQTT-enrolled test device and remains unusable with the checked-in ESP32 sketches.
- Acoustic live points can only appear after a compatible publisher persists backend samples. The UI renders actual stored points and does not synthesize a cloud waveform.
- Pressure, heart rate and event-pressure alignment cannot be verified against the current hardware. Pressure remains hidden unless the backend confirms calibration and mmHg provenance.
- Network outage samples are not replayable because firmware has no persistent history; the design must surface transmission gaps after firmware supports them.
- No clinical validation, estimator validation, calibration study, or safety certification has been performed. Never use these experimental outputs for diagnosis or treatment.
