export const PHONE_BLE = {
  // Preserve the original GATT service and characteristics for existing grants.
  service: '4b505001-6b70-4c46-9c87-687572617465',
  deviceInfo: '4b505002-6b70-4c46-9c87-687572617465',
  state: '4b505003-6b70-4c46-9c87-687572617465',
  pressure: '4b505004-6b70-4c46-9c87-687572617465',
  heartRate: '4b505005-6b70-4c46-9c87-687572617465',
  korotkoff: '4b505006-6b70-4c46-9c87-687572617465',
  progress: '4b505007-6b70-4c46-9c87-687572617465',
  command: '4b505008-6b70-4c46-9c87-687572617465',
  response: '4b505009-6b70-4c46-9c87-687572617465',
  fault: '4b50500a-6b70-4c46-9c87-687572617465',
  wifi: '4b50500b-6b70-4c46-9c87-687572617465',
} as const

type ValueEvent = Event & { target: EventTarget & { value?: DataView } }
type Characteristic = EventTarget & {
  value?: DataView
  readValue(): Promise<DataView>
  startNotifications(): Promise<Characteristic>
  writeValueWithResponse(value: BufferSource): Promise<void>
}
type GattService = { getCharacteristic(uuid: string): Promise<Characteristic> }
type GattServer = { connected: boolean; connect(): Promise<GattServer>; getPrimaryService(uuid: string): Promise<GattService>; disconnect(): void }
type BleDevice = EventTarget & { name?: string; gatt?: GattServer }
type BluetoothApi = { requestDevice(options: unknown): Promise<BleDevice> }
export type PhoneBleEvent = { kind: string; value: Record<string, unknown> }

async function withBleTimeout<T>(operation: Promise<T>, label: string, timeoutMs = 8000): Promise<T> {
  let timer = 0
  try {
    return await Promise.race([
      operation,
      new Promise<T>((_, reject) => { timer = window.setTimeout(() => reject(new Error(`Timed out while ${label}. Check the controller is powered and nearby, then reconnect.`)), timeoutMs) }),
    ])
  } finally { window.clearTimeout(timer) }
}

function isGattAuthorizationError(error: unknown) {
  const name = error instanceof Error ? error.name : ''
  const message = error instanceof Error ? error.message.toLowerCase() : String(error).toLowerCase()
  return name === 'NotAllowedError' || name === 'SecurityError' || message.includes('not authorized') || message.includes('not permitted')
}

export class PhoneBleClient {
  private device?: BleDevice
  private server?: GattServer
  private chars = new Map<string, Characteristic>()
  private valueListeners = new Map<Characteristic, (event: Event) => void>()
  private legacyMicPollTimer?: number
  private lastLegacyMicTimestamp?: number
  private legacyMicReadPending = false
  private waveformAvailable = false
  private lastWaveformNotificationAt = 0
  private measurementActive = false
  private listener?: (event: PhoneBleEvent) => void
  private cancelPicker?: () => void
  private pending = new Map<string, { resolve: () => void; reject: (error: Error) => void; timer: number }>()
  private intentionalDisconnect = false
  private connectGeneration = 0
  private reconnectAttempt = 0
  private reconnectTimer?: number
  private disconnected = () => {
    this.server = undefined; this.clearCharacteristics()
    for (const waiter of this.pending.values()) { window.clearTimeout(waiter.timer); waiter.reject(new Error('Device disconnected before the controller acknowledged the command.')) }
    this.pending.clear(); this.listener?.({ kind: 'disconnected', value: {} })
    if (!this.intentionalDisconnect && this.device) this.scheduleReconnect()
  }

  async connect(listener: (event: PhoneBleEvent) => void) {
    const generation = ++this.connectGeneration
    const bluetooth = (navigator as Navigator & { bluetooth?: BluetoothApi }).bluetooth
    if (!bluetooth) throw new Error('Bluetooth access is unavailable here. Open this app in a Web Bluetooth-enabled browser over HTTPS.')
    this.listener = listener
    this.listener({ kind: 'connectionProgress', value: { message: 'Choose BP Controller in the browser Bluetooth picker.' } })
    const picker = bluetooth.requestDevice({
      filters: [{ services: [PHONE_BLE.service] }],
      optionalServices: [PHONE_BLE.service],
    })
    const device = await Promise.race([picker, new Promise<BleDevice>((_, reject) => {
      this.cancelPicker = () => reject(new DOMException('Bluetooth connection cancelled.', 'AbortError'))
    })])
    this.cancelPicker = undefined
    if (generation !== this.connectGeneration) throw new DOMException('Bluetooth connection cancelled.', 'AbortError')
    this.device = device
    this.intentionalDisconnect = false
    this.reconnectAttempt = 0
    device.removeEventListener('gattserverdisconnected', this.disconnected)
    device.addEventListener('gattserverdisconnected', this.disconnected)
    return this.attach(device)
  }

  cancelConnect() {
    this.connectGeneration++
    this.intentionalDisconnect = true
    this.cancelPicker?.()
    this.cancelPicker = undefined
    this.device?.gatt?.disconnect()
    this.device = undefined
    this.server = undefined
    this.clearCharacteristics()
  }

  private async attach(device: BleDevice) {
    this.clearCharacteristics()
    this.listener?.({ kind: 'connectionProgress', value: { message: 'Connecting to the controller over Bluetooth…' } })
    const server = await withBleTimeout(device.gatt?.connect() ?? Promise.reject(new Error('The selected device does not expose a GATT server.')), 'connecting to the controller')
    if (!server) throw new Error('The selected device does not expose a GATT server.')
    this.server = server
    this.listener?.({ kind: 'transportConnected', value: {} })
    this.listener?.({ kind: 'connectionProgress', value: { message: 'Bluetooth connected. Discovering controller services…' } })
    const service = await withBleTimeout(server.getPrimaryService(PHONE_BLE.service), 'discovering the controller service')
    for (const key of ['deviceInfo', 'state', 'pressure', 'heartRate', 'korotkoff', 'progress', 'command', 'response', 'fault', 'wifi'] as const) {
      let characteristic: Characteristic
      try {
        this.listener?.({ kind: 'connectionProgress', value: { message: 'Discovering controller BLE characteristics…' } })
        characteristic = await withBleTimeout(service.getCharacteristic(PHONE_BLE[key]), `discovering the ${key} characteristic`)
      } catch (error) {
        if (key === 'wifi') {
          const message = isGattAuthorizationError(error)
            ? 'BLE is connected. Pair with the controller before using Wi-Fi provisioning.'
            : 'BLE is connected, but this browser GATT cache does not expose Wi-Fi provisioning. Measurement controls remain available.'
          this.listener?.({ kind: 'wifiNotificationUnavailable', value: { message } })
          continue
        }
        throw error
      }
      this.chars.set(key, characteristic)
      if (key !== 'command' && key !== 'deviceInfo') {
        const valueListener = (event: Event) => {
          const view = (event as ValueEvent).target.value
          if (!view) return
          if (key === 'korotkoff' && view.byteLength >= 10) {
            const packet = new DataView(view.buffer, view.byteOffset, view.byteLength)
            const version = packet.getUint8(0)
            const count = packet.getUint8(1)
            if ((version === 1 && count > 0 && count <= 80 && view.byteLength === 10 + count * 2)
              || (version === 2 && count > 0 && count <= 42 && view.byteLength === 12 + count * 4)) {
              this.lastWaveformNotificationAt = Date.now()
              if (!this.waveformAvailable) {
                this.waveformAvailable = true
                this.listener?.({ kind: 'waveformAvailable', value: {} })
              }
              const firstSequence = packet.getUint32(2, true)
              const firstTimestampUs = packet.getUint32(6, true)
              const batchThreshold = version === 2 ? packet.getUint16(10, true) / 4 : undefined
              const samples = new Array<number | { filtered: number; envelope?: number; threshold?: number }>(count)
              for (let i = 0; i < count; i++) {
                if (version === 1) samples[i] = packet.getInt16(10 + i * 2, true) / 16
                else samples[i] = { filtered: packet.getInt16(12 + i * 4, true) / 16, envelope: packet.getUint16(14 + i * 4, true) / 4, threshold: batchThreshold }
              }
              this.listener?.({ kind: 'waveform', value: { version, firstSequence, firstTimestampUs, samples } })
              return
            }
          }
          try {
            const value = JSON.parse(new TextDecoder().decode(view)) as Record<string, unknown>
            if (key === 'korotkoff' && typeof value.device_timestamp_ms === 'number') this.lastLegacyMicTimestamp = value.device_timestamp_ms
            if (key === 'state') this.measurementActive = ['precheck', 'inflating', 'measuring', 'deflating'].includes(String(value.state ?? ''))
            if (key === 'response' && typeof value.command_id === 'string') {
              const waiter = this.pending.get(value.command_id)
              if (waiter) { window.clearTimeout(waiter.timer); this.pending.delete(value.command_id); value.ok === true ? waiter.resolve() : waiter.reject(new Error(String(value.error_code ?? 'The controller rejected the command.'))) }
            }
            this.listener?.({ kind: key, value })
          } catch { /* Ignore malformed firmware notifications. */ }
        }
        characteristic.addEventListener('characteristicvaluechanged', valueListener)
        this.valueListeners.set(characteristic, valueListener)
        try {
          await withBleTimeout(characteristic.startNotifications(), `enabling ${key} notifications`)
        } catch (error) {
          if (key !== 'wifi' || !isGattAuthorizationError(error)) throw error
          this.listener?.({ kind: 'wifiNotificationUnavailable', value: { message: 'BLE is connected. Pair with the controller before using Wi-Fi provisioning.' } })
        }
      }
    }
    // Older firmware has no batched waveform characteristic. Poll its readable
    // live-sample characteristic so the chart still receives real samples.
    const legacyMic = this.chars.get('korotkoff')
    if (legacyMic) {
      this.legacyMicPollTimer = window.setInterval(() => {
        if (!this.connected || !this.measurementActive || this.legacyMicReadPending || (this.waveformAvailable && Date.now() - this.lastWaveformNotificationAt < 750)) return
        this.legacyMicReadPending = true
        void legacyMic.readValue().then(view => {
          try {
            const value = JSON.parse(new TextDecoder().decode(view)) as Record<string, unknown>
            const timestamp = value.device_timestamp_ms
            if (typeof timestamp !== 'number' || timestamp === this.lastLegacyMicTimestamp) return
            this.lastLegacyMicTimestamp = timestamp
            this.listener?.({ kind: 'korotkoff', value })
          } catch { /* Ignore incomplete legacy sample values. */ }
        }).catch(() => undefined).finally(() => { this.legacyMicReadPending = false })
      }, 250)
    }
    const infoCharacteristic = this.chars.get('deviceInfo')
    const info = infoCharacteristic ? await withBleTimeout(infoCharacteristic.readValue(), 'reading controller information') : undefined
    if (info) this.listener?.({ kind: 'deviceInfo', value: JSON.parse(new TextDecoder().decode(info)) as Record<string, unknown> })
    this.reconnectAttempt = 0
    this.listener?.({ kind: 'connected', value: { name: device.name ?? 'BP Controller' } })
    void this.send('GET_STATUS').catch(error => this.listener?.({ kind: 'error', value: { message: error instanceof Error ? error.message : 'Could not read controller status.' } }))
    return device.name ?? 'BP Controller'
  }

  private scheduleReconnect() {
    if (this.reconnectTimer !== undefined) window.clearTimeout(this.reconnectTimer)
    const delay = Math.min(1000 * (2 ** Math.min(this.reconnectAttempt++, 5)), 30000)
    this.listener?.({ kind: 'reconnecting', value: { attempt: this.reconnectAttempt } })
    this.reconnectTimer = window.setTimeout(() => {
      const device = this.device
      if (!device || this.intentionalDisconnect) return
      this.reconnectTimer = undefined
      void this.attach(device).catch(() => { if (!this.intentionalDisconnect && this.device) this.scheduleReconnect() })
    }, delay)
  }

  disconnect() {
    this.intentionalDisconnect = true
    if (this.reconnectTimer !== undefined) window.clearTimeout(this.reconnectTimer)
    const device = this.device
    device?.gatt?.disconnect(); device?.removeEventListener('gattserverdisconnected', this.disconnected)
    this.device = undefined; this.server = undefined; this.clearCharacteristics()
    for (const waiter of this.pending.values()) { window.clearTimeout(waiter.timer); waiter.reject(new Error('Device disconnected before the controller acknowledged the command.')) }
    this.pending.clear()
  }
  private clearCharacteristics() {
    if (this.legacyMicPollTimer !== undefined) window.clearInterval(this.legacyMicPollTimer)
    this.legacyMicPollTimer = undefined
    this.lastLegacyMicTimestamp = undefined
    this.legacyMicReadPending = false
    this.waveformAvailable = false
    this.lastWaveformNotificationAt = 0
    this.measurementActive = false
    for (const [characteristic, listener] of this.valueListeners) characteristic.removeEventListener('characteristicvaluechanged', listener)
    this.valueListeners.clear()
    this.chars.clear()
  }
  get connected() { return this.server?.connected === true }

  async send(command: 'GET_STATUS' | 'START_MEASUREMENT' | 'STOP_MEASUREMENT' | 'RESET_FAULT' | 'REQUEST_LIVE_DATA' | 'GET_DEVICE_INFO') {
    const characteristic = this.chars.get('command')
    if (!this.connected || !characteristic) throw new Error('Connect to the controller over BLE first.')
    const commandId = crypto.randomUUID()
    const packet = new TextEncoder().encode(JSON.stringify({ v: 1, command_id: commandId, command }))
    const acknowledged = new Promise<void>((resolve, reject) => {
      const timer = window.setTimeout(() => { this.pending.delete(commandId); reject(new Error('The controller did not acknowledge the command.')) }, 5000)
      this.pending.set(commandId, { resolve, reject, timer })
    })
    try { await characteristic.writeValueWithResponse(packet); await acknowledged }
    catch (error) { const waiter = this.pending.get(commandId); if (waiter) window.clearTimeout(waiter.timer); this.pending.delete(commandId); throw error }
  }

  async wifiCommand(command: 'SCAN_WIFI' | 'SET_WIFI', details: { ssid?: string; password?: string } = {}) {
    const characteristic = this.chars.get('wifi')
    if (!this.connected || !characteristic) throw new Error('Connect to the controller over BLE first.')
    const commandId = crypto.randomUUID()
    const packet = new TextEncoder().encode(JSON.stringify({ v: 1, command_id: commandId, command, ...details }))
    const acknowledged = new Promise<void>((resolve, reject) => {
      const timer = window.setTimeout(() => { this.pending.delete(commandId); reject(new Error('The controller did not acknowledge Wi-Fi setup.')) }, 30000)
      this.pending.set(commandId, { resolve, reject, timer })
    })
    try { await characteristic.writeValueWithResponse(packet); await acknowledged }
    catch (error) { const waiter = this.pending.get(commandId); if (waiter) window.clearTimeout(waiter.timer); this.pending.delete(commandId); throw error }
  }
}

export const phoneBle = new PhoneBleClient()
