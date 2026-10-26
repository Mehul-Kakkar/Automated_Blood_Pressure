# Backend implementation plan

**Status:** design baseline. Phase implementations are recorded at the end of this file; the repository state is no longer planning-only.  
**Scope:** compatible backend/API design based on the checked-in repository as inspected on 2026-10-26.

## 1. Existing repository findings

- The repository has `frontend/`, an empty `backend/`, and firmware under `ESP Code Main/` (there is no `espcode/` directory). The root has no README or checked-in `AGENTS.md`; the frontend README is the project documentation. The user-provided repository instructions require current Context7 documentation for future implementation questions involving frameworks/libraries.
- `frontend/` is a React 19, TypeScript, Vite SPA. React Router routes `/`, `/history`, `/device`, `/settings`, and `/user`; `/measure` redirects to `/`. Zustand keeps current app/session state in memory. Zod validates the frontend telemetry shape. There are no API calls, API client, login, or persisted history.
- `VITE_API_BASE_URL` is reserved and blank in `.env.example`; `VITE_DEMO_MODE` defaults false. Vite environment variables are public. Demo telemetry is simulated, explicitly labeled, and not uploaded. The PWA service worker precaches static assets only and does not cache health/session data.
- The Start flow explicitly checks/connects, then starts only after the demo adapter acknowledges. Stop is explicit and waits for acknowledgement; loss of connection during a session is represented as uncertain. These are useful UX semantics but currently apply only to the simulator. The UI warns that backend availability cannot establish device stop state and that hardware safety belongs in firmware.
- `frontend/src/domain.ts` defines optional `heartRateBpm`, `cuffPressureMmHg`, and `acousticEnvelope`, plus `deviceState: idle | measuring | stopped | fault`. Its serial CSV decoder expects exactly seven numeric columns. The application does not connect it to a serial port or a real device. The frontend README and Device view state that the current firmware has no BLE or session-control protocol.
- `ESP Code Main/korotkoffDebug/korotkoffDebug.ino` configures an ESP32 ADC microphone input (GPIO 4), samples at 2 kHz, applies a high-pass/low-pass filter, runs `KorotkoffDetector`, and prints one diagnostic CSV row per eight samples at 460800 baud. CSV header and fields are `ms,raw,filtered,envelope,noise,threshold,event`.
- That sketch accepts newline-delimited `M=<float>`, `F=<float>`, `D=<float>` serial tuning commands with bounds (M 1–10, F 1–500, D 200–1000 ms). They tune detector settings only. There are no Start/Stop, status, or safety commands. `setup()` starts the detector’s logical deflation event collection, not a pump or cuff; no pump/valve GPIO or pressure hardware is present in the inspected sketch.
- `korotkoff_detector.h` retains up to 64 acoustic events in RAM, but its current callers do not persist or publish that event list. It accepts optional pressure with validity bounds 0–400 mmHg; both inspected sketches call `setPressure(NAN, false)`. Possible systolic/disappearance values require valid pressure and are explicitly only candidates, not validated results. Current prototype therefore has acoustic filtering/event-detection code, not a working blood-pressure measurement capability.
- `korotkoffTest.ino` is a second microphone/detector test that prints human-readable serial diagnostics and explicitly says pressure is unavailable. `korotkoffDebug/debug.py` uses pyserial and matplotlib for local serial plotting and sends the M/F/D threshold-tuning commands. No credentials or cloud configuration were found in the inspected project files: no Wi-Fi setup, broker, MQTT library/config, BLE UUIDs, TLS config, or cloud SDK. No real Wi-Fi secrets are present.
- There is a pre-existing worktree modification to `ESP Code Main/korotkoffTest/korotkoffTest.ino`; it was not changed as part of this planning task.

## 2. Proposed architecture

```text
ESP32 (local sensing, detector, session state, hardware safety)
  -- MQTT/TLS, device-authenticated --> broker --> ingestion worker/API service
                                                    |             |
                                                    v             v
                                               PostgreSQL     live event fanout
                                                    |             |
Browser SPA -- HTTPS REST + SSE --------------------+-------------+
```

Use a Python/FastAPI service with Pydantic request/event models, SQLAlchemy ORM, and Alembic migrations. A managed or self-hosted PostgreSQL instance stores validated session metadata, telemetry, and report snapshots. Keep MQTT ingestion behind an adapter/worker boundary; do not block HTTP requests while waiting on a device. REST handles frontend commands, history, and report retrieval. Server-Sent Events (SSE) is the simplest browser live-stream option over HTTPS; use authenticated WebSocket only if bidirectional live behavior later requires it.

The ESP32 remains authoritative for acquisition, pressure conversion, acoustic processing, session transitions, and safe stopping. The cloud may request explicit Start/Stop but must never actuate pump/valve hardware or be required for safe stop. The existing firmware is not yet capable of the proposed MQTT path; firmware networking and a versioned protocol are prerequisite work, not assumed current features.

## 3. Actual current telemetry and command contracts

### Debug firmware serial output

- Transport: USB Serial, 460800 baud, newline-terminated CSV.
- Header: `ms,raw,filtered,envelope,noise,threshold,event`.
- Row types: `ms` is `millis()`; `raw` is ADC integer; `filtered`, `envelope`, `noise`, `threshold` are printed floats (two decimal places); `event` is `0` or `1` and indicates a pending detector event. One row every eight 2 kHz samples (nominally 250 rows/s).
- There is no device timestamp epoch, session identifier, sequence number, unit/calibration metadata, replay ID, pressure, heart rate, BLE UUID, or network envelope. Treat `ms` as a device-local wrapping counter, not UTC.
- The header line and rows are the entire debug stream. The separate test sketch prints human-readable messages rather than this CSV format.

### Actual debug serial commands

- `M=<float>\n`: threshold multiplier, accepted 1.0–10.0.
- `F=<float>\n`: threshold floor, accepted 1.0–500.0.
- `D=<float>\n`: minimum event interval in milliseconds, accepted 200–1000 and cast to integer.
- Invalid/unrecognized commands are silently ignored. There is no acknowledgement, command ID, session control, state query, or safety command. These must not be represented as cloud Start/Stop commands.

### Future cloud contract boundary

No MQTT telemetry or command contract currently exists. Any MQTT schema/topic below is proposed for future firmware/backend agreement and needs a compatibility review with actual firmware before implementation. Do not reinterpret raw serial CSV directly as blood-pressure telemetry. In particular, never manufacture pressure, heart rate, systolic, or diastolic values from the current data.

## 4. Proposed database schema

All tables use UUID primary keys unless otherwise stated, UTC `timestamptz`, foreign keys, and indexes for user/device/time access. Exact retention and account ownership rules need product decisions.

- **users**: `id`, identity-provider subject (unique; only after auth is selected), created/updated timestamps. Do not use the unconnected frontend profile name as identity.
- **devices**: `id`, opaque public device identifier, owner/user association or enrollment record, firmware/protocol version, credential reference/status, last-seen timestamp, created timestamp. Never store a reusable device private key in plaintext.
- **measurement_sessions**: `id`, `user_id`, `device_id`, client idempotency key, start/end timestamps, lifecycle state (`requested`, `starting`, `measuring`, `stopping`, `completed`, `cancelled`, `interrupted`, `fault`, `unknown`), source (`device` or `demo` only if demo is later intentionally persisted), protocol version, interruption/termination reason, created/updated timestamps. Unique `(user_id, idempotency_key)`; database constraints prevent invalid time ordering. Cloud `requested` is not device confirmation.
- **telemetry_samples**: `id`, `session_id`, `device_id`, device sequence number, device monotonic milliseconds, server receive time, optional device UTC time only if a future protocol establishes it, nullable validated acoustic diagnostics (`raw_adc`, `filtered`, `envelope`, `noise_floor`, `threshold`, `event`), nullable pressure/heart-rate fields only after sensors and units are confirmed, schema version, original event digest or bounded raw payload for audit. Unique `(device_id, protocol_version, sequence_number)` to deduplicate. Partition by receive/session date if volume requires it.
- **device_events**: `id`, `device_id`, optional `session_id`, event type, device sequence, server receive time, validated JSON details, deduplication key. Record state changes, command acknowledgements, faults, and connection/recovery events.
- **reports**: `id`, `session_id`, report version, generator version, deterministic JSON payload, generated timestamp, input watermark/sample count, and validation status. Reports must describe experimental signal data and unavailable metrics; they must not present clinical interpretations or claim clinical validity.

Avoid storing duplicate raw payloads indefinitely. Establish data minimization, access/deletion, retention, backup, and audit policies before handling real personal health-related data.

## 5. MQTT topic and authentication design (proposed, not currently supported)

Use an MQTT broker reachable only over TLS (MQTTS, typically port 8883). Require per-device identity and ACLs. Preferred device authentication is mutually authenticated TLS with a unique device certificate and rotation/revocation process; alternatively use short-lived, individually provisioned credentials if ESP32 TLS/certificate lifecycle constraints prevent mTLS. Never embed a shared fleet secret in firmware, commit credentials, or send credentials in topics/payloads. Broker-to-ingestion access also uses a separate service identity and restricted ACL.

Proposed topic namespace (versioned and illustrative):

- `bp/v1/devices/{device_id}/telemetry` — device publishes validated-envelope telemetry, QoS 1; backend deduplicates by stable device sequence/event ID.
- `bp/v1/devices/{device_id}/events` — device publishes state/fault/ack events, QoS 1.
- `bp/v1/devices/{device_id}/commands` — backend publishes explicit Start/Stop requests, QoS 1, non-retained, short expiry where broker supports it.
- `bp/v1/devices/{device_id}/command-acks` — device publishes accepted/rejected/completed acknowledgements and authoritative state, QoS 1.
- Optional `.../presence` retained online/offline last-will signal; it is connectivity evidence only, never proof of measurement state or safe stop.

Every proposed JSON envelope needs `schema_version`, unique `event_id`/`command_id`, device ID, session ID where applicable, sequence number, device uptime, and clearly named values/units. Those are new fields and are not in firmware today. Agree payload size/rate, sequence rollover, UTC/time synchronization, session association, validation bounds, QoS/retry semantics, command expiry, and state reconciliation before publishing a protocol spec. Never retain Start/Stop commands. Duplicate command IDs must be idempotent. Start is permitted only after user action and a device-ready state; stop remains a best-effort request whose completion is confirmed by device state, not broker delivery.

## 6. Proposed HTTPS REST API contract

Version all paths under `/api/v1`. These are proposed endpoints, not existing integration points.

- `GET /healthz`: liveness only; no secrets or infrastructure details.
- `GET /api/v1/devices`: list devices authorized to the current authenticated user, including online/last-seen and last reported state with an explicit staleness timestamp.
- `POST /api/v1/sessions`: create/request a session for a selected device. Require a client `Idempotency-Key` header. Return `202` plus session ID and requested/starting state; only mark measuring after device acknowledgement. This is invoked only by explicit Start UI action.
- `POST /api/v1/sessions/{session_id}/stop`: idempotent explicit Stop request; return `202` with stopping/unknown status. Only device acknowledgement confirms stopped/completed.
- `GET /api/v1/sessions?cursor=&limit=`: authenticated, owner-scoped paginated history metadata.
- `GET /api/v1/sessions/{session_id}`: session metadata, current/reconciled state, sample counts and data-availability flags.
- `GET /api/v1/sessions/{session_id}/telemetry?after_sequence=&limit=`: validated stored samples for history/report rendering; bounded pagination and ownership checks.
- `GET /api/v1/sessions/{session_id}/events`: authenticated SSE stream for live telemetry/state; replay from last event ID where retained. Signal stale/disconnected state; SSE disconnect must not stop or start the device.
- `GET /api/v1/sessions/{session_id}/report`: deterministic report JSON; optionally `POST .../report` to request generation if generation is asynchronous. Report output includes provenance, gaps, units, validation/version metadata, and explicit unavailable fields.

REST does not publish arbitrary device commands or accept telemetry from browsers. Separate authn/authz, device ownership checks, pagination limits, request body limits, rate limits, consistent error envelopes, and API OpenAPI docs are required. Authentication provider and enrollment flow are unresolved; do not expose session/report endpoints unauthenticated in production.

## 7. Frontend integration requirements

- Add a typed API module using the actual backend OpenAPI schema or generated TypeScript types; validate untrusted response/event payloads at runtime with Zod (or generated validators). `VITE_API_BASE_URL` is the base URL only and contains no secrets.
- Keep the `DeviceAdapter` boundary. A browser frontend cannot talk to the existing firmware over the internet directly; a real device needs network firmware plus backend/broker provisioning, or a separately designed phone-to-device bridge. Browser BLE alone does not provide a cloud MQTT bridge.
- Map session `requested/starting/measuring/stopping/completed/interrupted/fault/unknown` into UI states without treating HTTP success, MQTT publish success, or network disappearance as device acknowledgement. Reconcile on reconnect by reading the authoritative session/device state.
- Stream validated telemetry through SSE and update live UI; fetch paginated stored data for History and reports. Include stale-data/offline/gap indicators, timestamps, unavailable pressure fields, and clear distinction between device and demo data.
- Demo sessions must remain isolated and not be uploaded by default. Decide separately if demo records should ever appear in backend history.
- Current history/profile are in-memory only. Add account/auth flow and API-backed history only once ownership and identity are decided. Do not cache API health/session/report data in the PWA service worker.
- Preserve explicit Start/Stop UI and the existing safety language. The physical switch only powers the device; it must not launch a measurement. The firmware should boot idle/ready, and only explicit Start can begin a session.

## 8. Security and privacy considerations

- TLS for browser-to-API and device-to-broker; secure certificate/key provisioning, per-device ACLs, rotation, revocation, and environment-managed secrets.
- User authentication and authorization are prerequisites for personal history. Enforce user ownership on every session/report route; device identity alone is not user authorization.
- Validate MQTT topic identity against payload device ID and enrolled device; validate schema version, timestamps/uptime, finite numeric values, bounds, payload size, rate, and sequence/idempotency. Quarantine malformed data and record operational metrics without logging credentials or full sensitive payloads.
- Protect commands with authorization, short expiry, idempotency, replay protection, state checks, audit records, and non-retained delivery. Broker delivery is not device acceptance.
- Restrict CORS to deployed frontend origins; use secure session/token storage strategy and CSRF protection if cookie auth is chosen. No secrets in Vite config or the repository.
- Define retention, export/deletion, backups, least-privilege DB roles, encrypted storage/backups, migration review, and audit access. Avoid claiming regulatory compliance without an actual assessment. No clinical claims; this is an experimental project.

## 9. Network-loss behavior

The ESP32 must continue or safely stop according to its own local firmware state machine and hardware safety policy when Wi-Fi, broker, backend, or frontend is unavailable. It must not wait for a cloud response to stop safely. Because the chosen constraint is **no historical measurement persistence on the ESP32**, telemetry produced while disconnected cannot be replayed later and is permanently lost. Do not imply that MQTT QoS queues data across power loss or that QoS 1 is durable device storage. On reconnect, the device can report current state and subsequent telemetry, but there will be an explicit gap. Backend marks a session `interrupted`/`unknown` or partial according to the last confirmed state, records gap boundaries/last-seen, and never fabricates samples or retrospectively claims a complete report. Session closure and safe hardware shutdown must remain independent. Decide whether to discard incomplete sessions or retain partial sessions with a conspicuous data-gap marker.

## 10. Phased implementation steps and tests

1. **Protocol and hardware discovery:** verify intended board, sensor/pneumatic hardware, power behavior, Wi-Fi availability, firmware build target, and safety state machine. Write a versioned protocol proposal covering device identity, session commands/acks, state, telemetry, units, timestamps, sequence/dedupe, fault states, disconnects, and compatibility. Tests: packet/CSV golden fixtures; protocol parser malformed/bounds/duplicate/version tests; power-up does not start; local stop/safety tests on hardware before network integration.
2. **Backend foundation:** create FastAPI app, settings via environment, PostgreSQL SQLAlchemy models, Alembic migrations, Pydantic schemas, health endpoint, structured logging. Tests: migration up/down in disposable PostgreSQL, model constraints, config validation, health/liveness.
3. **Identity and access:** implement user auth choice, device enrollment/provisioning, user/device ownership, broker service identity, ACLs, key lifecycle. Tests: unauthorized/cross-user access denial, revoked device denial, secret redaction.
4. **MQTT ingestion:** add TLS broker integration and versioned parsing, idempotent event/sample insert, session association, bounds/rate/size checks, event state transitions. Tests: TLS/ACL integration, malformed payload rejection, duplicate QoS delivery, out-of-order sequence, reconnect and stale presence.
5. **REST and command orchestration:** implement authenticated list/create/stop/detail/history/report routes and command outbox/publisher. Tests: idempotency, only explicit Start/Stop requests, HTTP/MQTT/device ack distinction, expired/replayed command, timeout and uncertain state.
6. **Live updates and frontend integration:** implement SSE with event IDs/reconnect and typed client; connect Start/Stop, device state, live samples, history, reports without changing the existing React app wholesale. Tests: OpenAPI/type compatibility, Zod validation, SSE reconnect/replay, offline/gap UX, demo isolation, browser authorization.
7. **Deterministic reports and operational readiness:** generate from immutable validated rows with generator/version and data watermark; add retention, monitoring, backup/restore, deployment TLS, alerting, and data access policy. Tests: identical input/version gives byte-stable canonical output; partial/gapped input labels gaps; unavailable pressures stay unavailable; restore/retention checks.

Do not add tests until implementation work begins; the test list above is the recommended validation plan.

## 11. Unresolved questions and blockers

### Blockers to a safe, compatible end-to-end implementation

1. The current firmware cannot communicate with the proposed backend: no Wi-Fi/MQTT transport, provisioning, TLS identity, telemetry envelope, device ID, stable sequence/session ID, BLE service, or cloud commands.
2. There is no pressure sensor acquisition or pressure conversion in the inspected firmware; pressure is explicitly marked invalid. Acoustic candidates are not clinical blood-pressure readings. The requested pressure telemetry and pressure report cannot be safely implemented from current data.
3. There is no actual measurement session state machine or remote Start/Stop contract. `startDeflation()` currently initializes detector analysis only. A future implementation needs explicit state acknowledgements and hardware safety verification; no cloud control may substitute for that.
4. There is no broker, hosting, or secret/certificate provisioning setup. Phase 5 adds a minimal owner bearer-token model and ownership relationship, but the frontend has no sign-in flow and no production identity provider has been selected.

### Decisions needed before protocol freeze

- What exact hardware revision and pressure sensor/pump/valve are intended, and what independent fail-safe limits/behavior are verified?
- Will device use Wi-Fi directly, or will a phone/native app bridge device data? Which platforms are in scope? The existing web app has no native bridge.
- Who provisions devices and binds them to accounts? Which identity provider and account recovery model?
- Is acoustic diagnostic telemetry (250 rows/s) needed in the cloud, or should only event summaries be sent? What retention and expected volume are acceptable?
- Should a session with a network gap be retained as partial, and what data quality threshold permits report generation?
- Do reports need downloadable PDF/CSV, or is deterministic JSON sufficient for the first API phase?
- What timezone/source-of-truth timestamp policy, broker/hosting provider, deployment environment, CORS origins, and production domains should be used?

These questions block end-to-end real-device operation. The backend API can be exercised with mocked transport and local test data, but cannot receive compatible telemetry, pressure records, or remotely control the inspected ESP32 until firmware and infrastructure are extended and verified.

## 12. Implementation status after Phase 5

The backend now includes owner credentials (hashed at rest, created/rotated by the offline CLI), device ownership, session lifecycle routes, idempotent session creation, authorized Start/Stop command publication, stored event history and an SSE stream. Session history and sample pages are owner-scoped. Commands are not considered accepted/completed until a correlated device response is processed; stale state or missing broker/device confirmation remains an explicit failure/uncertain state. Session event/sample responses separate raw ADC diagnostics, processed scalar signal fields, detected acoustic events, and unavailable derived measurements.

Pressure sample ingestion is disabled unless an operator has explicitly confirmed device calibration and units. The current v1 MQTT contract has no pressure-sample reference from an acoustic event, so event-level pressure association is rejected. Device timestamps are preserved separately from server receipt timestamps; five-minute wall-clock skew is flagged, sequence regressions are retained and marked, and sequence gaps are surfaced as provisional. None of these proposed MQTT contracts are emitted by the inspected ESP32 firmware. No broker or PostgreSQL integration was available for live validation; mocked transport and local SQLite migration tests cover backend behavior only.

The API does not yet integrate with the frontend's demo-only adapter and no user sign-in/token delivery flow exists. Device provisioning, real firmware protocol design, hardware safety verification, broker setup, and PostgreSQL integration remain blocking work for end-to-end operation.

## 13. Phase 6 implementation status — deterministic reports

Added a dedicated report service and owner-scoped `POST/GET /api/v1/sessions/{session_id}/report` plus `GET .../report.pdf`. Report snapshots are persisted as JSON with SHA-256 content hash, report/generator versions, validity status, input row count and sequence watermark. The PDF is rendered from the immutable snapshot. Repeated generation for the same report version is idempotent. Reports use stored pressure only when device calibration is confirmed and unit is mmHg; heart rate is summarized only from valid samples. No pressure estimator exists, so systolic/diastolic remain explicitly unavailable. Envelope charts use stored samples without interpolation. The report labels experimental validity, missing data, flagged quality, and configured truncation limits. Added Alembic migration 0004 and synthetic-fixture tests. This does not unblock real-device reports: the firmware currently emits no network telemetry, pressure, or heart rate.

## 14. Phase 7 implementation status — frontend integration

The existing React SPA and its design/routes are retained. Added a typed Zod-validated API client for health/readiness, device records/status, session create/detail/history/completion/cancel, samples/events, report snapshots, PDF downloads, and authenticated fetch-based SSE. Added owner-scoped device registration/list/detail endpoints: registration creates an MQTT-disabled/offline record and is not device credential provisioning. The SPA supports in-memory owner-token entry, remote-device freshness, Start/Stop confirmation waits, uncertainty, bounded sample rendering, telemetry stale/gap labels, owner-scoped history/pagination and report download/limitations. Explicit demo mode stays local and is never used as an API-failure fallback. Real remote operation remains blocked by the inspected firmware's lack of Wi-Fi/MQTT, session control, pressure and heart-rate support, plus no provisioned broker/test device or production identity provider.
