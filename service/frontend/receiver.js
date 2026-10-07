const video=document.querySelector('#video');
const status=document.querySelector('#state');
const token=decodeURIComponent(location.pathname.split('/').filter(Boolean)[1]||'');
let config,ws,pc,pending=[];
const socketUrl=()=>{const url=new URL('/camera-ws',location.origin);url.protocol=location.protocol==='https:'?'wss:':'ws:';url.searchParams.set('token',token);return url};
const send=(type,payload)=>ws?.readyState===WebSocket.OPEN&&ws.send(JSON.stringify({type,payload}));
function closePeer(){pending=[];pc?.close();pc=null;video.srcObject=null;status.hidden=false}
async function acceptOffer(payload){
  closePeer();
  const options={iceServers:config.ice_servers||[]};
  if(config.mode==='internet')options.iceTransportPolicy='relay';
  pc=new RTCPeerConnection(options);
  pc.onicecandidate=event=>event.candidate&&send('ice-candidate',{candidate:event.candidate.candidate,mid:event.candidate.sdpMid});
  pc.ontrack=event=>{video.srcObject=event.streams[0]||new MediaStream([event.track]);video.play().catch(()=>{});status.hidden=true};
  await pc.setRemoteDescription(payload);
  for(const candidate of pending.splice(0))await pc.addIceCandidate(candidate).catch(()=>{});
  const answer=await pc.createAnswer();await pc.setLocalDescription(answer);send('answer',{type:'answer',sdp:pc.localDescription.sdp});
}
async function start(){
  const response=await fetch('/api/camera/config',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({socket_token:token})});
  if(!response.ok){status.textContent='SESJA WYGASŁA';return}
  config=await response.json();
  ws=new WebSocket(socketUrl());
  ws.onmessage=async event=>{const message=JSON.parse(event.data);if(message.type==='offer')await acceptOffer(message.payload);if(message.type==='ice-candidate'&&message.payload?.candidate){const candidate={candidate:message.payload.candidate,sdpMid:message.payload.mid};if(pc?.remoteDescription)await pc.addIceCandidate(candidate).catch(()=>{});else pending.push(candidate)}};
  ws.onclose=()=>{closePeer();status.textContent='POŁĄCZENIE ZAKOŃCZONE'};
}
window.addEventListener('pagehide',()=>{ws?.close();closePeer()});
start().catch(()=>{status.textContent='BŁĄD ODBIORNIKA'});
