# Backend

Foundational FastAPI service and PostgreSQL persistence layer for the experimental device project. The current ESP32 firmware does not send network telemetry or pressure readings; the telemetry schema and internal batch writer define a future, validated storage boundary, not an existing wire contract. This API makes no clinical claims.

## Requirements

- Windows PowerShell
- Python 3.10 or newer
- PostgreSQL 14 or newer for persistent local runs (not needed for unit tests)

## Install and configure (PowerShell)

From the repository root:

```powershell
Set-Location .\backend
py -3.10 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
Copy-Item .env.example .env
```

If PowerShell blocks virtual environment activation:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\.venv\Scripts\Activate.ps1
```

`.env.example` contains placeholders only. Set `CORS_ORIGINS` to exact trusted frontend origins. Keep `DATABASE_URL` empty when only running the API and tests. Never commit `.env`, credentials, keys, or certificates.

## Install PostgreSQL locally

Docker is not required. On Windows, install PostgreSQL using the [official PostgreSQL Windows installer page](https://www.postgresql.org/download/windows/), install the server and command-line tools, and keep the local PostgreSQL service running. Add its `bin` directory to `PATH` if `psql`, `createuser`, and `createdb` are not recognized.

Create a dedicated local role and database; `createuser` prompts for its password rather than storing it in this command:

```powershell
createuser --host 127.0.0.1 --username postgres --pwprompt bp_backend
createdb --host 127.0.0.1 --username postgres --owner bp_backend bp_monitor_test
```

Set `DATABASE_URL` in `backend/.env` to a SQLAlchemy psycopg URL. Replace the placeholder and URL-encode special characters in the password:

```dotenv
DATABASE_URL=postgresql+psycopg://bp_backend:URL_ENCODED_PASSWORD@127.0.0.1:5432/bp_monitor_test
```

Apply the schema and check the current migration:

```powershell
alembic upgrade head
alembic current
```

To run the app against PostgreSQL, keep that same `.env` configured. Without `DATABASE_URL`, the service still starts; `/ready` reports the database as `not_configured`. With a configured but unreachable database, `/ready` returns HTTP 503. A future Docker Compose file is an optional convenience and is not part of this phase.

## Run locally

From `backend/` with the virtual environment active:

```powershell
python -m uvicorn app.main:app --reload --host 127.0.0.1 --port 8000
```

- `GET http://127.0.0.1:8000/health` checks API liveness.
- `GET http://127.0.0.1:8000/ready` checks readiness and PostgreSQL when configured.
- `GET http://127.0.0.1:8000/docs` opens the development OpenAPI UI.

## Schema and current boundaries

Alembic owns schema changes. The initial migration creates:

- `devices`: unique opaque device identifier and hardware, firmware, and processing version metadata.
- `measurement_sessions`: device-linked lifecycle (`created`, `starting`, `measuring`, `stopping`, `completed`, `cancelled`, `interrupted`, `error`), provenance, timestamps, and versions. A completed state requires start and end timestamps. No disconnect path marks a session complete.
- `telemetry_batches`: per-device batch ID, canonical payload SHA-256, schema/version/provenance metadata, sample/event counts, server receipt time, and synchronization status. Device plus batch ID is unique.
- `telemetry_samples`: stable sample ID, optional device sequence/uptime and device time, server receipt time, optional pressure explicitly in mmHg, optional validated heart rate, diagnostic acoustic columns, validity/quality flags, versions, provenance, and synchronization metadata. Device plus sample ID is unique; optional sequence values are unique within a session. Missing pressure/heart rate remain NULL.
- `acoustic_events`: independently identified detected events, optional device time, strength, optional mmHg pressure, validity/quality, versions, and provenance.
- `reports`: report version, generator version, status, validation/sample watermark metadata, and safe failure code. Report content/generation is not implemented.

The schema includes owners, revocable bearer credentials, device ownership, and owner-scoped session authorization. There is no public telemetry ingestion endpoint: the current firmware has no MQTT/Wi-Fi contract or device authentication. The internal `ingest_telemetry_batch` service accepts only the documented future schema, verifies the registered device/session association, enforces sample and serialized-byte caps, and atomically inserts batches/samples/events. Repeating the same batch ID with the same canonical content is a no-op; reusing it with different content or colliding record IDs raises a conflict. No raw payload is logged or retained as a blob.

Current firmware output remains USB Serial diagnostics (`ms,raw,filtered,envelope,noise,threshold,event`) and has no pressure sensor values. Future protocol work must define and verify identifiers, sequences, units, timestamps, field bounds, device authentication, and provenance before the internal storage schema is exposed to a network transport.

## Tests

From `backend/` with the virtual environment active:

```powershell
python -m pytest -q
```

Most persistence tests use SQLite in memory to exercise SQLAlchemy relationships, constraints, foreign keys, batch transactions, duplicate handling, and invalid-value rejection. The migration test upgrades/checks/downgrades a temporary SQLite database for practical migration coverage. Against a dedicated disposable PostgreSQL test database (the name must contain `test`), set the integration URL and run:

```powershell
alembic upgrade head
alembic check
$env:TEST_POSTGRES_URL = $env:DATABASE_URL
python -m pytest -q tests\test_postgres_integration.py
Remove-Item Env:TEST_POSTGRES_URL
python -m pytest -q
```

The PostgreSQL-specific integration tests apply/check migrations and exercise batch idempotency and database constraints. They are skipped unless `TEST_POSTGRES_URL` is set. They leave the schema/migration installed in that dedicated database, but clean their test rows. Do not point this variable at production or a database with valuable data. This PostgreSQL integration pass was not run as part of this change: `pg_isready -h 127.0.0.1 -p 5432` reported no response and `DATABASE_URL` was not configured. The migration and SQLAlchemy PostgreSQL driver are included so those commands are runnable once a local server is installed and configured.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `APP_NAME` | `Experimental Device API` | Service name |
| `ENVIRONMENT` | `development` | Deployment label |
| `LOG_LEVEL` | `INFO` | Python log level |
| `DOCS_ENABLED` | `true` | Enable `/docs` and `/redoc` |
| `CORS_ORIGINS` | empty | Comma-separated exact browser origins; wildcard is not used |
| `DATABASE_URL` | unset | Optional SQLAlchemy PostgreSQL URL; treated as a secret |
| `DATABASE_CONNECT_TIMEOUT_SECONDS` | `2` | Database connect timeout (1–30 seconds) |
| `MAX_TELEMETRY_BATCH_SAMPLES` | `500` | Maximum records per validated batch (1–500) |
| `MAX_TELEMETRY_BATCH_BYTES` | `262144` | Maximum canonical serialized batch size (1 KiB–1 MiB) |
| `DEVICE_STATE_MAX_AGE_SECONDS` | `30` | How long a device state report remains fresh for Start/Stop requests (5–300 seconds) |

## Phase 4: secure MQTT transport

The backend includes an optional MQTT 5 client using Paho. It is disabled by default. When enabled, it verifies the broker certificate and hostname, requires backend service credentials, uses QoS 1, bounded queues, manual acknowledgements, reconnect behavior, and non-retained command publishes with MQTT message expiry. Client certificates may also be configured. TLS verification cannot be disabled.

No MQTT broker or cloud deployment is configured in this repository. To run this feature, provision a reachable broker with a TLS listener (normally 8883), a certificate whose SAN matches the broker hostname, a trusted CA, anonymous access disabled, a dedicated backend service identity, and an ACL file. Do not commit broker certificates, private keys, password files, or `.env` files. For a local broker such as Mosquitto, install it separately, generate a local development CA/server certificate, configure its TLS listener and password/ACL files, and create distinct backend and device credentials. Use a broker principal equal to the enrolled device UUID for each device. Broker ACLs, not payload claims alone, must restrict each device to its own topics. The backend verifies that the topic UUID, payload UUID, enrollment record, MQTT-enabled flag, and enrolled principal agree; standard MQTT message callbacks do not expose the broker-authenticated publisher identity to application code.

Configure `backend/.env` after copying `.env.example`:

```dotenv
MQTT_ENABLED=true
MQTT_HOST=broker-hostname-from-server-certificate
MQTT_PORT=8883
MQTT_CLIENT_ID=bp-backend
MQTT_USERNAME=backend-service-account
MQTT_PASSWORD=replace-with-a-locally-provisioned-secret
MQTT_CA_FILE=C:/path/to/trusted-ca.pem
MQTT_CLIENT_CERT_FILE=
MQTT_CLIENT_KEY_FILE=
```

`MQTT_CLIENT_CERT_FILE` and `MQTT_CLIENT_KEY_FILE` must either both be set or both be empty. The TLS context uses the operating system trust roots when `MQTT_CA_FILE` is empty. `DATABASE_URL` must point to an initialized PostgreSQL database before MQTT is enabled. Set reasonable `MQTT_QUEUE_SIZE`, `MQTT_MAX_PAYLOAD_BYTES`, `MQTT_PUBLISH_TIMEOUT_SECONDS`, and `MQTT_COMMAND_TTL_SECONDS` values for the deployment. Restart the API after changing settings.

### Proposed version 1 wire contract

These are backend-side proposed envelopes, not firmware contracts. The inspected ESP32 sketches currently have no network, MQTT, pressure sensor, session-control, or command-ack implementation. No firmware change or end-to-end device test is claimed in this phase.

| Topic | Publisher | Meaning |
| --- | --- | --- |
| `bp/v1/devices/{device_id}/telemetry` | Device | `{ "schema_version": "1", "payload": <validated telemetry batch> }` |
| `bp/v1/devices/{device_id}/status` | Device | Versioned status with message UUID, device UUID, sequence, state, optional session/uptime/firmware/algorithm version, lost sample count and optional device timestamp |
| `bp/v1/devices/{device_id}/commands` | Backend | Versioned command UUID, device UUID, optional session UUID, command type (`START`, `STOP`, `STATUS_REQUEST`), issue time and expiry |
| `bp/v1/devices/{device_id}/responses` | Device | Versioned response UUID correlated by command UUID, device UUID, status (`accepted`, `rejected`, `completed`), optional state/reason/time |

Use `+` ACL wildcards only for the backend service identity. Device principals should be constrained to write their own `telemetry`, `status`, and `responses` topics and read their own `commands` topic. The backend service identity needs read access to the three ingress topic families and write access to `commands` only. Deny all other topic access. A deployment using a broker with different ACL syntax must implement equivalent per-identity restrictions.

The service subscribes only to telemetry, status, and response topics. Device identities must be trustedly enrolled in PostgreSQL with `mqtt_enabled=true` and `mqtt_principal` set to that device UUID. Owner-authenticated POST /api/v1/devices creates an MQTT-disabled record only; broker credentials and topic ACLs remain operator-provisioned. The Phase 5 session API uses revocable owner bearer credentials and checks device ownership. Command delivery is never retained or replayed after a disconnect, START commands expire, duplicate command IDs are rejected, and the device must independently enforce expiry, allowed state transitions, pressure limits, and safety interlocks. A broker publish acknowledgement only means broker delivery handling, not that a measurement started or stopped.

`GET /health/messaging` reports enabled/connected state, queue depth/capacity, timestamps, and aggregate counters without credentials or measurement payloads. Test transport behavior without a live broker using:

```powershell
python -m pytest -q tests\test_messaging.py
```

For a live broker connectivity smoke check, apply the PostgreSQL migrations, configure the TLS and service-account values above, start the API in one PowerShell window, then query the health endpoint in another:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health/messaging
```

Expect `state: connected` and `connected: true`; a broker or TLS/authentication problem remains `disconnected` and is represented in sanitized application logs. To test topic ACLs and end-to-end device identity, provision a dedicated test-device credential/ACL and enroll its UUID in the database with `mqtt_enabled=true` and `mqtt_principal` equal to that UUID, then have a protocol-compatible test client publish a valid status message. The session API is the only owner-authorized command path; there is no arbitrary direct-command route. The inspected firmware cannot act as that test client.

Live TLS/broker validation requires an operator-provisioned broker, database, backend credentials and at least one trusted test-device identity. Owner-authenticated POST /api/v1/devices creates an offline record only; it does not provision broker credentials or authorize device messaging. The current firmware still lacks network capability and a versioned protocol. Until then, the ESP32 may continue local operation during network loss, but it does not persist historical data: unsent transient samples are lost, and firmware must report a data gap/lost-sample count rather than imply backend receipt. The current inspected sketches do not yet implement that reporting behavior either.

## Phase 5: sessions, owner authorization, samples, and live events

All account-scoped endpoints require `Authorization: Bearer <owner-token>`. Tokens are high-entropy random values; only a SHA-256 digest is stored. `owners.username` is the unique primary key, and `display_name` is an independent editable profile value. Create an account and token after applying migrations:

```powershell
python -m app.cli create-owner device_owner "Local Device Owner"
```

The command prints the token once; place it in a local secret manager or a protected environment variable. Rotate and revoke that owner's active credentials by issuing:

```powershell
python -m app.cli rotate-credential device_owner
```

Devices must be associated with the owner through a trusted enrollment/admin process. The authenticated device registration endpoint creates an MQTT-disabled owner record only; broker enrollment and credential provisioning remain operator-controlled. For a local test only, update an already registered device in `psql` after replacing placeholders:

```sql
UPDATE devices
SET owner_username = 'device_owner'
WHERE id = '<DEVICE_UUID>';
```

Existing UUID-keyed owners are migrated to deterministic usernames `user_<32 lowercase UUID hex characters>`. Their original UUID is preserved in `legacy_id`, and existing credentials and device ownership are migrated to `owner_username`. Run `alembic upgrade head` before starting the updated service. Username is immutable through the profile API.

Authenticated profile operations:

- `GET /api/v1/profile` — username, display name, and avatar availability.
- `PUT /api/v1/profile` — update `display_name` without changing the account username.
- `PUT /api/v1/profile/avatar` — persist a PNG/JPEG/WebP/GIF image (up to 1 MiB).
- `GET /api/v1/profile/avatar` and `DELETE /api/v1/profile/avatar` — retrieve or remove that image.

The frontend User profile uses these authenticated endpoints. Image bytes currently reside in the owner row; the schema can be migrated to external object storage later. Stored sessions and report snapshots are owned transitively through the registered device. Report snapshots persist supported calculations and generation/version metadata; at present valid heart-rate min/max/mean may be calculated, while no systolic/diastolic estimator exists.

Do not set `pressure_calibration_confirmed` or a calibration version unless that device's conversion and units have actually been confirmed. MQTT pressure batches are rejected until both are set. The existing firmware has no pressure sensor or calibration and cannot produce this data.

| Endpoint | Behavior |
| --- | --- |
| `POST /api/v1/sessions` | Requires `{ "device_id": "...", "idempotency_key": "..." }`. Device must be owned by the token owner, MQTT-enrolled, recently seen, and reporting `idle`. Creates a session and publishes an expiring START request. Repeating the same key returns the same session without resending START. |
| `GET /api/v1/sessions?limit=50&offset=0` | Owner-scoped, stable newest-first history; limit 1–100. |
| `GET /api/v1/sessions/{session_id}` | Lifecycle, fresh/stale device state, latest command status, and sample/event counts. Stale device state is returned as unavailable. |
| `POST /api/v1/sessions/{session_id}/complete` | Requests STOP only for a fresh, device-confirmed measuring (or interrupted but freshly confirmed measuring) session. Returns `stopping`; a correlated completed response or fresh in-order status for this session can confirm stopped/idle state and move it to `completed`. Repeating while stopping does not publish another STOP. |
| `POST /api/v1/sessions/{session_id}/cancel` | Cancels a session still in `created`, or an interrupted session with no confirmed start only after a fresh idle/stopped device report. A started session cannot be locally declared stopped or cancelled. |
| `GET /api/v1/sessions/{session_id}/samples?limit=100&offset=0` | Bounded page with separate raw ADC sample, processed signal/envelope, acoustic-event and optional validated measurement fields. Both sample and acoustic-event arrays are limited to 200 rows. Derived systolic/diastolic measurements are unavailable. |
| `GET /api/v1/sessions/{session_id}/events?after_id=0&limit=100` | Durable, replayable event page; limit 1–200, monotonically increasing event ID cursor. |
| `GET /api/v1/sessions/{session_id}/events/stream` | Authenticated SSE stream. Resume with `Last-Event-ID`; telemetry-batch events announce available samples and provide the authenticated samples URL, so clients retrieve bounded sample pages instead of receiving an unbounded stream payload. Optional `max_events` (1–1000) bounds a stream for smoke checks. |

Example PowerShell requests after setting the token securely in the current process:

```powershell
$headers = @{ Authorization = "Bearer $env:BP_API_TOKEN" }
$body = @{ device_id = "<DEVICE_UUID>"; idempotency_key = [guid]::NewGuid().ToString() } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8000/api/v1/sessions -Headers $headers -ContentType "application/json" -Body $body
Invoke-RestMethod -Uri http://127.0.0.1:8000/api/v1/sessions?limit=50 -Headers $headers
```

### Telemetry limitations and time handling

Phase 4's version 1 JSON telemetry/status/response envelopes remain proposed and are the only MQTT contract accepted by the backend. The actual `korotkoffDebug` firmware emits USB Serial CSV (`ms,raw,filtered,envelope,noise,threshold,event`) at 460800 baud; it has no network transport, sequence number, session ID, pressure, heart rate, UTC clock, or command acknowledgement. It processes a 2 kHz microphone input locally and prints one diagnostic row every eight samples. The backend does not treat its single CSV rows as a pressure measurement, turn them into a synthetic waveform, or claim network compatibility.

Raw ADC counts and processed scalar diagnostics/envelope are distinct fields. A database-stored acoustic event is separate from those samples. Version 1 does not identify a pressure sample linked to an event, so event-level pressure is rejected and `associated_pressure_sample_id` remains null. Sample pressure is stored only after database enrollment explicitly confirms calibration and units. Heart rate is stored only when present and within the existing schema bounds; the current firmware has no heart-rate source. No systolic/diastolic output is derived.

For a future network-capable firmware, keep the 2 kHz acquisition, filtering, envelope and detector local. Do not stream audio-rate/PCM data. An initial diagnostic transport can batch the existing one-row-per-eight-samples scalar diagnostics (nominally 250 rows/second) in messages of at most 500 records and at most the configured 256 KiB, flushing at the earlier limit; at that output rate the record-count limit is about two seconds. Send detected acoustic-event metadata separately in the same batch where possible. This is a bounded transport proposal, not firmware behavior. Its 250-row/second bandwidth and retention cost still need measurement; decide whether to lower the diagnostic reporting rate before real deployment. The backend does not reconstruct a waveform from sparse samples.

Server receipt timestamps are authoritative for database ordering and lifecycle transitions. An aware device wall-clock timestamp is preserved, but values more than five minutes from server receive time are flagged `device_clock_skew_suspected`; device `millis()` values remain monotonic milliseconds and are never converted to UTC. Older/out-of-order sequence records are retained with `out_of_order_sequence`; sequence gaps are surfaced as provisional batch metadata because delayed/out-of-order batches can fill them. Exact duplicate batches/messages are idempotent. Device state is considered fresh for 30 seconds by default; after that the API marks it stale without asserting that hardware stopped. The current firmware cannot report status, lost samples, or data gaps. During network loss it continues only its existing local diagnostic behavior and has no historical storage/replay; generated data is lost.

Request logs contain request ID, method, path (without query string), status, and duration. They omit request bodies, client addresses, credentials, and measurement values.
## Phase 6 — deterministic reports

Reports are generated only from validated, persisted backend rows. The service stores an immutable JSON snapshot with a SHA-256 content hash, report version (`v1`), generator version, sample watermark and validation status. A repeated `POST` for the same session/version returns the existing snapshot; the PDF endpoint renders that stored snapshot and does not recompute measurements.

Authenticated, owner-scoped endpoints:

- `POST /api/v1/sessions/{session_id}/report` — create report once (`201`) or return existing (`200`). Empty and incomplete sessions produce explicit limited/uninterpretable reports rather than fabricated values.
- `GET /api/v1/sessions/{session_id}/report` — retrieve report metadata and deterministic JSON snapshot.
- `GET /api/v1/sessions/{session_id}/report.pdf` — download a server-rendered paginated PDF.

The PDF includes session/device/version metadata, timestamps, stored envelope and calibration-gated pressure charts, acoustic event rows, valid heart-rate summaries, gaps/quality metadata, and limitations. It does not interpolate waveform points, infer pressure from events, or provide systolic/diastolic estimates. Pressure is included only when the device calibration flag/version is confirmed and the sample is valid and explicitly in mmHg. Heart rate summaries are arithmetic min/max/mean of valid stored heart-rate samples only. The report is experimental and not clinically validated.

To install the pinned PDF dependency with the service: `python -m pip install -r requirements-dev.txt`. Apply the new migration with `alembic upgrade head`. Report snapshots limit included telemetry to 20,000 samples, acoustic-event JSON rows to 1,000 and session metadata events to 10,000; completeness metadata identifies included and total row counts and truncation. The current firmware does not emit this backend's proposed MQTT schema, pressure, or heart rate, so reports from real device sessions remain unavailable until protocol and hardware support are implemented and verified.

## Phase 7: frontend integration boundary

The companion frontend now calls the typed `/health`, `/ready`, `/api/v1/devices`, `/api/v1/sessions`, session sample/event, report, and PDF APIs with Zod response validation. Owner tokens remain in browser memory only. The API uses authenticated fetch-based SSE because native `EventSource` cannot attach the backend's bearer header. Frontend device status is derived only from a fresh backend device report; backend reachability and phone BLE are reported separately.

`POST /api/v1/devices` creates an owner-scoped record with MQTT disabled. It is not trusted hardware enrollment and cannot make a device online or authorized for commands. The frontend Start path requires current device `online`/`idle`, sends an idempotent session create request, then waits for device-confirmed `measuring`; Stop waits for `completed` and remains uncertain on timeout/disconnect. Batch events retrieve bounded sample pages and the UI retains 120 points. No REST call is made for every raw sample. History is owner-scoped and paginated, reports can be generated/downloaded, and demo samples never reach backend methods.

Device REST contract: `POST /api/v1/devices` accepts `{ "device_identifier": "...", "hardware_revision": "..." }` and creates an owner-bound, MQTT-disabled record. `GET /api/v1/devices` lists the caller-owned records; `GET /api/v1/devices/{device_id}` returns one caller-owned device or 404. The `online` flag requires MQTT enrollment and a state report newer than `DEVICE_STATE_MAX_AGE_SECONDS`. Registration does not provision a device credential or allow commands until trusted operator enrollment is completed.

The actual ESP32 sketches still provide no compatible remote transport/session protocol, pressure, or heart rate. Therefore Phase 7 does not make real-device commands, live telemetry, or pressure displays operational. A TLS broker, trusted device provisioning, protocol-compatible firmware, real calibration, and physical safety verification remain required. Frontend token entry is a local prototype affordance, not a production identity provider; production needs secure account authentication and HTTPS.
