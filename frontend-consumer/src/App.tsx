import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { Navigate, NavLink, Route, Routes, useNavigate } from 'react-router-dom'
import { Activity, Bluetooth, CalendarDays, Check, ChevronRight, CircleHelp, Clock3, HeartPulse, House, LogIn, LogOut, Radio, RefreshCw, ShieldCheck, UserRound, Waves, X } from 'lucide-react'
import { PhoneBleClient, type PhoneBleEvent } from '../../frontend/src/ble'
import { ConsumerApiError, consumerApi, type Auth, type Device, type Profile, type Session } from './api'

type LocalRecord = { id: string; startedAt: number; endedAt: number; state: 'completed' | 'incomplete'; source: 'Bluetooth' }
const authKey = 'pulsefield.consumer.auth.v1'
const historyKey = 'pulsefield.consumer.sessions.v1'
const loadJson = <T,>(key: string, fallback: T): T => { try { const value: unknown = JSON.parse(sessionStorage.getItem(key) ?? 'null'); return value === null ? fallback : value as T } catch { return fallback } }
const loadLocalHistory = (): LocalRecord[] => { try { const data: unknown = JSON.parse(localStorage.getItem(historyKey) ?? '[]'); return Array.isArray(data) ? data.filter((x): x is LocalRecord => Boolean(x && typeof x.id === 'string' && Number.isFinite(x.startedAt) && Number.isFinite(x.endedAt) && ['completed','incomplete'].includes(x.state) && x.source === 'Bluetooth')).slice(0, 50) : [] } catch { return [] } }
const saveLocalHistory = (records: LocalRecord[]) => { try { localStorage.setItem(historyKey, JSON.stringify(records)); return true } catch { return false } }
const friendlyError = (error: unknown, fallback: string) => error instanceof ConsumerApiError ? error.message : error instanceof Error && error.name === 'NotAllowedError' ? 'Bluetooth access was cancelled. Connect whenever you are ready.' : fallback
const formatDate = (date: number | string) => new Intl.DateTimeFormat(undefined, { weekday: 'short', month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }).format(new Date(date))
const formatDuration = (start: number, end: number) => { const seconds = Math.max(0, Math.round((end - start) / 1000)); return `${Math.floor(seconds / 60)} min ${seconds % 60} sec` }

function App() {
  const navigate = useNavigate()
  const [auth, setAuth] = useState<Auth | undefined>(() => { const value = loadJson<Auth | undefined>(authKey, undefined); return value && Date.parse(value.expires_at) > Date.now() ? value : undefined })
  const [profile, setProfile] = useState<Profile>()
  const [devices, setDevices] = useState<Device[]>([])
  const [sessions, setSessions] = useState<Session[]>([])
  const [localSessions, setLocalSessions] = useState<LocalRecord[]>(loadLocalHistory)
  const [bleConnected, setBleConnected] = useState(false)
  const [bleState, setBleState] = useState('disconnected')
  const [bleName, setBleName] = useState('BP Controller')
  const [activeStart, setActiveStart] = useState<number>()
  const [activeRemote, setActiveRemote] = useState<Session>()
  const [busy, setBusy] = useState(false)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [notice, setNotice] = useState('')
  const [refresh, setRefresh] = useState(0)
  const [tick, setTick] = useState(0)
  const [selectedDevice, setSelectedDevice] = useState('')
  const [ble] = useState(() => new PhoneBleClient())
  const bleStateRef = useRef('disconnected')
  const updateBleEvent = useCallback((event: PhoneBleEvent) => {
    if (event.kind === 'connected') { setBleConnected(true); setBleState('checking'); bleStateRef.current = 'checking'; if (typeof event.value.name === 'string') setBleName(event.value.name) }
      if (event.kind === 'disconnected') { setBleConnected(false); setBleState('disconnected'); bleStateRef.current = 'disconnected' }
    if (event.kind === 'connectionProgress') { setBleState('connecting'); bleStateRef.current = 'connecting' }
    if (event.kind === 'state' && typeof event.value.state === 'string') { setBleState(event.value.state); bleStateRef.current = event.value.state }
    if (event.kind === 'error') setError('The device connection needs attention. Reconnect and check the controller before continuing.')
  }, [])

  useEffect(() => {
    consumerApi.setToken(auth?.access_token)
    try { if (auth) sessionStorage.setItem(authKey, JSON.stringify(auth)); else sessionStorage.removeItem(authKey) } catch { /* keep the active sign-in in memory when storage is unavailable */ }
  }, [auth])
  const refreshCloud = useCallback(async () => {
    if (!auth) return
    setLoading(true); setError('')
    try {
      const [p, d, s] = await Promise.all([consumerApi.profile(), consumerApi.devices(), consumerApi.sessions()])
      setProfile(p); setDevices(d.items); setSessions(s.items)
      setSelectedDevice(current => d.items.some(item => item.id === current) ? current : d.items[0]?.id ?? '')
      setActiveRemote(current => current ?? s.items.find(item => ['starting','measuring','stopping','interrupted'].includes(item.lifecycle)))
    } catch (reason) {
      if (reason instanceof ConsumerApiError && reason.kind === 'authorization') setAuth(undefined)
      setError(friendlyError(reason, 'We could not load your account. Please try again.'))
    } finally { setLoading(false) }
  }, [auth])
  useEffect(() => { void refreshCloud() }, [refreshCloud, refresh])
  useEffect(() => { if (!activeStart && !activeRemote) return; setTick(0); const timer = window.setInterval(() => setTick(value => value + 1), 1000); return () => window.clearInterval(timer) }, [activeStart, activeRemote?.id])
  useEffect(() => {
    if (!activeRemote) return
    let current = true
    let interruptionNotified = false
    const poll = async () => { try { const session = await consumerApi.session(activeRemote.id); if (!current) return; setActiveRemote(session); if (session.lifecycle === 'interrupted' && !interruptionNotified) { interruptionNotified = true; setNotice('The device session was interrupted. Check its status before another attempt.') } if (session.lifecycle === 'completed' || session.lifecycle === 'error' || session.lifecycle === 'cancelled') { setNotice(session.lifecycle === 'completed' ? 'Session saved to your account.' : 'The device session ended early. Check the device before another attempt.'); setActiveRemote(undefined); await refreshCloud() } } catch (reason) { if (current) setError(friendlyError(reason, 'We lost the live session update. Check device status before continuing.')) } }
    const id = window.setInterval(() => { if (current) void poll() }, 1800)
    return () => { current = false; window.clearInterval(id) }
  }, [activeRemote?.id, refreshCloud])

  const connectBle = async () => { setBusy(true); setError(''); try { await ble.connect(updateBleEvent); setBleConnected(true) } catch (reason) { setError(friendlyError(reason, 'We could not connect to the controller. Keep it nearby and try again.')); setBleConnected(false); setBleState('disconnected'); bleStateRef.current = 'disconnected' } finally { setBusy(false) } }
  const startMeasurement = async () => {
    setBusy(true); setError(''); setNotice('')
    if (bleConnected) {
      if (!['idle', 'completed'].includes(bleStateRef.current)) { setError('Your controller is not ready yet. Check its state before starting.'); setBusy(false); return }
      try { await ble.send('START_MEASUREMENT'); const startedAt = Date.now(); setTick(0); setActiveStart(startedAt); setBleState('precheck'); setNotice('Start was acknowledged. Stay still and follow your device’s instructions.') }
      catch { setError('The controller did not confirm Start. Check the cuff and device before trying again.') }
      finally { setBusy(false) }
      return
    }
    const device = devices.find(item => item.id === selectedDevice)
    if (!auth) { navigate('/account'); setNotice('Sign in to connect an enrolled device.'); setBusy(false); return }
    if (!device?.mqtt_enrolled || !device.online || device.reported_state !== 'idle') { setError('Your enrolled device needs to be online and ready before starting. Connect by Bluetooth or check device status.'); setBusy(false); return }
    try {
      const session = await consumerApi.start(device.id, crypto.randomUUID()); setActiveRemote(session)
      const deadline = Date.now() + 20000
      while (Date.now() < deadline) {
        const updated = await consumerApi.session(session.id); setActiveRemote(updated)
        if (updated.lifecycle === 'measuring') { setNotice('Measurement started. Follow your device’s instructions.'); setBusy(false); return }
        if (['error', 'cancelled', 'interrupted'].includes(updated.lifecycle)) { setActiveRemote(undefined); throw new Error('The device did not confirm Start. Check device status before trying again.') }
        await new Promise(resolve => window.setTimeout(resolve, 900))
      }
      setError('The start request was sent, but the device did not confirm it. Check device status before trying again.')
    } catch (reason) { setActiveRemote(undefined); void refreshCloud(); setError(friendlyError(reason, 'The device did not confirm Start. Check its status before trying again.')) }
    finally { setBusy(false) }
  }
  const stopMeasurement = async () => {
    setBusy(true); setError('')
    if (activeStart) {
      const start = activeStart
      try {
        await ble.send('STOP_MEASUREMENT')
        const deadline = Date.now() + 90000
        while (Date.now() < deadline) {
          if (!ble.connected) throw new Error('Connection was lost before release was confirmed.')
          if (['idle', 'completed'].includes(bleStateRef.current)) break
          await ble.send('GET_STATUS')
          await new Promise(resolve => window.setTimeout(resolve, 800))
        }
        if (!['idle', 'completed'].includes(bleStateRef.current)) throw new Error('Release was not confirmed in time.')
        const endedAt = Date.now(); const record: LocalRecord = { id: crypto.randomUUID(), startedAt: start, endedAt, state: 'completed', source: 'Bluetooth' }; const next = [record, ...localSessions].slice(0, 50); setLocalSessions(next); const persisted = saveLocalHistory(next); setActiveStart(undefined); setBleState('idle'); bleStateRef.current = 'idle'; setNotice(persisted ? 'The controller confirmed release. Session saved on this device, not to your cloud account.' : 'The controller confirmed release. This session is visible until you close this tab because browser storage is unavailable.')
      }
      catch { setError('Stop was requested, but release is not confirmed yet. Keep the controller nearby, reconnect, and check its state before disconnecting.') }
      finally { setBusy(false) }
      return
    }
    if (activeRemote) { try { await consumerApi.stop(activeRemote.id); setNotice('Stop request sent. Waiting for your device to finish…') } catch (reason) { setError(friendlyError(reason, 'Stop was not confirmed. Check the device before continuing.')) } finally { setBusy(false) } }
  }
  const signIn = async (mode: 'login' | 'register', values: { username: string; displayName: string; password: string }) => {
    setBusy(true); setError('')
    try { const result = mode === 'login' ? await consumerApi.login(values.username, values.password) : await consumerApi.register(values.username, values.displayName, values.password); setAuth(result); setNotice('You’re signed in. Your account sessions will appear in History.'); navigate('/'); }
    catch (reason) { setError(reason instanceof ConsumerApiError && reason.kind === 'validation' ? 'Check the username and password requirements, then try again.' : reason instanceof ConsumerApiError && reason.kind === 'authorization' ? 'Those details didn’t match. Check them and try again.' : friendlyError(reason, 'We could not sign you in. Please try again.')) }
    finally { setBusy(false) }
  }
  const signOut = () => { void consumerApi.logout().catch(() => undefined); setAuth(undefined); setProfile(undefined); setDevices([]); setSessions([]); setActiveRemote(undefined); setBusy(false); setNotice('You’ve been signed out.') }
  const active = Boolean(activeStart || activeRemote)
  const activeTimestamp = activeStart ?? Date.parse(activeRemote?.started_at ?? activeRemote?.created_at ?? new Date().toISOString())
  const duration = active ? formatDuration(activeTimestamp, activeTimestamp + tick * 1000) : ''
  const nav = [{ to: '/', label: 'Today', Icon: House }, { to: '/history', label: 'History', Icon: CalendarDays }, { to: '/device', label: 'Device', Icon: Bluetooth }, { to: '/account', label: 'Account', Icon: UserRound }]
  const headerState = bleConnected ? ['idle','completed'].includes(bleState) ? 'Controller ready' : 'Checking controller' : activeRemote ? 'Measurement in progress' : devices.some(item => item.mqtt_enrolled && item.online && item.reported_state === 'idle') ? 'Device ready' : 'No device connected'
  const reload = () => setRefresh(value => value + 1)

  return <div className="app-shell">
    <a className="skip-link" href="#main-content">Skip to content</a>
    <header className="topbar"><NavLink to="/" className="brand" aria-label="Device Companion home"><span className="brand-mark"><Activity size={19} aria-hidden="true"/></span><span>Device Companion</span></NavLink><span className={`top-status ${active ? 'is-active' : ''}`}><i/> {headerState}</span><button className="avatar-button" onClick={() => navigate('/account')} aria-label="Open your account"><UserRound aria-hidden="true" size={19}/></button></header>
    <div className="layout"><aside className="sidebar"><span className="side-label">YOUR HEALTH</span><nav aria-label="Primary navigation">{nav.map(({to,label,Icon}) => <NavLink to={to} end={to === '/'} key={to} className={({isActive}) => `nav-item ${isActive ? 'active' : ''}`}><Icon size={18} aria-hidden="true"/><span>{label}</span></NavLink>)}</nav><div className="side-note"><ShieldCheck size={17} aria-hidden="true"/><span>Sessions stay linked to your account.</span></div></aside>
      <main id="main-content" className="main-content"><div className="page-wrap"><div className="welcome-row"><div><p className="date-line">{new Intl.DateTimeFormat(undefined, {weekday:'long', month:'long', day:'numeric'}).format(new Date())}</p><p className="welcome-name">{auth ? `Hello, ${profile?.display_name || auth.display_name.split(' ')[0]}` : 'A little time for you'}</p></div><span className="privacy-chip"><ShieldCheck aria-hidden="true" size={15}/> Your sessions</span></div>
      {(error || notice) && <div className={`notice ${error ? 'notice-error' : ''}`} role={error ? 'alert' : 'status'} aria-live={error ? 'assertive' : 'polite'}><span>{error || notice}</span><button aria-label="Dismiss message" onClick={() => { setError(''); setNotice('') }}><X aria-hidden="true" size={17}/></button></div>}
      <Routes><Route path="/" element={<Home active={active} canStop={Boolean(activeStart) || ['measuring','interrupted'].includes(activeRemote?.lifecycle ?? '')} localMeasurement={Boolean(activeStart)} duration={duration} connected={bleConnected || Boolean(activeRemote)} bleState={bleState} deviceReady={devices.some(x => x.id === selectedDevice && x.mqtt_enrolled && x.online && x.reported_state === 'idle')} busy={busy} signedIn={Boolean(auth)} start={startMeasurement} stop={stopMeasurement} goDevice={() => navigate('/device')} goAccount={() => navigate('/account')} sessions={sessions.length + localSessions.length} latest={latestRecord(sessions, localSessions)}/>}/><Route path="/history" element={<History sessions={sessions} localSessions={localSessions} loading={loading} refresh={reload} signedIn={Boolean(auth)}/>}/><Route path="/device" element={<DevicePage bleConnected={bleConnected} bleName={bleName} bleState={bleState} bleBusy={busy} connect={connectBle} disconnect={() => { ble.disconnect(); setBleConnected(false); setBleState('disconnected'); bleStateRef.current = 'disconnected' }} devices={devices} selected={selectedDevice} select={setSelectedDevice} signedIn={Boolean(auth)} loading={loading} refresh={reload} active={active} localMeasurement={Boolean(activeStart)}/>}/><Route path="/account" element={<Account auth={auth} profile={profile} busy={busy} loading={loading} signIn={signIn} signOut={signOut} saveName={async name => { try { const updated = await consumerApi.updateProfile(name); setProfile(updated); setNotice('Your name has been updated.') } catch (reason) { setError(friendlyError(reason, 'We could not update your name.')) } }} refresh={reload}/>}/><Route path="*" element={<Navigate to="/" replace/>}/></Routes>
      <p className="prototype-note"><Waves aria-hidden="true" size={14}/> This is an experimental prototype. It does not provide a validated blood pressure result or medical advice.</p>
      </div></main></div>
    <nav className="bottom-nav" aria-label="Primary navigation">{nav.map(({to,label,Icon}) => <NavLink to={to} end={to === '/'} key={to} className={({isActive}) => `bottom-item ${isActive ? 'active' : ''}`}><Icon size={19} aria-hidden="true"/><span>{label}</span></NavLink>)}</nav>
  </div>
}

function latestRecord(cloud: Session[], local: LocalRecord[]) { const dates = [...cloud.map(x => Date.parse(x.ended_at ?? x.started_at ?? x.created_at)), ...local.map(x => x.endedAt)].filter(Number.isFinite); return dates.length ? Math.max(...dates) : undefined }

function Home({active, canStop, localMeasurement, duration, connected, bleState, deviceReady, busy, signedIn, start, stop, goDevice, goAccount, sessions, latest}:{active:boolean;canStop:boolean;localMeasurement:boolean;duration:string;connected:boolean;bleState:string;deviceReady:boolean;busy:boolean;signedIn:boolean;start:()=>void;stop:()=>void;goDevice:()=>void;goAccount:()=>void;sessions:number;latest?:number}) {
  const status = active ? 'Measurement in progress' : connected && !['idle','completed'].includes(bleState) ? 'Checking your controller' : connected || deviceReady ? 'Ready when you are' : 'Connect your device to begin'
  return <section className="home-view" aria-labelledby="home-title"><div className="hero-copy"><div className="readiness-pill"><i className={active ? 'dot-live' : ''}/>{status}</div><h1 id="home-title">{active ? 'Take a quiet moment.' : 'Your health, at your pace.'}</h1><p>{active ? 'Stay comfortable and still. Your controller will guide the measurement.' : connected ? `Your ${'controller'} is connected. Sit comfortably, then start when you feel ready.` : 'A calm place to begin a device session and keep track of when you measured.'}</p>
    <div className={`start-orbit ${active ? 'orbit-active' : ''}`}><button className="start-button" disabled={busy || (active && (!connected || !canStop)) || (!active && !connected && !deviceReady) || (!active && connected && !['idle','completed'].includes(bleState))} onClick={active ? stop : start} aria-label={active ? 'Stop measurement' : 'Start measurement'}>{busy ? <span className="spinner" aria-hidden="true"/> : active ? <><span className="stop-square" aria-hidden="true"/><span className="start-word">STOP</span></> : 'START'}</button></div>
    <p className="start-helper" aria-live="polite">{busy ? 'Waiting for your device…' : active && localMeasurement && !connected ? 'Controller disconnected. Reconnect before checking the session state.' : active && !canStop ? 'Waiting for your device to confirm the session…' : active ? `${duration} · Stop only when your device is ready` : connected && !['idle','completed'].includes(bleState) ? 'Checking that the controller is ready…' : connected || deviceReady ? 'Nothing begins until you press Start.' : 'Connect a device first to make Start available.'}</p>
    {!connected && !deviceReady && <button className="text-link" onClick={goDevice}><Bluetooth aria-hidden="true" size={17}/> Connect a device <ChevronRight aria-hidden="true" size={16}/></button>}
    {!signedIn && <button className="quiet-link" onClick={goAccount}><LogIn aria-hidden="true" size={16}/> Sign in to save sessions to your account</button>}
  </div><div className="home-lower"><div className="history-peek"><div><span className="peek-icon"><Clock3 aria-hidden="true" size={17}/></span><span><strong>{sessions ? `${sessions} ${sessions === 1 ? 'session' : 'sessions'} recorded` : 'Your history starts here'}</strong><small>{latest ? `Last session · ${formatDate(latest)}` : 'Completed sessions will appear here'}</small></span></div><NavLink to="/history" className="arrow-link">View history <ChevronRight aria-hidden="true" size={16}/></NavLink></div><div className="reassurance"><ShieldCheck aria-hidden="true" size={17}/><span>Your measurements remain experimental. No diagnosis or medical decisions should be based on them.</span></div></div></section>
}

function History({sessions, localSessions, loading, refresh, signedIn}:{sessions:Session[];localSessions:LocalRecord[];loading:boolean;refresh:()=>void;signedIn:boolean}) {
  const records = useMemo(() => [...sessions.map(session => ({ id:session.id, date:session.ended_at ?? session.started_at ?? session.created_at, state:session.lifecycle, duration: session.started_at && session.ended_at ? formatDuration(Date.parse(session.started_at), Date.parse(session.ended_at)) : undefined, source:'Account' })), ...localSessions.map(session => ({id:session.id,date:new Date(session.endedAt).toISOString(),state:session.state,duration:formatDuration(session.startedAt,session.endedAt),source:'This device'}))].sort((a,b) => Date.parse(b.date)-Date.parse(a.date)), [sessions,localSessions])
  return <section className="content-view"><PageTitle icon={<CalendarDays aria-hidden="true"/>} title="Your history" text="A simple record of your completed sessions." action={<button className="icon-button" onClick={refresh} aria-label="Refresh history"><RefreshCw aria-hidden="true" size={17}/></button>}/>
    {!signedIn && <div className="inline-callout"><ShieldCheck aria-hidden="true" size={18}/><p>Bluetooth sessions stay in this browser. Sign in to view sessions saved to your account.</p></div>}
    {loading ? <div className="empty-state"><span className="spinner dark-spinner"/><p>Loading your sessions…</p></div> : records.length ? <div className="record-list">{records.map(row => <article className="record-row" key={`${row.source}-${row.id}`}><span className="record-icon"><HeartPulse aria-hidden="true" size={18}/></span><div className="record-copy"><strong>{formatDate(row.date)}</strong><span>{row.source} · {row.duration ?? statusText(row.state)}</span></div><span className={`record-state ${row.state === 'completed' ? 'done' : ''}`}>{recordStatus(row.state)}</span></article>)}</div> : <div className="empty-state"><span className="empty-icon"><CalendarDays aria-hidden="true" size={23}/></span><h2>No sessions yet</h2><p>When you complete a session, its date and duration will appear here. This prototype doesn’t create blood pressure results.</p><NavLink to="/device" className="button-secondary">Connect a device</NavLink></div>}
    {records.length > 0 && <p className="history-disclaimer">History shows session dates and duration only. It does not contain validated blood pressure readings.</p>}
  </section>
}
const statusText = (state:string) => ['starting','measuring','stopping'].includes(state) ? 'In progress' : state === 'completed' ? 'Completed' : 'Ended early'
const recordStatus = (state:string) => ['starting','measuring','stopping'].includes(state) ? 'In progress' : state === 'completed' ? 'Complete' : 'Ended early'

function DevicePage({bleConnected,bleName,bleState,bleBusy,connect,disconnect,devices,selected,select,signedIn,loading,refresh,active,localMeasurement}:{bleConnected:boolean;bleName:string;bleState:string;bleBusy:boolean;connect:()=>void;disconnect:()=>void;devices:Device[];selected:string;select:(id:string)=>void;signedIn:boolean;loading:boolean;refresh:()=>void;active:boolean;localMeasurement:boolean}) {
  const remoteReady = devices.some(item => item.mqtt_enrolled && item.online && item.reported_state === 'idle')
  return <section className="content-view"><PageTitle icon={<Bluetooth aria-hidden="true"/>} title="Your device" text="Connect over Bluetooth, or check an enrolled cloud device." action={<button className="icon-button" onClick={refresh} aria-label="Refresh device status"><RefreshCw aria-hidden="true" size={17}/></button>}/>
    <article className="device-panel"><div className="device-illustration"><span><Activity aria-hidden="true" size={34}/></span><i className={bleConnected || remoteReady ? 'device-online' : ''}/></div><div className="device-copy"><span className={`device-state ${bleConnected || remoteReady ? 'online' : ''}`}><i/>{bleConnected ? 'Connected over Bluetooth' : remoteReady ? 'Cloud device ready' : 'Not connected'}</span><h2>{bleConnected ? bleName : remoteReady ? 'Your BP monitor' : 'Connect your BP monitor'}</h2><p>{bleConnected ? 'Your controller is nearby and ready when you are.' : 'Keep the controller nearby. Bluetooth is the simplest way to connect.'}</p></div><div className="device-actions">{bleConnected ? <button className="button-secondary" onClick={disconnect} disabled={active}>Disconnect</button> : <button className="button-primary" onClick={connect} disabled={bleBusy || active && !localMeasurement}>{bleBusy ? 'Connecting…' : localMeasurement ? 'Reconnect to controller' : 'Connect over Bluetooth'}</button>}</div></article>
    {!('bluetooth' in navigator) && <p className="inline-callout"><CircleHelp aria-hidden="true" size={18}/><span>Bluetooth setup needs a Web Bluetooth-enabled browser over HTTPS. Try Chrome on a supported device.</span></p>}
    {bleConnected && <div className="connection-detail"><span>Connection</span><strong>{['measuring','precheck','inflating','deflating'].includes(bleState) ? 'Measurement active' : ['idle','completed'].includes(bleState) ? 'Ready' : 'Checking controller'}</strong></div>}
    <div className="section-divider"><span>Account device</span></div>
    {!signedIn ? <div className="empty-state compact"><span className="empty-icon"><LogIn aria-hidden="true" size={22}/></span><h2>Sign in to check cloud status</h2><p>Your cloud device appears here after it has been set up on your account.</p><NavLink to="/account" className="button-secondary">Go to account</NavLink></div> : loading ? <div className="inline-loading"><span className="spinner dark-spinner"/> Checking device…</div> : devices.length ? <div className="cloud-devices">{devices.map((device,index) => <label className="cloud-row" key={device.id}><input type="radio" name="device" checked={selected === device.id} onChange={() => select(device.id)} disabled={active}/><span><strong>{devices.length > 1 ? `BP monitor ${index + 1}` : 'BP monitor'}</strong><small>{device.online ? device.reported_state === 'idle' ? 'Online · ready' : 'Online · busy' : 'Offline'}</small></span><i className={device.online && device.reported_state === 'idle' ? 'small-green-dot' : ''}/></label>)}</div> : <div className="empty-note"><Radio aria-hidden="true" size={18}/><p>No cloud devices are linked to your account yet. You can still connect over Bluetooth.</p></div>}
    <div className="setup-steps"><h3>Before you begin</h3><ol><li>Place the cuff as shown in your device instructions.</li><li>Sit comfortably with your arm supported.</li><li>Press Start only when you are ready.</li></ol></div>
  </section>
}

function Account({auth,profile,busy,loading,signIn,signOut,saveName,refresh}:{auth?:Auth;profile?:Profile;busy:boolean;loading:boolean;signIn:(mode:'login'|'register',values:{username:string;displayName:string;password:string})=>Promise<void>;signOut:()=>void;saveName:(name:string)=>Promise<void>;refresh:()=>void}) {
  const [mode,setMode]=useState<'login'|'register'>('login')
  const [name,setName]=useState(profile?.display_name ?? '')
  useEffect(()=>{setName(profile?.display_name ?? '')},[profile?.display_name])
  return <section className="content-view"><PageTitle icon={<UserRound aria-hidden="true"/>} title="Your account" text="Manage your sign-in and session privacy." action={auth ? <button className="icon-button" onClick={refresh} aria-label="Refresh account"><RefreshCw aria-hidden="true" size={17}/></button> : undefined}/>
    {auth ? <><article className="account-card"><span className="account-mark"><UserRound aria-hidden="true" size={24}/></span><div><h2>{profile?.display_name ?? auth.display_name}</h2><p>@{profile?.username ?? auth.username}</p></div><span className="account-badge"><Check aria-hidden="true" size={14}/> Signed in</span></article>
      <form className="settings-form" onSubmit={event=>{event.preventDefault();void saveName(name)}}><h3>Your name</h3><label htmlFor="display-name">Name shown in the app</label><input id="display-name" name="display_name" autoComplete="name" maxLength={128} required value={name} onChange={event=>setName(event.target.value)}/><button className="button-primary" disabled={busy || !name.trim() || name.trim() === profile?.display_name}>{busy?'Saving…':'Save changes'}</button></form>
      <div className="privacy-list"><div><ShieldCheck aria-hidden="true" size={18}/><span><strong>Account sessions</strong><small>Cloud history belongs to your signed-in account.</small></span></div><div><Bluetooth aria-hidden="true" size={18}/><span><strong>Bluetooth sessions</strong><small>Stored on this browser and not uploaded to your account.</small></span></div><div><Waves aria-hidden="true" size={18}/><span><strong>Experimental prototype</strong><small>No validated blood pressure readings or medical advice.</small></span></div></div>
      <button className="button-secondary signout-button" onClick={()=>void signOut()} disabled={busy}>{busy?'Signing out…':<><LogOut aria-hidden="true" size={16}/> Sign out</>}</button>
    </> : <><div className="account-intro"><span className="account-mark"><ShieldCheck aria-hidden="true" size={24}/></span><div><h2>Welcome back</h2><p>Sign in to view cloud sessions. Bluetooth remains available without an account.</p></div></div><div className="auth-tabs" aria-label="Account access"><button type="button" aria-pressed={mode==='login'} className={mode==='login'?'selected':''} onClick={()=>setMode('login')}>Sign in</button><button type="button" aria-pressed={mode==='register'} className={mode==='register'?'selected':''} onClick={()=>setMode('register')}>Create account</button></div><form className="auth-form" onSubmit={event=>{event.preventDefault();const data=new FormData(event.currentTarget);void signIn(mode,{username:String(data.get('username')).trim(),displayName:String(data.get('display_name')).trim(),password:String(data.get('password'))})}}>
      {mode==='register'&&<label>Display name<input name="display_name" autoComplete="name" maxLength={128} minLength={1} required/></label>}
      <label>Username<input name="username" autoComplete="username" minLength={3} maxLength={48} pattern="[a-z0-9_.-]{3,48}" title="3–48 lowercase letters, numbers, dots, dashes or underscores" spellCheck={false} required/></label>
      <label>Password<input name="password" type="password" autoComplete={mode==='login'?'current-password':'new-password'} minLength={12} maxLength={128} required/><small>Use at least 12 characters.</small></label>
      <button className="button-primary" type="submit" disabled={busy}>{busy?'Please wait…':mode==='login'?'Sign in':'Create account'}</button></form>
      {loading && <p className="inline-loading"><span className="spinner dark-spinner"/> Loading your account…</p>}
    </>}
  </section>
}

function PageTitle({icon,title,text,action}:{icon:ReactNode;title:string;text:string;action?:ReactNode}) { return <div className="page-title"><span className="page-title-icon">{icon}</span><div><h1>{title}</h1><p>{text}</p></div>{action}</div> }

export default App
