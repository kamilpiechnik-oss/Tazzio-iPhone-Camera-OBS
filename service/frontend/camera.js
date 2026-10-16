const $ = selector => document.querySelector(selector);
const profiles = {
  lowLatency: { width: 1920, height: 1080, fps: 60, bitrateAt30: 8000000 },
  balanced: { width: 1920, height: 1080, fps: 60, bitrateAt30: 14000000 },
  highQuality: { width: 1920, height: 1080, fps: 30, bitrateAt30: 20000000 },
  weakNetwork: { width: 1280, height: 720, fps: 30, bitrateAt30: 5000000 },
};
const state = { pairToken: '', socketToken: '', iceServers: [], mode: 'lan', ws: null, pc: null, stream: null, statsTimer: null, wakeLock: null, ready: false, previousStats: { at: 0, bytes: 0 }, commandBusy: false, lastVideoState: '', focusMode: '', resumeOnVisible: false, resuming: false };

function setStatus(message, connected = false) { $('#status').textContent = message; $('#status').classList.toggle('connected', connected); }
function apiError(data, fallback) { return data?.message || data?.error || fallback; }
function socketUrl(token) { const url = new URL('/camera-ws', location.origin); url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'; url.searchParams.set('token', token); return url; }
function activeProfile() { return profiles[$('#quality').value] || profiles.balanced; }
function requestedFps() { return Number($('#fps').value) || Number(state.stream?.getVideoTracks()[0]?.getSettings?.().frameRate) || activeProfile().fps; }
function targetBitrate() { const scaled = activeProfile().bitrateAt30 * Math.max(1, requestedFps() / 30); return Math.round(Math.min(scaled, selectedMode() === 'internet' ? 24000000 : 60000000)); }
function captureDimensions() { const profile = activeProfile(); return requestedFps() >= 50 ? { width: 1280, height: 720 } : { width: profile.width, height: profile.height }; }
function selectedMode() { return state.mode; }
function selectedIceServers() { return state.iceServers; }
function isIOSWebKit() { return /iP(?:hone|ad|od)/.test(navigator.userAgent) || (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1); }
function sendMessage(type, payload) { if (state.ws?.readyState === WebSocket.OPEN) state.ws.send(JSON.stringify({ type, payload })); }

async function pair() {
  const parts = location.pathname.split('/').filter(Boolean); state.pairToken = parts[0] === 'camera' ? parts[1] || '' : '';
  if (!state.pairToken) { setStatus('Nieprawidłowy kod QR.'); return; }
  try {
    const response = await fetch('/api/camera/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: decodeURIComponent(state.pairToken) }) });
    const data = await response.json();
    if (!response.ok) throw new Error(apiError(data, 'Nie udało się sparować kamery.'));
    state.socketToken = data.socket_token; state.iceServers = data.ice_servers || []; state.mode = data.mode === 'internet' ? 'internet' : 'lan';
    history.replaceState(null, '', '/camera/paired'); restoreChoices(); state.ready = true; $('#start').disabled = false; updateModeNote();
    setStatus('Kod zaakceptowany. Ustaw jakość i uruchom kamerę.');
  } catch (error) { setStatus(error.message); }
}
function restoreChoices() { $('#quality').value = localStorage.getItem('tazzioCameraQuality') || 'balanced'; $('#fps').value = localStorage.getItem('tazzioCameraFps') || '0'; $('#lens').value = localStorage.getItem('tazzioCameraLens') || 'environment'; $('#audio').checked = localStorage.getItem('tazzioCameraAudio') === 'true'; }
function saveChoices() { localStorage.setItem('tazzioCameraQuality', $('#quality').value); localStorage.setItem('tazzioCameraFps', $('#fps').value); localStorage.setItem('tazzioCameraLens', $('#lens').value); localStorage.setItem('tazzioCameraAudio', String($('#audio').checked)); }
function updateModeNote() { const lan = selectedMode() === 'lan'; $('#mode-note').textContent = lan ? 'LAN: obraz idzie bezpośrednio z iPhone’a do komputera. VPS służy tylko do sparowania urządzeń.' : 'Internet: obraz jest wymuszony przez bezpieczny przekaźnik TURN na VPS — użyj poza domowym Wi‑Fi.'; $('#mode-pill').textContent = lan ? 'LAN DIRECT' : 'VPS RELAY'; $('#mode-pill').classList.toggle('relay', !lan); }
function selectedVideoConstraints() { const dimensions = captureDimensions(); const fps = requestedFps(); const lens = $('#lens').value; const explicitFps = Number($('#fps').value); const frameRate = explicitFps >= 50 ? { exact: fps } : { ideal: fps, max: fps }; const video = { width: { ideal: dimensions.width }, height: { ideal: dimensions.height }, frameRate }; if (lens.startsWith('device:')) video.deviceId = { exact: lens.slice(7) }; else video.facingMode = { ideal: lens }; return video; }
async function preferH264(transceiver) { if (!transceiver?.setCodecPreferences || !RTCRtpSender.getCapabilities) return; const codecs = RTCRtpSender.getCapabilities('video')?.codecs || []; const h264 = codecs.filter(codec => codec.mimeType.toLowerCase() === 'video/h264'); const rest = codecs.filter(codec => codec.mimeType.toLowerCase() !== 'video/h264'); if (h264.length) transceiver.setCodecPreferences([...h264, ...rest]); }
async function applyQuality(strict = false) { const sender = state.pc?.getSenders().find(item => item.track?.kind === 'video'); if (!sender) return; const parameters = sender.getParameters(); if (!parameters.encodings?.length) parameters.encodings = [{}]; parameters.encodings[0].maxBitrate = targetBitrate(); parameters.encodings[0].maxFramerate = requestedFps(); parameters.encodings[0].scaleResolutionDownBy = 1; parameters.degradationPreference = 'maintain-resolution'; try { await sender.setParameters(parameters); } catch (error) { if (strict) throw new Error('Safari nie pozwoliło zmienić jakości nadajnika WebRTC.'); } }
async function restoreFocus(track) { const modes = track?.getCapabilities?.().focusMode; if (!Array.isArray(modes) || !modes.length) return; const preferred = modes.includes(state.focusMode) ? state.focusMode : modes.includes('continuous') ? 'continuous' : ''; if (!preferred) return; try { await track.applyConstraints({ advanced: [{ focusMode: preferred }] }); state.focusMode = preferred; } catch {} }
async function populateCameras() { const devices = (await navigator.mediaDevices.enumerateDevices()).filter(device => device.kind === 'videoinput'); const existing = new Set([...$('#lens').options].map(option => option.value)); devices.forEach((device, index) => { const value = `device:${device.deviceId}`; if (existing.has(value)) return; const option = document.createElement('option'); option.value = value; option.textContent = device.label || `Kamera ${index + 1}`; $('#lens').append(option); }); return devices; }
function updateCapabilities() { const track = state.stream?.getVideoTracks()[0]; const caps = track?.getCapabilities?.() || {}; const reportedMax = Number(caps.frameRate?.max || 0); const selectableMax = isIOSWebKit() ? Math.max(60, reportedMax) : reportedMax; for (const option of $('#fps').options) { if (option.value === '0') continue; option.disabled = selectableMax > 0 && Number(option.value) > selectableMax; } }

function rangeCapability(caps, settings, name) {
  const range = caps[name];
  if (!range || !Number.isFinite(Number(range.min)) || !Number.isFinite(Number(range.max))) return null;
  const min = Number(range.min); const max = Number(range.max); const step = Number(range.step) || Math.max((max - min) / 100, 0.01);
  const current = Number.isFinite(Number(settings[name])) ? Number(settings[name]) : min;
  return { type: 'range', min, max, step, value: Math.min(max, Math.max(min, current)) };
}
function enumCapability(caps, settings, name) {
  const values = Array.isArray(caps[name]) ? caps[name].filter(value => typeof value === 'string') : [];
  if (!values.length) return null;
  return { type: 'enum', values, value: values.includes(settings[name]) ? settings[name] : values[0] };
}
async function cameraCapabilities(videoOverride = {}) {
  const track = state.stream?.getVideoTracks()[0]; if (!track) return { devices: [], controls: {} };
  const caps = track.getCapabilities?.() || {}; const settings = track.getSettings?.() || {}; const devices = await populateCameras(); const controls = {};
  for (const name of ['zoom', 'focusDistance', 'exposureCompensation', 'exposureTime', 'iso', 'brightness', 'colorTemperature']) { const value = rangeCapability(caps, settings, name); if (value) controls[name] = value; }
  for (const name of ['focusMode', 'exposureMode', 'whiteBalanceMode']) { const value = enumCapability(caps, settings, name); if (value) controls[name] = value; }
  if (Object.prototype.hasOwnProperty.call(caps, 'torch')) controls.torch = { type: 'boolean', value: Boolean(settings.torch) };
  if (caps.frameRate && Number.isFinite(Number(caps.frameRate.max))) {
    const min = Number(caps.frameRate.min || 1); const max = Number(caps.frameRate.max); const current = Math.round(Number(settings.frameRate || 0));
    const selectableMax = isIOSWebKit() ? Math.max(60, max) : max;
    const values = [...new Set([24, 25, 30, 50, 60, 90, 120, 144, 240, current].filter(value => value >= min && value <= selectableMax))].sort((a, b) => a - b);
    if (values.length) controls.frameRate = { type: 'enum', values, value: current || values[0], max: selectableMax, reportedMax: max };
  }
  controls.quality = { type: 'enum', values: Object.keys(profiles), value: $('#quality').value };
  const preview = $('#preview'); const width = Number(preview.videoWidth || settings.width || 0); const height = Number(preview.videoHeight || settings.height || 0);
  return { devices: devices.map((device, index) => ({ id: device.deviceId, label: device.label || `Kamera ${index + 1}` })), activeDeviceId: settings.deviceId || '', facingMode: settings.facingMode || '', video: { width, height, frameRate: Number(settings.frameRate || 0), quality: $('#quality').value, targetBitrateMbps: targetBitrate() / 1000000, ...videoOverride }, controls };
}
async function reportCameraCapabilities(type = 'camera-capabilities', videoOverride = {}) { if (state.ws?.readyState !== WebSocket.OPEN || !state.stream) return; sendMessage(type, await cameraCapabilities(videoOverride)); }

async function replaceVideoTrack(video, lensValue = '') {
  const newStream = await navigator.mediaDevices.getUserMedia({ video, audio: false });
  const newTrack = newStream.getVideoTracks()[0]; if (!newTrack) throw new Error('Safari nie uruchomiło wybranej kamery.');
  if ('contentHint' in newTrack) newTrack.contentHint = 'detail';
  await restoreFocus(newTrack);
  const oldTrack = state.stream?.getVideoTracks()[0]; const sender = state.pc?.getSenders().find(item => item.track?.kind === 'video');
  try { if (sender) await sender.replaceTrack(newTrack); if (oldTrack) { state.stream.removeTrack(oldTrack); oldTrack.stop(); } state.stream.addTrack(newTrack); $('#preview').srcObject = state.stream; if (lensValue) $('#lens').value = lensValue; saveChoices(); updateCapabilities(); await applyQuality(); return newTrack; }
  catch (error) { newTrack.stop(); throw error; }
}
async function reopenVideoTrack(candidates, lensValue = '') {
  const oldTrack = state.stream?.getVideoTracks()[0]; if (!oldTrack) throw new Error('Kamera nie jest uruchomiona.');
  const sender = state.pc?.getSenders().find(item => item.track?.kind === 'video');
  const oldSettings = oldTrack.getSettings?.() || {};
  state.stream.removeTrack(oldTrack); oldTrack.stop();
  let newTrack; let lastError;
  for (const video of candidates) {
    try { const fresh = await navigator.mediaDevices.getUserMedia({ video, audio: false }); newTrack = fresh.getVideoTracks()[0]; if (newTrack) break; }
    catch (error) { lastError = error; }
  }
  if (!newTrack) {
    const recovery = { width: { ideal: oldSettings.width || 1920 }, height: { ideal: oldSettings.height || 1080 }, frameRate: { ideal: oldSettings.frameRate || 30 } };
    if (oldSettings.deviceId) recovery.deviceId = { exact: oldSettings.deviceId }; else recovery.facingMode = { ideal: $('#lens').value };
    try { const restored = await navigator.mediaDevices.getUserMedia({ video: recovery, audio: false }); newTrack = restored.getVideoTracks()[0]; }
    catch {}
    if (!newTrack) throw lastError || new Error('Safari nie uruchomiło ponownie kamery.');
    if (sender) await sender.replaceTrack(newTrack); state.stream.addTrack(newTrack); $('#preview').srcObject = state.stream; updateCapabilities(); await applyQuality();
    throw lastError || new Error('Wybrany FPS nie jest dostępny dla tej kamery.');
  }
  if ('contentHint' in newTrack) newTrack.contentHint = 'detail';
  await restoreFocus(newTrack);
  if (sender) await sender.replaceTrack(newTrack);
  state.stream.addTrack(newTrack); $('#preview').srcObject = state.stream; if (lensValue) $('#lens').value = lensValue; saveChoices(); updateCapabilities(); await applyQuality(true); return newTrack;
}
async function switchCamera(deviceId) {
  if (!deviceId) throw new Error('Nie wybrano kamery.');
  const video = selectedVideoConstraints(); delete video.facingMode; video.deviceId = { exact: deviceId };
  await replaceVideoTrack(video, `device:${deviceId}`);
}
async function setQualityProfile(value) {
  const name = String(value || ''); if (!Object.prototype.hasOwnProperty.call(profiles, name)) throw new Error('Nieprawidłowy profil jakości.');
  const track = state.stream?.getVideoTracks()[0]; if (!track) throw new Error('Kamera nie jest uruchomiona.');
  const currentFps = Math.round(Number(track.getSettings?.().frameRate || requestedFps()));
  $('#quality').value = name;
  const fpsOption = [...$('#fps').options].find(option => Number(option.value) === currentFps); if (fpsOption) $('#fps').value = fpsOption.value;
  saveChoices();
  const video = selectedVideoConstraints(); const deviceId = track.getSettings?.().deviceId;
  if (deviceId) { delete video.facingMode; video.deviceId = { exact: deviceId }; }
  try { await track.applyConstraints(video); }
  catch { await replaceVideoTrack(video, deviceId ? `device:${deviceId}` : ''); }
  await applyQuality(true);
}
async function setFrameRate(value) {
  const requested = Number(value); if (!Number.isFinite(requested) || requested <= 0) throw new Error('Nieprawidłowy FPS.');
  let track = state.stream?.getVideoTracks()[0];
  if (!track) throw new Error('Kamera nie jest uruchomiona.');
  const option = [...$('#fps').options].find(item => Number(item.value) === requested); if (option) $('#fps').value = option.value;
  const video = selectedVideoConstraints(); const deviceId = track.getSettings?.().deviceId;
  if (deviceId) { delete video.facingMode; video.deviceId = { exact: deviceId }; }
  if (requested >= 50) {
    const camera = deviceId ? { deviceId: { exact: deviceId } } : { facingMode: { ideal: $('#lens').value } };
    const exactFrameRate = { exact: requested };
    track = await reopenVideoTrack([
      { ...camera, width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: exactFrameRate },
      { ...camera, frameRate: exactFrameRate },
    ], deviceId ? `device:${deviceId}` : '');
  }
  else try { await track.applyConstraints(video); }
  catch { track = await replaceVideoTrack(video, deviceId ? `device:${deviceId}` : ''); }
  let actual = Number(track.getSettings?.().frameRate || 0);
  if (actual && actual < requested - 1) {
    track = await replaceVideoTrack(video, deviceId ? `device:${deviceId}` : ''); actual = Number(track.getSettings?.().frameRate || 0);
  }
  await restoreFocus(track);
  await applyQuality(true);
  if (actual && actual < requested - 1) throw new Error(`Safari ustawiło ${actual.toFixed(0)} FPS zamiast ${requested} FPS.`);
}
async function applyCameraCommand(command) {
  if (state.commandBusy) throw new Error('Poprzednia zmiana kamery jeszcze trwa.');
  state.commandBusy = true;
  try {
    const control = String(command?.control || ''); const value = command?.value;
    if (control === 'camera') await switchCamera(String(value || ''));
    else if (control === 'frameRate') await setFrameRate(value);
    else if (control === 'quality') await setQualityProfile(value);
    else {
      const track = state.stream?.getVideoTracks()[0]; if (!track) throw new Error('Kamera nie jest uruchomiona.');
      const caps = track.getCapabilities?.() || {}; const allowed = ['zoom', 'torch', 'focusMode', 'focusDistance', 'exposureMode', 'exposureCompensation', 'exposureTime', 'iso', 'brightness', 'whiteBalanceMode', 'colorTemperature'];
      if (!allowed.includes(control) || !Object.prototype.hasOwnProperty.call(caps, control)) throw new Error('Ta funkcja nie jest obsługiwana przez tę kamerę w Safari.');
      if (Array.isArray(caps[control]) && !caps[control].includes(value)) throw new Error('Nieprawidłowa wartość sterowania.');
      if (caps[control] && Number.isFinite(Number(caps[control].min))) { const numeric = Number(value); if (!Number.isFinite(numeric) || numeric < Number(caps[control].min) || numeric > Number(caps[control].max)) throw new Error('Wartość jest poza zakresem kamery.'); }
      if (control === 'focusMode') state.focusMode = String(value);
      if (control === 'focusDistance' && Array.isArray(caps.focusMode) && caps.focusMode.includes('manual')) {
        state.focusMode = 'manual';
        await track.applyConstraints({ advanced: [{ focusMode: 'manual' }] });
      }
      await track.applyConstraints({ advanced: [{ [control]: value }] });
    }
    sendMessage('camera-command-result', { id: command?.id || '', ok: true }); await reportCameraCapabilities('camera-state');
  } finally { state.commandBusy = false; }
}

async function start() {
  if (!state.ready || state.stream) return; saveChoices(); $('#start').disabled = true; setStatus('Uruchamianie kamery…');
  try {
    const audio = $('#audio').checked ? { echoCancellation: false, noiseSuppression: false, autoGainControl: false } : false;
    state.stream = await navigator.mediaDevices.getUserMedia({ video: selectedVideoConstraints(), audio }); const videoTrack = state.stream.getVideoTracks()[0]; if ('contentHint' in videoTrack) videoTrack.contentHint = 'detail'; await restoreFocus(videoTrack);
    $('#preview').srcObject = state.stream; $('#placeholder').hidden = true; $('#preview').onresize = () => reportCameraCapabilities('camera-state').catch(() => {}); await populateCameras(); updateCapabilities();
    const config = { iceServers: selectedIceServers() }; if (selectedMode() === 'internet') config.iceTransportPolicy = 'relay'; state.pc = new RTCPeerConnection(config);
    let videoTransceiver; for (const track of state.stream.getTracks()) { const sender = state.pc.addTrack(track, state.stream); if (track.kind === 'video') videoTransceiver = state.pc.getTransceivers().find(item => item.sender === sender); }
    await preferH264(videoTransceiver);
    state.pc.onicecandidate = event => { if (event.candidate) sendMessage('ice-candidate', { candidate: event.candidate.candidate, mid: event.candidate.sdpMid }); };
    state.pc.onconnectionstatechange = () => { const value = state.pc?.connectionState || 'closed'; setStatus(value === 'connected' ? `Połączono z OBS — ${selectedMode() === 'lan' ? 'LAN bezpośrednio' : 'przez VPS TURN'}.` : `WebRTC: ${value}`, value === 'connected'); };
    state.ws = new WebSocket(socketUrl(state.socketToken));
    state.ws.onmessage = async event => { const message = JSON.parse(event.data); if (message.type === 'peer') { if (message.ready) await sendOffer(); if (message.controllerReady) await reportCameraCapabilities(); } else if (message.type === 'answer') await state.pc.setRemoteDescription(message.payload); else if (message.type === 'ice-candidate' && message.payload?.candidate) await state.pc.addIceCandidate({ candidate: message.payload.candidate, sdpMid: message.payload.mid }); else if (message.type === 'camera-command') { try { await applyCameraCommand(message.payload || {}); } catch (error) { sendMessage('camera-command-result', { id: message.payload?.id || '', ok: false, message: error.message }); await reportCameraCapabilities('camera-state'); } } };
    state.ws.onopen = async () => { setStatus('Kamera działa — oczekiwanie na plugin OBS…'); await reportCameraCapabilities(); };
    state.ws.onclose = () => { if (state.stream) setStatus('Połączenie z serwerem zostało zamknięte.'); };
    $('#stop').hidden = false; $('#start').hidden = true; lockControls(true); await keepAwake(); startStats();
  } catch (error) { setStatus(error.name === 'NotAllowedError' ? 'Safari nie otrzymało dostępu do kamery.' : error.message); stop(); }
}
async function sendOffer() { if (!state.pc || state.pc.signalingState !== 'stable') return; const offer = await state.pc.createOffer(); await state.pc.setLocalDescription(offer); await applyQuality(); sendMessage('offer', { type: 'offer', sdp: state.pc.localDescription.sdp }); }
function lockControls(locked) { for (const control of document.querySelectorAll('.controls select,.controls input')) control.disabled = locked; }
async function keepAwake() { try { if ('wakeLock' in navigator) state.wakeLock = await navigator.wakeLock.request('screen'); } catch {} }
async function updateStats() { const track = state.stream?.getVideoTracks()[0]; if (!track) return; const settings = track.getSettings(); let outbound, pair, localCandidate, remoteCandidate; const reports = state.pc ? await state.pc.getStats() : []; reports.forEach(report => { if (report.type === 'outbound-rtp' && report.kind === 'video') outbound = report; if (report.type === 'candidate-pair' && report.state === 'succeeded' && (report.nominated || !pair)) pair = report; }); if (pair) reports.forEach(report => { if (report.id === pair.localCandidateId) localCandidate = report; if (report.id === pair.remoteCandidateId) remoteCandidate = report; }); const now = performance.now(); let bitrate = 0; if (outbound && state.previousStats.at) bitrate = (outbound.bytesSent - state.previousStats.bytes) * 8 / (now - state.previousStats.at) / 1000; if (outbound) state.previousStats = { at: now, bytes: outbound.bytesSent }; const relay = localCandidate?.candidateType === 'relay' || remoteCandidate?.candidateType === 'relay'; const route = relay ? 'VPS TURN' : selectedMode() === 'lan' ? 'LAN DIRECT' : 'DIRECT P2P'; const rtt = Math.round((pair?.currentRoundTripTime || 0) * 1000); const actualFps = Math.round(outbound?.framesPerSecond || settings.frameRate || 0); const width = Number($('#preview').videoWidth || settings.width || 0); const height = Number($('#preview').videoHeight || settings.height || 0); $('#stats').textContent = `${width || '?'}×${height || '?'} · ${actualFps} FPS · ${bitrate ? bitrate.toFixed(1) : '—'} Mb/s · ${route}${rtt ? ` · ${rtt} ms` : ''}`; const signature = `${width}x${height}@${actualFps}:${bitrate.toFixed(1)}`; if (signature !== state.lastVideoState) { state.lastVideoState = signature; if (!state.commandBusy) reportCameraCapabilities('camera-state', { width, height, frameRate: actualFps, actualBitrateMbps: bitrate }).catch(() => {}); } }
function startStats() { clearInterval(state.statsTimer); state.previousStats = { at: 0, bytes: 0 }; state.statsTimer = setInterval(() => updateStats().catch(() => {}), 1000); updateStats().catch(() => {}); }
function stop() { clearInterval(state.statsTimer); state.statsTimer = null; state.lastVideoState = ''; state.ws?.close(); state.ws = null; state.pc?.close(); state.pc = null; state.stream?.getTracks().forEach(track => track.stop()); state.stream = null; $('#preview').srcObject = null; $('#preview').onresize = null; $('#placeholder').hidden = false; state.wakeLock?.release?.(); state.wakeLock = null; $('#stop').hidden = true; $('#start').hidden = false; $('#start').disabled = !state.ready; lockControls(false); updateCapabilities(); setStatus(state.ready ? 'Transmisja zatrzymana. Możesz zmienić ustawienia i uruchomić ją ponownie.' : 'Transmisja zatrzymana.'); }
async function resumeAfterBackground() {
  if (!state.resumeOnVisible || state.resuming || !state.ready) return;
  state.resumeOnVisible = false; state.resuming = true;
  try { stop(); setStatus('Przywracanie kamery i sterowania po powrocie do Safari…'); await start(); }
  finally { state.resuming = false; }
}
$('#start').addEventListener('click', start); $('#stop').addEventListener('click', () => { state.resumeOnVisible = false; stop(); });
document.addEventListener('visibilitychange', () => { if (document.hidden) state.resumeOnVisible = Boolean(state.stream); else resumeAfterBackground().catch(error => setStatus(error.message)); });
window.addEventListener('pagehide', event => { if (event.persisted) state.resumeOnVisible = Boolean(state.stream); else stop(); });
window.addEventListener('pageshow', event => { if (event.persisted) resumeAfterBackground().catch(error => setStatus(error.message)); });
pair();
