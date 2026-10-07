const $=selector=>document.querySelector(selector);
const state={pairToken:'',socketToken:'',iceServers:[],ws:null,pc:null,stream:null,statsTimer:null,wakeLock:null,ready:false};

function setStatus(message,connected=false){$('#status').textContent=message;$('#status').classList.toggle('connected',connected)}
function apiError(data,fallback){return data?.message||data?.error||fallback}
function socketUrl(token){const url=new URL('/camera-ws',location.origin);url.protocol=location.protocol==='https:'?'wss:':'ws:';url.searchParams.set('token',token);return url}

async function pair(){
  const parts=location.pathname.split('/').filter(Boolean);state.pairToken=parts[0]==='camera'?parts[1]||'':'';
  if(!state.pairToken){setStatus('Nieprawidłowy kod QR.');return}
  try{
    const response=await fetch('/api/camera/pair',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({pair_token:decodeURIComponent(state.pairToken)})});
    const data=await response.json();if(!response.ok)throw new Error(apiError(data,'Nie udało się sparować kamery.'));
    state.socketToken=data.socket_token;state.iceServers=data.ice_servers||[];
    history.replaceState(null,'','/camera/paired');
    state.ready=true;$('#start').disabled=false;setStatus('Kod zaakceptowany. Uruchom kamerę i pozostaw Safari otwarte.');
  }catch(error){setStatus(error.message)}
}

function selectedVideoConstraints(){
  const [width,height]=String($('#quality').value).split(':')[0].split('x').map(Number);const fps=Number($('#fps').value);
  return {facingMode:{ideal:$('#lens').value},width:{ideal:width},height:{ideal:height},frameRate:{ideal:fps,max:fps}};
}

async function preferH264(transceiver){
  if(!transceiver?.setCodecPreferences||!RTCRtpSender.getCapabilities)return;
  const codecs=RTCRtpSender.getCapabilities('video')?.codecs||[];
  const h264=codecs.filter(codec=>codec.mimeType.toLowerCase()==='video/h264');
  const rest=codecs.filter(codec=>codec.mimeType.toLowerCase()!=='video/h264');
  if(h264.length)transceiver.setCodecPreferences([...h264,...rest]);
}

async function applyBitrate(){
  const bitrate=Number(String($('#quality').value).split(':')[1]);const sender=state.pc?.getSenders().find(item=>item.track?.kind==='video');
  if(!sender)return;const parameters=sender.getParameters();if(!parameters.encodings?.length)parameters.encodings=[{}];parameters.encodings[0].maxBitrate=bitrate;
  try{await sender.setParameters(parameters)}catch{}
}

async function start(){
  if(!state.ready||state.stream)return;
  $('#start').disabled=true;setStatus('Uruchamianie kamery…');
  try{
    state.stream=await navigator.mediaDevices.getUserMedia({video:selectedVideoConstraints(),audio:$('#audio').checked});
    $('#preview').srcObject=state.stream;
    $('#placeholder').hidden=true;
    state.pc=new RTCPeerConnection({iceServers:state.iceServers});
    let videoTransceiver;
    for(const track of state.stream.getTracks()){
      const sender=state.pc.addTrack(track,state.stream);if(track.kind==='video')videoTransceiver=state.pc.getTransceivers().find(item=>item.sender===sender);
    }
    await preferH264(videoTransceiver);await applyBitrate();
    state.pc.onicecandidate=event=>{if(event.candidate&&state.ws?.readyState===WebSocket.OPEN)state.ws.send(JSON.stringify({type:'ice-candidate',payload:{candidate:event.candidate.candidate,mid:event.candidate.sdpMid}}))};
    state.pc.onconnectionstatechange=()=>{const value=state.pc?.connectionState||'closed';setStatus(value==='connected'?'Połączono z OBS — transmisja działa.':`WebRTC: ${value}`,value==='connected')};
    state.ws=new WebSocket(socketUrl(state.socketToken));
    state.ws.onmessage=async event=>{const message=JSON.parse(event.data);if(message.type==='peer'&&message.ready)await sendOffer();else if(message.type==='answer')await state.pc.setRemoteDescription(message.payload);else if(message.type==='ice-candidate'&&message.payload?.candidate)await state.pc.addIceCandidate({candidate:message.payload.candidate,sdpMid:message.payload.mid})};
    state.ws.onopen=()=>setStatus('Kamera działa — oczekiwanie na plugin OBS…');
    state.ws.onclose=()=>{if(state.stream)setStatus('Połączenie z serwerem zostało zamknięte.')};
    $('#stop').hidden=false;$('#start').hidden=true;await keepAwake();startStats();
  }catch(error){setStatus(error.name==='NotAllowedError'?'Safari nie otrzymało dostępu do kamery.':error.message);stop()}
}

async function sendOffer(){
  if(!state.pc||state.pc.signalingState!=='stable')return;
  const offer=await state.pc.createOffer();await state.pc.setLocalDescription(offer);
  state.ws.send(JSON.stringify({type:'offer',payload:{type:'offer',sdp:offer.sdp}}));
}

async function keepAwake(){try{if('wakeLock'in navigator)state.wakeLock=await navigator.wakeLock.request('screen')}catch{}}
async function updateStats(){
  const track=state.stream?.getVideoTracks()[0];if(!track)return;const settings=track.getSettings();let outbound='';
  const reports=state.pc?await state.pc.getStats():[];reports.forEach(report=>{if(report.type==='outbound-rtp'&&report.kind==='video')outbound=` · wysłano ${Math.round((report.bytesSent||0)/1048576)} MB`});
  $('#stats').textContent=`${settings.width||'?'}×${settings.height||'?'} · ${Math.round(settings.frameRate||0)} FPS${outbound}`;
}
function startStats(){clearInterval(state.statsTimer);state.statsTimer=setInterval(()=>updateStats().catch(()=>{}),1000)}
function stop(){clearInterval(state.statsTimer);state.statsTimer=null;state.ws?.close();state.ws=null;state.pc?.close();state.pc=null;state.stream?.getTracks().forEach(track=>track.stop());state.stream=null;$('#preview').srcObject=null;$('#placeholder').hidden=false;state.wakeLock?.release?.();state.wakeLock=null;$('#stop').hidden=true;$('#start').hidden=false;$('#start').disabled=!state.ready;setStatus(state.ready?'Transmisja zatrzymana. Możesz uruchomić ją ponownie.':'Transmisja zatrzymana.')}

$('#start').addEventListener('click',start);$('#stop').addEventListener('click',stop);window.addEventListener('pagehide',stop);pair();
