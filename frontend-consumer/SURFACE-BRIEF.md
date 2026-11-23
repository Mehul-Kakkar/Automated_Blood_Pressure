# Consumer surface brief

- **Job and audience:** Let a home user explicitly start a device session, know whether the controller is ready, and find session history without seeing developer controls. Operate mode; mobile use is primary.
- **Outcome and proof:** A user can connect by the existing BLE GATT contract or sign in and use an already-enrolled, online cloud device. Success is an acknowledged session lifecycle and a dated session record, never an invented pressure result.
- **Direction:** Inherit the Device Companion warm stone and brick-red system. Today gives the red START control the focal point; readiness and the explicit user gesture surround it. History is a calm chronological list; device setup and account details stay on their own surfaces. A compact top bar and sidebar adapt to bottom navigation on phones.
- **Scope and boundaries:** New independent Vite/React application at `frontend-consumer`; no changes to the existing frontend, backend, firmware, or database. No device provisioning, sensor diagnostics, raw values, clinical labels, or estimated systolic/diastolic values.
- **States and interactions:** First-use connection guidance, empty history, busy, disconnected/offline, session progress, confirmed completion, network/auth errors, and local Bluetooth history. BLE Start/Stop is explicit and acknowledgement-gated; cloud Start/Stop follows the existing session lifecycle.
- **Constraints:** Existing FastAPI contracts and `VITE_API_BASE_URL`; bearer auth held in tab session storage. React 19, TypeScript, Vite, browser Web Bluetooth when available; keyboard and screen-reader support; responsive at 360, 390, 768, and 1440px.

## Reference research and design rationale

The public OMRON Connect page describes an at-a-glance health dashboard, data backup, diary tracking, detailed visual insights, and progress trends. The Apple Health page foregrounds a few understandable health topics and presents insights over time while making privacy salient. Withings describes measurement history as part of a broader long-term health picture and emphasizes connecting measurement devices to an app.

These references informed three principles, not a copy of their product UI: put one clear measurement action first; make history chronological and easy to scan; keep privacy and device readiness visible. This prototype does not have verified BP readings or validated insight logic, so the interface deliberately does not imitate their BP number cards, target ranges, classifications, or trend charts. Research was limited to the referenced public pages; no native apps or screenshots were inspected.
