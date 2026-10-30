# Automated Blood Pressure Detection System

An experimental automated blood-pressure monitoring and signal-analysis project developed by **Group 21** for the Design Practicum at the **Indian Institute of Technology Mandi**.

The repository contains the frontend application, FastAPI backend, PostgreSQL persistence layer, ESP32 signal-processing experiments, and a separate ESP32-based display prototype.

> **Project status:** Experimental prototype under development. The frontend and backend have substantial functionality, while the main ESP32 firmware, pressure acquisition, device communication, and complete end-to-end integration remain works in progress.

> **Disclaimer:** This project is not a certified medical device. It has not undergone clinical validation and must not be used for diagnosis, treatment, or medical decisions.

---

## Contents

- [Overview](#overview)
- [System architecture](#system-architecture)
- [Repository structure](#repository-structure)
- [Technology stack](#technology-stack)
- [Hardware and firmware](#hardware-and-firmware)
- [Frontend](#frontend)
- [Backend](#backend)
- [Database](#database)
- [Running the project](#running-the-project)
- [Environment configuration](#environment-configuration)
- [Testing](#testing)
- [Current implementation status](#current-implementation-status)
- [Known limitations](#known-limitations)
- [Development guidelines](#development-guidelines)

---

## Overview

The goal of this project is to develop an experimental blood-pressure monitoring system using an embedded controller, acoustic signal processing, a pneumatic cuff system, and a companion application.

The intended system is composed of four major parts:

1. **Embedded controller:** Acquires sensor data, processes acoustic signals, manages the measurement process, and controls the pneumatic hardware.
2. **Display controller:** Provides a local touchscreen interface for interacting with and monitoring the device.
3. **Backend:** Provides authentication, device management, session management, telemetry persistence, and report generation.
4. **Frontend:** Provides a mobile-first interface for device management, measurement monitoring, history, and reports.

The software is designed to keep device-side measurement and safety behavior independent of network connectivity. The backend and frontend provide additional monitoring and data-management capabilities.

## System architecture

The intended architecture is:

```text
                  ┌──────────────────────┐
                  │   React Frontend     │
                  │                      │
                  │ Dashboard / History  │
                  │ Device / Settings    │
                  │ Reports / Profiles   │
                  └──────────┬───────────┘
                             │
                         HTTPS / REST
                         SSE (live data)
                             │
                  ┌──────────▼───────────┐
                  │    FastAPI Backend   │
                  │                      │
                  │ Authentication       │
                  │ Device Management    │
                  │ Session Management   │
                  │ Telemetry Validation │
                  │ Report Generation    │
                  └──────────┬───────────┘
                             │
                      SQLAlchemy ORM
                             │
                  ┌──────────▼───────────┐
                  │     PostgreSQL       │
                  │                      │
                  │ Owners / Devices     │
                  │ Sessions / Samples   │
                  │ Events / Reports     │
                  └──────────────────────┘


       Future device communication architecture

                  ┌──────────────────────┐
                  │   Display ESP32      │
                  │   Touchscreen UI     │
                  └──────────┬───────────┘
                             │
                          ESP-NOW
                             │
                  ┌──────────▼───────────┐
                  │   Main ESP32-S3      │
                  │                      │
                  │ Sensor Acquisition   │
                  │ Acoustic Processing  │
                  │ Pressure Processing  │
                  │ Device State Machine │
                  │ Hardware Safety      │
                  └──────────┬───────────┘
                             │
                         MQTT over TLS
                             │
                       MQTT Broker
                             │
                       FastAPI Backend
```

The device communication architecture shown above represents the intended integration. The current firmware sketches do not yet implement the complete communication flow.

---

## Repository structure

```text
Automated_Blood_Pressure/
│
├── DisplayCode/
│   ├── displayConnectionTest/
│   │   └── displayConnectionTest.ino
│   │
│   └── displayTest/
│       ├── displayTest.ino
│       └── lv_conf.h
│
├── ESP Code Main/
│   ├── connectionTest/
│   │   └── connectionTest.ino
│   │
│   ├── espnowTest/
│   │   └── espnowTest.ino
│   │
│   ├── korotkoffDebug/
│   │   ├── debug.py
│   │   ├── dependencies.txt
│   │   ├── korotkoffDebug.ino
│   │   └── korotkoff_detector.h
│   │
│   ├── korotkoffTest/
│   │   ├── korotkoffTest.ino
│   │   └── korotkoff_detector.h
│   │
│   └── pumpvalveTest/
│       └── pumpvalveTest.ino
│
├── backend/
│   ├── app/
│   │   ├── api/
│   │   │   ├── routes/
│   │   │   │   ├── auth.py
│   │   │   │   ├── devices.py
│   │   │   │   ├── health.py
│   │   │   │   ├── profile.py
│   │   │   │   ├── reports.py
│   │   │   │   └── sessions.py
│   │   │   └── router.py
│   │   │
│   │   ├── core/
│   │   │   ├── config.py
│   │   │   ├── errors.py
│   │   │   ├── logging.py
│   │   │   └── middleware.py
│   │   │
│   │   ├── db/
│   │   │   ├── base.py
│   │   │   └── session.py
│   │   │
│   │   ├── messaging/
│   │   │   ├── schemas.py
│   │   │   ├── service.py
│   │   │   ├── topics.py
│   │   │   └── transport.py
│   │   │
│   │   ├── models/
│   │   │   ├── device.py
│   │   │   ├── device_command.py
│   │   │   ├── enums.py
│   │   │   ├── measurement_session.py
│   │   │   ├── owner.py
│   │   │   ├── report.py
│   │   │   ├── session_event.py
│   │   │   └── telemetry.py
│   │   │
│   │   ├── schemas/
│   │   │   ├── auth.py
│   │   │   ├── devices.py
│   │   │   ├── errors.py
│   │   │   ├── health.py
│   │   │   ├── messaging.py
│   │   │   ├── profile.py
│   │   │   ├── reports.py
│   │   │   ├── sessions.py
│   │   │   └── telemetry.py
│   │   │
│   │   ├── security/
│   │   │   ├── auth.py
│   │   │   └── passwords.py
│   │   │
│   │   ├── services/
│   │   │   ├── database.py
│   │   │   ├── reports.py
│   │   │   ├── session_lifecycle.py
│   │   │   └── telemetry.py
│   │   │
│   │   ├── cli.py
│   │   └── main.py
│   │
│   ├── migrations/
│   │   ├── versions/
│   │   │   ├── 0001_initial_persistence.py
│   │   │   ├── 0002_secure_mqtt_commands.py
│   │   │   ├── 0003_owners_session_events.py
│   │   │   ├── 0004_report_snapshot.py
│   │   │   ├── 0005_username_primary_key_profile.py
│   │   │   └── 0006_password_auth.py
│   │   ├── env.py
│   │   └── script.py.mako
│   │
│   ├── tests/
│   │   ├── conftest.py
│   │   ├── test_auth.py
│   │   ├── test_config.py
│   │   ├── test_devices_api.py
│   │   ├── test_health.py
│   │   ├── test_messaging.py
│   │   ├── test_migrations.py
│   │   ├── test_persistence.py
│   │   ├── test_postgres_integration.py
│   │   ├── test_profile.py
│   │   ├── test_reports.py
│   │   ├── test_sessions_api.py
│   │   └── test_validation_and_errors.py
│   │
│   ├── .env.example
│   ├── .dockerignore
│   ├── .gitignore
│   ├── Dockerfile
│   ├── IMPLEMENTATION_PLAN.md
│   ├── README.md
│   ├── alembic.ini
│   ├── requirements-dev.txt
│   └── requirements.txt
│
├── frontend/
│   ├── public/
│   │   └── icon.svg
│   │
│   ├── src/
│   │   ├── App.tsx
│   │   ├── api.ts
│   │   ├── api.test.ts
│   │   ├── domain.ts
│   │   ├── domain.test.ts
│   │   ├── main.tsx
│   │   ├── pwa.d.ts
│   │   ├── remote.test.ts
│   │   ├── store.ts
│   │   ├── store.test.ts
│   │   └── styles.css
│   │
│   ├── .env.example
│   ├── .gitignore
│   ├── README.md
│   ├── index.html
│   ├── package.json
│   ├── package-lock.json
│   ├── postcss.config.js
│   ├── tsconfig.app.json
│   ├── tsconfig.json
│   ├── tsconfig.node.json
│   ├── vercel.json
│   ├── vite.config.ts
│   └── vitest.config.ts
│
├── .gitignore
├── docker-compose.yml
├── heart_sound_plot.py
├── test.py
└── README.md
```

### Directory responsibilities

| Directory / file | Responsibility |
|---|---|
| `DisplayCode/` | ESP32 touchscreen and display experiments |
| `ESP Code Main/` | Main controller experiments, signal processing, communication and pneumatic hardware tests |
| `backend/app/api/` | HTTP API endpoints and routing |
| `backend/app/core/` | Configuration, logging, middleware and error handling |
| `backend/app/db/` | Database engine and session management |
| `backend/app/messaging/` | MQTT transport, topics, message schemas and service |
| `backend/app/models/` | SQLAlchemy database models |
| `backend/app/schemas/` | Pydantic request and response schemas |
| `backend/app/security/` | Authentication and password handling |
| `backend/app/services/` | Session lifecycle, telemetry, database and report logic |
| `backend/migrations/` | Alembic database migrations |
| `backend/tests/` | Backend unit and integration tests |
| `frontend/src/` | React application, API client, domain logic and state management |
| `heart_sound_plot.py` | Standalone Python acoustic signal plotting and heart-rate experiment |
| `test.py` | Basic serial communication test |

---

## Technology stack

### Embedded hardware

- ESP32 / ESP32-S3 development boards
- Arduino framework
- ESP-NOW communication experiments
- I²C touchscreen communication
- TFT display and LVGL
- Acoustic signal acquisition and processing
- Pneumatic pump and valve control experiments

### Frontend

| Technology | Purpose |
|---|---|
| React 19 | UI framework |
| TypeScript | Application logic and type safety |
| Vite | Development server and build tooling |
| React Router | Client-side routing |
| Zustand | Application state management |
| Zod | Runtime data validation |
| Recharts | Data visualization |
| Lucide React | Icons |
| Vitest | Frontend testing |
| vite-plugin-pwa | Progressive Web App support |

### Backend

| Technology | Purpose |
|---|---|
| Python | Backend language |
| FastAPI | REST API |
| Pydantic | Request and response validation |
| SQLAlchemy | ORM and database access |
| PostgreSQL | Persistent data storage |
| Alembic | Database migrations |
| Paho MQTT | MQTT client |
| ReportLab | PDF report generation |
| Pytest | Automated testing |

### Infrastructure

- Docker
- Docker Compose
- PostgreSQL 16 container
- Vercel configuration for frontend deployment

---

## Hardware and firmware

The firmware directory currently contains independent experiments rather than one fully integrated firmware application.

### Main controller experiments

#### `korotkoffDebug`

Location: `ESP Code Main/korotkoffDebug/`

This is the primary acoustic debugging sketch.

It includes:

- ESP32 ADC microphone acquisition.
- 2 kHz sampling.
- High-pass and low-pass filtering.
- Acoustic envelope processing.
- Adaptive noise and threshold calculations.
- Korotkoff event detection.
- USB serial diagnostics.
- Python-based live plotting and detector tuning.

The serial output uses the following CSV format:

```text
ms,raw,filtered,envelope,noise,threshold,event
```

The Python plotting utility is:

```text
debug.py
```

Install its dependencies from this directory:

```powershell
pip install -r dependencies.txt
```

Run it with:

```powershell
python debug.py
```

The serial port and baud rate must match the connected ESP32 configuration.

The firmware currently does not send network telemetry or provide pressure readings.

#### `korotkoffTest`

Location: `ESP Code Main/korotkoffTest/`

A separate acoustic detector test sketch that uses the shared `korotkoff_detector.h` implementation and outputs diagnostic information through Serial.

#### `pumpvalveTest`

Location: `ESP Code Main/pumpvalveTest/`

A standalone pump and valve control experiment.

Use this sketch to examine the pneumatic hardware control code independently of the acoustic detector.

#### `connectionTest`

Location: `ESP Code Main/connectionTest/`

A communication and connectivity experiment for the main controller.

#### `espnowTest`

Location: `ESP Code Main/espnowTest/`

An ESP-NOW communication experiment.

### Display experiments

#### `displayTest`

Location: `DisplayCode/displayTest/`

Contains the touchscreen display experiment.

It includes:

- TFT initialization.
- LVGL user interface.
- Touch input handling.
- Start and dashboard screens.
- Basic measurement display placeholders.

The current display sketch is a UI experiment. Its Start button changes the local screen; it does not initiate a physical measurement.

#### `displayConnectionTest`

Location: `DisplayCode/displayConnectionTest/`

A standalone ESP-NOW communication test for the display controller.

It supports basic communication commands and peer discovery experiments.

### Python signal-processing experiments

#### `heart_sound_plot.py`

Standalone Python experiment for:

- Serial acquisition of microphone samples.
- Band-pass filtering from 20–300 Hz.
- Noise estimation and gating.
- Peak detection.
- Candidate beat interval calculation.
- Estimated heart-rate visualization.

#### `test.py`

A basic serial port communication test that checks whether the ESP32 is transmitting data.

---

## Frontend

Location: `frontend/`

The frontend is a mobile-first React application intended to serve as the companion interface for the device.

### Main features

- Dashboard
- Device management
- Measurement session interface
- Measurement history
- User profile
- Settings
- Development demo mode
- Live telemetry interface
- Report viewing and PDF downloads

### Important source files

| File | Responsibility |
|---|---|
| `src/App.tsx` | Main application and page composition |
| `src/api.ts` | Typed backend API client |
| `src/domain.ts` | Domain types and telemetry validation |
| `src/store.ts` | Zustand application state |
| `src/styles.css` | Application styling |
| `src/main.tsx` | Application entry point |

### Frontend setup

Requirements:

- Node.js 20+
- npm 10+

From the repository root:

```powershell
cd frontend
npm install
Copy-Item .env.example .env.local
```

Configure `frontend/.env.local`:

```dotenv
VITE_API_BASE_URL=http://127.0.0.1:8000
VITE_DEMO_MODE=false
```

Start the development server:

```powershell
npm run dev
```

The frontend is available at:

```text
http://localhost:5173
```

### Available scripts

```powershell
npm run dev
npm run build
npm run preview
npm run typecheck
npm test
npm run test:watch
```

**Important:** Variables beginning with `VITE_` are exposed to the browser. Never store passwords, private keys, owner tokens, or MQTT credentials in frontend environment variables.

---

## Backend

Location: `backend/`

The backend is a FastAPI service with PostgreSQL persistence.

### Implemented functionality

- Account registration and login.
- Password hashing and authentication.
- Owner-scoped device registration and management.
- Measurement session lifecycle management.
- Telemetry schema validation.
- Telemetry batch persistence.
- Acoustic event persistence.
- Session event tracking.
- Live session updates through Server-Sent Events.
- Report generation and PDF downloads.
- Optional MQTT messaging infrastructure.
- Health and readiness endpoints.

### Backend API groups

| Route prefix | Purpose |
|---|---|
| `/api/v1/auth` | Registration and login |
| `/api/v1/devices` | Device registration and management |
| `/api/v1/profile` | User profile and account settings |
| `/api/v1/sessions` | Measurement sessions, history and reports |
| `/health` | API liveness |
| `/ready` | API and database readiness |
| `/health/messaging` | MQTT service health |

The full interactive API documentation is available at `/docs` when enabled.

### Backend setup (local Python)

Requirements:

- Python 3.10+
- PostgreSQL 14+ for persistent local operation

From the repository root:

```powershell
cd backend

py -3.10 -m venv .venv
.\.venv\Scripts\Activate.ps1

python -m pip install --upgrade pip
pip install -r requirements-dev.txt

Copy-Item .env.example .env
```

Configure `backend/.env` with the appropriate database URL and CORS origins.

Apply database migrations:

```powershell
alembic upgrade head
```

Start the backend:

```powershell
python -m uvicorn app.main:app --reload --host 127.0.0.1 --port 8000
```

The backend will be available at:

```text
http://127.0.0.1:8000
```

API documentation:

```text
http://127.0.0.1:8000/docs
```

Health checks:

```text
http://127.0.0.1:8000/health
http://127.0.0.1:8000/ready
```

---

## Database

PostgreSQL is used for persistent application data.

The database models cover:

- Owners and authentication credentials.
- Registered devices.
- Measurement sessions.
- Device commands.
- Telemetry batches.
- Individual telemetry samples.
- Acoustic events.
- Session events.
- Generated reports.

Database schema changes are managed using Alembic.

To apply migrations:

```powershell
cd backend
alembic upgrade head
```

To inspect the current migration:

```powershell
alembic current
```

To check whether model changes require a migration:

```powershell
alembic check
```

Do not manually modify the database schema without creating the corresponding migration.

---

## Running with Docker Compose

The repository includes a root-level `docker-compose.yml`.

It defines:

- PostgreSQL 16.
- FastAPI backend.
- Persistent PostgreSQL volume.
- Database health checks.
- Backend dependency on PostgreSQL readiness.

### 1. Configure the root environment

Create a `.env` file in the repository root:

```dotenv
POSTGRES_PASSWORD=your_local_database_password
```

Use a strong local password. Do not commit this file.

### 2. Start the services

From the repository root:

```powershell
docker compose up --build -d
```

### 3. Check running containers

```powershell
docker compose ps
```

### 4. View logs

```powershell
docker compose logs -f backend
```

For PostgreSQL:

```powershell
docker compose logs -f postgres
```

### 5. Access the services

| Service | Address |
|---|---|
| Backend API | `http://localhost:8000` |
| API documentation | `http://localhost:8000/docs` |
| PostgreSQL | `localhost:5433` |

The PostgreSQL container listens internally on port `5432` and is mapped to host port `5433`.

The Compose configuration currently sets MQTT to disabled. Starting these containers does not establish a connection to the ESP32.

### 6. Stop the services

```powershell
docker compose down
```

To also remove the persistent PostgreSQL volume and its data:

```powershell
docker compose down -v
```

**Warning:** The second command permanently removes the Compose database volume.

---

## Environment configuration

There are separate environment configurations for the frontend, backend, and Docker Compose.

| File | Purpose |
|---|---|
| `.env` (repository root) | Docker Compose PostgreSQL password |
| `backend/.env` | Backend configuration and optional database/MQTT settings |
| `frontend/.env.local` | Frontend API URL and development demo setting |

Example files are provided:

- `backend/.env.example`
- `frontend/.env.example`

Never commit real environment files or credentials.

### MQTT configuration

The backend contains an optional MQTT 5 transport using Paho.

It is disabled by default.

Enabling it requires additional infrastructure and provisioning:

- A reachable MQTT broker.
- TLS listener and trusted CA certificate.
- Broker certificate hostname verification.
- A dedicated backend service identity.
- Separate device credentials.
- Per-device topic access-control rules.
- Device enrollment in the backend database.
- Firmware implementing the agreed message protocol.

The backend's MQTT implementation does not automatically provision broker credentials or configure an external broker.

Do not enable MQTT in a deployment until the broker, device identity, ACLs, and firmware protocol have been configured and tested.

---

## Testing

### Backend tests

From the repository root:

```powershell
cd backend
.\.venv\Scripts\Activate.ps1
python -m pytest -q
```

The backend test suite covers authentication, configuration, device APIs, messaging, migrations, persistence, profiles, reports, sessions, and validation.

PostgreSQL-specific integration tests require a dedicated disposable test database.

### Frontend tests

From the repository root:

```powershell
cd frontend
npm test
```

Run TypeScript checks:

```powershell
npm run typecheck
```

Build the production frontend:

```powershell
npm run build
```

### Hardware tests

Hardware testing must be performed separately on the relevant ESP32 boards.

The repository's firmware sketches are independent experiments. A successful frontend build or backend test does not verify sensor accuracy, pressure conversion, pump operation, valve operation, or hardware safety.

---

## Current implementation status

The following table distinguishes existing functionality from work that remains.

| Component | Current status |
|---|---|
| React frontend | Implemented; experimental |
| Frontend API client | Implemented |
| Frontend demo mode | Implemented |
| FastAPI backend | Implemented |
| User authentication | Implemented |
| Device registration | Implemented |
| Session persistence | Implemented |
| Telemetry validation and storage boundary | Implemented |
| Report generation | Implemented with explicit limitations |
| PostgreSQL migrations | Implemented |
| Docker Compose | Configured |
| MQTT backend transport | Implemented as optional infrastructure; disabled by default |
| MQTT broker deployment | Not configured |
| Main ESP32 acoustic detector | Experimental firmware exists |
| Main ESP32 pressure acquisition and calibrated conversion | Not integrated |
| Main ESP32 pneumatic measurement state machine | Not integrated |
| Main ESP32 MQTT communication | Not implemented in the inspected sketches |
| Display touchscreen UI | Experimental sketch exists |
| Display-to-main-controller communication | ESP-NOW test exists; complete integration remains |
| End-to-end hardware/frontend/backend operation | Not completed |
| Validated systolic/diastolic estimator | Not implemented |
| Clinical validation | Not performed |

---

## Known limitations

1. **No complete hardware integration:** The current ESP32 sketches are separate experiments and do not yet form one integrated controller firmware.
2. **No current device-to-backend telemetry connection:** The inspected main acoustic debugging sketch communicates through USB Serial, not MQTT or Wi-Fi.
3. **No validated pressure conversion:** Pressure values must not be treated as mmHg without verified sensor calibration and conversion.
4. **No validated systolic/diastolic estimator:** The backend does not calculate clinically validated systolic or diastolic blood pressure.
5. **No clinical validation:** All readings and reports are experimental.
6. **MQTT requires provisioning:** The backend transport is optional and disabled by default. Broker credentials, device enrollment, TLS and ACL configuration are separate requirements.
7. **Demo data is not device data:** Demo mode generates simulated signals and does not represent real hardware measurements.

---

## Development guidelines

When extending this repository:

- Keep the main ESP32 controller autonomous.
- Do not make local measurement or safety behavior dependent on Wi-Fi, MQTT, BLE, or the display.
- A display or phone connection must never independently trigger a physical measurement.
- Start and stop operations must be explicitly initiated and acknowledged.
- Do not invent GPIO assignments; verify them against the actual board and wiring.
- Preserve existing working signal-processing algorithms unless changes are justified and tested.
- Keep pressure values unavailable until calibration and unit conversion are verified.
- Keep demo data clearly separated from real device data.
- Validate all incoming telemetry at the backend boundary.
- Use database migrations for schema changes.
- Keep credentials and private certificates out of source control.
- Add tests when modifying API, domain, telemetry, or session behavior.
- Do not describe experimental measurements as clinically accurate or validated.

---

## Project information

**Project:** Automated Blood Pressure Detection System  
**Institution:** Indian Institute of Technology Mandi  
**Course:** Design Practicum  
**Group:** 21  

**Repository:** [Automated_Blood_Pressure](https://github.com/Mehul-Kakkar/Automated_Blood_Pressure)

---

*This repository is an ongoing engineering prototype. Documentation and implementation status should be updated as hardware, firmware, backend, and frontend integration progresses.*