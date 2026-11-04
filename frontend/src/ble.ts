export const PHONE_BLE = {
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

export class PhoneBleClient {
  private device?: BleDevice
  private server?: GattServer
  private chars = new Map<string, Characteristic>()
  private listener?: (event: PhoneBleEvent) => void
  private pending = new Map<string, { resolve: () => void; reject: (error: Error) => void; timer: number }>()
  private intentionalDisconnect = false
  private reconnectAttempt = 0
  private reconnectTimer?: number
  private disconnected = () => {
    this.server = undefined; this.chars.clear()
    for (const waiter of this.pending.values()) { window.clearTimeout(waiter.timer); waiter.reject(new Error('Device disconnected before the controller acknowledged the command.')) }
    this.pending.clear(); this.listener?.({ kind: 'disconnected', value: {} })
    if (!this.intentionalDisconnect && this.device) this.scheduleReconnect()
  }

  async connect(listener: (event: PhoneBleEvent) => void) {
    const bluetooth = (navigator as Navigator & { bluetooth?: BluetoothApi }).bluetooth
    if (!bluetooth) throw new Error('Bluetooth access is unavailable here. Open this app in a Web Bluetooth-enabled browser over HTTPS.')
    this.listener = listener
    const device = await bluetooth.requestDevice({ filters: [{ services: [PHONE_BLE.service] }], optionalServices: [PHONE_BLE.service] })
    this.device = device
    this.intentionalDisconnect = false
    this.reconnectAttempt = 0
    device.removeEventListener('gattserverdisconnected', this.disconnected)
    device.addEventListener('gattserverdisconnected', this.disconnected)
    return this.attach(device)
  }

  private async attach(device: BleDevice) {
    const server = await device.gatt?.connect()
    if (!server) throw new Error('The selected device does not expose a GATT server.')
    this.server = server
    const service = await server.getPrimaryService(PHONE_BLE.service)
    for (const key of ['deviceInfo', 'state', 'pressure', 'heartRate', 'korotkoff', 'progress', 'command', 'response', 'fault'] as const) {
      const characteristic = await service.getCharacteristic(PHONE_BLE[key])
      this.chars.set(key, characteristic)
      if (key !== 'command' && key !== 'deviceInfo') {
        characteristic.addEventListener('characteristicvaluechanged', (event: Event) => {
          const view = (event as ValueEvent).target.value
          if (!view) return
          try {
            const value = JSON.parse(new TextDecoder().decode(view)) as Record<string, unknown>
            if (key === 'response' && typeof value.command_id === 'string') {
              const waiter = this.pending.get(value.command_id)
              if (waiter) { window.clearTimeout(waiter.timer); this.pending.delete(value.command_id); value.ok === true ? waiter.resolve() : waiter.reject(new Error(String(value.error_code ?? 'The controller rejected the command.'))) }
            }
            this.listener?.({ kind: key, value })
          } catch { /* Ignore malformed firmware notifications. */ }
        })
        await characteristic.startNotifications()
      }
    }
    const info = await this.chars.get('deviceInfo')?.readValue()
    if (info) this.listener?.({ kind: 'deviceInfo', value: JSON.parse(new TextDecoder().decode(info)) as Record<string, unknown> })
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
      void this.attach(device).catch(() => { if (!this.intentionalDisconnect && this.device) this.scheduleReconnect() })
    }, delay)
  }

  disconnect() {
    this.intentionalDisconnect = true
    if (this.reconnectTimer !== undefined) window.clearTimeout(this.reconnectTimer)
    const device = this.device
    device?.gatt?.disconnect(); device?.removeEventListener('gattserverdisconnected', this.disconnected)
    this.device = undefined; this.server = undefined; this.chars.clear()
    for (const waiter of this.pending.values()) { window.clearTimeout(waiter.timer); waiter.reject(new Error('Device disconnected before the controller acknowledged the command.')) }
    this.pending.clear()
  }
  get connected() { return this.server?.connected === true }

  async send(command: 'GET_STATUS' | 'START_MEASUREMENT' | 'STOP_MEASUREMENT' | 'REQUEST_LIVE_DATA' | 'GET_DEVICE_INFO') {
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
}

export const phoneBle = new PhoneBleClient()
