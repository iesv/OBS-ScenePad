// HTTP LAN pages cannot rely on WebCrypto's secure-context-only digest API.
const constants = new Uint32Array([
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
const rotate = (x,n) => (x>>>n)|(x<<(32-n));
export function sha256(text) {
  const bytes = new TextEncoder().encode(text);
  const data = new Uint8Array(Math.ceil((bytes.length+9)/64)*64);
  data.set(bytes); data[bytes.length]=128;
  const view = new DataView(data.buffer);
  view.setUint32(data.length-8,Math.floor(bytes.length/0x20000000));
  view.setUint32(data.length-4,bytes.length*8);
  const hash = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const w = new Uint32Array(64);
  for(let offset=0;offset<data.length;offset+=64) {
    for(let i=0;i<16;i++) w[i]=view.getUint32(offset+4*i);
    for(let i=16;i<64;i++) {
      const a=w[i-15],b=w[i-2];
      w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>>10));
    }
    let [a,b,c,d,e,f,g,h]=hash;
    for(let i=0;i<64;i++) {
      const t1=(h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+constants[i]+w[i])>>>0;
      const t2=((rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c)))>>>0;
      h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;
    }
    [a,b,c,d,e,f,g,h].forEach((v,i)=>{hash[i]+=v;});
  }
  const out=new Uint8Array(32); const output=new DataView(out.buffer);
  hash.forEach((v,i)=>output.setUint32(i*4,v)); return out;
}
const base64 = bytes => btoa(String.fromCharCode(...bytes));
const authenticationSecret = (password,salt) => base64(sha256(password+salt));

export class Connection {
  constructor(onEvent,onClose) { this.onEvent=onEvent;this.onClose=onClose;this.pending=new Map();this.counter=0; }
  connect(url,password,remembered=null) {
    return new Promise((resolve,reject)=>{
      const socket=this.socket=new WebSocket(url);
      let identified=false;
      const timer=setTimeout(()=>{reject(new Error('timeout'));socket.close();},10000);
      socket.onmessage=event=>{
        let packet;try { packet=JSON.parse(event.data); } catch { socket.close();return; }
        const {op,d}=packet;
        if(op===0) {
          if(!d.authentication) { clearTimeout(timer);reject(new Error('password-required'));socket.close();return; }
          const {salt,challenge}=d.authentication;
          const secret=password?authenticationSecret(password,salt):remembered?.salt===salt?remembered.secret:'';
          if(!secret){clearTimeout(timer);reject(new Error('authentication'));socket.close();return;}
          this.credential={salt,secret};
          password='';
          socket.send(JSON.stringify({op:1,d:{rpcVersion:1,eventSubscriptions:512,
            authentication:base64(sha256(secret+challenge))}}));
        } else if(op===2) { identified=true;clearTimeout(timer);resolve(); }
        else if(op===7) {
          const pending=this.pending.get(d.requestId); if(!pending)return;
          this.pending.delete(d.requestId);clearTimeout(pending.timer);
          if(!d.requestStatus?.result) pending.reject(new Error(d.requestStatus?.comment||'request'));
          else {
            try { pending.resolve(JSON.parse(d.responseData.responseData.payload)); }
            catch { pending.reject(new Error('response')); }
          }
        } else if(op===5 && d.eventType==='VendorEvent' && d.eventData?.vendorName==='ScenePad' &&
          (d.eventData.eventType==='Changed'||d.eventData.eventType==='Scroll')) {
          try { this.onEvent(JSON.parse(d.eventData.eventData.payload),d.eventData.eventType); } catch { /* Ignore malformed vendor events. */ }
        }
      };
      socket.onclose=event=>{
        clearTimeout(timer); if(!identified)reject(new Error(event?.code===4009?'authentication':'connection'));
        for(const pending of this.pending.values()) {clearTimeout(pending.timer);pending.reject(new Error('disconnected'));}
        this.pending.clear();this.onClose(event);
      };
      socket.onerror=()=>socket.close();
    });
  }
  request(input) {
    return new Promise((resolve,reject)=>{
      if(this.socket?.readyState!==1) {reject(new Error('disconnected'));return;}
      const id=String(++this.counter);
      const timer=setTimeout(()=>{this.pending.delete(id);reject(new Error('timeout'));},6000);
      this.pending.set(id,{resolve,reject,timer});
      this.socket.send(JSON.stringify({op:6,d:{requestType:'CallVendorRequest',requestId:id,requestData:{
        vendorName:'ScenePad',requestType:'Request',requestData:{payload:JSON.stringify(input)}}}}));
    });
  }
  close() {this.socket?.close();}
}
