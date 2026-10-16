const video=document.querySelector('#video');
const status=document.querySelector('#state');
const token=decodeURIComponent(location.pathname.split('/').filter(Boolean)[1]||'');
let config,ws,pc,pending=[];
const socketUrl=()=>{const url=new URL('/camera-ws',location.origin);url.protocol=location.protocol==='https:'?'wss:':'ws:';url.searchParams.set('token',token);return url};
const send=(type,payload)=>ws?.readyState===WebSocket.OPEN&&ws.send(JSON.stringify({type,payload}));
const setStreamVisible=visible=>document.body.classList.toggle('stream-active',Boolean(visible));
function syncStreamVisibility(){
  const track=video.srcObject?.getVideoTracks?.()[0];
  setStreamVisible(Boolean(pc?.connectionState==='connected'&&track?.readyState==='live'&&!track.muted&&video.readyState>=HTMLMediaElement.HAVE_CURRENT_DATA&&!video.paused));
}
function closePeer(clearCandidates=true){setStreamVisible(false);if(clearCandidates)pending=[];pc?.close();pc=null;video.pause();video.srcObject=null}
async function acceptOffer(payload){
  closePeer(false);
  const options={iceServers:config.ice_servers||[]};
  if(config.mode==='internet')options.iceTransportPolicy='relay';
  pc=new RTCPeerConnection(options);
  pc.onicecandidate=event=>event.candidate&&send('ice-candidate',{candidate:event.candidate.candidate,mid:event.candidate.sdpMid});
  pc.onconnectionstatechange=()=>{if(['disconnected','failed','closed'].includes(pc?.connectionState))setStreamVisible(false);else syncStreamVisibility()};
  pc.ontrack=event=>{const track=event.track;video.srcObject=event.streams[0]||new MediaStream([track]);track.addEventListener('mute',()=>setStreamVisible(false));track.addEventListener('ended',()=>setStreamVisible(false));track.addEventListener('unmute',syncStreamVisibility);video.play().then(syncStreamVisibility).catch(()=>setStreamVisible(false))};
  await pc.setRemoteDescription(payload);
  for(const candidate of pending.splice(0))await pc.addIceCandidate(candidate).catch(()=>{});
  const answer=await pc.createAnswer();await pc.setLocalDescription(answer);send('answer',{type:'answer',sdp:pc.localDescription.sdp});
}
async function start(){
  const response=await fetch('/api/camera/config',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({socket_token:token})});
  if(!response.ok){closePeer();return}
  config=await response.json();
  ws=new WebSocket(socketUrl());
  ws.onmessage=async event=>{const message=JSON.parse(event.data);if(message.type==='peer'&&!message.ready)closePeer();if(message.type==='offer')await acceptOffer(message.payload);if(message.type==='ice-candidate'&&message.payload?.candidate){const candidate={candidate:message.payload.candidate,sdpMid:message.payload.mid};if(pc?.remoteDescription)await pc.addIceCandidate(candidate).catch(()=>{});else pending.push(candidate)}};
  ws.onclose=closePeer;
}
for(const event of ['playing','canplay'])video.addEventListener(event,syncStreamVisibility);
for(const event of ['waiting','stalled','emptied','abort','error','pause'])video.addEventListener(event,()=>setStreamVisible(false));
window.addEventListener('pagehide',()=>{ws?.close();closePeer()});
start().catch(closePeer);
